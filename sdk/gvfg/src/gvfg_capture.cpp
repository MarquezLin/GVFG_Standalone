#include "gvfg_capture.h"

#include "gvfg_debug.h"
#include "pcies2mm_capture_session.h"

#include <algorithm>
#include <array>
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
    static_assert(GVFG_EVENT_MASK_SIGNAL_CONNECTED == PCIES2MM_EVENT_MASK_PLUG_IN);
    static_assert(GVFG_EVENT_MASK_SIGNAL_DISCONNECTED == PCIES2MM_EVENT_MASK_PLUG_OUT);
    static_assert(GVFG_EVENT_MASK_STREAM_READY == PCIES2MM_EVENT_MASK_STREAM_READY);
    static_assert(GVFG_EVENT_MASK_FORMAT_CHANGE_BEGIN == PCIES2MM_EVENT_MASK_FORMAT_CHANGE_BEGIN);

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

struct gvfg_channel_session_t
{
    ~gvfg_channel_session_t()
    {
        close();
    }

    gvfg_status_t open(int index,
                       int channelIndex = GVFG_CHANNEL_0,
                       gvfg::internal::PcieS2mmCaptureSession *sharedDeviceSource = nullptr)
    {
        if (index < 0 || (channelIndex != GVFG_CHANNEL_0 && channelIndex != GVFG_CHANNEL_1))
            return GVFG_EINVAL;
        close();

        backend = std::make_unique<gvfg::internal::PcieS2mmCaptureSession>();
        const pcies2mm_status_t stOpen = sharedDeviceSource
                                             ? backend->open_shared_device(*sharedDeviceSource,
                                                                           static_cast<uint32_t>(channelIndex))
                                             : backend->open_device_index(static_cast<size_t>(index));
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

        if (zeroCopyRequested)
        {
            const pcies2mm_status_t stZeroCopy = backend->set_zero_copy_enabled(true);
            if (stZeroCopy != PCIES2MM_OK)
            {
                recordError(pcies2mm_error_text(stZeroCopy, backend.get()));
                close();
                return map_status(stZeroCopy);
            }
        }

        // querySignal();
        return GVFG_OK;
    }

    gvfg_status_t setZeroCopyEnabled(bool enabled)
    {
        if (backend)
            return GVFG_ESTATE;
        zeroCopyRequested = enabled;
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
        releaseHeldFrameForStop();

        pcies2mm_status_t st = PCIES2MM_OK;
        if (backend)
        {
            // stop_stream() joins the backend event thread, so no new event can
            // be queued after it returns.
            st = backend->stop_stream();
            if (st != PCIES2MM_OK)
                recordError(pcies2mm_error_text(st, backend.get()));
        }

        {
            std::lock_guard<std::mutex> lock(eventMutex);
            eventQueue.clear();
        }
        eventCv.notify_all();
        return map_status(st);
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
        return map_status(status);
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
        return GVFG_OK;
    }

    void syncBackendEventCallback()
    {
        if (!backend)
            return;
        backend->set_event_callback(&gvfg_channel_session_t::onBackendEvent,
                                    this,
                                    eventMask);
    }

    static void onBackendEvent(pcies2mm_event_type_t event, void *user)
    {
        auto *self = static_cast<gvfg_channel_session_t *>(user);
        if (!self)
            return;
        self->emitEvent(event);
    }

    void emitEvent(pcies2mm_event_type_t event)
    {
        const gvfg_event_type_t type = map_event_type(event);
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            if (eventQueue.size() >= 64)
                eventQueue.pop_front();
            gvfg_event_t out{};
            out.struct_size = sizeof(out);
            out.type = type;
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
        if (signalStatus != PCIES2MM_OK)
            return map_status(signalStatus);

        bool waitingForSignal = false;
        uint32_t configureWidth = 0;
        uint32_t configureHeight = 0;
        pcies2mm_pixel_format_t configureFormat = PCIES2MM_PIXFMT_UNKNOWN;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            waitingForSignal = !signalConnected;
            configureWidth = waitingForSignal ? 2 : width;
            configureHeight = waitingForSignal ? 1 : height;
            configureFormat =
                waitingForSignal || pixelFormat == PCIES2MM_PIXFMT_UNKNOWN ? PCIES2MM_PIXFMT_YUY2 : pixelFormat;
        }

        // configure_stream() is also used to enter event-monitoring mode. Keep
        // its inactive placeholder buffer minimal; the real signal descriptor
        // replaces it before DMA is enabled after reconnect.

