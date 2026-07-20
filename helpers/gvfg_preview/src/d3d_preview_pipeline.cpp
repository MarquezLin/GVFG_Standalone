#include "d3d_preview_pipeline.h"

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

static inline uint16_t normalize_y210_word_for_upload(uint16_t v)
{
    // Y210 stores each 10-bit component left-aligned in a 16-bit WORD.
    // Bits [15:6] are valid, bits [5:0] are padding.
    return static_cast<uint16_t>((v >> 6) & 0x03FFu);
}

static inline uint32_t read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
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
    case DXGI_FORMAT_NV12:
        return "NV12";
    case DXGI_FORMAT_P010:
        return "P010";
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

static ComPtr<ID3D11ShaderResourceView> createSRV_NV12(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv)
{
    if (!dev || !tex)
        return {};

    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    d.Format = uv ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;

    ComPtr<ID3D11ShaderResourceView> s;
    if (FAILED(dev->CreateShaderResourceView(tex, &d, &s)))
        return {};
    return s;
}

static ComPtr<ID3D11ShaderResourceView> createSRV_P010(ID3D11Device *dev, ID3D11Texture2D *tex, bool uv)
{
    if (!dev || !tex)
        return {};

    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    d.Format = uv ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;

    ComPtr<ID3D11ShaderResourceView> s;
    if (FAILED(dev->CreateShaderResourceView(tex, &d, &s)))
        return {};
    return s;
}

static const char *g_vs_src = R"(
struct VSIn  { float2 pos:POSITION; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(VSIn i){
  VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o;
}
)";

static const char *g_ps_nv12 = R"(
Texture2D texY   : register(t0);
Texture2D texUV  : register(t1);
SamplerState samL: register(s0);


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
    // y,u,v are normalized 0..1, assume limited->full & BT.709
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

float sampleY(float2 uv)
{
    return texY.Sample(samL, uv).r;
}

float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target
{
    // ---- Sharpness on luma (Y) via small unsharp mask ----
    float yC = sampleY(uv);
    float yL = sampleY(uv + float2(-invW, 0));
    float yR = sampleY(uv + float2( invW, 0));
    float yU = sampleY(uv + float2(0, -invH));
    float yD = sampleY(uv + float2(0,  invH));

    float blur = (yC*4.0 + yL + yR + yU + yD) / 8.0;
    float y  = saturate(yC + sharpAmt * (yC - blur));

    float2 uv2 = texUV.Sample(samL, uv).rg;
    uv2 = rotate_uv(uv2);

    float3 rgb = yuv_to_rgb709(y, uv2.x, uv2.y);
    rgb = apply_rgb_procamp(rgb);
    return float4(rgb, 1.0);
}
)";

static const char *g_ps_p010 = R"(
Texture2D<float>  texY16   : register(t0);
Texture2D<float2> texUV16  : register(t1);
SamplerState samL: register(s0);


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

float loadY(int2 ip)
{
    ip.x = clamp(ip.x, 0, (int)width - 1);
    ip.y = clamp(ip.y, 0, (int)height - 1);
    return saturate(texY16.Load(int3(ip, 0)).r);
}

float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target
{
    int2 ip = int2(pos.xy);

    // Sharpness on luma
    float yC = loadY(ip);
    float yL = loadY(ip + int2(-1, 0));
    float yR = loadY(ip + int2( 1, 0));
    float yU = loadY(ip + int2( 0,-1));
    float yD = loadY(ip + int2( 0, 1));
    float blur = (yC*4.0 + yL + yR + yU + yD) / 8.0;
    float y = saturate(yC + sharpAmt * (yC - blur));

    int2 uvp = int2(ip.x / 2, ip.y / 2);
    float2 uvv = saturate(texUV16.Load(int3(uvp, 0)).rg);
    float u = uvv.x;
    float v = uvv.y;

    float2 uv2 = rotate_uv(float2(u, v));

    float3 rgb = yuv_to_rgb709(y, uv2.x, uv2.y);
    rgb = apply_rgb_procamp(rgb);
    return float4(rgb, 1.0);
}
)";

// YUY2??:2:2 packed???
// ?????????????????????RGBA8_UINT texel??
//   R=Y0, G=U, B=Y1, A=V
// texture width = ceil(w/2)
static const char *g_ps_yuy2 = R"(
// YUY2 (4:2:2 packed):
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
    float u = (float)p.g / 255.0;
    float v = (float)p.a / 255.0;
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

