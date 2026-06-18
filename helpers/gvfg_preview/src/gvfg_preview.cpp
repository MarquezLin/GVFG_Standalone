#include "gvfg_preview.h"

#include "d3d_preview_pipeline.h"

#include <d3d11_4.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class PreviewRenderer
{
public:
    PreviewRenderer() = default;
    ~PreviewRenderer() { shutdown(); }

    bool configure(void *hwnd)
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

    bool render(const gvfg_frame_t &frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_ || !hwnd_ || !frame.data || frame.width <= 0 || frame.height <= 0)
        {
            clearActiveInfo();
            return false;
        }

        if (!ensureDevice() || !ensurePipeline(frame.width, frame.height, frame.bit_depth > 0 ? frame.bit_depth : 8))
        {
            clearActiveInfo();
            return false;
        }

        gvfg::internal::gvfg_render_pixfmt_t renderFmt = gvfg::internal::GVFG_RENDER_FMT_YUY2;
        const uint8_t *base = static_cast<const uint8_t *>(frame.data);
        int stride = 0;
        bool uploaded = false;

        switch (frame.pixel_format)
        {
        case GVFG_PIXFMT_YUY2:
            renderFmt = gvfg::internal::GVFG_RENDER_FMT_YUY2;
            stride = frame.width * 2;
            break;
        case GVFG_PIXFMT_Y210:
            renderFmt = gvfg::internal::GVFG_RENDER_FMT_Y210;
            stride = frame.width * 4;
            break;
        default:
            return false;
        }

        gvfg_frame_layout_t layout{};
        layout.struct_size = sizeof(layout);
        if (gvfg_get_frame_layout(&frame, &layout) == GVFG_OK &&
            layout.plane_count > 0 &&
            layout.plane_data[0] &&
            layout.plane_stride[0] > 0)
        {
            base = static_cast<const uint8_t *>(layout.plane_data[0]);
            stride = layout.plane_stride[0];
        }

        switch (frame.pixel_format)
        {
        case GVFG_PIXFMT_YUY2:
            uploaded = pipeline_->upload_yuy2_frame(base, stride, frame.width, frame.height);
            break;
        case GVFG_PIXFMT_Y210:
            uploaded = pipeline_->upload_y210_frame(base, stride, frame.width, frame.height);
            break;
        default:
            return false;
        }

        if (!uploaded ||
            !pipeline_->render_uploaded_yuv_to_fp16(renderFmt, frame.width, frame.height) ||
            !pipeline_->copy_fp16_to_scene())
        {
            clearActiveInfo();
            return false;
        }

        bool ok = true;
        if (!pipeline_->preview_swapchain_10bit())
            ok = pipeline_->blit_fp16_to_rgba8(frame.width, frame.height);
        if (!ok)
        {
            clearActiveInfo();
            return false;
        }

        if (!pipeline_->present_preview(frame.width, frame.height))
        {
            clearActiveInfo();
            return false;
        }

        width_.store(pipeline_->preview_w_, std::memory_order_relaxed);
        height_.store(pipeline_->preview_h_, std::memory_order_relaxed);
        bitDepth_.store(pipeline_->preview_swapchain_10bit() ? 10 : 8, std::memory_order_relaxed);
        swapchain10Bit_.store(pipeline_->preview_swapchain_10bit(), std::memory_order_relaxed);
        active_.store(true, std::memory_order_relaxed);
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pipeline_)
            pipeline_->release_preview_swapchain();
        pipeline_.reset();
        d3d_.reset();
        configured_ = false;
        hwnd_ = nullptr;
        clearActiveInfo();
    }

    bool active() const
    {
        return active_.load(std::memory_order_relaxed);
    }

    int width() const
    {
        return width_.load(std::memory_order_relaxed);
    }

    int height() const
    {
        return height_.load(std::memory_order_relaxed);
    }

    int bitDepth() const
    {
        return bitDepth_.load(std::memory_order_relaxed);
    }

    const char *pixelFormat() const
    {
        return swapchain10Bit_.load(std::memory_order_relaxed) ? "RGB10A2" : "BGRA8";
    }

