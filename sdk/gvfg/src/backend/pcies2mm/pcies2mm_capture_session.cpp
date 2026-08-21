#include "pcies2mm_capture_session.h"
#include "giga_ioctl.h"
#include "pcies2mm_reg.h"
#include "pcies2mm_video_format.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

namespace
{
    constexpr uint32_t kMaxChannels = 2;
    constexpr uint32_t kDefaultWidth = 1920;
    constexpr uint32_t kDefaultHeight = 1080;

    static bool is_write_only_register(uint32_t offset)
    {
        if (offset == VIDEO_OUTPUT_FORMAT_REGISTER)
            return true;

        for (uint32_t channel = 0; channel < kMaxChannels; ++channel)
        {
            const uint32_t videoBase = CH_VIDEO_BASE(channel);
            if (offset == videoBase + VIDEO_DMA_DESC_WR_OFFSET ||
                offset == videoBase + VIDEO_DMA_SOFT_RESET_OFFSET)
                return true;

            const uint32_t audioBase = CH_AUDIO_BASE(channel);
            if (offset == audioBase + AUDIO_DMA_DESC_WR_OFFSET ||
                offset == audioBase + AUDIO_DMA_SOFT_RESET_OFFSET)
                return true;
        }
        return false;
    }

    static bool is_transient_frame_not_ready_error(DWORD error)
    {
        return error == ERROR_NOT_READY ||
               error == ERROR_BUSY ||
               error == ERROR_RETRY ||
               error == ERROR_NO_MORE_ITEMS;
    }

    static std::string wide_to_utf8(const std::wstring &s)
    {
        if (s.empty())
            return std::string();
        const int needed = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return std::string();
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], needed, nullptr, nullptr);
        if (!out.empty() && out.back() == '\0')
            out.pop_back();
        return out;
    }

    static std::string win32_error(DWORD err)
    {
        char *msg = nullptr;
        const DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                             FORMAT_MESSAGE_FROM_SYSTEM |
                                             FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         err,
                                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                         reinterpret_cast<LPSTR>(&msg),
                                         0,
                                         nullptr);
        std::string out;
        if (len && msg)
        {
            out.assign(msg, msg + len);
            while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
                out.pop_back();
        }
        if (msg)
            LocalFree(msg);
        if (out.empty())
        {
            std::ostringstream oss;
            oss << "win32=" << err;
            out = oss.str();
        }
        return out;
    }

    static void trace_log(bool forceDebugOutput, const char *tag, const char *fmt, ...)
    {
#if GVFG_INTERNAL_DIAGNOSTICS
        (void)forceDebugOutput;
#else
        if (!forceDebugOutput)
            return;
#endif
        char msg[1024] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        char line[1200] = {};
        std::snprintf(line,
                      sizeof(line),
                      "[GVFG][PCIES2MM]%s %s\n",
                      tag ? tag : "",
                      msg);
        OutputDebugStringA(line);
    }

#define PCIES2MM_LOG(...) trace_log(false, "", __VA_ARGS__)
#define PCIES2MM_ERROR_LOG(...) trace_log(true, "[error]", __VA_ARGS__)

    static uint32_t event_mask_for_type(pcies2mm_event_type_t type)
    {
        switch (type)
        {
        case PCIES2MM_EVENT_PLUG_IN:
            return PCIES2MM_EVENT_MASK_PLUG_IN;
        case PCIES2MM_EVENT_PLUG_OUT:
            return PCIES2MM_EVENT_MASK_PLUG_OUT;
        case PCIES2MM_EVENT_STREAM_READY:
            return PCIES2MM_EVENT_MASK_STREAM_READY;
        case PCIES2MM_EVENT_FORMAT_CHANGE_BEGIN:
            return PCIES2MM_EVENT_MASK_FORMAT_CHANGE_BEGIN;
        default:
            return 0;
        }
    }

    static void reset_stats(pcies2mm_stream_stats_t &stats, pcies2mm_stream_state_t state)
    {
        std::memset(&stats, 0, sizeof(stats));
        stats.state = state;
    }
}

namespace gvfg::internal
{
    PcieS2mmCaptureSession::SharedDevice::~SharedDevice()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    PcieS2mmCaptureSession::PcieS2mmCaptureSession()
    {
        stream_desc_.width = kDefaultWidth;
        stream_desc_.height = kDefaultHeight;
        stream_desc_.pixel_format = PCIES2MM_PIXFMT_YUY2;
        stream_desc_.buffer_count = 1;
        stream_bit_depth_ = 8;
        reset_stats(stats_, PCIES2MM_STREAM_STOPPED);
    }

    PcieS2mmCaptureSession::~PcieS2mmCaptureSession()
    {
        close();
    }

    pcies2mm_status_t PcieS2mmCaptureSession::open_device_index(size_t deviceIndex)
    {
        const auto devices = enumerate_pcies2mm_devices();
        if (deviceIndex >= devices.size())
            return fail(PCIES2MM_ENODEV, "enumerate_pcies2mm_devices", ERROR_NOT_FOUND);
        return open_device(devices[deviceIndex]);
    }

