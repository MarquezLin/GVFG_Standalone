#include "gvfg_capture.h"

#include "gvfg_debug.h"
#include "pcies2mm_capture_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gvfg::internal;

namespace
{
    void copy_cstr(char *dst, size_t dstSize, const char *src)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = 0;
        if (src)
            strncpy_s(dst, dstSize, src, _TRUNCATE);
    }

    uint64_t now_ns()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    gvfg_status_t map_status(pcies2mm_status_t st)
    {
        switch (st)
        {
        case PCIES2MM_OK:
            return GVFG_OK;
        case PCIES2MM_EINVAL:
            return GVFG_EINVAL;
        case PCIES2MM_ENODEV:
            return GVFG_ENODEV;
        case PCIES2MM_ESTATE:
            return GVFG_ESTATE;
        case PCIES2MM_ETIMEOUT:
            return GVFG_ETIMEOUT;
        case PCIES2MM_ENOTSUP:
            return GVFG_ENOTSUP;
        case PCIES2MM_EIO:
        default:
            return GVFG_EIO;
        }
    }

    const char *pcies2mm_status_text(pcies2mm_status_t st)
    {
        switch (st)
        {
        case PCIES2MM_OK:
            return "ok";
        case PCIES2MM_EINVAL:
            return "invalid argument";
        case PCIES2MM_ENODEV:
            return "device not found";
        case PCIES2MM_ESTATE:
            return "invalid state";
        case PCIES2MM_ENOTSUP:
            return "not supported";
        case PCIES2MM_ETIMEOUT:
            return "timeout";
        case PCIES2MM_EIO:
            return "i/o error";
        default:
            return "unknown";
        }
    }

    const char *pcies2mm_error_text(pcies2mm_status_t st, const gvfg::internal::PcieS2mmCaptureSession *session)
    {
        const char *detail = session ? session->last_error() : nullptr;
        if (detail && detail[0])
            return detail;
        return pcies2mm_status_text(st);
    }

    void copy_wide_to_utf8(const std::wstring &src, char *dst, size_t dstSize)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = '\0';
        if (src.empty())
            return;
        WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, dst, static_cast<int>(dstSize), nullptr, nullptr);
        dst[dstSize - 1] = '\0';
    }

    gvfg_event_type_t map_event_type(pcies2mm_event_type_t type)
    {
        switch (type)
        {
        case PCIES2MM_EVENT_PLUG_IN:
            return GVFG_EVENT_SIGNAL_CONNECTED;
        case PCIES2MM_EVENT_PLUG_OUT:
            return GVFG_EVENT_SIGNAL_DISCONNECTED;
        case PCIES2MM_EVENT_CAPTURE_PAUSED:
            return GVFG_EVENT_CAPTURE_PAUSED;
        case PCIES2MM_EVENT_CAPTURE_RESUMED:
            return GVFG_EVENT_CAPTURE_RESUMED;
        default:
            return GVFG_EVENT_UNKNOWN;
        }
    }

    int to_gvfg_pixel_format(pcies2mm_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case PCIES2MM_PIXFMT_YUY2:
            return GVFG_PIXFMT_YUY2;
        case PCIES2MM_PIXFMT_UYVY:
            return GVFG_PIXFMT_UYVY;
        case PCIES2MM_PIXFMT_RGB24:
            return GVFG_PIXFMT_RGB24;
        case PCIES2MM_PIXFMT_BGRX32:
            return GVFG_PIXFMT_BGRX32;
        case PCIES2MM_PIXFMT_NV12:
            return GVFG_PIXFMT_NV12;
        case PCIES2MM_PIXFMT_P010:
            return GVFG_PIXFMT_P010;
        case PCIES2MM_PIXFMT_Y210:
            return GVFG_PIXFMT_Y210;
        case PCIES2MM_PIXFMT_YUV444:
            return GVFG_PIXFMT_YUV444;
        case PCIES2MM_PIXFMT_V210:
            return GVFG_PIXFMT_V210;
        default:
            return GVFG_PIXFMT_UNKNOWN;
        }
    }

    const char *gvfg_pixel_format_name(int fmt)
    {
        switch (fmt)
        {
        case GVFG_PIXFMT_YUY2:
            return "YUY2";
        case GVFG_PIXFMT_UYVY:
            return "UYVY";
        case GVFG_PIXFMT_RGB24:
            return "RGB24";
        case GVFG_PIXFMT_BGRX32:
            return "BGRX32";
        case GVFG_PIXFMT_NV12:
            return "NV12";
        case GVFG_PIXFMT_P010:
            return "P010";
        case GVFG_PIXFMT_Y210:
            return "Y210";
        case GVFG_PIXFMT_YUV444:
            return "YUV444";
        case GVFG_PIXFMT_V210:
            return "V210";
        case GVFG_PIXFMT_BGRA8:
            return "BGRA8";
        default:
            return "UNKNOWN";
        }
    }

    bool checked_mul_u64(uint64_t a, uint64_t b, uint64_t &out)
    {
        if (a != 0 && b > UINT64_MAX / a)
            return false;
        out = a * b;
        return true;
    }

    bool checked_add_u64(uint64_t a, uint64_t b, uint64_t &out)
    {
        if (b > UINT64_MAX - a)
            return false;
        out = a + b;
        return true;
    }

    bool set_frame_plane(const gvfg_frame_t &frame,
                         gvfg_frame_layout_t &layout,
                         int index,
                         uint64_t offset,
                         uint64_t size,
                         int stride)
    {
        if (index < 0 || index >= GVFG_MAX_PLANES || !frame.data || stride < 0)
            return false;

        uint64_t end = 0;
        if (!checked_add_u64(offset, size, end) || end > frame.data_size)
            return false;

        layout.plane_data[index] = static_cast<const uint8_t *>(frame.data) + offset;
        layout.plane_offset[index] = offset;
        layout.plane_size[index] = size;
        layout.plane_stride[index] = stride;
        return true;
    }

    void set_fallback_frame_layout(const gvfg_frame_t &frame, gvfg_frame_layout_t &layout)
    {
        layout.row_bytes = 0;
        layout.plane_count = frame.data ? 1 : 0;
        layout.layout_flags = 0;
        if (layout.plane_count == 0)
            return;

        int stride = 0;
        if (frame.height > 0)
        {
            const uint64_t guessedStride = frame.data_size / static_cast<uint64_t>(frame.height);
            if (guessedStride <= static_cast<uint64_t>(INT_MAX))
                stride = static_cast<int>(guessedStride);
        }

        layout.row_bytes = stride;
        if (set_frame_plane(frame, layout, 0, 0, frame.data_size, stride))
            layout.layout_flags = GVFG_FRAME_LAYOUT_CONTIGUOUS | GVFG_FRAME_LAYOUT_SDK_DERIVED;
    }

    gvfg_status_t populate_frame_layout(const gvfg_frame_t &frame, gvfg_frame_layout_t &layout)
    {
        layout.layout_flags = 0;
        layout.row_bytes = 0;
        layout.plane_count = 0;
        std::memset(layout.plane_data, 0, sizeof(layout.plane_data));
        std::memset(layout.plane_stride, 0, sizeof(layout.plane_stride));
        std::memset(layout.plane_size, 0, sizeof(layout.plane_size));
        std::memset(layout.plane_offset, 0, sizeof(layout.plane_offset));
        std::memset(layout.reserved, 0, sizeof(layout.reserved));

        if (!frame.data || frame.data_size == 0 || frame.width <= 0 || frame.height <= 0)
            return GVFG_EINVAL;

        const uint64_t width = static_cast<uint64_t>(frame.width);
        const uint64_t height = static_cast<uint64_t>(frame.height);
        uint64_t row = 0;
        uint64_t size0 = 0;
        uint64_t size1 = 0;

        switch (frame.pixel_format)
        {
        case GVFG_PIXFMT_YUY2:
        case GVFG_PIXFMT_UYVY:
            if (!checked_mul_u64(width, 2u, row) || !checked_mul_u64(row, height, size0))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) && set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)))
                layout.plane_count = 1;
            break;
        case GVFG_PIXFMT_Y210:
        case GVFG_PIXFMT_BGRX32:
        case GVFG_PIXFMT_BGRA8:
            if (!checked_mul_u64(width, 4u, row) || !checked_mul_u64(row, height, size0))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) && set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)))
                layout.plane_count = 1;
            break;
        case GVFG_PIXFMT_RGB24:
            if (!checked_mul_u64(width, 3u, row) || !checked_mul_u64(row, height, size0))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) && set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)))
                layout.plane_count = 1;
            break;
        case GVFG_PIXFMT_YUV444:
            if (!checked_mul_u64(width, frame.bit_depth > 8 ? 6u : 3u, row) ||
                !checked_mul_u64(row, height, size0))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) && set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)))
                layout.plane_count = 1;
            break;
        case GVFG_PIXFMT_V210:
            if (!checked_mul_u64((width + 5u) / 6u, 16u, row) || !checked_mul_u64(row, height, size0))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) && set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)))
                layout.plane_count = 1;
            break;
        case GVFG_PIXFMT_NV12:
            row = width;
            if (!checked_mul_u64(row, height, size0) ||
                !checked_mul_u64(row, height / 2u, size1))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) &&
                set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)) &&
                set_frame_plane(frame, layout, 1, size0, size1, static_cast<int>(row)))
                layout.plane_count = 2;
            break;
        case GVFG_PIXFMT_P010:
            if (!checked_mul_u64(width, 2u, row) ||
                !checked_mul_u64(row, height, size0) ||
                !checked_mul_u64(row, height / 2u, size1))
                break;
            if (row <= static_cast<uint64_t>(INT_MAX) &&
                set_frame_plane(frame, layout, 0, 0, size0, static_cast<int>(row)) &&
                set_frame_plane(frame, layout, 1, size0, size1, static_cast<int>(row)))
                layout.plane_count = 2;
            break;
        default:
            break;
        }

        if (layout.plane_count > 0)
        {
            layout.row_bytes = layout.plane_stride[0];
            layout.layout_flags = GVFG_FRAME_LAYOUT_CONTIGUOUS | GVFG_FRAME_LAYOUT_SDK_DERIVED;
            return GVFG_OK;
        }

        set_fallback_frame_layout(frame, layout);
        return layout.plane_count > 0 ? GVFG_OK : GVFG_ENOTSUP;
    }

}