// NV12 ??RGBA ??Compute Shader ???

// Y210??:2:2 packed, 10-bit in 16-bit container???
// ????texel ??? 2 ??????R=Y0, G=U, B=Y1, A=V
// texture width = ceil(w/2), format = R16G16B16A16_UINT
static const char *g_ps_y210 = R"(
Texture2D<uint4> texP : register(t0);

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
    uint yy = ((x & 1) != 0) ? p.b : p.r;
    return (float)(yy & 1023) / 1023.0;
}

float2 loadUV01(int x, int y)
{
    x = clamp(x, 0, (int)width - 1);
    y = clamp(y, 0, (int)height - 1);
    uint4 p = texP.Load(int3(x >> 1, y, 0));
    float u = (float)(p.g & 1023) / 1023.0;
    float v = (float)(p.a & 1023) / 1023.0;
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

static const char *g_cs_nv12 = R"(
Texture2D<float>   texY    : register(t0);
Texture2D<float2>  texUV   : register(t1);
RWTexture2D<float4> texOut : register(u0);

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
    rgb = lerp(float3(l,l,l), rgb, sat);
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

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;

    float yC = texY.Load(int3(x, y, 0));
    float yL = texY.Load(int3((int)max((int)x - 1, 0), (int)y, 0));
    float yR = texY.Load(int3((int)min((int)x + 1, (int)width - 1), (int)y, 0));
    float yU = texY.Load(int3((int)x, (int)max((int)y - 1, 0), 0));
    float yD = texY.Load(int3((int)x, (int)min((int)y + 1, (int)height - 1), 0));
    float blur = (yC*4.0 + yL + yR + yU + yD) / 8.0;
    float yNorm = saturate(yC + sharpAmt * (yC - blur));

    float2 uvNorm = texUV.Load(int3(x / 2, y / 2, 0));
    uvNorm = rotate_uv(uvNorm);

    // Convert to RGB (BT.709 limited->full)
    float Y = yNorm * 255.0;
    float U = (uvNorm.x - 0.5) * 255.0;
    float V = (uvNorm.y - 0.5) * 255.0;

    float c = Y - 16.0;
    float d = U;
    float e = V;

    float r = 1.164383 * c + 1.792741 * e;
    float g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
    float b = 1.164383 * c + 2.112402 * d;

    float3 rgb = float3(r, g, b) / 255.0;
    rgb = apply_rgb_procamp(rgb);

    texOut[uint2(x, y)] = float4(rgb, 1.0);
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
    preview_use_fp16_ = (desc.use_fp16_pipeline != 0);
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
    // Compile shaders
    ComPtr<ID3DBlob> vsb, psb1, psb2, psb3, psb4, psb5, psb6, err;
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

    if (FAILED(D3DCompile(g_ps_nv12, strlen(g_ps_nv12), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb1, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb1->GetBufferPointer(), psb1->GetBufferSize(), nullptr, &ps_nv12_)))
        return false;

    if (FAILED(D3DCompile(g_ps_p010, strlen(g_ps_p010), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb2, &err)))
        return false;
    if (FAILED(d3d_->CreatePixelShader(psb2->GetBufferPointer(), psb2->GetBufferSize(), nullptr, &ps_p010_)))
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
                               rt_stage_;

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
    rt_stage_.Reset();

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
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_fp16_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_fp16_.Get(), nullptr, &rtv_fp16_)))
        return false;
    if (FAILED(d3d_->CreateShaderResourceView(rt_fp16_.Get(), nullptr, &srv_fp16_)))
        return false;

    // rt ???/?????????compute path ??UAV ?????????
    rt_uav_.Reset();

    // 2) Scene FP16 target used by preview and readback paths
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_scene_fp16_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_scene_fp16_.Get(), nullptr, &rtv_scene_fp16_)))
        return false;
    if (FAILED(d3d_->CreateShaderResourceView(rt_scene_fp16_.Get(), nullptr, &srv_scene_fp16_)))
        return false;

    // 3) RGBA8 target for CPU readback / compatibility output
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_rgba_)))
        return false;
    if (FAILED(d3d_->CreateRenderTargetView(rt_rgba_.Get(), nullptr, &rtv_rgba_)))
        return false;
    if (FAILED(d3d_->CreateShaderResourceView(rt_rgba_.Get(), nullptr, &srv_rgba_)))
        return false;

    // 4) staging for readback
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Usage = D3D11_USAGE_STAGING;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &rt_stage_)))
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

