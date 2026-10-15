#include "d3d_conversion_pipeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

using namespace gvfg::internal;

#include <d3dcompiler.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

static void d3d_preview_log_debug(const char *message)
{
#if GVFG_INTERNAL_DIAGNOSTICS
    OutputDebugStringA(message ? message : "");
#else
    (void)message;
#endif
}

static const char *ss_dxgi_format_name(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_UNKNOWN:
        return "UNKNOWN";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    default:
        return "OTHER";
    }
}

static void ssp_log_text(const char *msg)
{
    if (!msg)
        return;
    d3d_preview_log_debug(msg);
    d3d_preview_log_debug("\n");
}

#include <string>
#include <algorithm>

static const char *g_vs_src = R"(
struct VSIn  { float2 pos:POSITION; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(VSIn i){
  VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o;
}
)";

static const char *g_ps_422_to_nv12 = R"(
Texture2D<uint4> packed : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    int2 pixel = int2(pos.xy);
#if UV_PLANE
    uint4 a = packed.Load(int3(pixel.x, pixel.y * 2, 0));
    uint4 b = packed.Load(int3(pixel.x, pixel.y * 2 + 1, 0));
#if Y210_INPUT
    float2 uv = (float2((a.g >> 6) & 1023, (a.a >> 6) & 1023) +
                 float2((b.g >> 6) & 1023, (b.a >> 6) & 1023)) * (0.5 / 1020.0);
#else
    float2 uv = (float2(a.g, a.a) + float2(b.g, b.a)) * (0.5 / 255.0);
#endif
    return float4(saturate(uv), 0, 1);
#else
    uint4 p = packed.Load(int3(pixel.x >> 1, pixel.y, 0));
    uint y = (pixel.x & 1) ? p.b : p.r;
#if Y210_INPUT
    float luma = float((y >> 6) & 1023) / 1020.0;
#else
    float luma = float(y) / 255.0;
#endif
    return float4(saturate(luma), 0, 0, 1);
#endif
}
)";

// One shared RGB kernel for public buffer/texture conversion and preview.
// Native-size output reads one pixel. Scaled output interpolates decoded RGB
// to preserve the previous preview's bilinear filtering and clipping order.
static const char *g_ps_native_rgb = R"(
#if SOURCE_KIND == 4
Texture2D<float> sourceTexture : register(t0);
#elif SOURCE_KIND == 5
Texture2D<float4> sourceTexture : register(t0);
#else
Texture2D<uint4> sourceTexture : register(t0);
#endif
cbuffer NativeParameters : register(b0) { uint width; uint height; uint bits; uint filterMode; };
float3 read_rgb(int2 pixel)
{
    pixel = clamp(pixel, int2(0,0), int2(width-1,height-1));
#if SOURCE_KIND == 4
    float gray = sourceTexture.Load(int3(pixel,0));
    return gray.xxx;
#elif SOURCE_KIND == 5
    return sourceTexture.Load(int3(pixel,0)).rgb;
#else
    uint4 p = sourceTexture.Load(int3(pixel.x >> 1,pixel.y,0));
#if SOURCE_KIND == 6
    float maximum = exp2(float(clamp(bits,1u,16u))) - 1.0;
    float center = exp2(float(clamp(bits,1u,16u))-1.0);
    float y = float((pixel.x & 1) ? p.g : p.r) / maximum;
    float cb = (float(p.b)-center)/maximum;
    float cr = (float(p.a)-center)/maximum;
    return saturate(float3(y+1.402*cr,y-0.344136*cb-0.714136*cr,y+1.772*cb));
#else
#if SOURCE_KIND == 2
    uint y10 = (((pixel.x & 1) ? p.b : p.r) >> 6) & 1023;
    uint u10 = (p.g >> 6) & 1023;
    uint v10 = (p.a >> 6) & 1023;
    if ((width & 1) && pixel.x == int(width)-1) v10 = u10;
    float y = float(y10)/1023.0;
    float u = float(u10)/1023.0;
    float v = float(v10)/1023.0;
#else
    float y = float((pixel.x & 1) ? p.b : p.r)/255.0;
    float u = float(p.g)/255.0;
    float v = float(p.a)/255.0;
#endif
    float c=y*255.0-16.0, d=(u-0.5)*255.0, e=(v-0.5)*255.0;
    return saturate(float3(1.164383*c+1.792741*e,
                           1.164383*c-0.213249*d-0.532909*e,
                           1.164383*c+2.112402*d)/255.0);
#endif
#endif
}
float4 main(float4 pos : SV_Position,float2 uv : TEXCOORD0) : SV_Target
{
#if FILTERED
    float2 source = uv*float2(width,height)-0.5;
    int2 base = int2(floor(source));
    float2 fraction = frac(source);
    float3 top=lerp(read_rgb(base),read_rgb(base+int2(1,0)),fraction.x);
    float3 bottom=lerp(read_rgb(base+int2(0,1)),read_rgb(base+int2(1,1)),fraction.x);
    return float4(lerp(top,bottom,fraction.y),1);
#else
    return float4(read_rgb(int2(uv*float2(width,height))),1);
#endif
}
)";