struct gvfg_handle_t
{
    ~gvfg_handle_t()
    {
        close();
    }

    gvfg_status_t open(int index, int channelIndex = GVFG_CHANNEL_0)
    {
        if (index < 0 || (channelIndex != GVFG_CHANNEL_0 && channelIndex != GVFG_CHANNEL_1))
            return GVFG_EINVAL;
        if (callbackModeActive.load(std::memory_order_acquire) || isInCallbackThread())
            return GVFG_ESTATE;

        close();

        backend = std::make_unique<gvfg::internal::PcieS2mmCaptureSession>();
        const pcies2mm_status_t stOpen = backend->open_device_index(static_cast<size_t>(index));
        if (stOpen != PCIES2MM_OK)
        {
            recordError(pcies2mm_error_text(stOpen, backend.get()));
            backend.reset();
            return map_status(stOpen);
        }

        currentIndex = index;
        syncBackendEventCallback();
        selectedChannel = static_cast<uint32_t>(channelIndex);
        resetRuntimeCounters();
        const pcies2mm_status_t stChannel = backend->set_channel(selectedChannel);
        if (stChannel != PCIES2MM_OK)
        {
            recordError(pcies2mm_error_text(stChannel, backend.get()));
            close();
            return map_status(stChannel);
        }

        querySignal();
        return GVFG_OK;
    }