bool D3DPreviewPipeline::upload_nv12_frame(const uint8_t *y, int stride_y, const uint8_t *uv, int stride_uv, int frame_w, int frame_h)
{
    if (!d3d_ || !ctx_ || !y || !uv || frame_w <= 0 || frame_h <= 0 || stride_y <= 0 || stride_uv <= 0)
        return false;

    if (!upload_nv12_)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)frame_w;
        td.Height = (UINT)frame_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &upload_nv12_)))
            return false;
    }
    else
    {
        D3D11_TEXTURE2D_DESC td{};
        upload_nv12_->GetDesc(&td);
        if ((int)td.Width != frame_w || (int)td.Height != frame_h || td.Format != DXGI_FORMAT_NV12)
        {
            upload_nv12_.Reset();
            return upload_nv12_frame(y, stride_y, uv, stride_uv, frame_w, frame_h);
        }
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx_->Map(upload_nv12_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr))
        return false;

    uint8_t *dst = static_cast<uint8_t *>(m.pData);
    for (int row = 0; row < frame_h; ++row)
        std::memcpy(dst + (size_t)row * m.RowPitch, y + (size_t)row * stride_y, (size_t)frame_w);

    uint8_t *dst_uv = dst + (size_t)m.RowPitch * (size_t)frame_h;
    for (int row = 0; row < frame_h / 2; ++row)
        std::memcpy(dst_uv + (size_t)row * m.RowPitch, uv + (size_t)row * stride_uv, (size_t)frame_w);

    ctx_->Unmap(upload_nv12_.Get(), 0);
    return true;
}

bool D3DPreviewPipeline::upload_p010_frame(const uint8_t *y, int stride_y, const uint8_t *uv, int stride_uv, int frame_w, int frame_h)
{
    if (!ctx_ || !d3d_ || !y || !uv || frame_w <= 0 || frame_h <= 0)
        return false;

    if (!upload_nv12_)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)frame_w;
        td.Height = (UINT)frame_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_P010;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &upload_nv12_)))
            return false;
    }
    else
    {
        D3D11_TEXTURE2D_DESC td{};
        upload_nv12_->GetDesc(&td);
        if ((int)td.Width != frame_w || (int)td.Height != frame_h || td.Format != DXGI_FORMAT_P010)
        {
            upload_nv12_.Reset();
            return upload_p010_frame(y, stride_y, uv, stride_uv, frame_w, frame_h);
        }
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx_->Map(upload_nv12_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr))
        return false;

    uint8_t *dstY = reinterpret_cast<uint8_t *>(m.pData);
    for (int r = 0; r < frame_h; ++r)
        std::memcpy(dstY + (size_t)r * m.RowPitch, y + (size_t)r * stride_y, (size_t)frame_w * 2u);

    uint8_t *dstUV = dstY + (size_t)m.RowPitch * (size_t)frame_h;
    for (int r = 0; r < frame_h / 2; ++r)
        std::memcpy(dstUV + (size_t)r * m.RowPitch, uv + (size_t)r * stride_uv, (size_t)frame_w * 2u);

    ctx_->Unmap(upload_nv12_.Get(), 0);
    return true;
}

bool D3DPreviewPipeline::upload_yuy2_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h)
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
            return upload_yuy2_frame(data, src_stride, frame_w, frame_h);
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

    const size_t rowBytes = (size_t)frame_w * 4u;
    for (int row = 0; row < frame_h; ++row)
    {
        const uint8_t *srcRow = data + (size_t)row * (size_t)effectiveStride;
        uint8_t *dstRow = static_cast<uint8_t *>(m.pData) + (size_t)row * (size_t)m.RowPitch;
        const uint16_t *src16 = reinterpret_cast<const uint16_t *>(srcRow);
        uint16_t *dst16 = reinterpret_cast<uint16_t *>(dstRow);
        const int rowWords = (int)(rowBytes / 2u);

        for (int x = 0; x < w2; ++x)
        {
            const int srcX = x * 4;
            // Standard Y210 is Y0, Cb(U), Y1, Cr(V). The current FPGA DMA
            // payload arrives as Y0, Cr(V), Y1, Cb(U), so normalize it here.
            const uint16_t Y0 = normalize_y210_word_for_upload(src16[srcX + 0]);
            const uint16_t V = normalize_y210_word_for_upload(src16[srcX + 1]);
            const uint16_t Y1 = (srcX + 2 < rowWords) ? normalize_y210_word_for_upload(src16[srcX + 2]) : Y0;
            const uint16_t U = (srcX + 3 < rowWords) ? normalize_y210_word_for_upload(src16[srcX + 3]) : V;

            uint16_t *d4 = dst16 + x * 4;
            d4[0] = Y0;
            d4[1] = U;
            d4[2] = Y1;
            d4[3] = V;
        }
    }

    ctx_->Unmap(upload_y210_packed_.Get(), 0);
    return true;
}

