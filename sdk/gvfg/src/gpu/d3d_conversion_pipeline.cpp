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
    OutputDebugStringA(message ? message : "");
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

static const char *g_vs_src = R"(
struct VSIn  { float2 pos:POSITION; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(VSIn i){
  VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o;
}
)";

// YVYU 4:2:2 packed input.
// Each upload texture texel stores two pixels as R=Y0, G=U, B=Y1, A=V.
//   R=Y0, G=U, B=Y1, A=V
// texture width = ceil(w/2)
static const char *g_ps_yuy2 = R"(
// YVYU (4:2:2 packed):
// Each texel packs 2 pixels: R=Y0, G=U, B=Y1, A=V
// texture width = ceil(w/2)
Texture2D<uint4> texP : register(t0);


cbuffer ProcAmp : register(b0)
{
    uint  width;
    uint  height;
    float invW;
    float invH;

    float br;     // brightness offset (normalized, about [-0.5..0.5])
    float ct;     // contrast factor (1.0 = neutral)
    float sat;    // saturation factor (1.0 = neutral)
    float hueSin; // sin(hue)

    float hueCos;   // cos(hue)
    float sharpAmt; // [-1..+1], 0 = neutral
    float pad0;
    float pad1;
};

static float3 apply_rgb_procamp(float3 rgb)
{
    // contrast + brightness
    rgb = (rgb - 0.5) * ct + 0.5 + br;

    // saturation
    float l = dot(rgb, float3(0.299, 0.587, 0.114));
    rgb = lerp(float3(l, l, l), rgb, sat);

    return saturate(rgb);
}

static float2 rotate_uv(float2 uv01)
{
    // uv01 is 0..1, convert to signed around 0
    float2 uv = uv01 - 0.5;
    float u = uv.x;
    float v = uv.y;
    float u2 = u * hueCos - v * hueSin;
    float v2 = u * hueSin + v * hueCos;
    return float2(u2, v2) + 0.5;
}


float3 yuv_to_rgb709(float y, float u, float v)
{
    y = y * 255.0;
    u = (u - 0.5) * 255.0;
    v = (v - 0.5) * 255.0;
    float c = y - 16.0;
    float d = u;
    float e = v;
    float r = 1.164383 * c + 1.792741 * e;
    float g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
    float b = 1.164383 * c + 2.112402 * d;
    return float3(r,g,b)/255.0;
}

float loadY(int x, int y)
{
    x = clamp(x, 0, (int)width - 1);
    y = clamp(y, 0, (int)height - 1);
    uint4 p = texP.Load(int3(x >> 1, y, 0));
    uint yy = ((x & 1) != 0) ? p.b : p.r;
    return (float)yy / 255.0;
}

float2 loadUV01(int x, int y)
{
    x = clamp(x, 0, (int)width - 1);
    y = clamp(y, 0, (int)height - 1);
    uint4 p = texP.Load(int3(x >> 1, y, 0));
    float u = (float)p.a / 255.0;
    float v = (float)p.g / 255.0;
    return float2(u, v);
}

float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target
{
    int2 ip = int2(pos.xy);
    int px = ip.x;
    int py = ip.y;

    // Sharpness on luma
    float yC = loadY(px, py);
    float yL = loadY(px - 1, py);
    float yR = loadY(px + 1, py);
    float yU = loadY(px, py - 1);
    float yD = loadY(px, py + 1);
    float blur = (yC*4.0 + yL + yR + yU + yD) / 8.0;
    float y = saturate(yC + sharpAmt * (yC - blur));

    float2 uv01 = loadUV01(px, py);
    uv01 = rotate_uv(uv01);

    float3 rgb = yuv_to_rgb709(y, uv01.x, uv01.y);
    rgb = apply_rgb_procamp(rgb);
    return float4(rgb, 1.0);
}
)";

// Raw FPGA Y210 payload with each 10-bit component left-aligned in a 16-bit word.
// Each upload texture texel stores two pixels as R=Y0, G=V, B=Y1, A=U.
// The shader performs the 10-bit normalization and U/V reorder.
// texture width = ceil(w/2), format = R16G16B16A16_UINT
static const char *g_ps_y210 = R"(
Texture2D<uint4> texP : register(t0);

