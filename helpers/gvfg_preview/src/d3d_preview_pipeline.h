#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <wrl.h>

namespace gvfg::internal
{

typedef enum
{
    GVFG_RENDER_FMT_YUY2,
    GVFG_RENDER_FMT_Y210
} gvfg_render_pixfmt_t;

typedef enum
{
    GVFG_RENDER_PREVIEW_BITDEPTH_8BIT = 0,
    GVFG_RENDER_PREVIEW_BITDEPTH_10BIT = 1,
    GVFG_RENDER_PREVIEW_BITDEPTH_AUTO = 2
} gvfg_render_preview_bitdepth_t;

typedef struct
{
    void *hwnd;
    int enable_preview;
    int swapchain_10bit;
} gvfg_render_preview_desc_t;

class D3DPreviewPipeline
{
public:
    D3DPreviewPipeline() = default;
    ~D3DPreviewPipeline() = default;

    bool initialize(ID3D11Device *d3d,
                    ID3D11DeviceContext *ctx);

    bool configurePreview(const gvfg_render_preview_desc_t &desc);
    void set_source_bit_depth(int bits);
    void release_preview_swapchain();

    bool create_shaders_and_states();
    bool ensure_rt_and_pipeline(int w, int h);
    bool ensure_preview_swapchain(int w, int h);
    bool preview_swapchain_10bit() const { return preview_swapchain_10bit_; }
    bool present_preview(int src_w, int src_h);
    DXGI_FORMAT preview_backbuffer_format() const;
    DXGI_FORMAT scene_texture_format() const;
    DXGI_FORMAT linear_fp16_texture_format() const;
    bool blit_fp16_to_rgba8(int frame_w, int frame_h);
    bool upload_yuy2_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h);
    bool upload_y210_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h);
    bool upload_v210_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h);
    bool render_uploaded_yuv_to_fp16(gvfg_render_pixfmt_t fmt, int frame_w, int frame_h);
    bool copy_fp16_to_scene();

    ID3D11Device *d3d_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_fp16_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_fp16_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_fp16_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_scene_fp16_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_scene_fp16_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_scene_fp16_;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_rgba_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_rgba8_to_preview_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_rgba_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_yuy2_packed_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_y210_packed_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_rgba_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cs_params_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_yuy2_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_y210_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_fp16_to_rgba8_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_fp16_to_preview_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> il_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;

    void *preview_hwnd_ = nullptr;
    bool preview_enabled_ = false;
    int preview_swapchain_mode_ = GVFG_RENDER_PREVIEW_BITDEPTH_10BIT;
    int preview_source_bit_depth_ = 0;
    bool preview_swapchain_10bit_ = false;
    DXGI_FORMAT preview_swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    int preview_w_ = 0;
    int preview_h_ = 0;

    int rt_w_ = 0;
    int rt_h_ = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> preview_swapchain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> preview_backbuf_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> preview_rtv_;
};


}

