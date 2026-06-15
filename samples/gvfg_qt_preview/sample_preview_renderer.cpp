#include "sample_preview_renderer.h"

#include "gvfg_render_types.h"
#include "shared_scene_pipeline.h"

#include <d3d11_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct SamplePreviewRenderer::D3DState
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

SamplePreviewRenderer::SamplePreviewRenderer() = default;

SamplePreviewRenderer::~SamplePreviewRenderer()
{
    shutdown();
}

bool SamplePreviewRenderer::configure(void *hwnd)
{
    std::lock_guard<std::mutex> lock(mutex_);
    hwnd_ = hwnd;
    configured_ = hwnd_ != nullptr;
    if (pipeline_)
    {
        gvfg::internal::gvfg_render_preview_desc_t desc{};
        desc.hwnd = hwnd_;
        desc.enable_preview = configured_ ? 1 : 0;
        desc.use_fp16_pipeline = 1;
        desc.swapchain_10bit = gvfg::internal::GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
        pipeline_->configurePreview(desc);
    }
    return configured_;
}

bool SamplePreviewRenderer::render(const gvfg_frame_t &frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || !hwnd_ || !frame.data || frame.width <= 0 || frame.height <= 0)
        return false;

    if (!ensureDevice() || !ensurePipeline(frame.width, frame.height, frame.bit_depth > 0 ? frame.bit_depth : 8))
        return false;

    const auto *base = static_cast<const uint8_t *>(frame.data);
    gvfg::internal::gvfg_render_pixfmt_t renderFmt = gvfg::internal::GVFG_RENDER_FMT_YUY2;
    bool uploaded = false;

    switch (frame.pixel_format)
    {
    case GVFG_PIXFMT_YUY2:
        renderFmt = gvfg::internal::GVFG_RENDER_FMT_YUY2;
        uploaded = pipeline_->upload_yuy2_frame(base, frame.width * 2, frame.width, frame.height);
        break;
    case GVFG_PIXFMT_Y210:
        renderFmt = gvfg::internal::GVFG_RENDER_FMT_Y210;
        uploaded = pipeline_->upload_y210_frame(base, frame.width * 4, frame.width, frame.height);
        break;
    default:
        return false;
    }

    if (!uploaded ||
        !pipeline_->render_uploaded_yuv_to_fp16(renderFmt, frame.width, frame.height) ||
        !pipeline_->copy_fp16_to_scene())
        return false;

    bool ok = true;
    if (!pipeline_->preview_swapchain_10bit())
        ok = pipeline_->blit_fp16_to_rgba8(frame.width, frame.height);
    if (!ok)
        return false;

    pipeline_->present_preview(frame.width, frame.height);
    width_ = pipeline_->preview_w_;
    height_ = pipeline_->preview_h_;
    bitDepth_ = pipeline_->preview_swapchain_10bit() ? 10 : 8;
    swapchain10Bit_ = pipeline_->preview_swapchain_10bit();
    return true;
}

void SamplePreviewRenderer::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipeline_)
        pipeline_->release_preview_swapchain();
    pipeline_.reset();
    d3d_.reset();
    configured_ = false;
    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
    bitDepth_ = 0;
    swapchain10Bit_ = false;
}

bool SamplePreviewRenderer::active() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return configured_ && pipeline_ && width_ > 0 && height_ > 0;
}

int SamplePreviewRenderer::width() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return width_;
}

int SamplePreviewRenderer::height() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return height_;
}

int SamplePreviewRenderer::bitDepth() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bitDepth_;
}

const char *SamplePreviewRenderer::pixelFormat() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return swapchain10Bit_ ? "RGB10A2" : "BGRA8";
}

bool SamplePreviewRenderer::ensureDevice()
{
    if (d3d_ && d3d_->device && d3d_->context)
        return true;

    auto state = std::make_unique<D3DState>();
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   flags,
                                   levels,
                                   _countof(levels),
                                   D3D11_SDK_VERSION,
                                   &state->device,
                                   &got,
                                   &state->context);
#ifdef _DEBUG
    if (FAILED(hr))
    {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr,
                               D3D_DRIVER_TYPE_HARDWARE,
                               nullptr,
                               flags,
                               levels,
                               _countof(levels),
                               D3D11_SDK_VERSION,
                               &state->device,
                               &got,
                               &state->context);
    }
#endif
    if (FAILED(hr) || !state->device || !state->context)
        return false;

    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(state->device.As(&mt)) && mt)
        mt->SetMultithreadProtected(TRUE);

    d3d_ = std::move(state);
    return true;
}

bool SamplePreviewRenderer::ensurePipeline(int width, int height, int sourceBitDepth)
{
    if (!pipeline_)
        pipeline_ = std::make_unique<gvfg::internal::SharedScenePipeline>();

    if (!pipeline_->initialize(d3d_->device.Get(), d3d_->context.Get()))
        return false;

    gvfg::internal::gvfg_render_preview_desc_t desc{};
    desc.hwnd = hwnd_;
    desc.enable_preview = configured_ ? 1 : 0;
    desc.use_fp16_pipeline = 1;
    desc.swapchain_10bit = gvfg::internal::GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
    pipeline_->configurePreview(desc);
    pipeline_->set_source_bit_depth(sourceBitDepth > 0 ? sourceBitDepth : 8);
    return pipeline_->ensure_rt_and_pipeline(width, height) &&
           pipeline_->ensure_preview_swapchain(width, height);
}
