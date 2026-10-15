#pragma once
#include "../../include/gvfg_capture.h"
#include <cstdint>
namespace gvfg::internal {
enum gvfg_render_pixfmt_t { GVFG_RENDER_FMT_YUY2=1, GVFG_RENDER_FMT_Y210=2,
    GVFG_RENDER_FMT_GRAY16=4, GVFG_RENDER_FMT_RGBA16=5, GVFG_RENDER_FMT_DICOM_YBR_FULL_422=6 };
enum gvfg_render_preview_bitdepth_t { GVFG_RENDER_PREVIEW_BITDEPTH_8BIT=0,
    GVFG_RENDER_PREVIEW_BITDEPTH_10BIT=1, GVFG_RENDER_PREVIEW_BITDEPTH_AUTO=2 };
enum gvfg_preview_present_result_t { GVFG_PREVIEW_PRESENT_FAILED=0,
    GVFG_PREVIEW_PRESENTED=1, GVFG_PREVIEW_PRESENT_SKIPPED=2 };
struct gvfg_render_preview_desc_t { void *hwnd; int enable_preview; int swapchain_10bit; };
}
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
namespace gvfg::internal {
class D3DPreviewPipeline {
public:
    bool initialize(ID3D11Device *, ID3D11DeviceContext *);
    bool ensure_states();
    bool ensure_export_target(int width, int height, int output_format);
    bool draw_rgb(ID3D11Texture2D *, ID3D11ShaderResourceView *, gvfg_render_pixfmt_t,
                  int width, int height, int bits, ID3D11RenderTargetView *, const D3D11_VIEWPORT &);
    bool draw_uploaded_rgb(gvfg_render_pixfmt_t, int width, int height, DXGI_FORMAT);
    bool blit_uploaded_422_to_nv12(gvfg_render_pixfmt_t, int width, int height);
    bool upload_packed_422_frame(const uint8_t *, int stride, int width, int height);
    bool upload_y210_frame(const uint8_t *, int stride, int width, int height);
    bool readback_to_buffer(void *, uint64_t, int stride, DXGI_FORMAT, int width, int height);
    bool readback_nv12_to_buffer(void *, uint64_t, int stride, int width, int height);
    bool configurePreview(const gvfg_render_preview_desc_t &);
    void set_source_bit_depth(int);
    void release_preview_swapchain();
    bool ensure_preview_swapchain(int width, int height);
    gvfg_preview_present_result_t present_texture(ID3D11Texture2D *, ID3D11ShaderResourceView *, gvfg_render_pixfmt_t, int width, int height, int bits);
    bool clear_preview_black();
    bool preview_swapchain_10bit() const { return preview_swapchain_10bit_; }
    DXGI_FORMAT preview_backbuffer_format() const;
private:
    bool ensure_target(Microsoft::WRL::ComPtr<ID3D11Texture2D> &,
                       Microsoft::WRL::ComPtr<ID3D11RenderTargetView> &, int, int, DXGI_FORMAT);
    bool ensure_input_view(ID3D11Texture2D *, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &);
    void bind_quad();
    bool upscale_rgb(ID3D11Texture2D *, ID3D11ShaderResourceView *, gvfg_render_pixfmt_t,
                     int width, int height, int bits, ID3D11RenderTargetView *, const D3D11_VIEWPORT &);
    ID3D11Device *d3d_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> il_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_, cs_params_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_rgb_[7][2];
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_422_to_nv12_[2][2];
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_scaled_rgb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linear_sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scaled_rgb_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_scaled_rgb_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_scaled_rgb_;
    std::array<uint32_t,4> native_params_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_yuy2_packed_, upload_y210_packed_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_uploaded_422_[2], native_view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_rgba_, rt_rgb10_, rt_nv12_y_, rt_nv12_uv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_rgba_, rtv_rgb10_, rtv_nv12_y_, rtv_nv12_uv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> readback_bgra8_, readback_rgb10_, readback_nv12_y_, readback_nv12_uv_;
    void *preview_hwnd_ = nullptr;
    bool preview_enabled_ = false;
    int preview_swapchain_mode_ = GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
    int preview_source_bit_depth_ = 0;
    bool preview_swapchain_10bit_ = false;
    DXGI_FORMAT preview_swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    int preview_w_ = 0, preview_h_ = 0;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> preview_swapchain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> preview_backbuf_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> preview_rtv_;
};
}
