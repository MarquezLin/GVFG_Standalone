#include "gvfg_capture.h"

#include "gvfg_debug.h"
#include "pcies2mm_capture_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#ifndef GVFG_VERSION_STRING
#define GVFG_VERSION_STRING "0.0.0"
#endif

using namespace gvfg::internal;

namespace
{
#if INTPTR_MAX == INT64_MAX
    static_assert(std::is_standard_layout_v<gvfg_frame_t>);
    static_assert(sizeof(gvfg_frame_t) == 48, "gvfg_frame_t x64 ABI must remain frozen");
    static_assert(offsetof(gvfg_frame_t, data) == 0);
    static_assert(offsetof(gvfg_frame_t, data_size) == 8);
    static_assert(offsetof(gvfg_frame_t, width) == 16);
    static_assert(offsetof(gvfg_frame_t, height) == 20);
    static_assert(offsetof(gvfg_frame_t, row_stride_bytes) == 24);
    static_assert(offsetof(gvfg_frame_t, pixel_format) == 28);
    static_assert(offsetof(gvfg_frame_t, bit_depth) == 32);
    static_assert(offsetof(gvfg_frame_t, frame_id) == 40);
#endif

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
        case PCIES2MM_EVENT_STREAM_READY:
            return GVFG_EVENT_STREAM_READY;
        case PCIES2MM_EVENT_FORMAT_CHANGE_BEGIN:
            return GVFG_EVENT_FORMAT_CHANGE_BEGIN;
        case PCIES2MM_EVENT_FRAME_LOSS:
            return GVFG_EVENT_FRAME_LOSS;
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
        case PCIES2MM_PIXFMT_Y210:
            return GVFG_PIXFMT_Y210;
        default:
            return GVFG_PIXFMT_UNKNOWN;
        }
    }

    const char *pixel_format_name(int fmt)
    {
        switch (fmt)
        {
        case GVFG_PIXFMT_YUY2:
            return "YUY2";
        case GVFG_PIXFMT_Y210:
            return "Y210";
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

    gvfg_status_t native_row_stride_bytes(uint32_t width,
                                          pcies2mm_pixel_format_t pixelFormat,
                                          int &outStride)
    {
        uint64_t bytesPerPixel = 0;
        if (pixelFormat == PCIES2MM_PIXFMT_YUY2)
            bytesPerPixel = 2;
        else if (pixelFormat == PCIES2MM_PIXFMT_Y210)
            bytesPerPixel = 4;
        else
            return GVFG_ENOTSUP;

        uint64_t row = 0;
        if (!checked_mul_u64(static_cast<uint64_t>(width), bytesPerPixel, row) ||
            row > static_cast<uint64_t>(INT_MAX))
            return GVFG_EINVAL;

        outStride = static_cast<int>(row);
        return GVFG_OK;
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
        running = false;
        eventCv.notify_all();
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
            out.pixel_format = to_gvfg_pixel_format(pixelFormat);
            out.bit_depth = static_cast<int>(bitDepth);
        }
        // A disconnected input is a normal query result, not a missing-device
        // error. The opened device remains usable for plug-in monitoring.
        return status == PCIES2MM_ENODEV ? GVFG_OK : map_status(status);
    }

    gvfg_status_t getRuntimeInfo(gvfg_runtime_info_t &out)
    {
        if (!backend)
            return GVFG_ESTATE;

        std::memset(&out, 0, sizeof(out));
        out.capture_fps = runtimeFps.load(std::memory_order_relaxed);
        out.delivered_frames = deliveredFrames.load(std::memory_order_relaxed);
        pcies2mm_stream_stats_t stats{};
        pcies2mm_debug_state_t debugState{};
        uint64_t waitTimeouts = 0;
        backend->get_debug_stats(stats, waitTimeouts, debugState);
        out.lost_frames = stats.frames_dropped;
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

    static void onBackendEvent(pcies2mm_event_type_t event, void *user)
    {
        auto *self = static_cast<gvfg_handle_t *>(user);
        if (!self)
            return;
        self->emitEvent(event);
    }

    void emitEvent(pcies2mm_event_type_t event)
    {
        const gvfg_event_type_t type = map_event_type(event);
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            if (type == GVFG_EVENT_FRAME_LOSS && !eventQueue.empty() &&
                eventQueue.back().type == GVFG_EVENT_FRAME_LOSS)
            {
                ++eventQueue.back().count;
                eventCv.notify_one();
                return;
            }
            if (eventQueue.size() >= 64)
                eventQueue.pop_front();
            gvfg_event_t out{};
            out.struct_size = sizeof(out);
            out.type = type;
            out.count = type == GVFG_EVENT_FRAME_LOSS ? 1 : 0;
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

    gvfg_status_t readFrame(gvfg_frame_t &out, uint32_t timeoutMs)
    {
        std::memset(&out, 0, sizeof(out));
        if (!backend || !running)
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

        if (!frame.data || frame.width == 0 || frame.height == 0)
        {
            backend->release_frame(frame);
            std::lock_guard<std::mutex> lock(frameMutex);
            readInProgress = false;
            return GVFG_EIO;
        }

        bool stoppedWhileWaiting = false;
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (!running.load(std::memory_order_acquire))
            {
                readInProgress = false;
                stoppedWhileWaiting = true;
            }
            else
            {
                heldBackendFrame = frame;
                readInProgress = false;
                frameHeld = true;
            }
        }
        if (stoppedWhileWaiting)
        {
            backend->release_frame(frame);
            return GVFG_ESTATE;
        }

        updateRuntimeFps(now_ns());

        out.data = frame.data;
        out.data_size = static_cast<uint64_t>(frame.data_size_bytes);
        out.width = static_cast<int>(frame.width);
        out.height = static_cast<int>(frame.height);
        const gvfg_status_t strideStatus =
            native_row_stride_bytes(frame.width, frame.pixel_format, out.row_stride_bytes);
        if (strideStatus != GVFG_OK)
        {
            backend->release_frame(frame);
            std::lock_guard<std::mutex> lock(frameMutex);
            heldBackendFrame = {};
            frameHeld = false;
            return strideStatus;
        }
        out.pixel_format = to_gvfg_pixel_format(frame.pixel_format);
        out.bit_depth = static_cast<int>(frame.bit_depth);
        out.frame_id = frame.frame_id;
        noteDeliveredFrame(out.width, out.height, out.bit_depth, out.pixel_format);

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

            int expectedStride = 0;
            if (native_row_stride_bytes(heldBackendFrame.width,
                                        heldBackendFrame.pixel_format,
                                        expectedStride) != GVFG_OK)
                return GVFG_ESTATE;

            if (frameToken.data != heldBackendFrame.data ||
                frameToken.data_size != static_cast<uint64_t>(heldBackendFrame.data_size_bytes) ||
                frameToken.width != static_cast<int>(heldBackendFrame.width) ||
                frameToken.height != static_cast<int>(heldBackendFrame.height) ||
                frameToken.row_stride_bytes != expectedStride ||
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
        if (!backend || !running.load(std::memory_order_acquire))
            return GVFG_ESTATE;

        std::unique_lock<std::mutex> lock(eventMutex);
        const auto eventAvailableOrStopped = [this]()
        {
            return !eventQueue.empty() || !running.load(std::memory_order_acquire);
        };

        bool waitConditionMet = false;
        if (timeoutMs == 0)
        {
            // Non-blocking: inspect the current state without sleeping.
            waitConditionMet = eventAvailableOrStopped();
        }
        else if (timeoutMs == GVFG_TIMEOUT_INFINITE)
        {
            // Wake when an event arrives or capture is stopped.
            eventCv.wait(lock, eventAvailableOrStopped);
            waitConditionMet = true;
        }
        else
        {
            waitConditionMet = eventCv.wait_for(lock,
                                                std::chrono::milliseconds(timeoutMs),
                                                eventAvailableOrStopped);
        }

        if (!waitConditionMet)
            return GVFG_ETIMEOUT;

        // A wake-up without an event means capture was stopped while waiting.
        if (eventQueue.empty())
            return GVFG_ESTATE;

        out = eventQueue.front();
        eventQueue.pop_front();
        return GVFG_OK;
    }

    gvfg_status_t getDebugBackendStats(gvfg_debug_backend_stats_t &out)
    {
        std::memset(&out, 0, sizeof(out));
        out.sdk_running = running.load(std::memory_order_relaxed) ? 1 : 0;
        out.runtime_fps = runtimeFps.load(std::memory_order_relaxed);
        out.frames_returned = deliveredFrames.load(std::memory_order_relaxed);
        out.last_frame.valid = out.sdk_running && out.frames_returned > 0 ? 1 : 0;
        if (out.last_frame.valid)
        {
            out.last_frame.width = static_cast<int>(deliveredWidth.load(std::memory_order_relaxed));
            out.last_frame.height = static_cast<int>(deliveredHeight.load(std::memory_order_relaxed));
            out.last_frame.bit_depth = static_cast<int>(deliveredBitDepth.load(std::memory_order_relaxed));
            out.last_frame.pixel_format = deliveredPixelFormat.load(std::memory_order_relaxed);
        }

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

    gvfg_status_t debugReadRegister(uint32_t offset, uint32_t &outValue)
    {
        if (!backend)
            return GVFG_ESTATE;
        return map_status(backend->debug_read_register(offset, outValue));
    }

    gvfg_status_t debugWriteRegister(uint32_t offset, uint32_t value)
    {
        if (!backend)
            return GVFG_ESTATE;
        return map_status(backend->debug_write_register(offset, value));
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
};

extern "C"
{
    int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices)
    {
        const std::vector<gvfg::internal::PcieS2mmDevice> devices =
            gvfg::internal::enumerate_pcies2mm_devices();
        const int n = static_cast<int>(devices.size());
        if (n <= 0)
            return n;

        if (!out_devices || max_devices <= 0)
            return n;

        const int written = (std::min)((std::min)(n, max_devices),
                                       static_cast<int>(GVFG_MAX_DEVICES));
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
        delete handle;
        return GVFG_OK;
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

    gvfg_status_t gvfg_read_frame(gvfg_handle handle, gvfg_frame_t *out_frame, uint32_t timeout_ms)
    {
        if (!handle || !out_frame)
            return GVFG_EINVAL;
        return handle->readFrame(*out_frame, timeout_ms);
    }

    gvfg_status_t gvfg_release_frame(gvfg_handle handle, const gvfg_frame_t *frame)
    {
        if (!handle || !frame)
            return GVFG_EINVAL;
        return handle->releaseFrame(*frame);
    }

    gvfg_status_t gvfg_poll_event(gvfg_handle handle, gvfg_event_t *out_event, uint32_t timeout_ms)
    {
        if (!handle || !out_event || out_event->struct_size < sizeof(gvfg_event_t))
            return GVFG_EINVAL;
        gvfg_event_t event{};
        event.struct_size = sizeof(event);
        const gvfg_status_t status = handle->pollEvent(event, timeout_ms);
        if (status == GVFG_OK)
            *out_event = event;
        return status;
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
        gvfg_signal_status_t statusInfo{};
        const gvfg_status_t status = handle->getSignalStatus(statusInfo);
        if (status != GVFG_OK)
            return status;

        *out_status = statusInfo;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle, gvfg_runtime_info_t *out_info)
    {
        if (!handle || !out_info)
            return GVFG_EINVAL;
        gvfg_runtime_info_t runtimeInfo{};
        const gvfg_status_t status = handle->getRuntimeInfo(runtimeInfo);
        if (status != GVFG_OK)
            return status;

        *out_info = runtimeInfo;
        return GVFG_OK;
    }

    const char *gvfg_get_version(void)
    {
        return GVFG_VERSION_STRING;
    }

    const char *gvfg_pixel_format_name(int pixel_format)
    {
        return pixel_format_name(pixel_format);
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

    gvfg_status_t gvfg_get_last_error_detail(gvfg_handle handle,
                                             char *out_message,
                                             uint32_t out_message_size)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->getLastErrorDetail(out_message, out_message_size);
    }

    gvfg_status_t gvfg_debug_get_backend_stats(gvfg_handle handle,
                                               gvfg_debug_backend_stats_t *out_stats)
    {
        if (!handle || !out_stats)
            return GVFG_EINVAL;

        gvfg_debug_backend_stats_t stats{};
        const gvfg_status_t status = handle->getDebugBackendStats(stats);
        if (status != GVFG_OK)
            return status;

        *out_stats = stats;
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

    gvfg_status_t gvfg_debug_read_register(gvfg_handle handle,
                                           uint32_t offset,
                                           uint32_t *out_value)
    {
        if (!handle || !out_value || (offset & 0x3u) != 0)
            return GVFG_EINVAL;
        return handle->debugReadRegister(offset, *out_value);
    }

    gvfg_status_t gvfg_debug_write_register(gvfg_handle handle,
                                            uint32_t offset,
                                            uint32_t value)
    {
        if (!handle || (offset & 0x3u) != 0)
            return GVFG_EINVAL;
        return handle->debugWriteRegister(offset, value);
    }
}



