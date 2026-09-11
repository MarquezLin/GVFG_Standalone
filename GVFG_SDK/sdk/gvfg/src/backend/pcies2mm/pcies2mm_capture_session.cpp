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
    constexpr auto kZeroCopyUnplugGracePeriod = std::chrono::milliseconds(100);

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

    static std::wstring utf8_to_wide(const char *s)
    {
        if (!s || !*s)
            return std::wstring();
        const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               s, -1, nullptr, 0);
        if (needed <= 0)
            return std::wstring();
        std::wstring out(static_cast<size_t>(needed), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s, -1, &out[0], needed) <= 0)
            return std::wstring();
        if (!out.empty() && out.back() == L'\0')
            out.pop_back();
        return out;
    }

    static std::string win32_error(DWORD err)
    {
        wchar_t *msg = nullptr;
        const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                             FORMAT_MESSAGE_FROM_SYSTEM |
                                             FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         err,
                                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                         reinterpret_cast<LPWSTR>(&msg),
                                         0,
                                         nullptr);
        std::string out;
        if (len && msg)
        {
            std::wstring wide(msg, msg + len);
            while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' '))
                wide.pop_back();
            out = wide_to_utf8(wide);
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
        const std::wstring wideLine = utf8_to_wide(line);
        if (!wideLine.empty())
            OutputDebugStringW(wideLine.c_str());
        else
            OutputDebugStringA(line);
    }