    pcies2mm_status_t PcieS2mmCaptureSession::open_device(const PcieS2mmDevice &device)
    {
        close();
        base_path_ = device.interface_path;
        friendly_name_ = device.friendly_name.empty() ? L"PcieS2mm Capture Device" : device.friendly_name;

        auto sharedDevice = std::make_shared<SharedDevice>();
        sharedDevice->handle = CreateFileW(base_path_.c_str(),
                                           GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
        if (sharedDevice->handle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            close_handles();
            return fail(PCIES2MM_EIO, "CreateFile(PcieS2mm)", err);
        }
        shared_device_ = std::move(sharedDevice);
        device_ = shared_device_->handle;

        opened_ = true;
        configured_ = false;
        signal_presence_known_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_metadata_valid_ = false;
        clear_last_error();
        PCIES2MM_LOG("open_device: %s", wide_to_utf8(base_path_).c_str());
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::open_shared_device(
        const PcieS2mmCaptureSession &source,
        uint32_t channel)
    {
        if (channel >= kMaxChannels)
            return PCIES2MM_EINVAL;
        if (!source.opened_ || !source.shared_device_ ||
            source.shared_device_->handle == INVALID_HANDLE_VALUE)
            return PCIES2MM_ESTATE;

        close();
        shared_device_ = source.shared_device_;
        device_ = shared_device_->handle;
        base_path_ = source.base_path_;
        friendly_name_ = source.friendly_name_;
        opened_ = true;
        configured_ = false;
        signal_presence_known_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_metadata_valid_ = false;
        clear_last_error();
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::close()
    {
        pcies2mm_status_t result = stop_stream();
        stop_event_monitoring();
        if (zero_copy_enabled_ && device_ != INVALID_HANDLE_VALUE)
        {
            const pcies2mm_status_t disableStatus = set_zero_copy_enabled(false);
            if (result == PCIES2MM_OK)
                result = disableStatus;
        }
        close_handles();
        opened_ = false;
        configured_ = false;
        zero_copy_enabled_ = false;
        signal_presence_known_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_metadata_valid_ = false;
        base_path_.clear();
        friendly_name_.clear();
        reset_stats(stats_, PCIES2MM_STREAM_STOPPED);
        return result;
    }

    void PcieS2mmCaptureSession::close_handles()
    {
        close_event_handles();
        device_ = INVALID_HANDLE_VALUE;
        shared_device_.reset();
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_channel(uint32_t channel)
    {
        if (!opened_)
            return PCIES2MM_ESTATE;
        if (running_)
            return PCIES2MM_ESTATE;
        if (channel >= kMaxChannels)
            return PCIES2MM_EINVAL;
        channel_ = channel;
        signal_metadata_valid_ = false;
        if (!start_event_monitoring())
            return fail(PCIES2MM_EIO, "start event monitoring");
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_zero_copy_enabled(bool enabled)
    {
        if (!opened_ || device_ == INVALID_HANDLE_VALUE)
            return PCIES2MM_ESTATE;
        if (running_)
            return PCIES2MM_ESTATE;
        if (zero_copy_enabled_ == enabled)
            return PCIES2MM_OK;

        if (!enabled)
        {
            // Even while the stream is nominally stopped, an earlier failed
            // teardown may have left a driver frame held. No disable
            // IOCTL is allowed until every outstanding frame has been
            // returned successfully.
            const pcies2mm_status_t releaseStatus = release_all_zero_copy_frames(true);
            if (releaseStatus != PCIES2MM_OK)
                return releaseStatus;
            stop_event_monitoring();
        }

        if (!giga_ioctl_set_frame_zerocopy(device_, active_channel(), enabled ? TRUE : FALSE))
            return fail(PCIES2MM_EIO, enabled ? "enable zero copy" : "disable zero copy");
        zero_copy_enabled_ = enabled;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_video_format(pcies2mm_pixel_format_t format)
    {
        if (!opened_)
            return PCIES2MM_ESTATE;

        uint32_t value = 0;
        if (format == PCIES2MM_PIXFMT_Y210)
            value = 1;
        else if (format != PCIES2MM_PIXFMT_YUY2)
            return PCIES2MM_EINVAL;

        if (!write_reg(VIDEO_OUTPUT_FORMAT_REGISTER, value))
            return fail(PCIES2MM_EIO, "set_video_format");

        std::lock_guard<std::mutex> lock(mutex_);
        signal_metadata_valid_ = false;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::get_signal_status(pcies2mm_signal_status_t &out) const
    {
        if (!opened_)
            return PCIES2MM_ESTATE;

        std::memset(&out, 0, sizeof(out));
        out.channel = channel_;

        uint32_t modeLocked = 0;
        uint32_t rxTLocked = 0;
        if (!read_reg(video_base() + VIDEO_RX_MODE_LOCKED_OFFSET, modeLocked) ||
            !read_reg(video_base() + VIDEO_RX_T_LOCKED_OFFSET, rxTLocked))
            return fail(PCIES2MM_EIO, "read signal lock registers");

        const bool signalPresent = modeLocked == 1u && rxTLocked == 1u;
        signal_presence_known_.store(true, std::memory_order_release);
        signal_present_.store(signalPresent, std::memory_order_release);
        if (!signalPresent)
        {
            out.connected = 0;
            std::unique_lock<std::mutex> lock(mutex_);
            signal_metadata_valid_ = false;
            return PCIES2MM_OK;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (signal_metadata_valid_)
        {
            out = cached_signal_;
            return PCIES2MM_OK;
        }

        uint32_t rawWidth = 0;
        uint32_t rawHeight = 0;
        uint32_t rawFormat = 0;
        const bool widthOk = read_reg(video_base() + VIDEO_HSIZE_OFFSET, rawWidth);
        const bool heightOk = read_reg(video_base() + VIDEO_VSIZE_OFFSET, rawHeight);
        const bool formatOk = read_reg(video_base() + VIDEO_FORMAT_OFFSET, rawFormat);

        const bool haveRawSize = widthOk && heightOk && rawWidth != 0 && rawHeight != 0;
        pcies2mm_pixel_format_t fmt = formatOk ? decode_pixel_format(rawFormat) : PCIES2MM_PIXFMT_UNKNOWN;

        const uint32_t width = signalPresent && haveRawSize ? rawWidth : 0;
        const uint32_t height = signalPresent && haveRawSize ? rawHeight : 0;
        const uint32_t bitDepth = bit_depth_for_pixfmt(fmt);
        // Signal presence is defined only by the two receiver lock bits. Size
        // and format are metadata and must not participate in that decision.
        const bool connected = signalPresent;

        out.connected = connected ? 1 : 0;
        out.channel = channel_;
        out.width = width;
        out.height = height;
        out.pixel_format = fmt;
        out.bit_depth = bitDepth;
        cached_signal_ = out;
        signal_metadata_valid_ = widthOk && heightOk && formatOk && haveRawSize;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_event_callback(pcies2mm_event_callback_t callback, void *user, uint32_t eventMask)
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
        event_callback_ = callback;
        event_callback_user_ = user;
        event_mask_filter_ = eventMask;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::configure_stream(const pcies2mm_stream_desc_t &desc)
    {
        if (!opened_)
            return PCIES2MM_ESTATE;
        if (running_)
            return PCIES2MM_ESTATE;
        if (desc.width == 0 || desc.height == 0)
            return PCIES2MM_EINVAL;

        pcies2mm_pixel_format_t fmt = desc.pixel_format == PCIES2MM_PIXFMT_UNKNOWN ? PCIES2MM_PIXFMT_YUY2 : desc.pixel_format;
        switch (fmt)
        {
        case PCIES2MM_PIXFMT_YUY2:
        case PCIES2MM_PIXFMT_Y210:
            break;
        default:
            return fail(PCIES2MM_ENOTSUP, "configure_stream(pixel_format)", ERROR_NOT_SUPPORTED);
        }

        stream_desc_ = desc;
        stream_desc_.pixel_format = fmt;
        stream_desc_.buffer_count = 1;
        stream_bit_depth_ = bit_depth_for_pixfmt(fmt);
        configured_ = true;
        reset_stats(stats_, PCIES2MM_STREAM_CONFIGURED);
        clear_last_error();
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::start_stream()
    {
        if (!opened_ || !configured_)
            return PCIES2MM_ESTATE;
        if (running_)
            return PCIES2MM_OK;

        // Do not overwrite a retained token from an earlier failed release.
        // Retry it before rebuilding the buffer or restarting DMA.
        const pcies2mm_status_t pendingReleaseStatus = release_all_zero_copy_frames(true);
        if (pendingReleaseStatus != PCIES2MM_OK)
            return pendingReleaseStatus;

        const size_t bytes = frame_size_bytes();
        if (bytes == 0 || bytes > (std::numeric_limits<DWORD>::max)())
            return fail(PCIES2MM_EINVAL, "frame_size_bytes", ERROR_INVALID_PARAMETER);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy_buffer_.clear();
            if (!zero_copy_enabled_)
                copy_buffer_.assign(bytes, 0);
            held_frame_ = {};
            frame_held_ = false;
            read_in_progress_ = false;
            active_delivery_started_ = {};
            latest_sequence_ = 0;
            delivered_sequence_ = 0;
            wait_timeout_count_ = 0;
            get_frame_timing_samples_ = 0;
            get_frame_timing_window_samples_ = 0;
            get_frame_timing_total_us_ = 0.0;
            get_frame_timing_window_max_us_ = 0.0;
            get_frame_timing_last_max300_us_ = 0.0;
            get_frame_timing_lifetime_max_us_ = 0.0;
            event_wait_timing_samples_ = 0;
            event_wait_timing_window_samples_ = 0;
            event_wait_timing_total_us_ = 0.0;
            event_wait_timing_window_max_us_ = 0.0;
            event_wait_timing_last_max300_us_ = 0.0;
            event_wait_timing_lifetime_max_us_ = 0.0;
            sdk_processing_timing_samples_ = 0;
            sdk_processing_timing_window_samples_ = 0;
            sdk_processing_timing_total_us_ = 0.0;
            sdk_processing_timing_window_max_us_ = 0.0;
            sdk_processing_timing_last_max300_us_ = 0.0;
            sdk_processing_timing_lifetime_max_us_ = 0.0;
            stream_error_ = false;
            reset_stats(stats_, PCIES2MM_STREAM_CONFIGURED);
        }

        const uint32_t channel = active_channel();
        // stop_stream() joins the event thread so no zero-copy acquire can race
        // teardown. Recreate event monitoring when the same open handle starts
        // another stream.
        if (!monitoring_.load(std::memory_order_acquire) && !start_event_monitoring())
            return fail(PCIES2MM_EIO, "start event monitoring");
        // configureStream() immediately preceded this call and already read
        // and validated the signal descriptor. Reuse it for the initial DMA
        // probe instead of reading width/height/format a second time.
        const bool probeAvailable = signal_presence_known_.load(std::memory_order_acquire) &&
                                    signal_present_.load(std::memory_order_acquire) &&
                                    stream_desc_.width > 0 &&
                                    stream_desc_.height > 0 &&
                                    stream_desc_.pixel_format != PCIES2MM_PIXFMT_UNKNOWN;
        running_ = true;
        capture_active_ = false;
        reader_ready_ = false;
        signal_probe_active_ = probeAvailable;
        stream_ready_pending_ = false;
        PCIES2MM_LOG("start: channel=%u base=0x%x bytes=%zu probe_available=%d reader_deferred=1",
                     channel,
                     video_base(),
                     frame_size_bytes(),
                     probeAvailable ? 1 : 0);
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::stop_stream()
    {
        if (!running_)
        {
            const pcies2mm_status_t releaseStatus = release_all_zero_copy_frames(true);
            stop_event_monitoring();
            return releaseStatus;
        }

        running_ = false;
        capture_active_ = false;
        reader_ready_ = false;
        signal_probe_active_ = false;
        stream_ready_pending_ = false;

        if (device_ != INVALID_HANDLE_VALUE)
            stop_video(active_channel());

        if (dma_event_)
            SetEvent(dma_event_);

        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] { return !read_in_progress_; });
        }
        const pcies2mm_status_t releaseStatus = release_all_zero_copy_frames(true);
        stop_event_monitoring();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            held_frame_ = {};
            frame_held_ = false;
            stats_.state = configured_ ? PCIES2MM_STREAM_CONFIGURED : PCIES2MM_STREAM_STOPPED;
        }
        return releaseStatus;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::wait_frame(uint32_t timeoutMs, pcies2mm_frame_t &out)
    {
        reader_ready_.store(true, std::memory_order_release);
        std::memset(&out, 0, sizeof(out));
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_ || frame_held_ || read_in_progress_)
                return PCIES2MM_ESTATE;
            if (!capture_active_.load(std::memory_order_acquire))
            {
                if (!signal_presence_known_.load(std::memory_order_acquire) ||
                    !signal_present_.load(std::memory_order_acquire))
                    return PCIES2MM_ETIMEOUT;
                if (!ResetEvent(dma_event_))
                {
                    const DWORD err = GetLastError();
                    ++stats_.dma_errors;
                    lock.unlock();
                    return fail(PCIES2MM_EIO, "reset DMA event", err);
                }
                if (!start_video(active_channel()))
                {
                    const DWORD err = GetLastError();
                    ++stats_.dma_errors;
                    lock.unlock();
                    return fail(PCIES2MM_EIO, "enable reader capture", err);
                }
                capture_active_.store(true, std::memory_order_release);
                stream_ready_pending_.store(true, std::memory_order_release);
            }
            read_in_progress_ = true;
        }

        const auto finishRead = [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            read_in_progress_ = false;
            read_finished_cv_.notify_all();
        };

        const bool infiniteWait = timeoutMs == UINT32_MAX;
        const auto attemptStarted = std::chrono::steady_clock::now();
        const auto deadline = infiniteWait
                                  ? (std::chrono::steady_clock::time_point::max)()
                                  : attemptStarted + std::chrono::milliseconds(timeoutMs);
        const DWORD bytes = static_cast<DWORD>(frame_size_bytes());
        const uint8_t *data = nullptr;
        double eventWaitUs = 0.0;
        double getFrameUs = 0.0;
        for (;;)
        {
            DWORD waitMs = INFINITE;
            if (!infiniteWait)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    waitMs = 0;
                else
                {
                    const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
                    waitMs = static_cast<DWORD>((remainingUs + 999) / 1000);
                }
            }

            const auto eventWaitStarted = std::chrono::steady_clock::now();
            const DWORD waitResult = WaitForSingleObject(dma_event_, waitMs);
            const auto eventWaitFinished = std::chrono::steady_clock::now();
            eventWaitUs += std::chrono::duration<double, std::micro>(
                               eventWaitFinished - eventWaitStarted).count();
            if (waitResult == WAIT_TIMEOUT)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++wait_timeout_count_;
                }
                finishRead();
                return PCIES2MM_ETIMEOUT;
            }
            if (waitResult != WAIT_OBJECT_0)
            {
                const DWORD err = GetLastError();
                finishRead();
                return fail(PCIES2MM_EIO, "wait DMA event", err);
            }
            if (!running_.load(std::memory_order_acquire))
            {
                finishRead();
                return PCIES2MM_ESTATE;
            }
            if (!capture_active_.load(std::memory_order_acquire))
            {
                finishRead();
                return PCIES2MM_ETIMEOUT;
            }

            const auto getFrameStarted = std::chrono::steady_clock::now();
            int ret = -1;
            DWORD frameError = ERROR_SUCCESS;
            if (zero_copy_enabled_)
            {
                if (acquire_zero_copy_frame(active_channel(), data))
                    ret = static_cast<int>(bytes);
                else
                    frameError = GetLastError();
            }
            else
            {
                ret = get_frame(active_channel(), (std::numeric_limits<uint32_t>::max)(),
                                copy_buffer_.data(), bytes);
                if (ret < 0)
                    frameError = GetLastError();
                else if (static_cast<DWORD>(ret) != bytes)
                    frameError = ERROR_INVALID_DATA;
            }
            getFrameUs += std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - getFrameStarted).count();
            if (ret >= 0 && static_cast<DWORD>(ret) == bytes)
                break;

            if (!is_transient_frame_not_ready_error(frameError))
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.dma_errors;
                }
                finishRead();
                return fail(PCIES2MM_EIO,
                            zero_copy_enabled_ ? "zero-copy acquire frame" : "copy get frame",
                            frameError);
            }

            PCIES2MM_LOG("frame not ready after DMA event: channel=%u winerr=%lu; wait for next event",
                         active_channel(),
                         static_cast<unsigned long>(frameError));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            read_in_progress_ = false;
            read_finished_cv_.notify_all();
            if (!running_)
            {
                if (zero_copy_enabled_ && data)
                    release_zero_copy_frame(active_channel());
                return PCIES2MM_ESTATE;
            }
            record_get_frame_timing(getFrameUs);
            ++latest_sequence_;
            delivered_sequence_ = latest_sequence_;
            ++stats_.interrupt_count;
            ++stats_.frames_captured;
            ++stats_.frames_delivered;
            stats_.state = PCIES2MM_STREAM_RUNNING;
            signal_present_.store(true, std::memory_order_release);
            signal_presence_known_.store(true, std::memory_order_release);
            active_delivery_started_ = std::chrono::steady_clock::now();
            out.data = zero_copy_enabled_ ? data : copy_buffer_.data();
            out.data_size_bytes = bytes;
            out.frame_id = delivered_sequence_;
            out.width = stream_desc_.width;
            out.height = stream_desc_.height;
            out.pixel_format = stream_desc_.pixel_format;
            out.bit_depth = stream_bit_depth_;
            held_frame_ = out;
            frame_held_ = true;
            const double backendTotalUs = std::chrono::duration<double, std::micro>(
                                              std::chrono::steady_clock::now() - attemptStarted).count();
            record_event_wait_timing(eventWaitUs);
            record_sdk_processing_timing((std::max)(0.0, backendTotalUs - eventWaitUs - getFrameUs));
        }
        if (stream_ready_pending_.exchange(false, std::memory_order_acq_rel))
            emit_event(PCIES2MM_EVENT_STREAM_READY);
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::release_frame(const pcies2mm_frame_t &frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!frame_held_)
            return PCIES2MM_ESTATE;
        if (frame.data != held_frame_.data ||
            frame.data_size_bytes != held_frame_.data_size_bytes ||
            frame.frame_id != held_frame_.frame_id ||
            frame.width != held_frame_.width ||
            frame.height != held_frame_.height ||
            frame.pixel_format != held_frame_.pixel_format ||
            frame.bit_depth != held_frame_.bit_depth)
            return PCIES2MM_EINVAL;