// true: interpret the raw chroma words as Y0,V,Y1,U.
// false: interpret them as standard Y210 Y0,U,Y1,V.
static const bool kSwapUV = true;

cbuffer ProcAmp : register(b0)
{
    uint  width;
    uint  height;
    float invW;
    float invH;

    float br;
    float ct;
    float sat;
    float hueSin;

    float hueCos;
    float sharpAmt;
    float pad0;
    float pad1;
};

static float3 apply_rgb_procamp(float3 rgb)
{
    rgb = (rgb - 0.5) * ct + 0.5 + br;
    float l = dot(rgb, float3(0.299, 0.587, 0.114));
    rgb = lerp(float3(l, l, l), rgb, sat);
    return saturate(rgb);
}

static float2 rotate_uv(float2 uv01)
{
    float2 uv = uv01 - 0.5;
    float u = uv.x;
    float v = uv.y;
    float u2 = u * hueCos - v * hueSin;
    float v2 = u * hueSin + v * hueCos;
    return float2(u2, v2) + 0.5;
}

float3 yuv_to_rgb709(float y, float u, float v)
{
    y = y * 255.0;
    u = (u - 0.5) * 255.0;
    v = (v - 0.5) * 255.0;
    float c = y - 16.0;
    float d = u;
    float e = v;
    float r = 1.164383 * c + 1.792741 * e;
    float g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
    float b = 1.164383 * c + 2.112402 * d;
    return float3(r,g,b)/255.0;
}

float loadY(int x, int y)
{
    x = clamp(x, 0, (int)width - 1);
    y = clamp(y, 0, (int)height - 1);
    uint4 p = texP.Load(int3(x >> 1, y, 0));
    uint yy = (((x & 1) != 0) ? p.b : p.r) >> 6;
    return (float)(yy & 1023) / 1023.0;
}

float2 loadUV01(int x, int y)
{
    x = clamp(x, 0, (int)width - 1);
    y = clamp(y, 0, (int)height - 1);
    uint4 p = texP.Load(int3(x >> 1, y, 0));
    uint chroma1 = (p.g >> 6) & 1023;
    uint chroma2 = (p.a >> 6) & 1023;
    uint u10 = kSwapUV ? chroma2 : chroma1;
    uint v10 = kSwapUV ? chroma1 : chroma2;
    // An odd-width tail has no complete second chroma word; match the old
    // upload behavior by reusing the available chroma component.
    if (((width & 1) != 0) && x == (int)width - 1)
    {
        if (kSwapUV)
            u10 = v10;
        else
            v10 = u10;
    }
    float u = (float)u10 / 1023.0;
    float v = (float)v10 / 1023.0;
    return float2(u, v);
}

float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target
{
    int2 ip = int2(pos.xy);
    int px = ip.x;
    int py = ip.y;

    float yC = loadY(px, py);
    float yL = loadY(px - 1, py);
    float yR = loadY(px + 1, py);
    float yU = loadY(px, py - 1);
    float yD = loadY(px, py + 1);
    float blur = (yC*4.0 + yL + yR + yU + yD) / 8.0;
    float y = saturate(yC + sharpAmt * (yC - blur));

    float2 uv01 = loadUV01(px, py);
    uv01 = rotate_uv(uv01);

    float3 rgb = yuv_to_rgb709(y, uv01.x, uv01.y);
    rgb = apply_rgb_procamp(rgb);
    return float4(rgb, 1.0);
}
)";

static const char *g_ps_fp16_to_rgba8 = R"(
Texture2D<float4> tex0 : register(t0);
SamplerState samL : register(s0);

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    float4 c = tex0.Sample(samL, i.uv);
    return saturate(c);
}
)";

// RGB to studio-range BT.709 NV12. Rendering the UV target at half resolution
// makes the linear sample at each output texel average the corresponding 2x2
// source pixels.
static const char *g_ps_fp16_to_nv12_y = R"(
Texture2D<float4> tex0 : register(t0);
SamplerState samL : register(s0);
float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 rgb = saturate(tex0.Sample(samL, uv).rgb);
    return saturate(16.0 / 255.0 + dot(rgb, float3(0.182586, 0.614231, 0.062007)));
}
)";