        pcies2mm_stream_desc_t desc{};
        desc.width = configureWidth;
        desc.height = configureHeight;
        desc.pixel_format = configureFormat;
        desc.buffer_count = 1;

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
        if (!backend)
            return GVFG_ESTATE;

        std::unique_lock<std::mutex> lock(eventMutex);
        const auto eventAvailableOrStopped = [this]()
        {
            return !eventQueue.empty() ||
                   !running.load(std::memory_order_acquire);
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

        if (eventQueue.empty())
            return running.load(std::memory_order_acquire) ? GVFG_ETIMEOUT
                                                           : GVFG_ESTATE;

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
            out.backend_dma_errors = stats.dma_errors;
            out.backend_interrupt_count = stats.interrupt_count;
            out.backend_wait_timeouts = waitTimeouts;
            out.backend_running = debugState.running;
            out.backend_capture_active = debugState.capture_active;
            out.backend_latest_sequence = debugState.latest_sequence;
            out.backend_delivered_sequence = debugState.delivered_sequence;
            out.get_frame_zero_copy = debugState.get_frame_zero_copy;
            out.get_frame_timing_samples = debugState.get_frame_timing_samples;
            out.get_frame_timing_average_us = debugState.get_frame_timing_average_us;
            out.get_frame_timing_max300_us = debugState.get_frame_timing_max300_us;
            out.get_frame_timing_max_us = debugState.get_frame_timing_max_us;
            out.event_wait_timing_samples = debugState.event_wait_timing_samples;
            out.event_wait_timing_average_us = debugState.event_wait_timing_average_us;
            out.event_wait_timing_max300_us = debugState.event_wait_timing_max300_us;
            out.event_wait_timing_max_us = debugState.event_wait_timing_max_us;
            out.sdk_processing_timing_samples = debugState.sdk_processing_timing_samples;
            out.sdk_processing_timing_average_us = debugState.sdk_processing_timing_average_us;
            out.sdk_processing_timing_max300_us = debugState.sdk_processing_timing_max300_us;
            out.sdk_processing_timing_max_us = debugState.sdk_processing_timing_max_us;
        }