    gvfg_status_t start()
    {
        if (!backend)
            return GVFG_ESTATE;
        if (callbackModeActive.load(std::memory_order_acquire))
            return GVFG_ESTATE;
        if (running)
            return GVFG_OK;

        const gvfg_status_t cfg = configureStream();
        if (cfg != GVFG_OK)
            return cfg;

        resetRuntimeCounters();
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            eventQueue.clear();
        }

        const pcies2mm_status_t st = backend->start_stream();
        if (st != PCIES2MM_OK)
        {
            recordError(pcies2mm_error_text(st, backend.get()));
            return map_status(st);
        }

        running = true;
        return GVFG_OK;
    }

    gvfg_status_t stop()
    {
        if (callbackModeActive.load(std::memory_order_acquire))
            return stopCallbackMode();
        running = false;
        releaseHeldFrameForStop();
        if (backend)
            backend->stop_stream();
        return GVFG_OK;
    }

    void close()
    {
        stop();
        if (backend)
        {
            backend->set_event_callback(nullptr, nullptr, 0);
            backend->close();
            backend.reset();
        }
        currentIndex = -1;
    }

    gvfg_status_t getSignalStatus(gvfg_signal_status_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        const pcies2mm_status_t status = querySignal();
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            out.connected = signalConnected ? 1 : 0;
            out.channel = static_cast<int>(selectedChannel);
            out.width = static_cast<int>(width);
            out.height = static_cast<int>(height);
            copy_cstr(out.pixel_format, sizeof(out.pixel_format), gvfg_pixel_format_name(to_gvfg_pixel_format(pixelFormat)));
            out.bit_depth = static_cast<int>(bitDepth);
        }
        return map_status(status);
    }

    gvfg_status_t getRuntimeInfo(gvfg_runtime_info_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        getSignalStatus(out.input_signal);
        const uint64_t frames = deliveredFrames.load(std::memory_order_relaxed);
        const bool deliveredValid = running.load(std::memory_order_relaxed) && frames > 0;

        out.last_frame.valid = deliveredValid ? 1 : 0;
        if (deliveredValid)
        {
            out.last_frame.width = static_cast<int>(deliveredWidth.load(std::memory_order_relaxed));
            out.last_frame.height = static_cast<int>(deliveredHeight.load(std::memory_order_relaxed));
            out.last_frame.bit_depth = static_cast<int>(deliveredBitDepth.load(std::memory_order_relaxed));
            copy_cstr(out.last_frame.pixel_format,
                      sizeof(out.last_frame.pixel_format),
                      gvfg_pixel_format_name(deliveredPixelFormat.load(std::memory_order_relaxed)));
        }
        out.capture_fps = runtimeFps.load(std::memory_order_relaxed);
        out.delivered_frames = frames;
        return GVFG_OK;
    }

    void syncBackendEventCallback()
    {
        if (!backend)
            return;
        backend->set_event_callback(&gvfg_handle_t::onBackendEvent,
                                    this,
                                    PCIES2MM_EVENT_MASK_DEFAULT);
    }

    static void onBackendEvent(const pcies2mm_event_t *event, void *user)
    {
        auto *self = static_cast<gvfg_handle_t *>(user);
        if (!self || !event)
            return;
        self->emitEvent(*event);
    }

    void emitEvent(const pcies2mm_event_t &event)
    {
        gvfg_event_t out{};
        out.type = map_event_type(event.type);
        out.timestamp_ns = event.timestamp_ns;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            if (eventQueue.size() >= 64)
                eventQueue.pop_front();
            eventQueue.push_back(out);
        }
        eventCv.notify_one();

    }

    pcies2mm_status_t querySignal()
    {
        if (!backend)
            return PCIES2MM_ESTATE;

        pcies2mm_signal_status_t sig{};
        const pcies2mm_status_t status = backend->get_signal_status(sig);

        std::lock_guard<std::mutex> lock(stateMutex);
        width = status == PCIES2MM_OK ? sig.width : 0;
        height = status == PCIES2MM_OK ? sig.height : 0;
        bitDepth = status == PCIES2MM_OK ? sig.bit_depth : 0;
        pixelFormat = status == PCIES2MM_OK ? sig.pixel_format : PCIES2MM_PIXFMT_UNKNOWN;
        signalConnected = status == PCIES2MM_OK && sig.connected != 0;
        return status;
    }

    gvfg_status_t configureStream()
    {
        if (!backend)
            return GVFG_ESTATE;

        const pcies2mm_status_t signalStatus = querySignal();
        if (signalStatus != PCIES2MM_OK && signalStatus != PCIES2MM_ENODEV)
            return map_status(signalStatus);

        const bool waitingForSignal = signalStatus == PCIES2MM_ENODEV;
        // configure_stream() is also used to enter event-monitoring mode. Keep
        // its inactive placeholder ring minimal; the real signal descriptor
        // replaces it before DMA is enabled after reconnect.
        const uint32_t configureWidth = waitingForSignal ? 2 : width;
        const uint32_t configureHeight = waitingForSignal ? 1 : height;
        const pcies2mm_pixel_format_t configureFormat =
            waitingForSignal || pixelFormat == PCIES2MM_PIXFMT_UNKNOWN ? PCIES2MM_PIXFMT_YUY2 : pixelFormat;

        pcies2mm_stream_desc_t desc{};
        desc.channel = selectedChannel;
        desc.width = configureWidth;
        desc.height = configureHeight;
        desc.pixel_format = configureFormat;
        desc.buffer_count = 3;

        const pcies2mm_status_t st = backend->configure_stream(desc);
        if (st != PCIES2MM_OK)
        {
            recordError(pcies2mm_error_text(st, backend.get()));
            return map_status(st);
        }
        if (waitingForSignal)
            recordError(nullptr);
        return GVFG_OK;
    }

    gvfg_status_t readFrame(gvfg_frame_t &out, uint32_t timeoutMs, bool allowCallbackMode = false)
    {
        std::memset(&out, 0, sizeof(out));
        if (!backend || !running)
            return GVFG_ESTATE;
        if (callbackModeActive.load(std::memory_order_acquire) && !allowCallbackMode)
            return GVFG_ESTATE;

        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (readInProgress || frameHeld)
                return GVFG_ESTATE;
            readInProgress = true;
        }

        pcies2mm_frame_t frame{};
        const pcies2mm_status_t st = backend->wait_frame(timeoutMs, frame);
        if (st != PCIES2MM_OK)
        {
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                readInProgress = false;
            }
            if (st != PCIES2MM_ETIMEOUT && st != PCIES2MM_ESTATE)
                recordError(pcies2mm_error_text(st, backend.get()));
            return map_status(st);
        }

        updateRuntimeFps(now_ns());

        if (!frame.data || frame.width == 0 || frame.height == 0)
        {
            backend->release_frame(frame);
            std::lock_guard<std::mutex> lock(frameMutex);
            readInProgress = false;
            return GVFG_EIO;
        }

        out.data = frame.data;
        out.data_size = static_cast<uint64_t>(frame.data_size_bytes);
        out.width = static_cast<int>(frame.width);
        out.height = static_cast<int>(frame.height);
        out.pixel_format = to_gvfg_pixel_format(frame.pixel_format);
        out.bit_depth = static_cast<int>(frame.bit_depth);
        out.frame_id = frame.frame_id;
        noteDeliveredFrame(out.width, out.height, out.bit_depth, out.pixel_format);

        std::lock_guard<std::mutex> lock(frameMutex);
        heldBackendFrame = frame;
        readInProgress = false;
        frameHeld = true;
        return GVFG_OK;
    }

    gvfg_status_t releaseFrame(const gvfg_frame_t &frameToken)
    {
        if (!backend)
            return GVFG_ESTATE;

        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (!frameHeld)
                return GVFG_ESTATE;

            if (frameToken.data != heldBackendFrame.data ||
                frameToken.data_size != static_cast<uint64_t>(heldBackendFrame.data_size_bytes) ||
                frameToken.width != static_cast<int>(heldBackendFrame.width) ||
                frameToken.height != static_cast<int>(heldBackendFrame.height) ||
                frameToken.pixel_format != to_gvfg_pixel_format(heldBackendFrame.pixel_format) ||
                frameToken.bit_depth != static_cast<int>(heldBackendFrame.bit_depth) ||
                frameToken.frame_id != heldBackendFrame.frame_id)
                return GVFG_EINVAL;

            const pcies2mm_status_t st = backend->release_frame(heldBackendFrame);
            if (st != PCIES2MM_OK)
                return map_status(st);

            heldBackendFrame = {};
            frameHeld = false;
        }

        return GVFG_OK;
    }

    void releaseHeldFrameForStop()
    {
        if (!backend)
            return;

        pcies2mm_frame_t frame{};
        bool shouldRelease = false;
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (frameHeld)
            {
                frame = heldBackendFrame;
                heldBackendFrame = {};
                frameHeld = false;
                shouldRelease = true;
            }
        }

        if (shouldRelease)
            backend->release_frame(frame);
    }

    gvfg_status_t pollEvent(gvfg_event_t &out, uint32_t timeoutMs)
    {
        if (callbackModeActive.load(std::memory_order_acquire))
            return GVFG_ESTATE;

        std::unique_lock<std::mutex> lock(eventMutex);
        const auto hasEvent = [this]()
        {
            return !eventQueue.empty();
        };

        if (timeoutMs == 0)
        {
            if (!hasEvent())
                return GVFG_ETIMEOUT;
        }
        else if (!eventCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasEvent))
        {
            return GVFG_ETIMEOUT;
        }

        out = eventQueue.front();
        eventQueue.pop_front();
        return GVFG_OK;
    }

    gvfg_status_t setFrameCallback(gvfg_frame_callback_t callback, void *userData)
    {
        if (callbackModeActive.load(std::memory_order_acquire))
            return GVFG_ESTATE;

        std::lock_guard<std::mutex> lock(callbackMutex);
        frameCallback = callback;
        frameCallbackUserData = userData;
        return GVFG_OK;
    }

    gvfg_status_t setEventCallback(gvfg_event_callback_t callback, void *userData)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        eventCallback = callback;
        eventCallbackUserData = userData;
        return GVFG_OK;
    }

    gvfg_status_t startCallbackMode()
    {
        if (!backend)
            return GVFG_ESTATE;
        if (callbackModeActive.load(std::memory_order_acquire))
            return GVFG_OK;
        if (running.load(std::memory_order_acquire))
            return GVFG_ESTATE;

        gvfg_frame_callback_t callback = nullptr;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = frameCallback;
        }
        if (!callback)
            return GVFG_EINVAL;

        const gvfg_status_t cfg = configureStream();
        if (cfg != GVFG_OK)
            return cfg;

        resetRuntimeCounters();
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            eventQueue.clear();
        }
        const pcies2mm_status_t st = backend->start_stream();
        if (st != PCIES2MM_OK)
        {
            recordError(pcies2mm_error_text(st, backend.get()));
            return map_status(st);
        }

        callbackStop.store(false, std::memory_order_release);
        callbackModeActive.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);

        try
        {
            callbackThread = std::thread(&gvfg_handle_t::callbackThreadProc, this);
        }
        catch (...)
        {
            callbackModeActive.store(false, std::memory_order_release);
            callbackStop.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            backend->stop_stream();
            eventCv.notify_all();
            if (callbackThread.joinable())
                callbackThread.join();
            return GVFG_EIO;
        }

        return GVFG_OK;
    }

    gvfg_status_t stopCallbackMode()
    {
        if (callbackThread.joinable() && std::this_thread::get_id() == callbackThread.get_id())
            return GVFG_ESTATE;

        if (!callbackModeActive.load(std::memory_order_acquire))
            return GVFG_OK;

        callbackStop.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
        eventCv.notify_all();
        if (backend)
            backend->stop_stream();

        if (callbackThread.joinable())
            callbackThread.join();
        callbackModeActive.store(false, std::memory_order_release);
        releaseHeldFrameForStop();
        return GVFG_OK;
    }

    bool isInCallbackThread() const
    {
        return callbackThread.joinable() && std::this_thread::get_id() == callbackThread.get_id();
    }

    void dispatchPendingEvents()
    {
        for (;;)
        {
            gvfg_event_t event{};
            {
                std::lock_guard<std::mutex> lock(eventMutex);
                if (eventQueue.empty())
                    return;
                event = eventQueue.front();
                eventQueue.pop_front();
            }

            gvfg_event_callback_t callback = nullptr;
            void *userData = nullptr;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                callback = eventCallback;
                userData = eventCallbackUserData;
            }

            if (callback)
                callback(this, &event, userData);
        }
    }

    void callbackThreadProc()
    {
        while (!callbackStop.load(std::memory_order_acquire))
        {
            dispatchPendingEvents();
            if (callbackStop.load(std::memory_order_acquire))
                break;

            gvfg_frame_t frame{};
            const gvfg_status_t st = readFrame(frame, 100, true);
            if (st == GVFG_ETIMEOUT)
                continue;
            if (st != GVFG_OK)
            {
                if (!callbackStop.load(std::memory_order_acquire))
                {
                    std::unique_lock<std::mutex> lock(eventMutex);
                    eventCv.wait_for(lock,
                                     std::chrono::milliseconds(100),
                                     [this]()
                                     { return callbackStop.load(std::memory_order_acquire) || !eventQueue.empty(); });
                }
                continue;
            }

            // An event may have arrived while wait_frame() was blocked. Deliver
            // it before the newly returned frame so a resume notification is
            // observed before the first post-reconnect frame callback.
            dispatchPendingEvents();
            if (callbackStop.load(std::memory_order_acquire))
            {
                releaseFrame(frame);
                break;
            }

            gvfg_frame_callback_t callback = nullptr;
            void *userData = nullptr;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                callback = frameCallback;
                userData = frameCallbackUserData;
            }

            if (callback)
                callback(this, &frame, userData);

            releaseFrame(frame);
            dispatchPendingEvents();
        }
    }

    gvfg_status_t getDebugBackendStats(gvfg_debug_backend_stats_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        out.sdk_running = running.load(std::memory_order_relaxed) ? 1 : 0;
        out.runtime_fps = runtimeFps.load(std::memory_order_relaxed);
        out.frames_returned = deliveredFrames.load(std::memory_order_relaxed);
        out.last_frame_width = static_cast<int>(deliveredWidth.load(std::memory_order_relaxed));
        out.last_frame_height = static_cast<int>(deliveredHeight.load(std::memory_order_relaxed));
        out.last_frame_pixel_format = deliveredPixelFormat.load(std::memory_order_relaxed);
        out.last_frame_bit_depth = static_cast<int>(deliveredBitDepth.load(std::memory_order_relaxed));

        {
            std::lock_guard<std::mutex> lock(frameMutex);
            out.frame_held = frameHeld ? 1 : 0;
        }
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            out.event_queue_depth = static_cast<uint32_t>(eventQueue.size());
        }

        if (backend)
        {
            pcies2mm_stream_stats_t stats{};
            pcies2mm_debug_state_t debugState{};
            uint64_t waitTimeouts = 0;
            backend->get_debug_stats(stats, waitTimeouts, debugState);
            out.backend_state = static_cast<int>(stats.state);
            out.backend_frames_captured = stats.frames_captured;
            out.backend_frames_delivered = stats.frames_delivered;
            out.backend_frames_dropped = stats.frames_dropped;
            out.backend_dma_errors = stats.dma_errors;
            out.backend_interrupt_count = stats.interrupt_count;
            out.backend_wait_timeouts = waitTimeouts;
            out.backend_running = debugState.running;
            out.backend_capture_active = debugState.capture_active;
            out.backend_pending_events = debugState.pending_events;
            out.backend_latest_sequence = debugState.latest_sequence;
            out.backend_delivered_sequence = debugState.delivered_sequence;
            out.backend_active_delivery_slot = debugState.active_delivery_slot;
            out.backend_next_write_slot = debugState.next_write_slot;
            out.backend_ring_size = debugState.ring_size;
        }

        return GVFG_OK;
    }

    gvfg_status_t getLastErrorDetail(char *outMessage, uint32_t outMessageSize)
    {
        if (!outMessage || outMessageSize == 0)
            return GVFG_EINVAL;
        outMessage[0] = '\0';
        if (!lastError.empty())
            copy_cstr(outMessage, outMessageSize, lastError.c_str());
        else
            copy_cstr(outMessage, outMessageSize, backend ? backend->last_error() : "");
        return GVFG_OK;
    }

    void recordError(const char *msg)
    {
        lastError = msg ? msg : "";
    }

    void updateRuntimeFps(uint64_t ptsNs)
    {
        const uint64_t prevPtsNs = lastPtsNs.exchange(ptsNs, std::memory_order_relaxed);
        if (prevPtsNs != 0 && ptsNs > prevPtsNs)
        {
            const double fps = 1e9 / static_cast<double>(ptsNs - prevPtsNs);
            if (fps > 0.0 && fps < 1000.0)
            {
                const double current = runtimeFps.load(std::memory_order_relaxed);
                runtimeFps.store((current <= 0.0) ? fps : current * 0.9 + fps * 0.1,
                                 std::memory_order_relaxed);
            }
        }
    }

    void resetRuntimeCounters()
    {
        lastPtsNs.store(0, std::memory_order_relaxed);
        deliveredFrames.store(0, std::memory_order_relaxed);
        deliveredWidth.store(0, std::memory_order_relaxed);
        deliveredHeight.store(0, std::memory_order_relaxed);
        deliveredBitDepth.store(0, std::memory_order_relaxed);
        deliveredPixelFormat.store(GVFG_PIXFMT_UNKNOWN, std::memory_order_relaxed);
        runtimeFps.store(0.0, std::memory_order_relaxed);
    }

    void noteDeliveredFrame(int frameWidth, int frameHeight, int bitDepth, int pixelFormat)
    {
        deliveredWidth.store(frameWidth > 0 ? static_cast<uint32_t>(frameWidth) : 0, std::memory_order_relaxed);
        deliveredHeight.store(frameHeight > 0 ? static_cast<uint32_t>(frameHeight) : 0, std::memory_order_relaxed);
        deliveredBitDepth.store(bitDepth > 0 ? static_cast<uint32_t>(bitDepth) : 0, std::memory_order_relaxed);
        deliveredPixelFormat.store(pixelFormat, std::memory_order_relaxed);
        deliveredFrames.fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_ptr<gvfg::internal::PcieS2mmCaptureSession> backend;
    int currentIndex = -1;
    uint32_t selectedChannel = GVFG_CHANNEL_0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitDepth = 0;
    pcies2mm_pixel_format_t pixelFormat = PCIES2MM_PIXFMT_UNKNOWN;
    bool signalConnected = false;
    mutable std::mutex stateMutex;
    std::atomic<uint64_t> lastPtsNs{0};
    std::atomic<uint64_t> deliveredFrames{0};
    std::atomic<uint32_t> deliveredWidth{0};
    std::atomic<uint32_t> deliveredHeight{0};
    std::atomic<uint32_t> deliveredBitDepth{0};
    std::atomic<int> deliveredPixelFormat{GVFG_PIXFMT_UNKNOWN};
    std::atomic<double> runtimeFps{0.0};

    std::string lastError;

    std::atomic<bool> running{false};
    std::mutex frameMutex;
    bool readInProgress = false;
    bool frameHeld = false;
    pcies2mm_frame_t heldBackendFrame{};
    std::mutex eventMutex;
    std::condition_variable eventCv;
    std::deque<gvfg_event_t> eventQueue;
    std::mutex callbackMutex;
    gvfg_frame_callback_t frameCallback = nullptr;
    void *frameCallbackUserData = nullptr;
    gvfg_event_callback_t eventCallback = nullptr;
    void *eventCallbackUserData = nullptr;
    std::atomic<bool> callbackModeActive{false};
    std::atomic<bool> callbackStop{false};
    std::thread callbackThread;

};