static const char *g_ps_fp16_to_nv12_uv = R"(
Texture2D<float4> tex0 : register(t0);
SamplerState samL : register(s0);
float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 rgb = saturate(tex0.Sample(samL, uv).rgb);
    float u = 128.0 / 255.0 + dot(rgb, float3(-0.100644, -0.338572, 0.439216));
    float v = 128.0 / 255.0 + dot(rgb, float3( 0.439216, -0.398942, -0.040274));
    return saturate(float2(u, v));
}
)";

static const char *g_ps_fp16_to_preview = R"(
Texture2D<float4> tex0 : register(t0);
SamplerState samL : register(s0);

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    float4 c = tex0.Sample(samL, i.uv);
    return float4(saturate(c.rgb), 1.0);
}
)";

static const char *g_ps_rgba8_to_preview = R"(
Texture2D<float4> tex0 : register(t0);
SamplerState samL : register(s0);

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    float4 c = tex0.Sample(samL, i.uv);
    c.rgb = saturate(c.rgb);
    return c;
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

bool D3DPreviewPipeline::create_shaders_and_states()
{
    if (vs_ && il_ &&
        ps_yuy2_ && ps_y210_ &&
        ps_fp16_to_rgba8_ &&
        ps_fp16_to_nv12_y_ && ps_fp16_to_nv12_uv_ &&
        ps_fp16_to_preview_ && ps_rgba8_to_preview_ &&
        vb_ && samp_ && cs_params_)
        return true;

    // Compile shaders
    ComPtr<ID3DBlob> vsb, psb3, psb4, psb5, psb6, err;
    if (FAILED(D3DCompile(g_vs_src, strlen(g_vs_src), nullptr, nullptr, nullptr,
                          "main", "vs_5_0", 0, 0, &vsb, &err)))
        return false;
    if (FAILED(d3d_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)))
        return false;

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(d3d_->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il_)))
        return false;

    if (FAILED(D3DCompile(g_ps_yuy2, strlen(g_ps_yuy2), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb3, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb3->GetBufferPointer(), psb3->GetBufferSize(), nullptr, &ps_yuy2_)))
        return false;

    ComPtr<ID3DBlob> psbY210;
    if (FAILED(D3DCompile(g_ps_y210, strlen(g_ps_y210), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psbY210, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psbY210->GetBufferPointer(), psbY210->GetBufferSize(), nullptr, &ps_y210_)))
        return false;

    if (FAILED(D3DCompile(g_ps_fp16_to_rgba8, strlen(g_ps_fp16_to_rgba8), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb4, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb4->GetBufferPointer(), psb4->GetBufferSize(), nullptr, &ps_fp16_to_rgba8_)))
        return false;

    ComPtr<ID3DBlob> psbNv12Y, psbNv12Uv;
    if (FAILED(D3DCompile(g_ps_fp16_to_nv12_y, strlen(g_ps_fp16_to_nv12_y), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psbNv12Y, &err)) ||
        FAILED(d3d_->CreatePixelShader(psbNv12Y->GetBufferPointer(), psbNv12Y->GetBufferSize(),
                                       nullptr, &ps_fp16_to_nv12_y_)))
        return false;
    if (FAILED(D3DCompile(g_ps_fp16_to_nv12_uv, strlen(g_ps_fp16_to_nv12_uv), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psbNv12Uv, &err)) ||
        FAILED(d3d_->CreatePixelShader(psbNv12Uv->GetBufferPointer(), psbNv12Uv->GetBufferSize(),
                                       nullptr, &ps_fp16_to_nv12_uv_)))
        return false;

    if (FAILED(D3DCompile(g_ps_fp16_to_preview, strlen(g_ps_fp16_to_preview), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb5, &err)))
        return false;
    if (FAILED(D3DCompile(g_ps_rgba8_to_preview, strlen(g_ps_rgba8_to_preview),
                          nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb6, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb5->GetBufferPointer(), psb5->GetBufferSize(), nullptr, &ps_fp16_to_preview_)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb6->GetBufferPointer(),
                                       psb6->GetBufferSize(),
                                       nullptr,
                                       &ps_rgba8_to_preview_)))
        return false;
    // Fullscreen quad (two triangles)
    struct V
    {
        float x, y, u, v;
    };
    V quad[6] = {
        {-1.f, -1.f, 0.f, 1.f}, {-1.f, 1.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 0.f}, {-1.f, -1.f, 0.f, 1.f}, {1.f, 1.f, 1.f, 0.f}, {1.f, -1.f, 1.f, 1.f}};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = quad;
    if (FAILED(d3d_->CreateBuffer(&bd, &sd, &vb_)))
        return false;

    D3D11_SAMPLER_DESC ss{};
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(d3d_->CreateSamplerState(&ss, &samp_)))
        return false;

    // ProcAmp constant buffer (shared by PS/CS). Layout must match shader cbuffer Params.
    if (!cs_params_)
    {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 64; // 48 bytes used, round up to 64
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3d_->CreateBuffer(&cbd, nullptr, &cs_params_)))
            return false;
    }

    return true;
}

