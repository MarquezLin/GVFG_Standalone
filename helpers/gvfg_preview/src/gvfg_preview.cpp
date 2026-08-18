#include "gvfg_preview.h"

#include "d3d_conversion_pipeline.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <cstdio>
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <windows.h>
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
        if (configured_ && ensureDevice())
        {
            if (!pipeline_)
                pipeline_ = std::make_unique<gvfg::internal::D3DPreviewPipeline>();
            pipeline_->initialize(d3d_->device.Get(), d3d_->context.Get());
            gvfg::internal::gvfg_render_preview_desc_t desc{};
            desc.hwnd = hwnd_;
            desc.enable_preview = configured_ ? 1 : 0;
            desc.swapchain_10bit = gvfg::internal::GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
            pipeline_->configurePreview(desc);
            if (!pipeline_->create_shaders_and_states())
                return false;
        }
        else if (pipeline_)
        {
            gvfg::internal::gvfg_render_preview_desc_t desc{};
            desc.hwnd = hwnd_;
            desc.enable_preview = 0;
            desc.swapchain_10bit = gvfg::internal::GVFG_RENDER_PREVIEW_BITDEPTH_AUTO;
            pipeline_->configurePreview(desc);
        }
        return configured_;
    }

    bool render(const gvfg_preview_frame_t &frame)
    {
        using Clock = std::chrono::steady_clock;
        const auto submitStart = Clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        const auto lockEnd = Clock::now();
        if (!configured_ || !hwnd_ || !frame.data || frame.width <= 0 || frame.height <= 0)
            return false;

        int sourceBitDepth = frame.bit_depth > 0 ? frame.bit_depth : 8;
        if (frame.pixel_format == GVFG_PREVIEW_PIXFMT_Y210)
            sourceBitDepth = 10;
        if (!ensureDevice() || !ensureWorkerLocked() ||
            !ensureUploadSlotsLocked(frame.width, frame.height, frame.pixel_format))
            return false;

        size_t slotIndex = slots_.size();
        for (size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == SlotState::Free) { slotIndex = i; break; }
        if (slotIndex == slots_.size())
            for (size_t i = 0; i < slots_.size(); ++i)
                if (slots_[i].state == SlotState::Pending) { slotIndex = i; break; }
        if (slotIndex == slots_.size())
        {
            ++skippedSubmits_;
            return true;
        }

        UploadSlot &slot = slots_[slotIndex];
        if (slot.state == SlotState::Pending)
        {
            slot.commands.Reset();
            ++skippedSubmits_;
        }
        slot.state = SlotState::Uploading;
        ID3D11DeviceContext *deferred = slot.deferred.Get();
        ID3D11Texture2D *texture = slot.texture.Get();
        lock.unlock();
        const auto setupEnd = Clock::now();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(deferred->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            lock.lock(); slot.state = SlotState::Free; return false;
        }
        const auto mapEnd = Clock::now();
        const size_t rowBytes = frame.pixel_format == GVFG_PREVIEW_PIXFMT_Y210
                                    ? static_cast<size_t>(frame.width) * 4u
                                    : static_cast<size_t>((frame.width + 1) / 2) * 4u;
        if (mapped.RowPitch == rowBytes && static_cast<size_t>(frame.row_bytes) == rowBytes)
        {
            std::memcpy(mapped.pData, frame.data, rowBytes * static_cast<size_t>(frame.height));
        }
        else
        {
            for (int row = 0; row < frame.height; ++row)
                std::memcpy(static_cast<uint8_t *>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                            static_cast<const uint8_t *>(frame.data) + static_cast<size_t>(row) * frame.row_bytes,
                            rowBytes);
        }
        deferred->Unmap(texture, 0);
        const auto copyEnd = Clock::now();
        ComPtr<ID3D11CommandList> commands;
        if (FAILED(deferred->FinishCommandList(FALSE, &commands)))
        {
            lock.lock(); slot.state = SlotState::Free; return false;
        }
        const auto finishEnd = Clock::now();

        lock.lock();
        const auto publishLockEnd = Clock::now();
        for (UploadSlot &other : slots_)
            if (&other != &slot && other.state == SlotState::Pending)
                other.state = SlotState::Free;
        slot.commands = commands;
        slot.width = frame.width;
        slot.height = frame.height;
        slot.bitDepth = sourceBitDepth;
        slot.pixelFormat = frame.pixel_format;
        slot.frameId = frame.frame_id;
        slot.generation = clearGeneration_.load(std::memory_order_acquire);
        slot.state = SlotState::Pending;
        workerCv_.notify_one();
#if GVFG_INTERNAL_DIAGNOSTICS
        const auto submitEnd = Clock::now();
        const auto milliseconds = [](Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        const double totalMs = milliseconds(submitEnd - submitStart);
        if (totalMs >= 10.0)
        {
            char line[420] = {};
            std::snprintf(line, sizeof(line),
                          "[GVFG][PREVIEW] SLOW SUBMIT frame=%llu slot=%zu total=%.3f lock=%.3f setup=%.3f map=%.3f copy=%.3f finish=%.3f publish_lock=%.3f src_pitch=%d dst_pitch=%u ms\n",
                          static_cast<unsigned long long>(frame.frame_id), slotIndex, totalMs,
                          milliseconds(lockEnd - submitStart),
                          milliseconds(setupEnd - lockEnd),
                          milliseconds(mapEnd - setupEnd),
                          milliseconds(copyEnd - mapEnd),
                          milliseconds(finishEnd - copyEnd),
                          milliseconds(publishLockEnd - finishEnd),
                          frame.row_bytes, mapped.RowPitch);
            OutputDebugStringA(line);
        }
#endif
        return true;
    }

    bool prepare(int width, int height, int sourceBitDepth)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_ || !hwnd_ || width <= 0 || height <= 0)
            return false;
        return ensureDevice() &&
               ensurePipeline(width, height, sourceBitDepth > 0 ? sourceBitDepth : 8);
    }

    bool clear()
    {
        clearGeneration_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!configured_ || !pipeline_)
                return false;
            for (UploadSlot &slot : slots_)
            {
                if (slot.state == SlotState::Pending)
                {
                    slot.commands.Reset();
                    slot.state = SlotState::Free;
                }
            }
        }

        std::lock_guard<std::mutex> d3dLock(d3dMutex_);
        const bool cleared = pipeline_->clear_preview_black();
        if (cleared)
            clearActiveInfo();
        return cleared;
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerStopping_ = true;
            workerCv_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();

        std::lock_guard<std::mutex> lock(mutex_);
        if (pipeline_)
            pipeline_->release_preview_swapchain();
        for (UploadSlot &slot : slots_)
            slot = UploadSlot{};
        pipeline_.reset();
        d3d_.reset();
        configured_ = false;
        hwnd_ = nullptr;
        workerStarted_ = false;
        workerStopping_ = false;
        uploadWidth_ = 0;
        uploadHeight_ = 0;
        uploadPixelFormat_ = -1;
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
        stats.skipped_presents = skippedPresents_ + skippedSubmits_;
        stats.present_fps = active_.load(std::memory_order_relaxed)
                                ? calculatePresentFps(std::chrono::steady_clock::now())
                                : 0.0;
    }