// Only for enlargement: decode once per source pixel, then let the hardware
// sampler interpolate. It shares the exact same native RGB conversion kernel.
static const char *g_ps_scale_rgb = R"(
Texture2D<float4> sourceTexture : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 pos : SV_Position,float2 uv : TEXCOORD0) : SV_Target
{
    return float4(saturate(sourceTexture.Sample(linearSampler,uv).rgb),1);
}
)";
bool D3DPreviewPipeline::initialize(ID3D11Device *d3d,
                                    ID3D11DeviceContext *ctx)
{
    d3d_ = d3d;
    ctx_ = ctx;
    return d3d_ && ctx_;
}

bool D3DPreviewPipeline::configurePreview(const gvfg_render_preview_desc_t &desc)
{
    int requestedMode = desc.swapchain_10bit;
    if (requestedMode != GVFG_RENDER_PREVIEW_BITDEPTH_8BIT &&
        requestedMode != GVFG_RENDER_PREVIEW_BITDEPTH_10BIT &&
        requestedMode != GVFG_RENDER_PREVIEW_BITDEPTH_AUTO)
    {
        requestedMode = GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
    }

    const bool modeChanged = (preview_swapchain_mode_ != requestedMode);
    const bool hwndChanged = (preview_hwnd_ != desc.hwnd);
    const bool enabledChanged = (preview_enabled_ != (desc.enable_preview != 0));

    preview_hwnd_ = desc.hwnd;
    preview_enabled_ = (desc.enable_preview != 0);
    preview_swapchain_mode_ = requestedMode;

    if (modeChanged || hwndChanged || enabledChanged)
        release_preview_swapchain();

    return true;
}

void D3DPreviewPipeline::set_source_bit_depth(int bits)
{
    if (bits != 8 && bits != 10 && bits != 12 && bits != 16)
        bits = 0;

    const int oldEffectiveMode = (preview_swapchain_mode_ == GVFG_RENDER_PREVIEW_BITDEPTH_AUTO)
                                     ? ((preview_source_bit_depth_ >= 10) ? GVFG_RENDER_PREVIEW_BITDEPTH_10BIT : GVFG_RENDER_PREVIEW_BITDEPTH_8BIT)
                                     : preview_swapchain_mode_;
    const int newEffectiveMode = (preview_swapchain_mode_ == GVFG_RENDER_PREVIEW_BITDEPTH_AUTO)
                                     ? ((bits >= 10) ? GVFG_RENDER_PREVIEW_BITDEPTH_10BIT : GVFG_RENDER_PREVIEW_BITDEPTH_8BIT)
                                     : preview_swapchain_mode_;

    preview_source_bit_depth_ = bits;
    if (oldEffectiveMode != newEffectiveMode)
        release_preview_swapchain();
}

bool D3DPreviewPipeline::ensure_states()
{
    if (!d3d_ || !ctx_) return false;
    if (vs_ && il_ && vb_ && cs_params_) return true;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(g_vs_src, strlen(g_vs_src), nullptr, nullptr, nullptr,
                         "main", "vs_5_0", 0, 0, &code, &errors)) ||
        FAILED(d3d_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs_)))
        return false;
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if (FAILED(d3d_->CreateInputLayout(layout,2,code->GetBufferPointer(),code->GetBufferSize(),&il_))) return false;
    struct Vertex { float x,y,u,v; };
    const Vertex vertices[] = {{-1,-1,0,1},{-1,1,0,0},{1,1,1,0},
                               {-1,-1,0,1},{1,1,1,0},{1,-1,1,1}};
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth=sizeof(vertices); buffer.Usage=D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=vertices;
    if (FAILED(d3d_->CreateBuffer(&buffer,&initial,&vb_))) return false;
    buffer={}; buffer.ByteWidth=16; buffer.Usage=D3D11_USAGE_DYNAMIC;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(d3d_->CreateBuffer(&buffer,nullptr,&cs_params_));
}