private:
    struct D3DState
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
    };

    void clearActiveInfo()
    {
        width_.store(0, std::memory_order_relaxed);
        height_.store(0, std::memory_order_relaxed);
        bitDepth_.store(0, std::memory_order_relaxed);
        swapchain10Bit_.store(false, std::memory_order_relaxed);
        active_.store(false, std::memory_order_relaxed);
    }

    bool ensureDevice()
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

    bool ensurePipeline(int width, int height, int sourceBitDepth)
    {
        if (!pipeline_)
            pipeline_ = std::make_unique<gvfg::internal::D3DPreviewPipeline>();

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

    mutable std::mutex mutex_;
    void *hwnd_ = nullptr;
    bool configured_ = false;
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<int> bitDepth_{0};
    std::atomic<bool> swapchain10Bit_{false};
    std::atomic<bool> active_{false};
    std::unique_ptr<D3DState> d3d_;
    std::unique_ptr<gvfg::internal::D3DPreviewPipeline> pipeline_;
};

struct gvfg_preview_handle_t
{
    PreviewRenderer renderer;
};

namespace
{
    void copy_cstr(char *dst, size_t dstSize, const char *src)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = '\0';
        if (src)
            strncpy_s(dst, dstSize, src, _TRUNCATE);
    }
}

extern "C"
{
    gvfg_preview_status_t gvfg_preview_create(gvfg_preview_handle *out_handle)
    {
        if (!out_handle)
            return GVFG_PREVIEW_EINVAL;
        *out_handle = nullptr;
        auto handle = std::make_unique<gvfg_preview_handle_t>();
        *out_handle = handle.release();
        return GVFG_PREVIEW_OK;
    }

    gvfg_preview_status_t gvfg_preview_destroy(gvfg_preview_handle handle)
    {
        if (!handle)
            return GVFG_PREVIEW_OK;
        delete handle;
        return GVFG_PREVIEW_OK;
    }

    gvfg_preview_status_t gvfg_preview_attach_window(gvfg_preview_handle handle,
                                                     void *native_window_handle)
    {
        if (!handle)
            return GVFG_PREVIEW_EINVAL;
        if (!native_window_handle)
        {
            handle->renderer.shutdown();
            return GVFG_PREVIEW_EINVAL;
        }
        return handle->renderer.configure(native_window_handle) ? GVFG_PREVIEW_OK : GVFG_PREVIEW_ESTATE;
    }

    gvfg_preview_status_t gvfg_preview_render_frame(gvfg_preview_handle handle,
                                                    const gvfg_frame_t *frame)
    {
        if (!handle || !frame)
            return GVFG_PREVIEW_EINVAL;
        if (!frame->data || frame->width <= 0 || frame->height <= 0)
            return GVFG_PREVIEW_EINVAL;
        if (frame->pixel_format != GVFG_PIXFMT_YUY2 && frame->pixel_format != GVFG_PIXFMT_Y210)
            return GVFG_PREVIEW_ENOTSUP;
        return handle->renderer.render(*frame) ? GVFG_PREVIEW_OK : GVFG_PREVIEW_ERENDER;
    }

    gvfg_preview_status_t gvfg_preview_get_info(gvfg_preview_handle handle,
                                                gvfg_preview_info_t *out_info)
    {
        if (!handle || !out_info)
            return GVFG_PREVIEW_EINVAL;
        std::memset(out_info, 0, sizeof(*out_info));
        out_info->active = handle->renderer.active() ? 1 : 0;
        out_info->width = handle->renderer.width();
        out_info->height = handle->renderer.height();
        out_info->bit_depth = handle->renderer.bitDepth();
        copy_cstr(out_info->pixel_format,
                  sizeof(out_info->pixel_format),
                  handle->renderer.pixelFormat());
        return GVFG_PREVIEW_OK;
    }

    gvfg_preview_status_t gvfg_preview_shutdown(gvfg_preview_handle handle)
    {
        if (!handle)
            return GVFG_PREVIEW_EINVAL;
        handle->renderer.shutdown();
        return GVFG_PREVIEW_OK;
    }

    const char *gvfg_preview_strerror(gvfg_preview_status_t status)
    {
        switch (status)
        {
        case GVFG_PREVIEW_OK:
            return "OK";
        case GVFG_PREVIEW_EINVAL:
            return "Invalid argument";
        case GVFG_PREVIEW_ESTATE:
            return "Invalid state";
        case GVFG_PREVIEW_ENOTSUP:
            return "Unsupported frame format";
        case GVFG_PREVIEW_ERENDER:
            return "Render failed";
        default:
            return "Unknown";
        }
    }
}