bool D3DPreviewPipeline::ensure_rt_and_pipeline(int w, int h)
{
    const bool hasAllTargets = rt_fp16_ && rtv_fp16_ && srv_fp16_ &&
                               rt_scene_fp16_ && rtv_scene_fp16_ && srv_scene_fp16_ &&
                               rt_rgba_ && rtv_rgba_ && srv_rgba_ &&
                               rt_rgb10_ && rtv_rgb10_ &&
                               rt_nv12_y_ && rtv_nv12_y_ && rt_nv12_uv_ && rtv_nv12_uv_;

    if (hasAllTargets && rt_w_ == w && rt_h_ == h)
        return true;

    rt_fp16_.Reset();
    rtv_fp16_.Reset();
    srv_fp16_.Reset();
    rt_scene_fp16_.Reset();
    rtv_scene_fp16_.Reset();
    srv_scene_fp16_.Reset();
    rt_rgba_.Reset();
    rtv_rgba_.Reset();
    srv_rgba_.Reset();
    rt_rgb10_.Reset();
    rtv_rgb10_.Reset();
    readback_bgra8_.Reset();
    readback_rgb10_.Reset();
    rt_nv12_y_.Reset();
    rtv_nv12_y_.Reset();
    rt_nv12_uv_.Reset();
    rtv_nv12_uv_.Reset();
    readback_nv12_y_.Reset();
    readback_nv12_uv_.Reset();

    // 1) High precision intermediate target
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_fp16_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_fp16_.Get(), nullptr, &rtv_fp16_)))
        return false;
    if (FAILED(d3d_->CreateShaderResourceView(rt_fp16_.Get(), nullptr, &srv_fp16_)))
        return false;

    // 2) Scene FP16 target used by the preview path
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_scene_fp16_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_scene_fp16_.Get(), nullptr, &rtv_scene_fp16_)))
        return false;
    if (FAILED(d3d_->CreateShaderResourceView(rt_scene_fp16_.Get(), nullptr, &srv_scene_fp16_)))
        return false;

    // 3) RGBA8 target used by the 8-bit preview swapchain
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_rgba_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_rgba_.Get(), nullptr, &rtv_rgba_)))
        return false;
    if (FAILED(d3d_->CreateShaderResourceView(rt_rgba_.Get(), nullptr, &srv_rgba_)))
        return false;

    // 4) Packed 10:10:10:2 RGB target used by the public 10-bit buffer API.
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_rgb10_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_rgb10_.Get(), nullptr, &rtv_rgb10_)))
        return false;

    // 5) Separate render targets matching the two NV12 planes. D3D11 planar
    // render-target support varies by adapter; R8/RG8 targets are portable and
    // are packed into the caller's NV12 buffer during readback.
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_nv12_y_)) ||
        FAILED(d3d_->CreateRenderTargetView(rt_nv12_y_.Get(), nullptr, &rtv_nv12_y_)))
        return false;
    td.Width = static_cast<UINT>((w + 1) / 2);
    td.Height = static_cast<UINT>((h + 1) / 2);
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_nv12_uv_)) ||
        FAILED(d3d_->CreateRenderTargetView(rt_nv12_uv_.Get(), nullptr, &rtv_nv12_uv_)))
        return false;

    const bool ok = create_shaders_and_states();
    if (ok)
    {
        rt_w_ = w;
        rt_h_ = h;
    }
    return ok;
}