void D3DPreviewPipeline::bind_quad()
{
    UINT stride=sizeof(float)*4, offset=0;
    ID3D11Buffer *vertices=vb_.Get();
    ctx_->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get()); ctx_->VSSetShader(vs_.Get(),nullptr,0);
    ctx_->GSSetShader(nullptr,nullptr,0); ctx_->HSSetShader(nullptr,nullptr,0); ctx_->DSSetShader(nullptr,nullptr,0);
    ctx_->RSSetState(nullptr); ctx_->OMSetBlendState(nullptr,nullptr,0xffffffff);
    ctx_->OMSetDepthStencilState(nullptr,0);
}

bool D3DPreviewPipeline::ensure_target(ComPtr<ID3D11Texture2D> &texture,
    ComPtr<ID3D11RenderTargetView> &view,int w,int h,DXGI_FORMAT format)
{
    if (w<=0 || h<=0) return false;
    if (texture) {
        D3D11_TEXTURE2D_DESC current{}; texture->GetDesc(&current);
        if (current.Width==static_cast<UINT>(w) && current.Height==static_cast<UINT>(h) && current.Format==format)
            return view || SUCCEEDED(d3d_->CreateRenderTargetView(texture.Get(),nullptr,&view));
    }
    ctx_->OMSetRenderTargets(0,nullptr,nullptr); view.Reset(); texture.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=w; desc.Height=h; desc.MipLevels=1; desc.ArraySize=1;
    desc.SampleDesc.Count=1; desc.Format=format; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(format==DXGI_FORMAT_R16G16B16A16_FLOAT) desc.BindFlags|=D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(d3d_->CreateTexture2D(&desc,nullptr,&texture)) &&
           SUCCEEDED(d3d_->CreateRenderTargetView(texture.Get(),nullptr,&view));
}

bool D3DPreviewPipeline::ensure_export_target(int w,int h,int format)
{
    if (!ensure_states()) return false;
    if (format==GVFG_GPU_OUTPUT_BGRA8)
        return ensure_target(rt_rgba_,rtv_rgba_,w,h,DXGI_FORMAT_B8G8R8A8_UNORM);
    if (format==GVFG_GPU_OUTPUT_RGB10A2)
        return ensure_target(rt_rgb10_,rtv_rgb10_,w,h,DXGI_FORMAT_R10G10B10A2_UNORM);
    if (format==GVFG_GPU_OUTPUT_NV12 && !(w&1) && !(h&1))
        return ensure_target(rt_nv12_y_,rtv_nv12_y_,w,h,DXGI_FORMAT_R8_UNORM) &&
               ensure_target(rt_nv12_uv_,rtv_nv12_uv_,w/2,h/2,DXGI_FORMAT_R8G8_UNORM);
    return false;
}

bool D3DPreviewPipeline::ensure_input_view(ID3D11Texture2D *texture,ComPtr<ID3D11ShaderResourceView> &view)
{
    ComPtr<ID3D11Resource> previous;
    if(view) view->GetResource(&previous);
    if(previous.Get()==texture) return true;
    view.Reset();
    return SUCCEEDED(d3d_->CreateShaderResourceView(texture,nullptr,&view));
}