#define PCIES2MM_LOG(...) trace_log(false, "", __VA_ARGS__)
#define PCIES2MM_ERROR_LOG(...) trace_log(true, "[error]", __VA_ARGS__)
#define PCIES2MM_TIMING_LOG(...) trace_log(true, "[timing]", __VA_ARGS__)

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
    PcieS2mmDeviceConnection::~PcieS2mmDeviceConnection()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    PcieS2mmCaptureSession::PcieS2mmCaptureSession(ChannelErrorState &errorState)
        : error_state_(errorState)
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

        auto connection = std::make_shared<PcieS2mmDeviceConnection>();
        connection->handle = CreateFileW(device.interface_path.c_str(),
                                           GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
        if (connection->handle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            close_handles();
            return fail(PCIES2MM_EIO, "CreateFile(PcieS2mm)", err);
        }
        device_connection_ = std::move(connection);

        configured_ = false;
        signal_presence_known_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_metadata_valid_ = false;
        error_state_.clear();
        PCIES2MM_LOG("open_device: %s", wide_to_utf8(device.interface_path).c_str());
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::open_device_connection(
        const std::shared_ptr<PcieS2mmDeviceConnection> &connection)
    {
        if (!connection || connection->handle == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "open device connection rejected: connection is invalid");

        close();
        device_connection_ = connection;
        configured_ = false;
        signal_presence_known_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_metadata_valid_ = false;
        error_state_.clear();
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::close()
    {
        pcies2mm_status_t result = stop_stream();
        stop_event_monitoring();
        if (zero_copy_enabled_ && device_handle() != INVALID_HANDLE_VALUE)
        {
            const pcies2mm_status_t disableStatus = set_zero_copy_enabled(false);
            if (result == PCIES2MM_OK)
                result = disableStatus;
        }
        close_handles();
        configured_ = false;
        zero_copy_enabled_ = false;
        signal_presence_known_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_metadata_valid_ = false;
        reset_stats(stats_, PCIES2MM_STREAM_STOPPED);
        return result;
    }

    void PcieS2mmCaptureSession::close_handles()
    {
        close_event_handles();
        device_connection_.reset();
    }

    HANDLE PcieS2mmCaptureSession::device_handle() const
    {
        return device_connection_ ? device_connection_->handle : INVALID_HANDLE_VALUE;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_channel(uint32_t channel)
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "set channel rejected: device is not open");
        if (running_)
            return reject(PCIES2MM_ESTATE, "set channel rejected: stream is running");
        if (channel >= kMaxChannels)
            return reject(PCIES2MM_EINVAL, "set channel rejected: channel index is invalid");
        channel_ = channel;
        signal_metadata_valid_ = false;
        if (!start_event_monitoring())
            return fail(PCIES2MM_EIO, "start event monitoring");
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_zero_copy_enabled(bool enabled)
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "set zero copy rejected: device is not open");
        if (running_)
            return reject(PCIES2MM_ESTATE, "set zero copy rejected: stream is running");
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

        if (!giga_ioctl_set_frame_zerocopy(device_handle(), active_channel(), enabled ? TRUE : FALSE))
            return fail(PCIES2MM_EIO, enabled ? "enable zero copy" : "disable zero copy");
        zero_copy_enabled_ = enabled;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_video_format(pcies2mm_pixel_format_t format)
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "set video format rejected: device is not open");

        uint32_t value = 0;
        if (format == PCIES2MM_PIXFMT_Y210)
            value = 1;
        else if (format != PCIES2MM_PIXFMT_YUY2)
            return reject(PCIES2MM_EINVAL, "set video format rejected: pixel format is invalid");

        if (!write_reg(VIDEO_OUTPUT_FORMAT_REGISTER, value))
            return fail(PCIES2MM_EIO, "set_video_format");

        std::lock_guard<std::mutex> lock(mutex_);
        signal_metadata_valid_ = false;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_audio_enabled(bool enabled)
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "set audio enabled rejected: device is not open");
        if (running_)
            return reject(PCIES2MM_ESTATE, "set audio enabled rejected: stream is running");
        if (enabled && active_channel() != 0)
            return reject(PCIES2MM_ENOTSUP, "audio capture is currently supported only on channel 0");

        if (audio_enabled_ == enabled)
            return PCIES2MM_OK;

        // Event handles are created according to audio_enabled_. Rebuild the
        // registrations when the stopped channel changes between video-only
        // and video+audio; otherwise the new audio handles remain null.
        const bool wasMonitoring = monitoring_.load(std::memory_order_acquire);
        if (wasMonitoring)
            stop_event_monitoring();
        audio_enabled_ = enabled;
        if (wasMonitoring && !start_event_monitoring())
            return fail(PCIES2MM_EIO, "restart event monitoring after audio stream change");
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::get_audio_format(pcies2mm_audio_format_t &out) const
    {
        std::memset(&out, 0, sizeof(out));
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "get audio format rejected: device is not open");
        if (active_channel() != 0)
            return reject(PCIES2MM_ENOTSUP, "audio capture is currently supported only on channel 0");

        giga_ioctl_audio_info info{};
        if (!giga_ioctl_get_audio_info(device_handle(), active_channel(), &info))
            return fail(PCIES2MM_EIO, "get audio info");
        if (info.channels == 0 || info.samples_per_second == 0 ||
            info.bits_per_sample == 0 || info.frames_per_second == 0 ||
            info.frame_buffer_size == 0 || (info.bits_per_sample % 8) != 0)
            return reject(PCIES2MM_EIO, "driver returned an invalid audio format");

        out.sample_rate = info.samples_per_second;
        out.channels = info.channels;
        out.bits_per_sample = info.bits_per_sample;
        out.frames_per_second = info.frames_per_second;
        out.frame_bytes = info.frame_buffer_size;
        out.block_align = info.channels * (info.bits_per_sample / 8);
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::get_signal_status(pcies2mm_signal_status_t &out) const
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "get signal status rejected: device is not open");

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
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "configure stream rejected: device is not open");
        if (running_)
            return reject(PCIES2MM_ESTATE, "configure stream rejected: stream is running");
        if (desc.width == 0 || desc.height == 0)
            return reject(PCIES2MM_EINVAL, "configure stream rejected: frame dimensions are invalid");

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
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::start_stream()
    {
        if (device_handle() == INVALID_HANDLE_VALUE || !configured_)
            return reject(PCIES2MM_ESTATE, "start stream rejected: device is not configured");
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
            audio_read_in_progress_ = false;
            active_delivery_started_ = {};
            latest_sequence_ = 0;
            delivered_sequence_ = 0;
            wait_timeout_count_ = 0;
            audio_dma_event_wakes_.store(0, std::memory_order_relaxed);
            extra_audio_event_wakes_.store(0, std::memory_order_relaxed);
            audio_frames_from_driver_.store(0, std::memory_order_relaxed);
            audio_bytes_from_driver_.store(0, std::memory_order_relaxed);
            get_frame_timing_samples_ = 0;
            get_frame_timing_window_samples_ = 0;
            get_frame_timing_total_us_ = 0.0;
            get_frame_timing_window_max_us_ = 0.0;
            get_frame_timing_last_max300_us_ = 0.0;
            get_frame_timing_lifetime_max_us_ = 0.0;
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
        signal_transition_active_ = false;
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
        signal_transition_active_ = false;
        reader_ready_ = false;
        signal_probe_active_ = false;
        stream_ready_pending_ = false;

        if (device_handle() != INVALID_HANDLE_VALUE)
            stop_capture(active_channel());

        if (dma_event_)
            SetEvent(dma_event_);
        if (audio_event_)
            SetEvent(audio_event_);
        if (extra_video_event_)
            SetEvent(extra_video_event_);
        if (extra_audio_event_)
            SetEvent(extra_audio_event_);

        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] { return !read_in_progress_ && !audio_read_in_progress_; });
        }
        const pcies2mm_status_t releaseStatus = release_all_zero_copy_frames(true);
        stop_event_monitoring();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            held_frame_ = {};
            frame_held_ = false;
            audio_read_in_progress_ = false;
            stats_.state = configured_ ? PCIES2MM_STREAM_CONFIGURED : PCIES2MM_STREAM_STOPPED;
        }
        return releaseStatus;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::ensure_capture_started()
    {
        if (capture_active_.load(std::memory_order_acquire))
            return PCIES2MM_OK;
        if (!signal_presence_known_.load(std::memory_order_acquire) ||
            !signal_present_.load(std::memory_order_acquire) ||
            signal_transition_active_.load(std::memory_order_acquire))
            return PCIES2MM_ETIMEOUT;
        if (!ResetEvent(dma_event_) || !ResetEvent(extra_video_event_) ||
            (audio_enabled_ &&
             (!ResetEvent(audio_event_) || !ResetEvent(extra_audio_event_))))
            return fail(PCIES2MM_EIO, "reset capture event");

        if (!start_capture(active_channel()))
            return fail(PCIES2MM_EIO, "enable reader capture");
        capture_active_.store(true, std::memory_order_release);
        stream_ready_pending_.store(true, std::memory_order_release);
        first_video_timing_pending_ = true;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::wait_frame(uint32_t timeoutMs, pcies2mm_frame_t &out)
    {
        constexpr uint64_t kTimingWarmupFrames = 30;
        bool logFirstVideoTiming = false;
        reader_ready_.store(true, std::memory_order_release);
        std::memset(&out, 0, sizeof(out));
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_ || frame_held_ || read_in_progress_)
                return reject(PCIES2MM_ESTATE, "wait frame rejected: stream state does not allow another read");
            const pcies2mm_status_t startStatus = ensure_capture_started();
            if (startStatus != PCIES2MM_OK)
                return startStatus;
            logFirstVideoTiming = first_video_timing_pending_;
            read_in_progress_ = true;
        }

        const auto finishRead = [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            read_in_progress_ = false;
            read_finished_cv_.notify_all();
        };

        const bool infiniteWait = timeoutMs == UINT32_MAX;
        const auto deadline = infiniteWait
                                  ? (std::chrono::steady_clock::time_point::max)()
                                  : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        const DWORD bytes = static_cast<DWORD>(frame_size_bytes());
        const uint8_t *data = nullptr;
        double videoEventWaitMs = 0.0;
        double getFrameUs = 0.0;
        DWORD successfulWaitResult = WAIT_FAILED;
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

            HANDLE videoReadyEvents[] = {dma_event_, extra_video_event_};
            const auto waitStarted = std::chrono::steady_clock::now();
            const DWORD waitResult = WaitForMultipleObjects(2, videoReadyEvents, FALSE, waitMs);
            const auto waitReturned = std::chrono::steady_clock::now();
            videoEventWaitMs += std::chrono::duration<double, std::milli>(
                                    waitReturned - waitStarted).count();
            if (waitResult == WAIT_TIMEOUT)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++wait_timeout_count_;
                }
                finishRead();
                return PCIES2MM_ETIMEOUT;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_OBJECT_0 + 1)
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
            {
                successfulWaitResult = waitResult;
                break;
            }

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
            ++latest_sequence_;
            const bool timingWarmupComplete = latest_sequence_ > kTimingWarmupFrames;
            if (timingWarmupComplete)
                record_get_frame_timing(getFrameUs);
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
            out.timestamp_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            out.width = stream_desc_.width;
            out.height = stream_desc_.height;
            out.pixel_format = stream_desc_.pixel_format;
            out.bit_depth = stream_bit_depth_;
            held_frame_ = out;
            frame_held_ = true;
            if (logFirstVideoTiming)
                first_video_timing_pending_ = false;
        }
        if (logFirstVideoTiming)
        {
            const char *eventName = successfulWaitResult == WAIT_OBJECT_0
                                        ? "dma_event"
                                        : "extra_video_event";
            PCIES2MM_TIMING_LOG("CH%u | video wait=%.3f ms | wake=%s | acquire/get_frame=%.3f ms | mode=%s",
                                active_channel(),
                                videoEventWaitMs,
                                eventName,
                                getFrameUs / 1000.0,
                                zero_copy_enabled_ ? "zero-copy" : "copy");
        }
        if (stream_ready_pending_.exchange(false, std::memory_order_acq_rel))
            emit_event(PCIES2MM_EVENT_STREAM_READY);
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::wait_audio(uint32_t timeoutMs,
                                                         void *destination,
                                                         uint32_t destinationCapacity,
                                                         uint32_t &outBytes)
    {
        outBytes = 0;
        if (!destination || destinationCapacity == 0)
            return reject(PCIES2MM_EINVAL, "wait audio rejected: destination is invalid");

        pcies2mm_audio_format_t format{};
        const pcies2mm_status_t formatStatus = get_audio_format(format);
        if (formatStatus != PCIES2MM_OK)
            return formatStatus;
        if (destinationCapacity < format.frame_bytes)
            return reject(PCIES2MM_EINVAL, "wait audio rejected: destination is smaller than the driver frame");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || !audio_enabled_ || audio_read_in_progress_)
                return reject(PCIES2MM_ESTATE, "wait audio rejected: stream state does not allow another audio read");
            const pcies2mm_status_t startStatus = ensure_capture_started();
            if (startStatus != PCIES2MM_OK)
                return startStatus;
            audio_read_in_progress_ = true;
        }

        const auto finishRead = [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_read_in_progress_ = false;
            read_finished_cv_.notify_all();
        };
        const bool infiniteWait = timeoutMs == UINT32_MAX;
        const auto deadline = infiniteWait
                                  ? (std::chrono::steady_clock::time_point::max)()
                                  : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

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

            HANDLE audioReadyEvents[] = {audio_event_, extra_audio_event_};
            const DWORD waitResult = WaitForMultipleObjects(2, audioReadyEvents, FALSE, waitMs);
            if (waitResult == WAIT_TIMEOUT)
            {
                finishRead();
                return PCIES2MM_ETIMEOUT;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_OBJECT_0 + 1)
            {
                const DWORD err = GetLastError();
                finishRead();
                return fail(PCIES2MM_EIO, "wait audio event", err);
            }
            if (waitResult == WAIT_OBJECT_0)
                audio_dma_event_wakes_.fetch_add(1, std::memory_order_relaxed);
            else
                extra_audio_event_wakes_.fetch_add(1, std::memory_order_relaxed);
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

            const int ret = get_audio_frame(active_channel(),
                                            (std::numeric_limits<uint32_t>::max)(),
                                            destination,
                                            destinationCapacity);
            if (ret >= 0)
            {
                if (ret == 0 || static_cast<uint32_t>(ret) > format.frame_bytes)
                {
                    finishRead();
                    return reject(PCIES2MM_EIO, "driver returned an invalid audio byte count");
                }
                if (static_cast<uint32_t>(ret) != format.frame_bytes)
                    PCIES2MM_LOG("audio frame size mismatch: expected=%u returned=%d",
                                 format.frame_bytes, ret);
                outBytes = static_cast<uint32_t>(ret);
                audio_frames_from_driver_.fetch_add(1, std::memory_order_relaxed);
                audio_bytes_from_driver_.fetch_add(outBytes, std::memory_order_relaxed);
                finishRead();
                return PCIES2MM_OK;
            }

            const DWORD frameError = GetLastError();
            if (!is_transient_frame_not_ready_error(frameError))
            {
                finishRead();
                return fail(PCIES2MM_EIO, "copy get audio frame", frameError);
            }
        }
    }

    pcies2mm_status_t PcieS2mmCaptureSession::release_frame(const pcies2mm_frame_t &frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!frame_held_)
            return reject(PCIES2MM_ESTATE, "release frame rejected: no frame is currently held");
        if (frame.data != held_frame_.data ||
            frame.data_size_bytes != held_frame_.data_size_bytes ||
            frame.frame_id != held_frame_.frame_id ||
            frame.width != held_frame_.width ||
            frame.height != held_frame_.height ||
            frame.pixel_format != held_frame_.pixel_format ||
            frame.bit_depth != held_frame_.bit_depth)
            return reject(PCIES2MM_EINVAL, "release frame rejected: frame token does not match the held frame");

        if (zero_copy_enabled_ && !release_zero_copy_frame(active_channel()))
        {
            const DWORD err = GetLastError();
            // The driver invalidates its zero-copy release list as soon as the
            // input is unplugged. A frame that was valid when delivered can
            // therefore report ERROR_BAD_COMMAND when the application releases
            // it after the unplug interrupt. In that exact state, ownership has
            // already been revoked by the driver; clear the matching SDK token
            // so unplug handling and subsequent capture can make progress.
            const bool revokedByUnplug = zero_copy_release_revoked_by_unplug(err, lock);
            if (!revokedByUnplug)
            {
                lock.unlock();
                return fail(PCIES2MM_EIO, "release zero-copy frame", err);
            }
            PCIES2MM_LOG("zero-copy frame ownership revoked by unplug: frame=%llu",
                         static_cast<unsigned long long>(frame.frame_id));
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
        outDebugState.audio_dma_event_wakes =
            audio_dma_event_wakes_.load(std::memory_order_relaxed);
        outDebugState.extra_audio_event_wakes =
            extra_audio_event_wakes_.load(std::memory_order_relaxed);
        outDebugState.audio_frames_from_driver =
            audio_frames_from_driver_.load(std::memory_order_relaxed);
        outDebugState.audio_bytes_from_driver =
            audio_bytes_from_driver_.load(std::memory_order_relaxed);
        outDebugState.get_frame_zero_copy = zero_copy_enabled_ ? 1 : 0;
        outDebugState.get_frame_timing_samples = get_frame_timing_samples_;
        outDebugState.get_frame_timing_average_us =
            get_frame_timing_samples_ > 0
                ? get_frame_timing_total_us_ / static_cast<double>(get_frame_timing_samples_)
                : 0.0;
        outDebugState.get_frame_timing_max300_us = get_frame_timing_last_max300_us_;
        outDebugState.get_frame_timing_max_us = get_frame_timing_lifetime_max_us_;
    }

    bool PcieS2mmCaptureSession::read_reg(uint32_t offset, uint32_t &out) const
    {
        if (is_write_only_register(offset))
            return false;
        return giga_ioctl_read_register(device_handle(), offset, &out) != FALSE;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::debug_read_register(uint32_t offset,
                                                                  uint32_t &outValue) const
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "debug register read rejected: device is not open");
        if ((offset & 0x3u) != 0 || is_write_only_register(offset))
            return reject(PCIES2MM_EINVAL, "debug register read rejected: offset is invalid or write-only");
        return read_reg(offset, outValue) ? PCIES2MM_OK : fail(PCIES2MM_EIO, "debug_read_register");
    }

    bool PcieS2mmCaptureSession::write_reg(uint32_t offset, uint32_t value) const
    {
        return giga_ioctl_write_register(device_handle(), offset, value) != FALSE;
    }

    bool PcieS2mmCaptureSession::start_video(uint32_t channelIndex) const
    {
        return giga_ioctl_video_start(device_handle(), channelIndex) != FALSE;
    }

    bool PcieS2mmCaptureSession::stop_video(uint32_t channelIndex) const
    {
        return giga_ioctl_video_stop(device_handle(), channelIndex) != FALSE;
    }

    bool PcieS2mmCaptureSession::start_capture(uint32_t channelIndex) const
    {
        return audio_enabled_
                   ? giga_ioctl_start_video_audio(device_handle(), channelIndex) != FALSE
                   : start_video(channelIndex);
    }

    bool PcieS2mmCaptureSession::stop_capture(uint32_t channelIndex) const
    {
        return audio_enabled_
                   ? giga_ioctl_stop_video_audio(device_handle(), channelIndex) != FALSE
                   : stop_video(channelIndex);
    }

    bool PcieS2mmCaptureSession::acquire_zero_copy_frame(uint32_t channelIndex,
                                                         const uint8_t *&outData) const
    {
        const void *buffer = nullptr;
        if (!giga_ioctl_acquire_video_frame_zerocopy(device_handle(), channelIndex, &buffer))
            return false;
        outData = static_cast<const uint8_t *>(buffer);
        return true;
    }

    bool PcieS2mmCaptureSession::release_zero_copy_frame(uint32_t channelIndex) const
    {
        return giga_ioctl_release_video_frame(device_handle(), channelIndex) != FALSE;
    }

    bool PcieS2mmCaptureSession::zero_copy_release_revoked_by_unplug(
        DWORD error, std::unique_lock<std::mutex> &lock)
    {
        if (error != ERROR_BAD_COMMAND)
            return false;

        // The driver can reject release before its unplug event reaches this
        // thread. Wait only on this error path and accept it only if the event
        // monitor confirms that the signal disappeared within the grace period.
        const auto signalDisconnected = [this] {
            return signal_presence_known_.load(std::memory_order_acquire) &&
                   !signal_present_.load(std::memory_order_acquire);
        };
        return signalDisconnected() ||
               signal_presence_cv_.wait_for(lock, kZeroCopyUnplugGracePeriod,
                                            signalDisconnected);
    }

    pcies2mm_status_t PcieS2mmCaptureSession::release_all_zero_copy_frames(bool includeInUse)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!frame_held_ || !includeInUse)
            return PCIES2MM_OK;
        if (zero_copy_enabled_ && held_frame_.data && !release_zero_copy_frame(active_channel()))
        {
            const DWORD err = GetLastError();
            const bool revokedByUnplug = zero_copy_release_revoked_by_unplug(err, lock);
            if (!revokedByUnplug)
            {
                lock.unlock();
                return fail(PCIES2MM_EIO, "release pending zero-copy frame", err);
            }
            PCIES2MM_LOG("pending zero-copy frame ownership already revoked by unplug: frame=%llu",
                         static_cast<unsigned long long>(held_frame_.frame_id));
        }
        held_frame_ = {};
        frame_held_ = false;
        read_finished_cv_.notify_all();
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::debug_write_register(uint32_t offset,
                                                                   uint32_t value) const
    {
        if (device_handle() == INVALID_HANDLE_VALUE)
            return reject(PCIES2MM_ESTATE, "debug register write rejected: device is not open");
        if ((offset & 0x3u) != 0)
            return reject(PCIES2MM_EINVAL, "debug register write rejected: offset is not aligned");
        return write_reg(offset, value) ? PCIES2MM_OK : fail(PCIES2MM_EIO, "debug_write_register");
    }

    bool PcieS2mmCaptureSession::register_event(uint32_t channelIndex, uint32_t eventType, HANDLE eventHandle)
    {
        return giga_ioctl_register_event(device_handle(), channelIndex, eventType, eventHandle) != FALSE;
    }

    void PcieS2mmCaptureSession::unregister_event(uint32_t channelIndex, uint32_t eventType)
    {
        giga_ioctl_unregister_event(device_handle(), channelIndex, eventType);
    }

    bool PcieS2mmCaptureSession::create_and_register_events(uint32_t channelIndex)
    {
        dma_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        extra_video_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (audio_enabled_)
        {
            audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            extra_audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        format_change_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        plug_in_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        plug_out_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        monitor_stop_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!dma_event_ || !extra_video_event_ ||
            (audio_enabled_ && (!audio_event_ || !extra_audio_event_)) ||
            !format_change_event_ || !plug_in_event_ || !plug_out_event_ ||
            !monitor_stop_event_)
            return false;

        if (!register_event(channelIndex, GIGA_IOCTL_EVENT_VIDEO_DMA, dma_event_))
            return false;
        if (!register_event(channelIndex, GIGA_IOCTL_EVENT_EXTRA_VIDEO_FRAME, extra_video_event_))
            return false;
        if (audio_enabled_ &&
            (!register_event(channelIndex, GIGA_IOCTL_EVENT_AUDIO_DMA, audio_event_) ||
             !register_event(channelIndex, GIGA_IOCTL_EVENT_EXTRA_AUDIO_FRAME, extra_audio_event_)))
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
        unregister_event(channelIndex, GIGA_IOCTL_EVENT_EXTRA_VIDEO_FRAME);
        if (audio_enabled_)
        {
            unregister_event(channelIndex, GIGA_IOCTL_EVENT_AUDIO_DMA);
            unregister_event(channelIndex, GIGA_IOCTL_EVENT_EXTRA_AUDIO_FRAME);
        }
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
        if (audio_event_)
        {
            CloseHandle(audio_event_);
            audio_event_ = nullptr;
        }
        if (extra_video_event_)
        {
            CloseHandle(extra_video_event_);
            extra_video_event_ = nullptr;
        }
        if (extra_audio_event_)
        {
            CloseHandle(extra_audio_event_);
            extra_audio_event_ = nullptr;
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
        if (monitor_stop_event_)
        {
            CloseHandle(monitor_stop_event_);
            monitor_stop_event_ = nullptr;
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
        if (audio_event_)
            SetEvent(audio_event_);
        if (extra_video_event_)
            SetEvent(extra_video_event_);
        if (extra_audio_event_)
            SetEvent(extra_audio_event_);
        if (monitor_stop_event_)
            SetEvent(monitor_stop_event_);
        read_finished_cv_.notify_all();
        if (capture_thread_.joinable())
            capture_thread_.join();

        if (device_handle() != INVALID_HANDLE_VALUE)
            unregister_events(active_channel());
        close_event_handles();
    }

    int PcieS2mmCaptureSession::get_frame(uint32_t channelIndex, uint32_t frameIndex, uint8_t *buffer, DWORD bufferSize) const
    {
        uint32_t bytesReturned = 0;
        const BOOL ok = giga_ioctl_get_frame(device_handle(), channelIndex, frameIndex,
                                              buffer, bufferSize, &bytesReturned);
        if (!ok)
            return -1;
        return static_cast<int>(bytesReturned);
    }

    int PcieS2mmCaptureSession::get_audio_frame(uint32_t channelIndex,
                                                uint32_t frameIndex,
                                                void *buffer,
                                                DWORD bufferSize) const
    {
        uint32_t bytesReturned = 0;
        const BOOL ok = giga_ioctl_get_audio_frame(device_handle(), channelIndex, frameIndex,
                                                    buffer, bufferSize, &bytesReturned);
        if (!ok)
            return -1;
        return static_cast<int>(bytesReturned);
    }

    void PcieS2mmCaptureSession::capture_thread_proc()
    {
        const uint32_t channel = active_channel();
        HANDLE waitHandles[] = {
            monitor_stop_event_,
            format_change_event_,
            plug_in_event_,
            plug_out_event_};
        constexpr DWORD waitHandleCount = 4;

        while (monitoring_.load(std::memory_order_acquire))
        {
            const DWORD waitResult = WaitForMultipleObjects(waitHandleCount,
                                                            waitHandles,
                                                            FALSE,
                                                            INFINITE);
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
                break;
            case 1:
                PCIES2MM_LOG("event: FORMAT_CHANGE");
                handle_format_change_event(channel);
                break;
            case 2:
                PCIES2MM_LOG("event: PLUG_IN");
                handle_plugin_event(channel);
                break;
            case 3:
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
        signal_transition_active_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signal_metadata_valid_ = false;
        }
        emit_event(PCIES2MM_EVENT_FORMAT_CHANGE_BEGIN);
        capture_active_ = false;
        stream_ready_pending_ = false;
        if (dma_event_)
            SetEvent(dma_event_);
        if (audio_event_)
            SetEvent(audio_event_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] {
                return !monitoring_.load(std::memory_order_acquire) ||
                       (!read_in_progress_ && !audio_read_in_progress_ && !frame_held_);
            });
        }
        if (!monitoring_.load(std::memory_order_acquire))
            return;
        stop_capture(channel);
        if (running_.load(std::memory_order_acquire))
            refresh_stream_from_registers(true);
        signal_transition_active_.store(false, std::memory_order_release);
    }

    void PcieS2mmCaptureSession::handle_plugin_event(uint32_t channel)
    {
        if (signal_presence_known_.load(std::memory_order_acquire) &&
            signal_present_.load(std::memory_order_acquire))
            return;

        signal_transition_active_.store(true, std::memory_order_release);
        capture_active_.store(false, std::memory_order_release);
        if (dma_event_)
            SetEvent(dma_event_);
        if (audio_event_)
            SetEvent(audio_event_);
        signal_probe_active_.store(false, std::memory_order_release);
        signal_present_.store(true, std::memory_order_release);
        signal_presence_known_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signal_metadata_valid_ = false;
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] {
                return !monitoring_.load(std::memory_order_acquire) ||
                       (!read_in_progress_ && !audio_read_in_progress_ && !frame_held_);
            });
        }
        if (!monitoring_.load(std::memory_order_acquire))
            return;
        stop_capture(channel);
        if (running_.load(std::memory_order_acquire))
            refresh_stream_from_registers(true);
        signal_transition_active_.store(false, std::memory_order_release);
        emit_event(PCIES2MM_EVENT_PLUG_IN);
    }

    void PcieS2mmCaptureSession::handle_unplug_event(uint32_t channel)
    {
        signal_transition_active_.store(true, std::memory_order_release);
        signal_probe_active_.store(false, std::memory_order_release);
        stream_ready_pending_.store(false, std::memory_order_release);
        signal_present_.store(false, std::memory_order_release);
        signal_presence_known_.store(true, std::memory_order_release);
        signal_presence_cv_.notify_all();
        capture_active_ = false;
        if (dma_event_)
            SetEvent(dma_event_);
        if (audio_event_)
            SetEvent(audio_event_);
        emit_event(PCIES2MM_EVENT_PLUG_OUT);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            read_finished_cv_.wait(lock, [this] {
                return !monitoring_.load(std::memory_order_acquire) ||
                       (!read_in_progress_ && !audio_read_in_progress_ && !frame_held_);
            });
        }
        if (!monitoring_.load(std::memory_order_acquire))
            return;
        stop_capture(channel);
        signal_transition_active_.store(false, std::memory_order_release);

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
        error_state_.set(oss.str());
        PCIES2MM_ERROR_LOG("%s", oss.str().c_str());
        return status;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::reject(pcies2mm_status_t status,
                                                      const char *message) const
    {
        error_state_.set(message ? message : "PCIES2MM operation rejected");
        return status;
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

}
