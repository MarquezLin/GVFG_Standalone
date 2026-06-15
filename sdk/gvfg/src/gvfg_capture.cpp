#include "gvfg_capture.h"

#include "gvfg_debug.h"
#include "xdma_capture_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
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

    gvfg_status_t map_status(xdma_status_t st)
    {
        switch (st)
        {
        case XDMA_OK:
            return GVFG_OK;
        case XDMA_EINVAL:
            return GVFG_EINVAL;
        case XDMA_ENODEV:
            return GVFG_ENODEV;
        case XDMA_ESTATE:
            return GVFG_ESTATE;
        case XDMA_ETIMEOUT:
            return GVFG_ETIMEOUT;
        case XDMA_ENOTSUP:
            return GVFG_ENOTSUP;
        case XDMA_EIO:
        default:
            return GVFG_EIO;
        }
    }

    const char *xdma_status_text(xdma_status_t st)
    {
        switch (st)
        {
        case XDMA_OK:
            return "ok";
        case XDMA_EINVAL:
            return "invalid argument";
        case XDMA_ENODEV:
            return "device not found";
        case XDMA_ESTATE:
            return "invalid state";
        case XDMA_ENOTSUP:
            return "not supported";
        case XDMA_ETIMEOUT:
            return "timeout";
        case XDMA_EIO:
            return "i/o error";
        default:
            return "unknown";
        }
    }

    const char *xdma_error_text(xdma_status_t st, const gvfg::internal::XdmaCaptureSession *session)
    {
        const char *detail = session ? session->last_error() : nullptr;
        if (detail && detail[0])
            return detail;
        return xdma_status_text(st);
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

    gvfg_event_type_t map_event_type(xdma_event_type_t type)
    {
        switch (type)
        {
        case XDMA_EVENT_PLUG_IN:
            return GVFG_EVENT_PLUG_IN;
        case XDMA_EVENT_PLUG_OUT:
            return GVFG_EVENT_PLUG_OUT;
        case XDMA_EVENT_CAPTURE_PAUSED:
            return GVFG_EVENT_CAPTURE_PAUSED;
        case XDMA_EVENT_CAPTURE_RESUMED:
            return GVFG_EVENT_CAPTURE_RESUMED;
        default:
            return GVFG_EVENT_UNKNOWN;
        }
    }

    bool fpga_field_valid(uint32_t mask, int bit)
    {
        return (mask & (1u << bit)) != 0;
    }

    int to_gvfg_pixel_format(xdma_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case XDMA_PIXFMT_YUY2:
            return GVFG_PIXFMT_YUY2;
        case XDMA_PIXFMT_UYVY:
            return GVFG_PIXFMT_UYVY;
        case XDMA_PIXFMT_RGB24:
            return GVFG_PIXFMT_RGB24;
        case XDMA_PIXFMT_BGRX32:
            return GVFG_PIXFMT_BGRX32;
        case XDMA_PIXFMT_NV12:
            return GVFG_PIXFMT_NV12;
        case XDMA_PIXFMT_P010:
            return GVFG_PIXFMT_P010;
        case XDMA_PIXFMT_Y210:
            return GVFG_PIXFMT_Y210;
        case XDMA_PIXFMT_YUV444:
            return GVFG_PIXFMT_YUV444;
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
        case GVFG_PIXFMT_BGRA8:
            return "BGRA8";
        default:
            return "UNKNOWN";
        }
    }

    const char *fpga_video_format_name(uint32_t value)
    {
        switch (value & 0x3u)
        {
        case 0:
            return "YUV422";
        case 1:
            return "RGB";
        case 2:
            return "YUV444";
        case 3:
            return "YUV420";
        default:
            return "UNKNOWN";
        }
    }

    const char *fpga_frame_rate_name(uint32_t value)
    {
        switch (value & 0x0fu)
        {
        case 0x0:
            return "None";
        case 0x2:
            return "23.98";
        case 0x3:
            return "24";
        case 0x4:
            return "47.95";
        case 0x5:
            return "25";
        case 0x6:
            return "29.97";
        case 0x7:
            return "30";
        case 0x8:
            return "48";
        case 0x9:
            return "50";
        case 0xa:
            return "59.94";
        case 0xb:
            return "60";
        default:
            return "--";
        }
    }

}