bool D3DPreviewPipeline::blit_fp16_to_rgba8(int frame_w, int frame_h)
{
    if (!rtv_rgba_ || !srv_scene_fp16_ || !vs_ || !ps_fp16_to_rgba8_ || !ctx_)
        return false;

    UINT stride = sizeof(float) * 4, offset = 0;
    ID3D11Buffer *pVB = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &pVB, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get());

    ID3D11RenderTargetView *rtv = rtv_rgba_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<FLOAT>(frame_w);
    vp.Height = static_cast<FLOAT>(frame_h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    const float clear[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv_rgba_.Get(), clear);

    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_fp16_to_rgba8_.Get(), nullptr, 0);

    ID3D11ShaderResourceView *srvs[1] = {srv_scene_fp16_.Get()};
    ctx_->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState *ss = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &ss);

    ctx_->Draw(6, 0);

    ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, nullSrv);

    return true;
}

bool D3DPreviewPipeline::blit_fp16_to_rgb10a2(int frame_w, int frame_h)
{
    if (!rtv_rgb10_ || !srv_scene_fp16_ || !vs_ || !ps_fp16_to_rgba8_ || !ctx_)
        return false;

    UINT stride = sizeof(float) * 4, offset = 0;
    ID3D11Buffer *pVB = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &pVB, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get());

    ID3D11RenderTargetView *rtv = rtv_rgb10_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<FLOAT>(frame_w);
    vp.Height = static_cast<FLOAT>(frame_h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    const float clear[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv_rgb10_.Get(), clear);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_fp16_to_rgba8_.Get(), nullptr, 0);

    ID3D11ShaderResourceView *srvs[1] = {srv_scene_fp16_.Get()};
    ctx_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState *ss = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &ss);
    ctx_->Draw(6, 0);

    ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, nullSrv);
    return true;
}