bool D3DPreviewPipeline::upload_v210_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h)
{
    if (!ctx_ || !d3d_ || !data || frame_w <= 0 || frame_h <= 0)
        return false;

    const int groupsPerRow = (frame_w + 5) / 6;
    const int minStride = groupsPerRow * 16;
    const int effectiveStride = (src_stride > 0) ? src_stride : minStride;
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

    for (int row = 0; row < frame_h; ++row)
    {
        const uint8_t *srcRow = data + (size_t)row * (size_t)effectiveStride;
        uint8_t *dstRow = static_cast<uint8_t *>(m.pData) + (size_t)row * (size_t)m.RowPitch;
        uint16_t *dst16 = reinterpret_cast<uint16_t *>(dstRow);

        for (int group = 0; group < groupsPerRow; ++group)
        {
            const uint8_t *p = srcRow + group * 16;
            const uint32_t w0 = read_le32(p + 0);
            const uint32_t w1 = read_le32(p + 4);
            const uint32_t w2v = read_le32(p + 8);
            const uint32_t w3 = read_le32(p + 12);

            const uint16_t u01 = static_cast<uint16_t>((w0 >> 0) & 0x3ffu);
            const uint16_t y0 = static_cast<uint16_t>((w0 >> 10) & 0x3ffu);
            const uint16_t v01 = static_cast<uint16_t>((w0 >> 20) & 0x3ffu);
            const uint16_t y1 = static_cast<uint16_t>((w1 >> 0) & 0x3ffu);
            const uint16_t u23 = static_cast<uint16_t>((w1 >> 10) & 0x3ffu);
            const uint16_t y2 = static_cast<uint16_t>((w1 >> 20) & 0x3ffu);
            const uint16_t v23 = static_cast<uint16_t>((w2v >> 0) & 0x3ffu);
            const uint16_t y3 = static_cast<uint16_t>((w2v >> 10) & 0x3ffu);
            const uint16_t u45 = static_cast<uint16_t>((w2v >> 20) & 0x3ffu);
            const uint16_t y4 = static_cast<uint16_t>((w3 >> 0) & 0x3ffu);
            const uint16_t v45 = static_cast<uint16_t>((w3 >> 10) & 0x3ffu);
            const uint16_t y5 = static_cast<uint16_t>((w3 >> 20) & 0x3ffu);

            const int pair = group * 3;
            if (pair < w2)
            {
                uint16_t *d4 = dst16 + pair * 4;
                d4[0] = y0;
                d4[1] = u01;
                d4[2] = y1;
                d4[3] = v01;
            }
            if (pair + 1 < w2)
            {
                uint16_t *d4 = dst16 + (pair + 1) * 4;
                d4[0] = y2;
                d4[1] = u23;
                d4[2] = y3;
                d4[3] = v23;
            }
            if (pair + 2 < w2)
            {
                uint16_t *d4 = dst16 + (pair + 2) * 4;
                d4[0] = y4;
                d4[1] = u45;
                d4[2] = y5;
                d4[3] = v45;
            }
        }
    }

    ctx_->Unmap(upload_y210_packed_.Get(), 0);
    return true;
}