private:
    enum class SlotState { Free, Uploading, Pending, Presenting };

    struct UploadSlot
    {
        ComPtr<ID3D11DeviceContext> deferred;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11CommandList> commands;
        SlotState state = SlotState::Free;
        int width = 0;
        int height = 0;
        int bitDepth = 0;
        int pixelFormat = -1;
        uint64_t frameId = 0;
        uint64_t generation = 0;
    };

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
        skippedSubmits_ = 0;
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

    bool ensureWorkerLocked()
    {
        if (workerStarted_)
            return true;
        workerStopping_ = false;
        try
        {
            worker_ = std::thread(&PreviewRenderer::workerLoop, this);
            workerStarted_ = true;
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    bool ensureUploadSlotsLocked(int width, int height, int pixelFormat)
    {
        if (uploadWidth_ == width && uploadHeight_ == height &&
            uploadPixelFormat_ == pixelFormat && slots_[0].texture)
            return true;

        for (const UploadSlot &slot : slots_)
            if (slot.state == SlotState::Uploading || slot.state == SlotState::Presenting)
                return false;

        const DXGI_FORMAT format = pixelFormat == GVFG_PREVIEW_PIXFMT_Y210
                                       ? DXGI_FORMAT_R16G16B16A16_UINT
                                       : DXGI_FORMAT_R8G8B8A8_UINT;
        const UINT textureWidth = static_cast<UINT>((width + 1) / 2);
        std::array<UploadSlot, 3> replacement{};
        for (UploadSlot &slot : replacement)
        {
            if (FAILED(d3d_->device->CreateDeferredContext(0, &slot.deferred)))
                return false;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = textureWidth;
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(d3d_->device->CreateTexture2D(&desc, nullptr, &slot.texture)))
                return false;
        }
        slots_ = std::move(replacement);
        uploadWidth_ = width;
        uploadHeight_ = height;
        uploadPixelFormat_ = pixelFormat;
        return true;
    }

    void workerLoop()
    {
        for (;;)
        {
            size_t selected = slots_.size();
            UploadSlot work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                workerCv_.wait(lock, [this] {
                    if (workerStopping_)
                        return true;
                    for (const UploadSlot &slot : slots_)
                        if (slot.state == SlotState::Pending)
                            return true;
                    return false;
                });
                if (workerStopping_)
                    break;
                for (size_t i = 0; i < slots_.size(); ++i)
                    if (slots_[i].state == SlotState::Pending &&
                        (selected == slots_.size() || slots_[i].frameId > slots_[selected].frameId))
                        selected = i;
                for (size_t i = 0; i < slots_.size(); ++i)
                    if (i != selected && slots_[i].state == SlotState::Pending)
                    {
                        slots_[i].commands.Reset();
                        slots_[i].state = SlotState::Free;
                        ++skippedSubmits_;
                    }
                slots_[selected].state = SlotState::Presenting;
                work = slots_[selected];
            }

            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            bool ready = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ready = configured_ && !workerStopping_ &&
                        ensurePipeline(work.width, work.height, work.bitDepth);
            }
            const auto ensureEnd = Clock::now();
            bool rendered = false;
            bool copied = false;
            bool blitted = false;
            gvfg::internal::gvfg_preview_present_result_t presentResult =
                gvfg::internal::GVFG_PREVIEW_PRESENT_FAILED;
            if (ready)
            {
                std::lock_guard<std::mutex> d3dLock(d3dMutex_);
                if (work.generation == clearGeneration_.load(std::memory_order_acquire))
                {
                    d3d_->context->ExecuteCommandList(work.commands.Get(), FALSE);
                    const gvfg::internal::gvfg_render_pixfmt_t renderFmt =
                        work.pixelFormat == GVFG_PREVIEW_PIXFMT_Y210
                            ? gvfg::internal::GVFG_RENDER_FMT_Y210
                            : gvfg::internal::GVFG_RENDER_FMT_YUY2;
                    rendered = pipeline_->render_texture_to_fp16(work.texture.Get(), renderFmt,
                                                                  work.width, work.height);
                    copied = rendered && pipeline_->copy_fp16_to_scene();
                    blitted = copied && (pipeline_->preview_swapchain_10bit() ||
                                         pipeline_->blit_fp16_to_rgba8(work.width, work.height));
                    if (blitted)
                        presentResult = pipeline_->present_preview(work.width, work.height);
                }
            }
            const auto end = Clock::now();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ready && rendered && copied && blitted &&
                    presentResult != gvfg::internal::GVFG_PREVIEW_PRESENT_FAILED)
                {
                    recordPresentResult(presentResult);
                    width_.store(work.width, std::memory_order_relaxed);
                    height_.store(work.height, std::memory_order_relaxed);
                    bitDepth_.store(work.bitDepth, std::memory_order_relaxed);
                    swapchain10Bit_.store(pipeline_->preview_swapchain_10bit(), std::memory_order_relaxed);
                    active_.store(true, std::memory_order_relaxed);
                }
                else
                    clearActiveInfo();
                slots_[selected].commands.Reset();
                slots_[selected].state = SlotState::Free;
            }