bool D3DPreviewPipeline::draw_rgb(ID3D11Texture2D *texture,ID3D11ShaderResourceView *cachedView,
    gvfg_render_pixfmt_t format,int w,int h,int bits,ID3D11RenderTargetView *target,const D3D11_VIEWPORT &viewport)
{
    if(!texture || !target || w<=0 || h<=0 || !ensure_states()) return false;
    if(format!=GVFG_RENDER_FMT_YUY2 && format!=GVFG_RENDER_FMT_Y210 && format!=GVFG_RENDER_FMT_GRAY16 &&
       format!=GVFG_RENDER_FMT_RGBA16 && format!=GVFG_RENDER_FMT_DICOM_YBR_FULL_422) return false;
    if(!cachedView) {
        if(!ensure_input_view(texture,native_view_)) return false;
        cachedView=native_view_.Get();
    }
    const int filtered=(viewport.Width!=w || viewport.Height!=h) ? 1 : 0;
    if(filtered && static_cast<double>(viewport.Width)*viewport.Height>=static_cast<double>(w)*h)
        return upscale_rgb(texture,cachedView,format,w,h,bits,target,viewport);
    auto &shader=ps_rgb_[format][filtered];
    if(!shader) {
        const auto kind=std::to_string(static_cast<int>(format));
        D3D_SHADER_MACRO macros[]={{"SOURCE_KIND",kind.c_str()},{"FILTERED",filtered?"1":"0"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code, errors;
        if(FAILED(D3DCompile(g_ps_native_rgb,strlen(g_ps_native_rgb),nullptr,macros,nullptr,
                            "main","ps_5_0",0,0,&code,&errors)) ||
           FAILED(d3d_->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader))) return false;
    }
    const std::array<uint32_t,4> parameters={static_cast<uint32_t>(w),static_cast<uint32_t>(h),
        static_cast<uint32_t>((std::max)(1,(std::min)(16,bits))),static_cast<uint32_t>(filtered)};
    if(parameters!=native_params_) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx_->Map(cs_params_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return false;
        std::memcpy(mapped.pData,parameters.data(),sizeof(parameters)); ctx_->Unmap(cs_params_.Get(),0);
        native_params_=parameters;
    }
    bind_quad(); ctx_->OMSetRenderTargets(1,&target,nullptr); ctx_->RSSetViewports(1,&viewport);
    ID3D11Buffer *constants=cs_params_.Get(); ctx_->PSSetConstantBuffers(0,1,&constants);
    ctx_->PSSetShaderResources(0,1,&cachedView); ctx_->PSSetShader(shader.Get(),nullptr,0); ctx_->Draw(6,0);
    ID3D11ShaderResourceView *none=nullptr; ctx_->PSSetShaderResources(0,1,&none);
    return true;
}

bool D3DPreviewPipeline::upscale_rgb(ID3D11Texture2D *texture,ID3D11ShaderResourceView *view,
    gvfg_render_pixfmt_t format,int w,int h,int bits,ID3D11RenderTargetView *target,const D3D11_VIEWPORT &viewport)
{
    if(!ensure_target(scaled_rgb_,rtv_scaled_rgb_,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT) ||
       !ensure_input_view(scaled_rgb_.Get(),srv_scaled_rgb_)) return false;
    D3D11_VIEWPORT native{};native.Width=static_cast<float>(w);native.Height=static_cast<float>(h);native.MaxDepth=1;
    // Native-size recursion always takes the direct one-draw path.
    if(!draw_rgb(texture,view,format,w,h,bits,rtv_scaled_rgb_.Get(),native)) return false;
    if(!ps_scaled_rgb_) {
        ComPtr<ID3DBlob> code,errors;
        if(FAILED(D3DCompile(g_ps_scale_rgb,strlen(g_ps_scale_rgb),nullptr,nullptr,nullptr,
                            "main","ps_5_0",0,0,&code,&errors)) ||
           FAILED(d3d_->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps_scaled_rgb_)))
            return false;
    }
    if(!linear_sampler_) {
        D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(d3d_->CreateSamplerState(&desc,&linear_sampler_))) return false;
    }
    bind_quad();ctx_->OMSetRenderTargets(1,&target,nullptr);ctx_->RSSetViewports(1,&viewport);
    ID3D11ShaderResourceView *source=srv_scaled_rgb_.Get();ctx_->PSSetShaderResources(0,1,&source);
    ID3D11SamplerState *sampler=linear_sampler_.Get();ctx_->PSSetSamplers(0,1,&sampler);
    ctx_->PSSetShader(ps_scaled_rgb_.Get(),nullptr,0);ctx_->Draw(6,0);
    ID3D11ShaderResourceView *none=nullptr;ctx_->PSSetShaderResources(0,1,&none);
    return true;
}