bool D3DPreviewPipeline::blit_fp16_to_nv12(int frame_w, int frame_h)
{
    if (!rtv_nv12_y_ || !rtv_nv12_uv_ || !srv_scene_fp16_ || !vs_ ||
        !ps_fp16_to_nv12_y_ || !ps_fp16_to_nv12_uv_ || !ctx_ ||
        (frame_w & 1) != 0 || (frame_h & 1) != 0)
        return false;

    UINT stride = sizeof(float) * 4, offset = 0;
    ID3D11Buffer *vertexBuffer = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get());
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView *srv = srv_scene_fp16_.Get();
    ctx_->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState *sampler = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &sampler);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<FLOAT>(frame_w);
    viewport.Height = static_cast<FLOAT>(frame_h);
    viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView *target = rtv_nv12_y_.Get();
    ctx_->OMSetRenderTargets(1, &target, nullptr);
    ctx_->RSSetViewports(1, &viewport);
    ctx_->PSSetShader(ps_fp16_to_nv12_y_.Get(), nullptr, 0);
    ctx_->Draw(6, 0);

    viewport.Width = static_cast<FLOAT>(frame_w / 2);
    viewport.Height = static_cast<FLOAT>(frame_h / 2);
    target = rtv_nv12_uv_.Get();
    ctx_->OMSetRenderTargets(1, &target, nullptr);
    ctx_->RSSetViewports(1, &viewport);
    ctx_->PSSetShader(ps_fp16_to_nv12_uv_.Get(), nullptr, 0);
    ctx_->Draw(6, 0);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
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
                                DXGI_FORMAT format) {
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

    auto *dst = static_cast<uint8_t *>(destination);
    auto copyPlane = [this, dst, destination_row_bytes](ID3D11Texture2D *source,
                                                        ID3D11Texture2D *staging,
                                                        int rows, int copyBytes,
                                                        size_t destinationOffset) {
        ctx_->CopyResource(staging, source);
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
        std::memcpy(dstRow, srcRow, (size_t)w2 * 4);
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

bool D3DPreviewPipeline::render_uploaded_yuv_to_fp16(gvfg_render_pixfmt_t fmt, int frame_w, int frame_h)
{
    if (!ctx_ || !vs_ || !il_ || !vb_ || !rtv_fp16_ || !rt_fp16_ || frame_w <= 0 || frame_h <= 0)
        return false;

    ID3D11PixelShader *ps = nullptr;
    ComPtr<ID3D11ShaderResourceView> srv0;
    if (fmt == GVFG_RENDER_FMT_YVYU)
    {
        if (!upload_yuy2_packed_)
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(d3d_->CreateShaderResourceView(upload_yuy2_packed_.Get(), &sd, &srv0)) || !srv0)
            return false;
        ps = ps_yuy2_.Get();
        if (!ps)
            return false;
    }
    else if (fmt == GVFG_RENDER_FMT_Y210)
    {
        if (!upload_y210_packed_)
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16G16B16A16_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(d3d_->CreateShaderResourceView(upload_y210_packed_.Get(), &sd, &srv0)) || !srv0)
            return false;
        ps = ps_y210_.Get();
        if (!ps)
            return false;
    }
    else
    {
        return false;
    }

    struct CB
    {
        uint32_t width;
        uint32_t height;
        float invW;
        float invH;
        float brightness;
        float contrast;
        float saturation;
        float hueSin;
        float hueCos;
        float sharpAmount;
        float pad0;
        float pad1;
    } cb{};
    cb.width = (uint32_t)frame_w;
    cb.height = (uint32_t)frame_h;
    cb.invW = frame_w > 0 ? 1.0f / (float)frame_w : 0.0f;
    cb.invH = frame_h > 0 ? 1.0f / (float)frame_h : 0.0f;
    cb.brightness = 0.0f;
    cb.contrast = 1.0f;
    cb.saturation = 1.0f;
    cb.hueSin = 0.0f;
    cb.hueCos = 1.0f;
    cb.sharpAmount = 0.0f;
    cb.pad0 = 0.0f;

    if (cs_params_)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx_->Map(cs_params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, &cb, sizeof(cb));
            ctx_->Unmap(cs_params_.Get(), 0);
            ID3D11Buffer *cb0[1] = {cs_params_.Get()};
            ctx_->PSSetConstantBuffers(0, 1, cb0);
        }
    }

    UINT stride = sizeof(float) * 4, offset = 0;
    ID3D11Buffer *pVB = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &pVB, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get());
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<FLOAT>(frame_w);
    vp.Height = static_cast<FLOAT>(frame_h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    ID3D11RenderTargetView *rtv = rtv_fp16_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    const float clear[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv_fp16_.Get(), clear);

    ID3D11ShaderResourceView *srvs[1] = {srv0.Get()};
    ctx_->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState *ss = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &ss);
    ctx_->Draw(6, 0);

    ID3D11ShaderResourceView *nulls[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, nulls);

    return true;
}

bool D3DPreviewPipeline::copy_fp16_to_scene()
{
    if (!ctx_ || !rt_fp16_ || !rt_scene_fp16_)
        return false;
    ctx_->CopyResource(rt_scene_fp16_.Get(), rt_fp16_.Get());
    return true;
}

void D3DPreviewPipeline::release_preview_swapchain()
{
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

    ComPtr<IDXGIDevice> dxgiDev;
    if (!d3d_ || FAILED(d3d_->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || !dxgiDev)
        return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter)
        return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void **>(factory.GetAddressOf()))) ||
        !factory)
        return false;

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