#if GVFG_INTERNAL_DIAGNOSTICS
            const double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
            if (totalMs >= 10.0)
            {
                char line[320] = {};
                std::snprintf(line, sizeof(line),
                              "[GVFG][PREVIEW] SLOW ASYNC frame=%llu total=%.3f ensure=%.3f ms\n",
                              static_cast<unsigned long long>(work.frameId), totalMs,
                              std::chrono::duration<double, std::milli>(ensureEnd - start).count());
                OutputDebugStringA(line);
            }
#endif
        }
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
        const bool ready = pipeline_->ensure_rt_and_pipeline(width, height) &&
                           pipeline_->ensure_preview_swapchain(width, height);
        return ready;
    }

    mutable std::mutex mutex_;
    std::mutex d3dMutex_;
    std::atomic<uint64_t> clearGeneration_{0};
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
    uint64_t skippedSubmits_ = 0;
    std::array<UploadSlot, 3> slots_{};
    std::thread worker_;
    std::condition_variable workerCv_;
    bool workerStarted_ = false;
    bool workerStopping_ = false;
    int uploadWidth_ = 0;
    int uploadHeight_ = 0;
    int uploadPixelFormat_ = -1;
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

    gvfg_preview_status_t gvfg_preview_prepare(gvfg_preview_handle handle,
                                               int width,
                                               int height,
                                               int bit_depth)
    {
        if (!handle || width <= 0 || height <= 0)
            return GVFG_PREVIEW_EINVAL;
        return handle->renderer.prepare(width, height, bit_depth)
                   ? GVFG_PREVIEW_OK
                   : GVFG_PREVIEW_ERENDER;
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
        case GVFG_PREVIEW_PIXFMT_YUY2:
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

    gvfg_preview_status_t gvfg_preview_clear(gvfg_preview_handle handle)
    {
        if (!handle)
            return GVFG_PREVIEW_EINVAL;
        return handle->renderer.clear() ? GVFG_PREVIEW_OK : GVFG_PREVIEW_ERENDER;
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