bool D3DPreviewPipeline::draw_uploaded_rgb(gvfg_render_pixfmt_t format,int w,int h,DXGI_FORMAT output)
{
    const int index=format==GVFG_RENDER_FMT_Y210?1:0;
    auto *texture=index?upload_y210_packed_.Get():upload_yuy2_packed_.Get();
    if(!texture || !ensure_input_view(texture,srv_uploaded_422_[index])) return false;
    D3D11_VIEWPORT viewport{}; viewport.Width=static_cast<float>(w); viewport.Height=static_cast<float>(h); viewport.MaxDepth=1;
    auto *target=output==DXGI_FORMAT_B8G8R8A8_UNORM?rtv_rgba_.Get():rtv_rgb10_.Get();
    return draw_rgb(texture,srv_uploaded_422_[index].Get(),format,w,h,index?10:8,target,viewport);
}
bool D3DPreviewPipeline::blit_uploaded_422_to_nv12(gvfg_render_pixfmt_t fmt, int w, int h)
{
    if (!ctx_ || !d3d_ || !vs_ || !il_ || !vb_ || !rtv_nv12_y_ || !rtv_nv12_uv_ ||
        w <= 0 || h <= 0 || (w & 1) || (h & 1) ||
        (fmt != GVFG_RENDER_FMT_YUY2 && fmt != GVFG_RENDER_FMT_Y210))
        return false;
    const int format = fmt == GVFG_RENDER_FMT_Y210 ? 1 : 0;
    ID3D11Texture2D *texture = format ? upload_y210_packed_.Get() : upload_yuy2_packed_.Get();
    if (!texture) return false;
    auto &view = srv_uploaded_422_[format];
    ComPtr<ID3D11Resource> viewedResource;
    if (view) view->GetResource(&viewedResource);
    if (viewedResource.Get() != texture)
    {
        view.Reset();
        if (FAILED(d3d_->CreateShaderResourceView(texture, nullptr, &view))) return false;
    }
    for (int plane = 0; plane < 2; ++plane)
    {
        auto &shader = ps_422_to_nv12_[format][plane];
        if (!shader)
        {
            D3D_SHADER_MACRO macros[] = {
                {"Y210_INPUT", format ? "1" : "0"},
                {"UV_PLANE", plane ? "1" : "0"}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(D3DCompile(g_ps_422_to_nv12, strlen(g_ps_422_to_nv12), nullptr,
                                  macros, nullptr, "main", "ps_5_0", 0, 0, &code, &errors)) ||
                FAILED(d3d_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, &shader))) return false;
        }
    }
    bind_quad();
    ID3D11ShaderResourceView *source = view.Get();
    ctx_->PSSetShaderResources(0, 1, &source);
    for (int plane = 0; plane < 2; ++plane)
    {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<FLOAT>(plane ? w / 2 : w);
        viewport.Height = static_cast<FLOAT>(plane ? h / 2 : h);
        viewport.MaxDepth = 1.0f;
        ID3D11RenderTargetView *target = plane ? rtv_nv12_uv_.Get() : rtv_nv12_y_.Get();
        ctx_->OMSetRenderTargets(1, &target, nullptr);
        ctx_->RSSetViewports(1, &viewport);
        ctx_->PSSetShader(ps_422_to_nv12_[format][plane].Get(), nullptr, 0);
        ctx_->Draw(6, 0);
    }
    ID3D11ShaderResourceView *none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    return true;
}

bool D3DPreviewPipeline::readback_to_buffer(void *destination,
                                            uint64_t destination_size,
                                            int destination_row_bytes,
                                            DXGI_FORMAT destination_format,
                                            int frame_w,
                                            int frame_h)
{
    if (!d3d_ || !ctx_ || !destination || frame_w <= 0 || frame_h <= 0)
        return false;

    ID3D11Texture2D *source = nullptr;
    ComPtr<ID3D11Texture2D> *staging = nullptr;
    if (destination_format == DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        source = rt_rgba_.Get();
        staging = &readback_bgra8_;
    }
    else if (destination_format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        source = rt_rgb10_.Get();
        staging = &readback_rgb10_;
    }
    else
    {
        return false;
    }

    const uint64_t minimumRowBytes = static_cast<uint64_t>(frame_w) * 4u;
    if (!source || destination_row_bytes < minimumRowBytes ||
        destination_size < static_cast<uint64_t>(destination_row_bytes) * static_cast<uint64_t>(frame_h))
        return false;

    bool recreate = !*staging;
    if (*staging)
    {
        D3D11_TEXTURE2D_DESC existing{};
        (*staging)->GetDesc(&existing);
        recreate = existing.Width != static_cast<UINT>(frame_w) ||
                   existing.Height != static_cast<UINT>(frame_h) ||
                   existing.Format != destination_format;
    }
    if (recreate)
    {
        staging->Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(frame_w);
        desc.Height = static_cast<UINT>(frame_h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = destination_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d_->CreateTexture2D(&desc, nullptr, staging->ReleaseAndGetAddressOf())))
            return false;
    }

    ctx_->CopyResource(staging->Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging->Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    auto *dst = static_cast<uint8_t *>(destination);
    const size_t copyBytes = static_cast<size_t>(minimumRowBytes);
    for (int row = 0; row < frame_h; ++row)
    {
        const auto *srcRow = static_cast<const uint8_t *>(mapped.pData) +
                             static_cast<size_t>(row) * mapped.RowPitch;
        std::memcpy(dst + static_cast<size_t>(row) * static_cast<size_t>(destination_row_bytes),
                    srcRow,
                    copyBytes);
    }
    ctx_->Unmap(staging->Get(), 0);
    return true;
}