struct gvfg_handle_t
{
    ~gvfg_handle_t()
    {
        close();
    }

    gvfg_status_t open(int index)
    {
        if (index < 0)
            return GVFG_EINVAL;

        close();

        backend = std::make_unique<gvfg::internal::XdmaCaptureSession>();
        const xdma_status_t stOpen = backend->open_device_index(static_cast<size_t>(index));
        if (stOpen != XDMA_OK)
        {
            recordError(xdma_error_text(stOpen, backend.get()));
            backend.reset();
            return map_status(stOpen);
        }

        currentIndex = index;
        syncBackendEventCallback();
        selectedInput = XDMA_INPUT_SDI;
        resetRuntimeCounters();
        const xdma_status_t stInput = backend->set_input(selectedInput);
        if (stInput != XDMA_OK)
        {
            recordError(xdma_error_text(stInput, backend.get()));
            close();
            return map_status(stInput);
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
        const xdma_status_t st = backend->start_stream();
        if (st != XDMA_OK)
        {
            recordError(xdma_error_text(st, backend.get()));
            return map_status(st);
        }

        running = true;
        return GVFG_OK;
    }

    gvfg_status_t stop()
    {
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
        querySignal();
        bool haveSignalSize = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            haveSignalSize = fpgaWidthValid && fpgaHeightValid && fpgaWidthRaw != 0 && fpgaHeightRaw != 0;
            const bool videoFormatValid = fpga_field_valid(fpgaValidMask, 0);
            const bool frameRateValid = fpga_field_valid(fpgaValidMask, 1);
            const bool bitDepthValid = fpga_field_valid(fpgaValidMask, 2);
            const bool statusValid = fpga_field_valid(fpgaValidMask, 3);
            out.width = haveSignalSize ? static_cast<int>(fpgaWidthRaw) : 0;
            out.height = haveSignalSize ? static_cast<int>(fpgaHeightRaw) : 0;
            copy_cstr(out.video_format,
                      sizeof(out.video_format),
                      videoFormatValid ? fpga_video_format_name(fpgaVideoFormatRaw) : "--");
            copy_cstr(out.frame_rate_name,
                      sizeof(out.frame_rate_name),
                      frameRateValid ? fpga_frame_rate_name(fpgaFrameRateRaw) : "--");
            out.bit_depth = bitDepthValid ? static_cast<int>(fpgaBitDepthRaw) : 0;
            out.sdi_locked = statusValid && (fpgaStatusRaw & (1u << 0)) ? 1 : 0;
            out.hdmi_locked = statusValid && (fpgaStatusRaw & (1u << 2)) ? 1 : 0;
        }
        return haveSignalSize ? GVFG_OK : GVFG_ENODEV;
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
                                    XDMA_EVENT_MASK_DEFAULT);
    }

    static void onBackendEvent(const xdma_event_t *event, void *user)
    {
        auto *self = static_cast<gvfg_handle_t *>(user);
        if (!self || !event)
            return;
        self->emitEvent(*event);
    }

    void emitEvent(const xdma_event_t &event)
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

    void querySignal()
    {
        if (!backend)
            return;

        xdma_signal_status_t sig{};
        if (backend->get_signal_status(sig) != XDMA_OK)
            return;

        std::lock_guard<std::mutex> lock(stateMutex);
        if (sig.width > 0)
            width = sig.width;
        if (sig.height > 0)
            height = sig.height;
        if (sig.bit_depth > 0)
            bitDepth = sig.bit_depth;
        pixelFormat = sig.pixel_format != XDMA_PIXFMT_UNKNOWN ? sig.pixel_format : XDMA_PIXFMT_YUY2;
        fpgaValidMask = sig.fpga_valid_mask;
        fpgaWidthValid = sig.fpga_width_valid != 0;
        fpgaHeightValid = sig.fpga_height_valid != 0;
        fpgaWidthRaw = sig.fpga_width_raw;
        fpgaHeightRaw = sig.fpga_height_raw;
        fpgaVideoFormatRaw = sig.fpga_video_format_raw;
        fpgaFrameRateRaw = sig.fpga_frame_rate_raw;
        fpgaBitDepthRaw = sig.fpga_bit_depth_raw;
        fpgaStatusRaw = sig.fpga_status_raw;
    }

    static const char *inputName(xdma_input_t input)
    {
        switch (input)
        {
        case XDMA_INPUT_HDMI:
            return "HDMI";
        case XDMA_INPUT_SDI:
            return "SDI";
        default:
            return "unknown";
        }
    }

    gvfg_status_t validateSelectedInputReady()
    {
        const bool statusValid = fpga_field_valid(fpgaValidMask, 3);
        const bool sdiLocked = (fpgaStatusRaw & (1u << 0)) != 0;
        const bool sdiDdrOk = (fpgaStatusRaw & (1u << 1)) != 0;
        const bool hdmiLocked = (fpgaStatusRaw & (1u << 2)) != 0;
        const bool hdmiDdrOk = (fpgaStatusRaw & (1u << 3)) != 0;
        const bool selectedReady = (selectedInput == XDMA_INPUT_HDMI) ? (hdmiLocked && hdmiDdrOk)
                                                                         : (sdiLocked && sdiDdrOk);

        if (!statusValid || !selectedReady)
        {
            char msg[256] = {};
            std::snprintf(msg,
                          sizeof(msg),
                          "%s input not ready; FPGA status valid=%d raw=0x%08x sdi_lock=%d sdi_ddr=%d hdmi_lock=%d hdmi_ddr=%d",
                          inputName(selectedInput),
                          statusValid ? 1 : 0,
                          fpgaStatusRaw,
                          sdiLocked ? 1 : 0,
                          sdiDdrOk ? 1 : 0,
                          hdmiLocked ? 1 : 0,
                          hdmiDdrOk ? 1 : 0);
            recordError(msg);
            return GVFG_ENODEV;
        }

        if (!fpgaWidthValid || !fpgaHeightValid || fpgaWidthRaw == 0 || fpgaHeightRaw == 0)
        {
            char msg[192] = {};
            std::snprintf(msg,
                          sizeof(msg),
                          "%s input has no valid FPGA resolution; width_valid=%d width=%u height_valid=%d height=%u",
                          inputName(selectedInput),
                          fpgaWidthValid ? 1 : 0,
                          fpgaWidthRaw,
                          fpgaHeightValid ? 1 : 0,
                          fpgaHeightRaw);
            recordError(msg);
            return GVFG_ENODEV;
        }

        return GVFG_OK;
    }

    gvfg_status_t configureStream()
    {
        if (!backend)
            return GVFG_ESTATE;

        querySignal();
        const gvfg_status_t inputReady = validateSelectedInputReady();
        if (inputReady != GVFG_OK)
            return inputReady;

        if (width == 0)
            width = 1920;
        if (height == 0)
            height = 1080;

        xdma_stream_desc_t desc{};
        desc.input = selectedInput;
        desc.width = width;
        desc.height = height;
        desc.pixel_format = pixelFormat;
        desc.buffer_count = 3;

        const xdma_status_t st = backend->configure_stream(desc);
        if (st != XDMA_OK)
        {
            recordError(xdma_error_text(st, backend.get()));
            return map_status(st);
        }
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

        xdma_frame_t frame{};
        const xdma_status_t st = backend->wait_frame(timeoutMs, frame);
        if (st != XDMA_OK)
        {
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                readInProgress = false;
            }
            if (st != XDMA_ETIMEOUT && st != XDMA_ESTATE)
                recordError(xdma_error_text(st, backend.get()));
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

    gvfg_status_t releaseFrame(const gvfg_frame_t &)
    {
        if (!backend)
            return GVFG_ESTATE;

        xdma_frame_t frame{};
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (!frameHeld)
                return GVFG_ESTATE;
            frame = heldBackendFrame;
            heldBackendFrame = {};
            frameHeld = false;
        }

        return map_status(backend->release_frame(frame));
    }

    void releaseHeldFrameForStop()
    {
        if (!backend)
            return;

        xdma_frame_t frame{};
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
            xdma_stream_stats_t stats{};
            uint64_t waitTimeouts = 0;
            backend->get_debug_stats(stats, waitTimeouts);
            out.backend_state = static_cast<int>(stats.state);
            out.backend_frames_captured = stats.frames_captured;
            out.backend_frames_delivered = stats.frames_delivered;
            out.backend_frames_dropped = stats.frames_dropped;
            out.backend_dma_errors = stats.dma_errors;
            out.backend_interrupt_count = stats.interrupt_count;
            out.backend_wait_timeouts = waitTimeouts;
        }

        return GVFG_OK;
    }

    gvfg_status_t getDebugFpgaSignalRaw(gvfg_debug_fpga_signal_raw_t &out)
    {
        if (!backend)
            return GVFG_ESTATE;

        querySignal();
        std::lock_guard<std::mutex> lock(stateMutex);
        std::memset(&out, 0, sizeof(out));
        out.valid_mask = fpgaValidMask;
        out.width_valid = fpgaWidthValid ? 1u : 0u;
        out.height_valid = fpgaHeightValid ? 1u : 0u;
        out.width_raw = fpgaWidthRaw;
        out.height_raw = fpgaHeightRaw;
        out.video_format_raw = fpgaVideoFormatRaw;
        out.frame_rate_raw = fpgaFrameRateRaw;
        out.bit_depth_raw = fpgaBitDepthRaw;
        out.status_raw = fpgaStatusRaw;
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

    std::unique_ptr<gvfg::internal::XdmaCaptureSession> backend;
    int currentIndex = -1;
    xdma_input_t selectedInput = XDMA_INPUT_SDI;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitDepth = 8;
    xdma_pixel_format_t pixelFormat = XDMA_PIXFMT_YUY2;
    mutable std::mutex stateMutex;
    uint32_t fpgaValidMask = 0;
    bool fpgaWidthValid = false;
    bool fpgaHeightValid = false;
    uint32_t fpgaWidthRaw = 0;
    uint32_t fpgaHeightRaw = 0;
    uint32_t fpgaVideoFormatRaw = 0;
    uint32_t fpgaFrameRateRaw = 0;
    uint32_t fpgaBitDepthRaw = 0;
    uint32_t fpgaStatusRaw = 0;
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
    xdma_frame_t heldBackendFrame{};
    std::mutex eventMutex;
    std::condition_variable eventCv;
    std::deque<gvfg_event_t> eventQueue;

};

extern "C"
{
    int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices)
    {
        const std::vector<gvfg::internal::XdmaDevice> devices = gvfg::internal::enumerate_xdma_devices();
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
        delete handle;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_open(gvfg_handle handle, int device_index)
    {
        if (!handle)
            return GVFG_EINVAL;
        return handle->open(device_index);
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
        return handle->getDebugBackendStats(*out_stats);
    }

    gvfg_status_t gvfg_debug_get_fpga_signal_raw(gvfg_handle handle,
                                                 gvfg_debug_fpga_signal_raw_t *out_raw)
    {
        if (!handle || !out_raw)
            return GVFG_EINVAL;
        return handle->getDebugFpgaSignalRaw(*out_raw);
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