gvfg_preview_present_result_t D3DPreviewPipeline::present_preview(int src_w, int src_h)
{
    static bool s_loggedPresentPath = false;
    if (!preview_enabled_ || !preview_swapchain_ || !preview_backbuf_ || !ctx_)
        return GVFG_PREVIEW_PRESENT_FAILED;

    ID3D11RenderTargetView *nullRTV = nullptr;
    ctx_->OMSetRenderTargets(1, &nullRTV, nullptr);

    if (!preview_rtv_ || !vs_)
        return GVFG_PREVIEW_PRESENT_FAILED;

    const float srcW = static_cast<float>(src_w);
    const float srcH = static_cast<float>(src_h);
    const float dstW = static_cast<float>(preview_w_);
    const float dstH = static_cast<float>(preview_h_);

    if (srcW <= 0.0f || srcH <= 0.0f || dstW <= 0.0f || dstH <= 0.0f)
        return GVFG_PREVIEW_PRESENT_FAILED;

    float drawW = dstW;
    float drawH = dstH;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    const float srcAspect = srcW / srcH;
    const float dstAspect = dstW / dstH;

    if (srcAspect > dstAspect)
    {
        drawW = dstW;
        drawH = dstW / srcAspect;
        offsetX = 0.0f;
        offsetY = (dstH - drawH) * 0.5f;
    }
    else
    {
        drawH = dstH;
        drawW = dstH * srcAspect;
        offsetX = (dstW - drawW) * 0.5f;
        offsetY = 0.0f;
    }

    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    ID3D11Buffer *pVB = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &pVB, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(il_.Get());

    ID3D11RenderTargetView *rtv = preview_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);

    const float clear[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(preview_rtv_.Get(), clear);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = offsetX;
    vp.TopLeftY = offsetY;
    vp.Width = drawW;
    vp.Height = drawH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    ctx_->VSSetShader(vs_.Get(), nullptr, 0);

    ID3D11ShaderResourceView *srv = nullptr;
    if (preview_swapchain_10bit_)
    {
        if (!srv_scene_fp16_ || !ps_fp16_to_preview_)
            return GVFG_PREVIEW_PRESENT_FAILED;

        ctx_->PSSetShader(ps_fp16_to_preview_.Get(), nullptr, 0);
        srv = srv_scene_fp16_.Get();
    }
    else
    {
        if (!srv_rgba_ || !ps_rgba8_to_preview_)
            return GVFG_PREVIEW_PRESENT_FAILED;

        ctx_->PSSetShader(ps_rgba8_to_preview_.Get(), nullptr, 0);
        srv = srv_rgba_.Get();
    }

    if (!s_loggedPresentPath)
    {
        char msg[320] = {};
        std::snprintf(msg, sizeof(msg),
                      "[SharedScene] preview render path: scene=%s linear=%s backbuffer=%s present_shader=%s",
                      ss_dxgi_format_name(scene_texture_format()),
                      ss_dxgi_format_name(linear_fp16_texture_format()),
                      ss_dxgi_format_name(preview_backbuffer_format()),
                      preview_swapchain_10bit_ ? "FP16->R10G10B10A2" : "RGBA8->BGRA8");
        ssp_log_text(msg);
        s_loggedPresentPath = true;
    }

    ID3D11ShaderResourceView *srvs[1] = {srv};
    ctx_->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState *ss = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &ss);

    ctx_->Draw(6, 0);

    ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, nullSrv);

    HRESULT hr = preview_swapchain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
    {
        static uint64_t s_presentBusyCount = 0;
        ++s_presentBusyCount;
        if (s_presentBusyCount <= 5 || (s_presentBusyCount % 60) == 0)
        {
            char warn[256] = {};
            std::snprintf(warn, sizeof(warn),
                          "[SharedScene] preview present skipped: swapchain busy count=%llu",
                          static_cast<unsigned long long>(s_presentBusyCount));
            ssp_log_text(warn);
        }
        return GVFG_PREVIEW_PRESENT_SKIPPED;
    }
    if (FAILED(hr))
    {
        char err[256] = {};
        std::snprintf(err, sizeof(err),
                      "[SharedScene] preview present failed hr=0x%08X",
                      static_cast<unsigned>(hr));
        ssp_log_text(err);
        release_preview_swapchain();
        return GVFG_PREVIEW_PRESENT_FAILED;
    }
    return GVFG_PREVIEW_PRESENTED;
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

DXGI_FORMAT D3DPreviewPipeline::scene_texture_format() const
{
    if (!rt_scene_fp16_)
        return DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC d{};
    rt_scene_fp16_->GetDesc(&d);
    return d.Format;
}

DXGI_FORMAT D3DPreviewPipeline::linear_fp16_texture_format() const
{
    if (!rt_fp16_)
        return DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC d{};
    rt_fp16_->GetDesc(&d);
    return d.Format;
}