bool D3DPreviewPipeline::readback_nv12_to_buffer(void *destination,
                                                 uint64_t destination_size,
                                                 int destination_row_bytes,
                                                 int frame_w,
                                                 int frame_h)
{
    if (!d3d_ || !ctx_ || !destination || !rt_nv12_y_ || !rt_nv12_uv_ ||
        frame_w <= 0 || frame_h <= 0 || (frame_w & 1) != 0 || (frame_h & 1) != 0 ||
        destination_row_bytes < frame_w)
        return false;
    const uint64_t rows = static_cast<uint64_t>(frame_h) + static_cast<uint64_t>(frame_h / 2);
    if (destination_size < static_cast<uint64_t>(destination_row_bytes) * rows)
        return false;

    auto ensureStaging = [this](ComPtr<ID3D11Texture2D> &texture, UINT width, UINT height,
                                DXGI_FORMAT format)
    {
        if (texture)
        {
            D3D11_TEXTURE2D_DESC current{};
            texture->GetDesc(&current);
            if (current.Width == width && current.Height == height && current.Format == format)
                return true;
            texture.Reset();
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(d3d_->CreateTexture2D(&desc, nullptr, &texture));
    };
    if (!ensureStaging(readback_nv12_y_, static_cast<UINT>(frame_w), static_cast<UINT>(frame_h),
                       DXGI_FORMAT_R8_UNORM) ||
        !ensureStaging(readback_nv12_uv_, static_cast<UINT>(frame_w / 2), static_cast<UINT>(frame_h / 2),
                       DXGI_FORMAT_R8G8_UNORM))
        return false;

    ctx_->CopyResource(readback_nv12_y_.Get(), rt_nv12_y_.Get());
    ctx_->CopyResource(readback_nv12_uv_.Get(), rt_nv12_uv_.Get());
    auto *dst = static_cast<uint8_t *>(destination);
    auto copyPlane = [this, dst, destination_row_bytes](ID3D11Texture2D *source,
                                                        ID3D11Texture2D *staging,
                                                        int rows, int copyBytes,
                                                        size_t destinationOffset)
    {
        (void)source;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        for (int row = 0; row < rows; ++row)
            std::memcpy(dst + destinationOffset + static_cast<size_t>(row) * destination_row_bytes,
                        static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                        static_cast<size_t>(copyBytes));
        ctx_->Unmap(staging, 0);
        return true;
    };
    return copyPlane(rt_nv12_y_.Get(), readback_nv12_y_.Get(), frame_h, frame_w, 0) &&
           copyPlane(rt_nv12_uv_.Get(), readback_nv12_uv_.Get(), frame_h / 2, frame_w,
                     static_cast<size_t>(destination_row_bytes) * static_cast<size_t>(frame_h));
}

bool D3DPreviewPipeline::upload_packed_422_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h)
{
    if (!d3d_ || !ctx_ || !data || frame_w <= 0 || frame_h <= 0 || src_stride <= 0)
        return false;

    const int w2 = (frame_w + 1) / 2;
    if (!upload_yuy2_packed_)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w2;
        td.Height = (UINT)frame_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &upload_yuy2_packed_)))
            return false;
    }
    else
    {
        D3D11_TEXTURE2D_DESC td{};
        upload_yuy2_packed_->GetDesc(&td);
        if ((int)td.Width != w2 || (int)td.Height != frame_h || td.Format != DXGI_FORMAT_R8G8B8A8_UINT)
        {
            upload_yuy2_packed_.Reset();
            return upload_packed_422_frame(data, src_stride, frame_w, frame_h);
        }
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx_->Map(upload_yuy2_packed_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr))
        return false;

    for (int row = 0; row < frame_h; ++row)
    {
        const uint8_t *srcRow = data + (size_t)row * src_stride;
        uint8_t *dstRow = static_cast<uint8_t *>(m.pData) + (size_t)row * m.RowPitch;
        if ((frame_w & 1) != 0) std::memset(dstRow + static_cast<size_t>(frame_w) * 2, 0, 2);
        std::memcpy(dstRow, srcRow, static_cast<size_t>(frame_w) * 2);
    }

    ctx_->Unmap(upload_yuy2_packed_.Get(), 0);
    return true;
}

bool D3DPreviewPipeline::upload_y210_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h)
{
    if (!ctx_ || !d3d_ || !data || frame_w <= 0 || frame_h <= 0)
        return false;

    const int effectiveStride = (src_stride > 0) ? src_stride : (frame_w * 4);
    const int minStride = frame_w * 4;
    if (effectiveStride < minStride)
        return false;

    const int w2 = (frame_w + 1) / 2;
    if (upload_y210_packed_)
    {
        D3D11_TEXTURE2D_DESC desc{};
        upload_y210_packed_->GetDesc(&desc);
        if ((int)desc.Width != w2 || (int)desc.Height != frame_h || desc.Format != DXGI_FORMAT_R16G16B16A16_UINT)
            upload_y210_packed_.Reset();
    }

    if (!upload_y210_packed_)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w2;
        td.Height = (UINT)frame_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_UINT;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &upload_y210_packed_)))
            return false;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(upload_y210_packed_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;

    const size_t sourceRowBytes = (size_t)frame_w * 4u;
    const size_t textureRowBytes = (size_t)w2 * 8u;
    for (int row = 0; row < frame_h; ++row)
    {
        const uint8_t *srcRow = data + (size_t)row * (size_t)effectiveStride;
        uint8_t *dstRow = static_cast<uint8_t *>(m.pData) + (size_t)row * (size_t)m.RowPitch;
        // Keep the FPGA payload unchanged. The pixel shader performs the
        // left-aligned 10-bit normalization and U/V reorder on the GPU.
        if (textureRowBytes > sourceRowBytes)
            std::memset(dstRow, 0, textureRowBytes);
        std::memcpy(dstRow, srcRow, sourceRowBytes);
    }

    ctx_->Unmap(upload_y210_packed_.Get(), 0);
    return true;
}

