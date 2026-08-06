#include "gvfg_preview.h"

#include "d3d_conversion_pipeline.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
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
        resetPresentStats();
        hwnd_ = hwnd;
        configured_ = hwnd_ != nullptr;
        if (pipeline_)
        {
            gvfg::internal::gvfg_render_preview_desc_t desc{};
            desc.hwnd = hwnd_;
            desc.enable_preview = configured_ ? 1 : 0;
            desc.swapchain_10bit = gvfg::internal::GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
            pipeline_->configurePreview(desc);
        }
        return configured_;
    }

    bool render(const gvfg_preview_frame_t &frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_ || !hwnd_ || !frame.data || frame.width <= 0 || frame.height <= 0)
        {
            clearActiveInfo();
            return false;
        }

        int sourceBitDepth = frame.bit_depth > 0 ? frame.bit_depth : 8;
        if (frame.pixel_format == GVFG_PREVIEW_PIXFMT_Y210)
        {
            // These packed formats always carry 10-bit components. Do not let
            // a missing or incorrect caller hint silently select an 8-bit swapchain.
            sourceBitDepth = 10;
        }

        if (!ensureDevice() || !ensurePipeline(frame.width, frame.height, sourceBitDepth))
        {
            clearActiveInfo();
            return false;
        }

        gvfg::internal::gvfg_render_pixfmt_t renderFmt = gvfg::internal::GVFG_RENDER_FMT_YVYU;
        const uint8_t *base = static_cast<const uint8_t *>(frame.data);
        const int stride = frame.row_bytes;
        bool uploaded = false;

        switch (frame.pixel_format)
        {
        case GVFG_PREVIEW_PIXFMT_YVYU:
            renderFmt = gvfg::internal::GVFG_RENDER_FMT_YVYU;
            break;
        case GVFG_PREVIEW_PIXFMT_Y210:
            renderFmt = gvfg::internal::GVFG_RENDER_FMT_Y210;
            break;
        default:
            return false;
        }

        switch (frame.pixel_format)
        {
        case GVFG_PREVIEW_PIXFMT_YVYU:
            uploaded = pipeline_->upload_packed_422_frame(base, stride, frame.width, frame.height);
            break;
        case GVFG_PREVIEW_PIXFMT_Y210:
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

        const gvfg::internal::gvfg_preview_present_result_t presentResult =
            pipeline_->present_preview(frame.width, frame.height);
        if (presentResult == gvfg::internal::GVFG_PREVIEW_PRESENT_FAILED)
        {
            clearActiveInfo();
            return false;
        }
        recordPresentResult(presentResult);

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
        resetPresentStats();
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

    const char *adapterName() const
    {
        return adapterName_;
    }

    int adapterIndex() const
    {
        return adapterIndex_;
    }

    void getStats(gvfg_preview_stats_t &stats) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.presented_frames = presentedFrames_;
        stats.skipped_presents = skippedPresents_;
        stats.present_fps = active_.load(std::memory_order_relaxed)
                                ? calculatePresentFps(std::chrono::steady_clock::now())
                                : 0.0;
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

    using PresentClock = std::chrono::steady_clock;

    void resetPresentStats()
    {
        presentTimes_.clear();
        presentedFrames_ = 0;
        skippedPresents_ = 0;
    }

    void recordPresentResult(gvfg::internal::gvfg_preview_present_result_t result)
    {
        if (result == gvfg::internal::GVFG_PREVIEW_PRESENT_SKIPPED)
        {
            ++skippedPresents_;
            return;
        }
        if (result != gvfg::internal::GVFG_PREVIEW_PRESENTED)
            return;

        ++presentedFrames_;
        const PresentClock::time_point now = PresentClock::now();
        presentTimes_.push_back(now);
        const auto windowStart = now - std::chrono::seconds(5);
        while (presentTimes_.size() > 2 && presentTimes_.front() < windowStart)
            presentTimes_.pop_front();
    }

    double calculatePresentFps(PresentClock::time_point now) const
    {
        if (presentTimes_.size() < 2 ||
            now - presentTimes_.back() > std::chrono::seconds(1))
            return 0.0;

        const double elapsed =
            std::chrono::duration<double>(presentTimes_.back() - presentTimes_.front()).count();
        if (elapsed < 2.0)
            return 0.0;
        return static_cast<double>(presentTimes_.size() - 1) / elapsed;
    }

    static bool sameLuid(const LUID &a, const LUID &b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }

    static int dxgiAdapterIndexForLuid(const LUID &luid)
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
            return -1;

        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || !adapter)
                continue;

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && sameLuid(desc.AdapterLuid, luid))
                return static_cast<int>(i);
        }
        return -1;
    }

    void updateAdapterInfo(ID3D11Device *device)
    {
        adapterName_[0] = '\0';
        adapterIndex_ = -1;
        if (!device)
            return;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
            return;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter)
            return;

        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc)))
            return;

        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            adapterName_, static_cast<int>(sizeof(adapterName_)), nullptr, nullptr);
        adapterIndex_ = dxgiAdapterIndexForLuid(desc.AdapterLuid);
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

        updateAdapterInfo(state->device.Get());
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
    char adapterName_[160] = {};
    int adapterIndex_ = -1;
    std::deque<PresentClock::time_point> presentTimes_;
    uint64_t presentedFrames_ = 0;
    uint64_t skippedPresents_ = 0;
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
                                                    const gvfg_preview_frame_t *frame)
    {
        if (!handle || !frame)
            return GVFG_PREVIEW_EINVAL;
        if (!frame->data ||
            frame->width <= 0 ||
            frame->height <= 0 ||
            frame->row_bytes <= 0)
            return GVFG_PREVIEW_EINVAL;

        uint64_t minimumRowBytes = 0;
        switch (frame->pixel_format)
        {
        case GVFG_PREVIEW_PIXFMT_YVYU:
            minimumRowBytes = static_cast<uint64_t>(frame->width) * 2u;
            break;
        case GVFG_PREVIEW_PIXFMT_Y210:
            minimumRowBytes = static_cast<uint64_t>(frame->width) * 4u;
            break;
        default:
            return GVFG_PREVIEW_ENOTSUP;
        }

        const uint64_t rowBytes = static_cast<uint64_t>(frame->row_bytes);
        if (rowBytes < minimumRowBytes ||
            rowBytes > UINT64_MAX / static_cast<uint64_t>(frame->height) ||
            frame->data_size < rowBytes * static_cast<uint64_t>(frame->height))
            return GVFG_PREVIEW_EINVAL;

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
        copy_cstr(out_info->adapter_name,
                  sizeof(out_info->adapter_name),
                  handle->renderer.adapterName());
        out_info->adapter_index = handle->renderer.adapterIndex();
        return GVFG_PREVIEW_OK;
    }

    gvfg_preview_status_t gvfg_preview_get_stats(gvfg_preview_handle handle,
                                                 gvfg_preview_stats_t *out_stats)
    {
        if (!handle || !out_stats)
            return GVFG_PREVIEW_EINVAL;

        gvfg_preview_stats_t stats{};
        handle->renderer.getStats(stats);
        *out_stats = stats;
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