        if (zero_copy_enabled_ && !release_zero_copy_frame(active_channel()))
        {
            const DWORD err = GetLastError();
            lock.unlock();
            return fail(PCIES2MM_EIO, "release zero-copy frame", err);
        }
        const double heldMs = active_delivery_started_.time_since_epoch().count() != 0
                                  ? std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - active_delivery_started_).count()
                                  : 0.0;
        if (heldMs >= 10.0)
            PCIES2MM_LOG("slow frame release: frame=%llu held_ms=%.3f",
                         static_cast<unsigned long long>(frame.frame_id), heldMs);
        active_delivery_started_ = {};
        held_frame_ = {};
        frame_held_ = false;
        read_finished_cv_.notify_all();
        return PCIES2MM_OK;
    }

    const char *PcieS2mmCaptureSession::last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_.c_str();
    }

    void PcieS2mmCaptureSession::get_debug_stats(pcies2mm_stream_stats_t &outStats,
                                             uint64_t &outWaitTimeouts,
                                             pcies2mm_debug_state_t &outDebugState) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outStats = stats_;
        outWaitTimeouts = wait_timeout_count_;
        std::memset(&outDebugState, 0, sizeof(outDebugState));
        outDebugState.running = running_.load(std::memory_order_relaxed) ? 1 : 0;
        outDebugState.capture_active = capture_active_.load(std::memory_order_relaxed) ? 1 : 0;
        outDebugState.latest_sequence = latest_sequence_;
        outDebugState.delivered_sequence = delivered_sequence_;
        outDebugState.get_frame_zero_copy = zero_copy_enabled_ ? 1 : 0;
        outDebugState.get_frame_timing_samples = get_frame_timing_samples_;
        outDebugState.get_frame_timing_average_us =
            get_frame_timing_samples_ > 0
                ? get_frame_timing_total_us_ / static_cast<double>(get_frame_timing_samples_)
                : 0.0;
        outDebugState.get_frame_timing_max300_us = get_frame_timing_last_max300_us_;
        outDebugState.get_frame_timing_max_us = get_frame_timing_lifetime_max_us_;
        outDebugState.event_wait_timing_samples = event_wait_timing_samples_;
        outDebugState.event_wait_timing_average_us =
            event_wait_timing_samples_ > 0
                ? event_wait_timing_total_us_ / static_cast<double>(event_wait_timing_samples_)
                : 0.0;
        outDebugState.event_wait_timing_max300_us = event_wait_timing_last_max300_us_;
        outDebugState.event_wait_timing_max_us = event_wait_timing_lifetime_max_us_;
        outDebugState.sdk_processing_timing_samples = sdk_processing_timing_samples_;
        outDebugState.sdk_processing_timing_average_us =
            sdk_processing_timing_samples_ > 0
                ? sdk_processing_timing_total_us_ / static_cast<double>(sdk_processing_timing_samples_)
                : 0.0;
        outDebugState.sdk_processing_timing_max300_us = sdk_processing_timing_last_max300_us_;
        outDebugState.sdk_processing_timing_max_us = sdk_processing_timing_lifetime_max_us_;
    }

    bool PcieS2mmCaptureSession::read_reg(uint32_t offset, uint32_t &out) const
    {
        if (is_write_only_register(offset))
            return false;
        return giga_ioctl_read_register(device_, offset, &out) != FALSE;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::debug_read_register(uint32_t offset,
                                                                  uint32_t &outValue) const
    {
        if (!opened_ || device_ == INVALID_HANDLE_VALUE)
            return PCIES2MM_ESTATE;
        if ((offset & 0x3u) != 0 || is_write_only_register(offset))
            return PCIES2MM_EINVAL;
        return read_reg(offset, outValue) ? PCIES2MM_OK : fail(PCIES2MM_EIO, "debug_read_register");
    }

    bool PcieS2mmCaptureSession::write_reg(uint32_t offset, uint32_t value) const
    {
        return giga_ioctl_write_register(device_, offset, value) != FALSE;
    }

    bool PcieS2mmCaptureSession::start_video(uint32_t channelIndex) const
    {
        return giga_ioctl_video_start(device_, channelIndex) != FALSE;
    }

    bool PcieS2mmCaptureSession::stop_video(uint32_t channelIndex) const
    {
        return giga_ioctl_video_stop(device_, channelIndex) != FALSE;
    }

    bool PcieS2mmCaptureSession::acquire_zero_copy_frame(uint32_t channelIndex,
                                                         const uint8_t *&outData) const
    {
        const void *buffer = nullptr;
        if (!giga_ioctl_acquire_video_frame_zerocopy(device_, channelIndex, &buffer))
            return false;
        outData = static_cast<const uint8_t *>(buffer);
        return true;
    }

    bool PcieS2mmCaptureSession::release_zero_copy_frame(uint32_t channelIndex) const
    {
        return giga_ioctl_release_video_frame(device_, channelIndex) != FALSE;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::release_all_zero_copy_frames(bool includeInUse)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!frame_held_ || !includeInUse)
            return PCIES2MM_OK;
        if (zero_copy_enabled_ && held_frame_.data && !release_zero_copy_frame(active_channel()))
        {
            const DWORD err = GetLastError();
            lock.unlock();
            return fail(PCIES2MM_EIO, "release pending zero-copy frame", err);
        }
        held_frame_ = {};
        frame_held_ = false;
        read_finished_cv_.notify_all();
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::debug_write_register(uint32_t offset,
                                                                   uint32_t value) const
    {
        if (!opened_ || device_ == INVALID_HANDLE_VALUE)
            return PCIES2MM_ESTATE;
        if ((offset & 0x3u) != 0)
            return PCIES2MM_EINVAL;
        return write_reg(offset, value) ? PCIES2MM_OK : fail(PCIES2MM_EIO, "debug_write_register");
    }

    bool PcieS2mmCaptureSession::register_event(uint32_t channelIndex, uint32_t eventType, HANDLE eventHandle)
    {
        return giga_ioctl_register_event(device_, channelIndex, eventType, eventHandle) != FALSE;
    }

    void PcieS2mmCaptureSession::unregister_event(uint32_t channelIndex, uint32_t eventType)
    {
        giga_ioctl_unregister_event(device_, channelIndex, eventType);
    }

    bool PcieS2mmCaptureSession::create_and_register_events(uint32_t channelIndex)
    {
        dma_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        format_change_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        plug_in_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        plug_out_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!dma_event_ || !format_change_event_ || !plug_in_event_ || !plug_out_event_)
            return false;

        if (!register_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_DMA, dma_event_))
            return false;
        if ((event_mask_filter_ & PCIES2MM_EVENT_MASK_FORMAT_CHANGE_BEGIN) != 0 &&
            !register_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_FORMAT_CHANGE, format_change_event_))
            return false;
        if ((event_mask_filter_ & PCIES2MM_EVENT_MASK_PLUG_IN) != 0 &&
            !register_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_PLUGIN, plug_in_event_))
            return false;
        if ((event_mask_filter_ & PCIES2MM_EVENT_MASK_PLUG_OUT) != 0 &&
            !register_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_UNPLUG, plug_out_event_))
            return false;
        return true;
    }

    void PcieS2mmCaptureSession::unregister_events(uint32_t channelIndex)
    {
        unregister_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_DMA);
        if ((event_mask_filter_ & PCIES2MM_EVENT_MASK_FORMAT_CHANGE_BEGIN) != 0)
            unregister_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_FORMAT_CHANGE);
        if ((event_mask_filter_ & PCIES2MM_EVENT_MASK_PLUG_IN) != 0)
            unregister_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_PLUGIN);
        if ((event_mask_filter_ & PCIES2MM_EVENT_MASK_PLUG_OUT) != 0)
            unregister_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_UNPLUG);
    }

    void PcieS2mmCaptureSession::close_event_handles()
    {
        if (dma_event_)
        {
            CloseHandle(dma_event_);
            dma_event_ = nullptr;
        }
        if (format_change_event_)
        {
            CloseHandle(format_change_event_);
            format_change_event_ = nullptr;
        }
        if (plug_in_event_)
        {
            CloseHandle(plug_in_event_);
            plug_in_event_ = nullptr;
        }
        if (plug_out_event_)
        {
            CloseHandle(plug_out_event_);
            plug_out_event_ = nullptr;
        }
    }

    bool PcieS2mmCaptureSession::start_event_monitoring()
    {
        if (monitoring_.load(std::memory_order_acquire))
            return true;

        const uint32_t channel = active_channel();
        if (!create_and_register_events(channel))
        {
            unregister_events(channel);
            close_event_handles();
            return false;
        }

        monitoring_.store(true, std::memory_order_release);
        try
        {
            capture_thread_ = std::thread(&PcieS2mmCaptureSession::capture_thread_proc, this);
        }
        catch (...)
        {
            monitoring_.store(false, std::memory_order_release);
            unregister_events(channel);
            close_event_handles();
            return false;
        }
        return true;
    }

    void PcieS2mmCaptureSession::stop_event_monitoring()
    {
        if (!monitoring_.exchange(false, std::memory_order_acq_rel) &&
            !capture_thread_.joinable())
            return;

        if (dma_event_)
            SetEvent(dma_event_);
        if (capture_thread_.joinable())
            capture_thread_.join();

        if (device_ != INVALID_HANDLE_VALUE)
            unregister_events(active_channel());
        close_event_handles();
    }

    int PcieS2mmCaptureSession::get_frame(uint32_t channelIndex, uint32_t frameIndex, uint8_t *buffer, DWORD bufferSize) const
    {
        uint32_t bytesReturned = 0;
        const BOOL ok = giga_ioctl_get_frame(device_, channelIndex, frameIndex,
                                              buffer, bufferSize, &bytesReturned);
        if (!ok)
            return -1;
        return static_cast<int>(bytesReturned);
    }

    void PcieS2mmCaptureSession::capture_thread_proc()
    {
        const uint32_t channel = active_channel();
        HANDLE waitHandles[] = {format_change_event_, plug_in_event_, plug_out_event_};
        constexpr DWORD waitHandleCount = 3;

        while (monitoring_.load(std::memory_order_acquire))
        {
            const DWORD waitResult = WaitForMultipleObjects(waitHandleCount,
                                                            waitHandles,
                                                            FALSE,
                                                            1000);
            if (!monitoring_.load(std::memory_order_acquire))
                break;
            if (waitResult == WAIT_TIMEOUT)
                continue;
            if (waitResult < WAIT_OBJECT_0 || waitResult >= WAIT_OBJECT_0 + waitHandleCount)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                if (dma_event_)
                    SetEvent(dma_event_);
                break;
            }

            switch (waitResult - WAIT_OBJECT_0)
            {
            case 0:
                PCIES2MM_LOG("event: FORMAT_CHANGE");
                handle_format_change_event(channel);
                break;
            case 1:
                PCIES2MM_LOG("event: PLUG_IN");
                handle_plugin_event(channel);
                break;
            case 2:
                PCIES2MM_LOG("event: PLUG_OUT");
                handle_unplug_event(channel);
                break;
            default:
                break;
            }
        }

        write_reg(INTERRUPT_BASE + IRQ_MASK_W1C_OFFSET, video_irq_mask_bit());
    }

    void PcieS2mmCaptureSession::handle_format_change_event(uint32_t channel)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signal_metadata_valid_ = false;
        }
        emit_event(PCIES2MM_EVENT_FORMAT_CHANGE_BEGIN);
        stop_video(channel);
        capture_active_ = false;
        stream_ready_pending_ = false;
        if (dma_event_)
            SetEvent(dma_event_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] { return !read_in_progress_ && !frame_held_; });
        }
        if (running_.load(std::memory_order_acquire))
            refresh_stream_from_registers(true);
    }

    void PcieS2mmCaptureSession::handle_plugin_event(uint32_t channel)
    {
        if (signal_presence_known_.load(std::memory_order_acquire) &&
            signal_present_.load(std::memory_order_acquire))
            return;

        if (capture_active_.exchange(false, std::memory_order_acq_rel))
        {
            stop_video(channel);
        }
        if (dma_event_)
            SetEvent(dma_event_);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_present_.store(true, std::memory_order_release);
        signal_presence_known_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signal_metadata_valid_ = false;
        }
        emit_event(PCIES2MM_EVENT_PLUG_IN);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] { return !read_in_progress_ && !frame_held_; });
        }
        if (running_.load(std::memory_order_acquire))
            refresh_stream_from_registers(true);
    }

    void PcieS2mmCaptureSession::handle_unplug_event(uint32_t channel)
    {
        signal_probe_active_.store(false, std::memory_order_release);
        stream_ready_pending_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_presence_known_.store(true, std::memory_order_release);
        stop_video(channel);
        capture_active_ = false;
        if (dma_event_)
            SetEvent(dma_event_);
        emit_event(PCIES2MM_EVENT_PLUG_OUT);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            signal_metadata_valid_ = false;
            stats_.state = PCIES2MM_STREAM_CONFIGURED;
        }
    }

    bool PcieS2mmCaptureSession::refresh_stream_from_registers(bool resizeBuffer)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rawFormat = 0;
        if (!read_reg(video_base() + VIDEO_HSIZE_OFFSET, width) ||
            !read_reg(video_base() + VIDEO_VSIZE_OFFSET, height) ||
            !read_reg(video_base() + VIDEO_FORMAT_OFFSET, rawFormat) ||
            width == 0 || height == 0)
            return false;

        const pcies2mm_pixel_format_t pixelFormat = decode_pixel_format(rawFormat);
        const char fourccLe[5] = {
            static_cast<char>(rawFormat & 0xffu),
            static_cast<char>((rawFormat >> 8) & 0xffu),
            static_cast<char>((rawFormat >> 16) & 0xffu),
            static_cast<char>((rawFormat >> 24) & 0xffu),
            '\0'};
        const char *decodedFormat = "UNKNOWN";
        switch (pixelFormat)
        {
        case PCIES2MM_PIXFMT_YUY2:
            decodedFormat = "YUY2";
            break;
        case PCIES2MM_PIXFMT_Y210:
            decodedFormat = "Y210";
            break;
        default:
            break;
        }
        PCIES2MM_LOG("stream registers: width=%u height=%u VIDEO_FORMAT=0x%08X register_text='%c%c%c%c' fourcc_le='%c%c%c%c' decoded=%s frame_bytes=%zu",
                     width,
                     height,
                     rawFormat,
                     static_cast<char>((rawFormat >> 24) & 0xffu),
                     static_cast<char>((rawFormat >> 16) & 0xffu),
                     static_cast<char>((rawFormat >> 8) & 0xffu),
                     static_cast<char>(rawFormat & 0xffu),
                     fourccLe[0],
                     fourccLe[1],
                     fourccLe[2],
                     fourccLe[3],
                     decodedFormat,
                     bytes_per_frame(width, height, pixelFormat));
        if (pixelFormat == PCIES2MM_PIXFMT_UNKNOWN)
            return false;

        const uint32_t bitDepth = bit_depth_for_pixfmt(pixelFormat);
        const size_t bytes = bytes_per_frame(width, height, pixelFormat);
        if (bytes == 0 || bytes > (std::numeric_limits<DWORD>::max)())
        {
            PCIES2MM_ERROR_LOG("refresh_stream_from_registers invalid frame size width=%u height=%u bytes=%zu",
                               width,
                               height,
                               bytes);
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (frame_held_ || read_in_progress_)
            return false;
        stream_desc_.width = width;
        stream_desc_.height = height;
        stream_desc_.pixel_format = pixelFormat;
        stream_bit_depth_ = bitDepth;
        cached_signal_.connected = 1;
        cached_signal_.channel = channel_;
        cached_signal_.width = width;
        cached_signal_.height = height;
        cached_signal_.pixel_format = pixelFormat;
        cached_signal_.bit_depth = bitDepth;
        signal_metadata_valid_ = true;
        if (resizeBuffer)
        {
            copy_buffer_.clear();
            if (!zero_copy_enabled_)
                copy_buffer_.assign(bytes, 0);
        }
        return true;
    }

    void PcieS2mmCaptureSession::emit_event(pcies2mm_event_type_t type) const
    {
        pcies2mm_event_callback_t callback = nullptr;
        void *user = nullptr;
        {
            std::lock_guard<std::mutex> lock(event_callback_mutex_);
            const uint32_t typeMask = event_mask_for_type(type);
            if (!event_callback_ || typeMask == 0 || (event_mask_filter_ & typeMask) == 0)
                return;
            callback = event_callback_;
            user = event_callback_user_;
        }

        callback(type, user);
    }

    pcies2mm_status_t PcieS2mmCaptureSession::fail(pcies2mm_status_t status, const char *where, DWORD winerr) const
    {
        std::ostringstream oss;
        oss << (where ? where : "PcieS2mm") << " failed";
        if (winerr != NO_ERROR)
            oss << ": " << win32_error(winerr) << " (" << winerr << ")";
        set_last_error(oss.str());
        PCIES2MM_ERROR_LOG("%s", oss.str().c_str());
        return status;
    }

    void PcieS2mmCaptureSession::set_last_error(const std::string &message) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = message;
    }

    void PcieS2mmCaptureSession::clear_last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_.clear();
    }

    uint32_t PcieS2mmCaptureSession::active_channel() const
    {
        return channel_;
    }

    uint32_t PcieS2mmCaptureSession::video_event_mask() const
    {
        return video_irq_mask_bit();
    }

    uint32_t PcieS2mmCaptureSession::video_base() const
    {
        return CH_VIDEO_BASE(active_channel());
    }

    uint32_t PcieS2mmCaptureSession::video_irq_mask_bit() const
    {
        return IRQ_BIT_MASK(active_channel(), IRQ_SUB_VIDEO_DMA_CTRL);
    }

    size_t PcieS2mmCaptureSession::frame_size_bytes() const
    {
        return bytes_per_frame(stream_desc_.width, stream_desc_.height, stream_desc_.pixel_format);
    }

    void PcieS2mmCaptureSession::record_get_frame_timing(double elapsedUs)
    {
        ++get_frame_timing_samples_;
        ++get_frame_timing_window_samples_;
        get_frame_timing_total_us_ += elapsedUs;
        get_frame_timing_window_max_us_ = (std::max)(get_frame_timing_window_max_us_, elapsedUs);
        get_frame_timing_lifetime_max_us_ = (std::max)(get_frame_timing_lifetime_max_us_, elapsedUs);
        if (get_frame_timing_window_samples_ >= 300)
        {
            get_frame_timing_last_max300_us_ = get_frame_timing_window_max_us_;
            get_frame_timing_window_samples_ = 0;
            get_frame_timing_window_max_us_ = 0.0;
        }
    }

    void PcieS2mmCaptureSession::record_event_wait_timing(double elapsedUs)
    {
        ++event_wait_timing_samples_;
        ++event_wait_timing_window_samples_;
        event_wait_timing_total_us_ += elapsedUs;
        event_wait_timing_window_max_us_ = (std::max)(event_wait_timing_window_max_us_, elapsedUs);
        event_wait_timing_lifetime_max_us_ = (std::max)(event_wait_timing_lifetime_max_us_, elapsedUs);
        if (event_wait_timing_window_samples_ >= 300)
        {
            event_wait_timing_last_max300_us_ = event_wait_timing_window_max_us_;
            event_wait_timing_window_samples_ = 0;
            event_wait_timing_window_max_us_ = 0.0;
        }
    }

    void PcieS2mmCaptureSession::record_sdk_processing_timing(double elapsedUs)
    {
        ++sdk_processing_timing_samples_;
        ++sdk_processing_timing_window_samples_;
        sdk_processing_timing_total_us_ += elapsedUs;
        sdk_processing_timing_window_max_us_ = (std::max)(sdk_processing_timing_window_max_us_, elapsedUs);
        sdk_processing_timing_lifetime_max_us_ = (std::max)(sdk_processing_timing_lifetime_max_us_, elapsedUs);
        if (sdk_processing_timing_window_samples_ >= 300)
        {
            sdk_processing_timing_last_max300_us_ = sdk_processing_timing_window_max_us_;
            sdk_processing_timing_window_samples_ = 0;
            sdk_processing_timing_window_max_us_ = 0.0;
        }
    }
}