void D3DPreviewPipeline::release_preview_swapchain()
{
    if (ctx_)
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    preview_rtv_.Reset();
    preview_backbuf_.Reset();
    preview_swapchain_.Reset();
    preview_w_ = 0;
    preview_h_ = 0;
    preview_swapchain_10bit_ = false;
    preview_swapchain_format_ = DXGI_FORMAT_UNKNOWN;
}

bool D3DPreviewPipeline::ensure_preview_swapchain(int w, int h)
{
    if (!preview_enabled_ || !preview_hwnd_ || !d3d_)
        return false;

    if (w <= 0 || h <= 0)
        return false;

    RECT rc{};
    if (!GetClientRect((HWND)preview_hwnd_, &rc))
        return false;

    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;

    if (clientW <= 0 || clientH <= 0)
    {
        clientW = w;
        clientH = h;
    }

    const int effectiveMode = (preview_swapchain_mode_ == GVFG_RENDER_PREVIEW_BITDEPTH_AUTO)
                                  ? ((preview_source_bit_depth_ >= 10) ? GVFG_RENDER_PREVIEW_BITDEPTH_10BIT : GVFG_RENDER_PREVIEW_BITDEPTH_8BIT)
                                  : preview_swapchain_mode_;
    const DXGI_FORMAT desiredFormat = (effectiveMode == GVFG_RENDER_PREVIEW_BITDEPTH_8BIT)
                                          ? DXGI_FORMAT_B8G8R8A8_UNORM
                                          : DXGI_FORMAT_R10G10B10A2_UNORM;

    if (preview_swapchain_ && preview_swapchain_format_ != DXGI_FORMAT_UNKNOWN && preview_swapchain_format_ != desiredFormat)
    {
        char msg[320] = {};
        std::snprintf(msg, sizeof(msg),
                      "[SharedScene] preview swapchain format change: mode=%d sourceBitDepth=%d old=%s desired=%s",
                      preview_swapchain_mode_, preview_source_bit_depth_, ss_dxgi_format_name(preview_swapchain_format_), ss_dxgi_format_name(desiredFormat));
        ssp_log_text(msg);
        release_preview_swapchain();
    }

    if (!preview_swapchain_)
    {
        // Factory discovery is needed only while creating a swapchain.  It
        // used to run for every preview frame even when the existing
        // swapchain size and format were unchanged.
        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(d3d_->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || !dxgiDev)
            return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter)
            return false;

        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                      reinterpret_cast<void **>(factory.GetAddressOf()))) ||
            !factory)
            return false;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)clientW;
        sd.Height = (UINT)clientH;
        sd.Format = desiredFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        HRESULT hr = factory->CreateSwapChainForHwnd(
            d3d_,
            (HWND)preview_hwnd_,
            &sd,
            nullptr,
            nullptr,
            &preview_swapchain_);

        if (FAILED(hr) || !preview_swapchain_)
        {
            char err[320] = {};
            std::snprintf(err, sizeof(err),
                          "[SharedScene] preview swapchain create failed hr=0x%08X format=%s",
                          static_cast<unsigned>(hr),
                          ss_dxgi_format_name(sd.Format));
            ssp_log_text(err);
            return false;
        }

        preview_swapchain_format_ = sd.Format;
        preview_swapchain_10bit_ = (sd.Format == DXGI_FORMAT_R10G10B10A2_UNORM);

        {
            char msg[320] = {};
            std::snprintf(msg, sizeof(msg),
                          "[SharedScene] preview swapchain created: %dx%d requestedMode=%d sourceBitDepth=%d effectiveMode=%d actualFormat=%s actual10bit=%d",
                          clientW, clientH, preview_swapchain_mode_, preview_source_bit_depth_, effectiveMode, ss_dxgi_format_name(sd.Format), preview_swapchain_10bit_ ? 1 : 0);
            ssp_log_text(msg);
        }

        factory->MakeWindowAssociation((HWND)preview_hwnd_, DXGI_MWA_NO_ALT_ENTER);
    }
    else if (preview_w_ != clientW || preview_h_ != clientH)
    {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        preview_rtv_.Reset();
        preview_backbuf_.Reset();

        HRESULT hr = preview_swapchain_->ResizeBuffers(
            0,
            (UINT)clientW,
            (UINT)clientH,
            DXGI_FORMAT_UNKNOWN,
            0);

        if (FAILED(hr))
        {
            char err[256] = {};
            std::snprintf(err, sizeof(err),
                          "[SharedScene] preview swapchain resize failed hr=0x%08X size=%dx%d",
                          static_cast<unsigned>(hr),
                          clientW,
                          clientH);
            ssp_log_text(err);
            release_preview_swapchain();
            return false;
        }

        {
            char msg[256] = {};
            std::snprintf(msg, sizeof(msg),
                          "[SharedScene] preview swapchain resized: %dx%d",
                          clientW, clientH);
            ssp_log_text(msg);
        }
    }

    if (!preview_backbuf_)
    {
        HRESULT hr = preview_swapchain_->GetBuffer(0, IID_PPV_ARGS(&preview_backbuf_));
        if (FAILED(hr) || !preview_backbuf_)
        {
            char err[256] = {};
            std::snprintf(err, sizeof(err),
                          "[SharedScene] preview backbuffer get failed hr=0x%08X",
                          static_cast<unsigned>(hr));
            ssp_log_text(err);
            release_preview_swapchain();
            return false;
        }
    }

    if (!preview_rtv_)
    {
        HRESULT hr = d3d_->CreateRenderTargetView(preview_backbuf_.Get(), nullptr, &preview_rtv_);
        if (FAILED(hr) || !preview_rtv_)
        {
            char err[256] = {};
            std::snprintf(err, sizeof(err),
                          "[SharedScene] preview RTV create failed hr=0x%08X",
                          static_cast<unsigned>(hr));
            ssp_log_text(err);
            release_preview_swapchain();
            return false;
        }
    }

    preview_w_ = clientW;
    preview_h_ = clientH;
    return true;
}