bool D3DPreviewPipeline::render_uploaded_yuv_to_fp16(gvfg_render_pixfmt_t fmt, int frame_w, int frame_h)
{
    if (!ctx_ || !vs_ || !il_ || !vb_ || !rtv_fp16_ || !rt_fp16_ || frame_w <= 0 || frame_h <= 0)
        return false;

    ID3D11PixelShader *ps = nullptr;
    ComPtr<ID3D11ShaderResourceView> srv0, srv1;
    if (fmt == GVFG_RENDER_FMT_NV12)
    {
        if (!upload_nv12_)
            return false;
        srv0 = createSRV_NV12(d3d_, upload_nv12_.Get(), false);
        srv1 = createSRV_NV12(d3d_, upload_nv12_.Get(), true);
        ps = ps_nv12_.Get();
        if (!srv0 || !srv1 || !ps)
            return false;
    }
    else if (fmt == GVFG_RENDER_FMT_P010)
    {
        if (!upload_nv12_)
            return false;
        srv0 = createSRV_P010(d3d_, upload_nv12_.Get(), false);
        srv1 = createSRV_P010(d3d_, upload_nv12_.Get(), true);
        ps = ps_p010_.Get();
        if (!srv0 || !srv1 || !ps)
            return false;
    }
    else if (fmt == GVFG_RENDER_FMT_YUY2)
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

    if (fmt == GVFG_RENDER_FMT_NV12 || fmt == GVFG_RENDER_FMT_P010)
    {
        ID3D11ShaderResourceView *srvs[2] = {srv0.Get(), srv1.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
    }
    else
    {
        ID3D11ShaderResourceView *srvs[1] = {srv0.Get()};
        ctx_->PSSetShaderResources(0, 1, srvs);
        ID3D11ShaderResourceView *null1[1] = {nullptr};
        ctx_->PSSetShaderResources(1, 1, null1);
    }

    ID3D11SamplerState *ss = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &ss);
    ctx_->Draw(6, 0);

    if (fmt == GVFG_RENDER_FMT_NV12 || fmt == GVFG_RENDER_FMT_P010)
    {
        ID3D11ShaderResourceView *nulls[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, nulls);
    }
    else
    {
        ID3D11ShaderResourceView *nulls[1] = {nullptr};
        ctx_->PSSetShaderResources(0, 1, nulls);
    }

    return true;
}

bool D3DPreviewPipeline::copy_fp16_to_scene()
{
    if (!ctx_ || !rt_fp16_ || !rt_scene_fp16_)
        return false;
    ctx_->CopyResource(rt_scene_fp16_.Get(), rt_fp16_.Get());
    return true;
}

bool D3DPreviewPipeline::readback_to_frame(int frame_w, int frame_h, uint64_t pts_ns, uint64_t frame_id,
                                            gvfg_render_frame_t *out)
{
    if (!out || !ctx_ || !rt_stage_ || !rt_rgba_)
        return false;

    ctx_->CopyResource(rt_stage_.Get(), rt_rgba_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(rt_stage_.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;

    std::memset(out, 0, sizeof(*out));
    out->data[0] = m.pData;
    out->stride[0] = (int)m.RowPitch;
    out->plane_count = 1;
    out->width = frame_w;
    out->height = frame_h;
    out->format = GVFG_RENDER_FMT_ARGB;
    out->pts_ns = pts_ns;
    out->frame_id = frame_id;
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

        if ((FAILED(hr) || !preview_swapchain_) && sd.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            char warn[320] = {};
            std::snprintf(warn, sizeof(warn),
                          "[SharedScene] 10-bit preview swapchain failed hr=0x%08X, fallback to 8-bit BGRA",
                          static_cast<unsigned>(hr));
            ssp_log_text(warn);

            preview_swapchain_.Reset();
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            hr = factory->CreateSwapChainForHwnd(
                d3d_,
                (HWND)preview_hwnd_,
                &sd,
                nullptr,
                nullptr,
                &preview_swapchain_);
        }

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

bool D3DPreviewPipeline::present_preview(int src_w, int src_h)
{
    static bool s_loggedPresentPath = false;
    if (!preview_enabled_ || !preview_swapchain_ || !preview_backbuf_ || !ctx_)
        return false;

    ID3D11RenderTargetView *nullRTV = nullptr;
    ctx_->OMSetRenderTargets(1, &nullRTV, nullptr);

    if (!preview_rtv_ || !vs_)
        return false;

    const float srcW = static_cast<float>(src_w);
    const float srcH = static_cast<float>(src_h);
    const float dstW = static_cast<float>(preview_w_);
    const float dstH = static_cast<float>(preview_h_);

    if (srcW <= 0.0f || srcH <= 0.0f || dstW <= 0.0f || dstH <= 0.0f)
        return false;

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
            return false;

        ctx_->PSSetShader(ps_fp16_to_preview_.Get(), nullptr, 0);
        srv = srv_scene_fp16_.Get();
    }
    else
    {
        if (!srv_rgba_ || !ps_rgba8_to_preview_)
            return false;

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
        return true;
    }
    if (FAILED(hr))
    {
        char err[256] = {};
        std::snprintf(err, sizeof(err),
                      "[SharedScene] preview present failed hr=0x%08X",
                      static_cast<unsigned>(hr));
        ssp_log_text(err);
        release_preview_swapchain();
        return false;
    }
    return true;
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