        return GVFG_OK;
    }

    gvfg_status_t debugReadRegister(uint32_t offset, uint32_t &outValue)
    {
        if (!backend)
            return GVFG_ESTATE;
        return map_status(backend->debug_read_register(offset, outValue));
    }

    gvfg_status_t setVideoFormat(gvfg_pixel_format_t format)
    {
        if (!backend)
            return GVFG_ESTATE;

        pcies2mm_pixel_format_t backendFormat = PCIES2MM_PIXFMT_UNKNOWN;
        if (format == GVFG_PIXFMT_YUY2)
            backendFormat = PCIES2MM_PIXFMT_YUY2;
        else if (format == GVFG_PIXFMT_Y210)
            backendFormat = PCIES2MM_PIXFMT_Y210;
        else
            return GVFG_EINVAL;

        const pcies2mm_status_t status = backend->set_video_format(backendFormat);
        if (status != PCIES2MM_OK)
            recordError(pcies2mm_error_text(status, backend.get()));
        return map_status(status);
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
        constexpr uint64_t kFpsWindowNs = 1000000000ULL;
        const uint64_t windowStart = fpsWindowStartNs.load(std::memory_order_relaxed);
        if (windowStart == 0 || ptsNs <= windowStart)
        {
            fpsWindowStartNs.store(ptsNs, std::memory_order_relaxed);
            fpsWindowFrameCount.store(0, std::memory_order_relaxed);
            return;
        }

        const uint64_t frameCount = fpsWindowFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t elapsedNs = ptsNs - windowStart;
        if (elapsedNs < kFpsWindowNs)
            return;

        runtimeFps.store(static_cast<double>(frameCount) * 1e9 /
                             static_cast<double>(elapsedNs),
                         std::memory_order_relaxed);
        fpsWindowStartNs.store(ptsNs, std::memory_order_relaxed);
        fpsWindowFrameCount.store(0, std::memory_order_relaxed);
    }

    void resetRuntimeCounters()
    {
        fpsWindowStartNs.store(0, std::memory_order_relaxed);
        fpsWindowFrameCount.store(0, std::memory_order_relaxed);
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
    bool zeroCopyRequested = false;
    uint32_t eventMask = GVFG_EVENT_MASK_ALL;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitDepth = 0;
    pcies2mm_pixel_format_t pixelFormat = PCIES2MM_PIXFMT_UNKNOWN;
    bool signalConnected = false;
    mutable std::mutex stateMutex;
    std::atomic<uint64_t> fpsWindowStartNs{0};
    std::atomic<uint64_t> fpsWindowFrameCount{0};
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

struct gvfg_handle_t
{
    gvfg_channel_session_t *findChannel(int channelIndex)
    {
        if (channelIndex != GVFG_CHANNEL_0 && channelIndex != GVFG_CHANNEL_1)
            return nullptr;
        return channels[static_cast<size_t>(channelIndex)].get();
    }

    gvfg_channel_session_t *firstOpenChannel()
    {
        if (channels[GVFG_CHANNEL_0])
            return channels[GVFG_CHANNEL_0].get();
        return channels[GVFG_CHANNEL_1].get();
    }

    gvfg_status_t openChannel(int index, int channelIndex)
    {
        if (index < 0 || (channelIndex != GVFG_CHANNEL_0 && channelIndex != GVFG_CHANNEL_1))
            return GVFG_EINVAL;
        if (currentIndex >= 0 && currentIndex != index)
            return GVFG_ESTATE;

        const size_t slot = static_cast<size_t>(channelIndex);
        if (channels[slot])
            return GVFG_OK;

        auto channel = std::make_unique<gvfg_channel_session_t>();
        channel->zeroCopyRequested = zeroCopyRequested;
        channel->eventMask = eventMasks[slot];

        gvfg::internal::PcieS2mmCaptureSession *sharedSource = nullptr;
        if (gvfg_channel_session_t *existing = firstOpenChannel())
            sharedSource = existing->backend.get();

        const gvfg_status_t status = channel->open(index, channelIndex, sharedSource);
        if (status != GVFG_OK)
            return status;

        channels[slot] = std::move(channel);
        currentIndex = index;
        return GVFG_OK;
    }

    gvfg_status_t setZeroCopyEnabled(bool enabled)
    {
        if (firstOpenChannel())
            return GVFG_ESTATE;
        zeroCopyRequested = enabled;
        return GVFG_OK;
    }

    gvfg_status_t setChannelEventMask(int channelIndex, uint32_t eventMask)
    {
        if (channelIndex != GVFG_CHANNEL_0 && channelIndex != GVFG_CHANNEL_1)
            return GVFG_EINVAL;
        if ((eventMask & ~static_cast<uint32_t>(GVFG_EVENT_MASK_ALL)) != 0)
            return GVFG_EINVAL;
        const size_t slot = static_cast<size_t>(channelIndex);
        if (channels[slot])
            return GVFG_ESTATE;
        eventMasks[slot] = eventMask;
        return GVFG_OK;
    }

    gvfg_status_t stopAll()
    {
        gvfg_status_t result = GVFG_OK;
        for (const auto &channel : channels)
        {
            if (!channel)
                continue;
            const gvfg_status_t status = channel->stop();
            if (result == GVFG_OK)
                result = status;
        }
        return result;
    }

    gvfg_status_t debugReadRegister(uint32_t offset, uint32_t &outValue)
    {
        gvfg_channel_session_t *channel = firstOpenChannel();
        return channel ? channel->debugReadRegister(offset, outValue) : GVFG_ESTATE;
    }

    gvfg_status_t debugWriteRegister(uint32_t offset, uint32_t value)
    {
        gvfg_channel_session_t *channel = firstOpenChannel();
        return channel ? channel->debugWriteRegister(offset, value) : GVFG_ESTATE;
    }

    std::array<std::unique_ptr<gvfg_channel_session_t>, 2> channels;
    std::array<uint32_t, 2> eventMasks{GVFG_EVENT_MASK_ALL, GVFG_EVENT_MASK_ALL};
    int currentIndex = -1;
    bool zeroCopyRequested = false;
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
        return handle->openChannel(device_index, channel_index);
    }

    gvfg_status_t gvfg_set_channel_event_mask(gvfg_handle handle,
                                               int channel_index,
                                               uint32_t event_mask)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->setChannelEventMask(channel_index, event_mask);
    }

    gvfg_status_t gvfg_get_channel_event_mask(gvfg_handle handle,
                                               int channel_index,
                                               uint32_t *out_event_mask)
    {
        if (!handle || !out_event_mask ||
            (channel_index != GVFG_CHANNEL_0 && channel_index != GVFG_CHANNEL_1))
            return GVFG_EINVAL;
        *out_event_mask = handle->eventMasks[static_cast<size_t>(channel_index)];
        return GVFG_OK;
    }

    gvfg_status_t gvfg_set_zero_copy_enabled(gvfg_handle handle, int enabled)
    {
        if (!handle || (enabled != 0 && enabled != 1))
            return GVFG_EINVAL;
        return handle->setZeroCopyEnabled(enabled != 0);
    }

    gvfg_status_t gvfg_get_zero_copy_enabled(gvfg_handle handle, int *out_enabled)
    {
        if (!handle || !out_enabled)
            return GVFG_EINVAL;
        *out_enabled = handle->zeroCopyRequested ? 1 : 0;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_start_channel(gvfg_handle handle, int channel_index)
    {
        if (!handle)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->start() : GVFG_ESTATE;
    }

    gvfg_status_t gvfg_set_channel_video_format(gvfg_handle handle,
                                                 int channel_index,
                                                 gvfg_pixel_format_t format)
    {
        if (!handle)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->setVideoFormat(format) : GVFG_ESTATE;
    }

    gvfg_status_t gvfg_read_channel_frame(gvfg_handle handle,
                                           int channel_index,
                                           gvfg_frame_t *out_frame,
                                           uint32_t timeout_ms)
    {
        if (!handle || !out_frame)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->readFrame(*out_frame, timeout_ms) : GVFG_ESTATE;
    }

    gvfg_status_t gvfg_release_channel_frame(gvfg_handle handle,
                                              int channel_index,
                                              const gvfg_frame_t *frame)
    {
        if (!handle || !frame)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->releaseFrame(*frame) : GVFG_ESTATE;
    }

    gvfg_status_t gvfg_poll_channel_event(gvfg_handle handle,
                                           int channel_index,
                                           gvfg_event_t *out_event,
                                           uint32_t timeout_ms)
    {
        if (!handle || !out_event || out_event->struct_size < sizeof(gvfg_event_t))
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        if (!channel)
            return GVFG_ESTATE;
        gvfg_event_t event{};
        event.struct_size = sizeof(event);
        const gvfg_status_t status = channel->pollEvent(event, timeout_ms);
        if (status == GVFG_OK)
            *out_event = event;
        return status;
    }

    gvfg_status_t gvfg_stop(gvfg_handle handle)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->stopAll();
    }

    gvfg_status_t gvfg_stop_channel(gvfg_handle handle, int channel_index)
    {
        if (!handle)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->stop() : GVFG_ESTATE;
    }

    gvfg_status_t gvfg_get_channel_signal_status(gvfg_handle handle,
                                                  int channel_index,
                                                  gvfg_signal_status_t *out_status)
    {
        if (!handle || !out_status)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        if (!channel)
            return GVFG_ESTATE;
        return channel->getSignalStatus(*out_status);
    }

    gvfg_status_t gvfg_get_channel_runtime_info(gvfg_handle handle,
                                                 int channel_index,
                                                 gvfg_runtime_info_t *out_info)
    {
        if (!handle || !out_info)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->getRuntimeInfo(*out_info) : GVFG_ESTATE;
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

    gvfg_status_t gvfg_get_channel_last_error_detail(gvfg_handle handle,
                                                     int channel_index,
                                                     char *out_message,
                                                     uint32_t out_message_size)
    {
        if (!handle)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->getLastErrorDetail(out_message, out_message_size) : GVFG_ESTATE;
    }

    gvfg_status_t gvfg_debug_get_channel_backend_stats(gvfg_handle handle,
                                                       int channel_index,
                                                       gvfg_debug_backend_stats_t *out_stats)
    {
        if (!handle || !out_stats)
            return GVFG_EINVAL;

        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        if (!channel)
            return GVFG_ESTATE;
        gvfg_debug_backend_stats_t stats{};
        const gvfg_status_t status = channel->getDebugBackendStats(stats);
        if (status != GVFG_OK)
            return status;

        *out_stats = stats;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_debug_get_channel_last_error_detail(gvfg_handle handle,
                                                           int channel_index,
                                                           char *out_message,
                                                           uint32_t out_message_size)
    {
        if (!handle)
            return GVFG_EINVAL;
        gvfg_channel_session_t *channel = handle->findChannel(channel_index);
        return channel ? channel->getLastErrorDetail(out_message, out_message_size) : GVFG_ESTATE;
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