gvfg_preview_present_result_t D3DPreviewPipeline::present_texture(ID3D11Texture2D *texture, ID3D11ShaderResourceView *view, gvfg_render_pixfmt_t format, int width, int height, int bits)
{
    if(!preview_enabled_ || !preview_rtv_ || !preview_swapchain_ || !ctx_ || width<=0 || height<=0)
        return GVFG_PREVIEW_PRESENT_FAILED;
    float drawW=static_cast<float>(preview_w_), drawH=static_cast<float>(preview_h_), x=0, y=0;
    const float aspect=static_cast<float>(width)/height;
    if(drawW/drawH>aspect) {drawW=drawH*aspect; x=(preview_w_-drawW)*0.5f;}
    else {drawH=drawW/aspect; y=(preview_h_-drawH)*0.5f;}
    drawW=(std::min)(drawW,static_cast<float>(preview_w_)); drawH=(std::min)(drawH,static_cast<float>(preview_h_));
    x=(preview_w_-drawW)*0.5f; y=(preview_h_-drawH)*0.5f;
    const float black[4]={0,0,0,1}; ctx_->ClearRenderTargetView(preview_rtv_.Get(),black);
    D3D11_VIEWPORT viewport{}; viewport.TopLeftX=x; viewport.TopLeftY=y; viewport.Width=drawW; viewport.Height=drawH; viewport.MaxDepth=1;
    if(!draw_rgb(texture,
                 view,
                 format,width,height,bits,
                 preview_rtv_.Get(),viewport)) return GVFG_PREVIEW_PRESENT_FAILED;
    HRESULT result=preview_swapchain_->Present(0,0);
    if(FAILED(result)){release_preview_swapchain(); return GVFG_PREVIEW_PRESENT_FAILED;}
    return GVFG_PREVIEW_PRESENTED;
}
bool D3DPreviewPipeline::clear_preview_black()
{
    if (!preview_enabled_ || !preview_swapchain_ || !preview_rtv_ || !ctx_)
        return false;

    ID3D11RenderTargetView *rtv = preview_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    const float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv, black);
    ctx_->Flush();
    return SUCCEEDED(preview_swapchain_->Present(0, 0));
}

DXGI_FORMAT D3DPreviewPipeline::preview_backbuffer_format() const
{
    if (preview_swapchain_format_ != DXGI_FORMAT_UNKNOWN)
        return preview_swapchain_format_;
    if (!preview_backbuf_)
        return DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC d{};
    preview_backbuf_->GetDesc(&d);
    return d.Format;
}