extern "C"
{
    int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices)
    {
        const std::vector<gvfg::internal::PcieS2mmDevice> devices = gvfg::internal::enumerate_pcies2mm_devices();
        const int n = static_cast<int>(devices.size());
        if (n <= 0)
            return n;

        if (!out_devices || max_devices <= 0)
            return n;

        const int written = (std::min)(n, max_devices);
        for (int i = 0; i < written; ++i)
        {
            out_devices[i] = {};
            out_devices[i].index = i;
            copy_cstr(out_devices[i].name, sizeof(out_devices[i].name),
                      "GVFG Capture");
            copy_wide_to_utf8(devices[static_cast<size_t>(i)].friendly_name,
                              out_devices[i].name,
                              sizeof(out_devices[i].name));
            if (!out_devices[i].name[0])
                copy_cstr(out_devices[i].name, sizeof(out_devices[i].name), "GVFG Capture");
        }
        return written;
    }

    gvfg_status_t gvfg_create(gvfg_handle *out_handle)
    {
        if (!out_handle)
            return GVFG_EINVAL;
        auto h = std::make_unique<gvfg_handle_t>();
        *out_handle = h.release();
        return GVFG_OK;
    }

    gvfg_status_t gvfg_destroy(gvfg_handle handle)
    {
        if (handle && handle->isInCallbackThread())
            return GVFG_ESTATE;
        delete handle;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_open(gvfg_handle handle, int device_index)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->open(device_index, GVFG_CHANNEL_0);
    }

    gvfg_status_t gvfg_open_channel(gvfg_handle handle, int device_index, int channel_index)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->open(device_index, channel_index);
    }

    gvfg_status_t gvfg_start(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->start();
    }

    gvfg_status_t gvfg_set_frame_callback(gvfg_handle handle,
                                          gvfg_frame_callback_t callback,
                                          void *user_data)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->setFrameCallback(callback, user_data);
    }

    gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                          gvfg_event_callback_t callback,
                                          void *user_data)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->setEventCallback(callback, user_data);
    }

    gvfg_status_t gvfg_start_callback_mode(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->startCallbackMode();
    }

    gvfg_status_t gvfg_stop_callback_mode(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->stopCallbackMode();
    }

    gvfg_status_t gvfg_read_frame(gvfg_handle handle, gvfg_frame_t *out_frame, uint32_t timeout_ms)
    {
        if (!handle || !out_frame)
            return GVFG_EINVAL;
        return handle->readFrame(*out_frame, timeout_ms);
    }

    gvfg_status_t gvfg_get_frame_layout(const gvfg_frame_t *frame,
                                        gvfg_frame_layout_t *out_layout)
    {
        if (!frame || !out_layout)
            return GVFG_EINVAL;
        if (out_layout->struct_size < sizeof(gvfg_frame_layout_t))
            return GVFG_EINVAL;

        const uint32_t callerSize = out_layout->struct_size;
        std::memset(out_layout, 0, sizeof(*out_layout));
        out_layout->struct_size = callerSize;
        return populate_frame_layout(*frame, *out_layout);
    }

    gvfg_status_t gvfg_release_frame(gvfg_handle handle, const gvfg_frame_t *frame)
    {
        if (!handle || !frame)
            return GVFG_EINVAL;
        return handle->releaseFrame(*frame);
    }

    gvfg_status_t gvfg_poll_event(gvfg_handle handle, gvfg_event_t *out_event, uint32_t timeout_ms)
    {
        if (!handle || !out_event)
            return GVFG_EINVAL;
        return handle->pollEvent(*out_event, timeout_ms);
    }

    gvfg_status_t gvfg_stop(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->stop();
    }

    gvfg_status_t gvfg_get_signal_status(gvfg_handle handle, gvfg_signal_status_t *out_status)
    {
        if (!handle || !out_status)
            return GVFG_EINVAL;
        return handle->getSignalStatus(*out_status);
    }

    gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle, gvfg_runtime_info_t *out_info)
    {
        if (!handle || !out_info)
            return GVFG_EINVAL;
        return handle->getRuntimeInfo(*out_info);
    }

    const char *gvfg_strerror(gvfg_status_t status)
    {
        switch (status)
        {
        case GVFG_OK:
            return "OK";
        case GVFG_EINVAL:
            return "Invalid argument";
        case GVFG_ENODEV:
            return "No GVFG device";
        case GVFG_ESTATE:
            return "Invalid state";
        case GVFG_EIO:
            return "I/O error";
        case GVFG_ENOTSUP:
            return "Not supported";
        case GVFG_ETIMEOUT:
            return "Timeout";
        default:
            return "Unknown";
        }
    }

    gvfg_status_t gvfg_debug_get_backend_stats(gvfg_handle handle,
                                               gvfg_debug_backend_stats_t *out_stats)
    {
        if (!handle || !out_stats)
            return GVFG_EINVAL;

        const uint32_t callerSize = out_stats->struct_size;
        if (callerSize < sizeof(uint32_t) || callerSize > sizeof(gvfg_debug_backend_stats_t))
            return GVFG_EINVAL;

        gvfg_debug_backend_stats_t stats{};
        const gvfg_status_t status = handle->getDebugBackendStats(stats);
        if (status != GVFG_OK)
            return status;

        stats.struct_size = sizeof(stats);
        std::memcpy(out_stats, &stats, callerSize);
        out_stats->struct_size = callerSize;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_debug_get_last_error_detail(gvfg_handle handle,
                                                   char *out_message,
                                                   uint32_t out_message_size)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->getLastErrorDetail(out_message, out_message_size);
    }
}



