#include "gigabyte_capture_session.h"
#include "gigabyte_driver_extensions.h"

#include <algorithm>
#include <array>

namespace
{
constexpr uint32_t kFourccYuy2 = GVFG_MAKEFOURCC('Y', 'U', 'Y', '2');
constexpr uint32_t kFourccYuyv = GVFG_MAKEFOURCC('Y', 'U', 'Y', 'V');
constexpr uint32_t kFourccUyvy = GVFG_MAKEFOURCC('U', 'Y', 'V', 'Y');
constexpr uint32_t kFourccY210 = GVFG_MAKEFOURCC('Y', '2', '1', '0');

uint64_t monotonic_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

gvfg_pixel_format_t decode_fourcc(uint32_t fourcc)
{
    if (fourcc == kFourccY210)
        return GVFG_PIXFMT_Y210;
    if (fourcc == kFourccYuy2 || fourcc == kFourccYuyv || fourcc == kFourccUyvy)
        return GVFG_PIXFMT_YUY2;
    return GVFG_PIXFMT_UNKNOWN;
}

uint32_t bit_depth(int format)
{
    return format == GVFG_PIXFMT_Y210 ? 10u :
           format == GVFG_PIXFMT_YUY2 ? 8u : 0u;
}

}

namespace gvfg::internal
{
GigabyteCaptureSession::GigabyteCaptureSession(ChannelErrorState &errorState)
    : error_state_(errorState)
{
}

GigabyteCaptureSession::~GigabyteCaptureSession()
{
    close();
}

gvfg_status_t GigabyteCaptureSession::open_device_index(size_t deviceIndex)
{
    const auto devices = enumerate_gigabyte_devices();
    if (deviceIndex >= devices.size())
        return reject(GVFG_ENODEV, "GigabyteLib open rejected: device index is unavailable");
    return open_device(devices[deviceIndex]);
}

gvfg_status_t GigabyteCaptureSession::open_device_path(const std::wstring &devicePath)
{
    if (devicePath.empty())
        return reject(GVFG_EINVAL, "GigabyteLib open rejected: device selection is invalid");
    close();
    device_path_ = devicePath;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::open_device(const GigabyteDevice &device)
{
    close();
    device_path_ = device.interface_path;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::ensure_vendor_channel_open()
{
    if (channel_open_)
        return GVFG_OK;
    if (device_path_.empty())
        return reject(GVFG_ESTATE, "GigabyteLib channel open rejected: no device is selected");

    device_handle_ = CreateFileW(device_path_.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device_handle_ == INVALID_HANDLE_VALUE)
        return reject(GVFG_ENODEV, "CreateFile for GigabyteLib failed");

    const ULONG memoryMode = zero_copy_enabled_
                                 ? GVFG_VIDEO_FRAME_ZERO_COPY | GVFG_AUDIO_FRAME_BUF_COPY
                                 : GVFG_VIDEO_FRAME_BUF_COPY | GVFG_AUDIO_FRAME_BUF_COPY;
    gvfg_status_t status = from_vendor(
        GvfgOpenDev(device_handle_, &context_, memoryMode));
    if (status != GVFG_OK)
    {
        CloseHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
        context_ = nullptr;
        return status;
    }

    status = from_vendor(GvfgCreateEvents(&events_, audio_enabled_ ? FALSE : TRUE));
    if (status != GVFG_OK)
    {
        GvfgCloseDev(&context_);
        CloseHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
        return status;
    }
    events_created_ = true;

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_)
    {
        close_vendor_channel();
        return reject(GVFG_EIO, "CreateEvent for GigabyteLib cancellation failed");
    }
    recovery_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!recovery_event_)
    {
        close_vendor_channel();
        return reject(GVFG_EIO, "CreateEvent for GigabyteLib recovery failed");
    }

    status = from_vendor(GvfgOpenVideoChn(context_, channel_, &events_));
    if (status != GVFG_OK)
    {
        close_vendor_channel();
        return status;
    }
    channel_open_ = true;
    return GVFG_OK;
}

void GigabyteCaptureSession::close_vendor_channel()
{
    if (channel_open_ && context_)
        GvfgCloseVideoChn(context_, channel_);
    channel_open_ = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        video_info_valid_ = false;
        audio_info_valid_ = false;
        cached_signal_ = {};
        cached_signal_.channel = channel_;
        video_info_ = {};
        audio_info_ = {};
    }
    if (context_)
        GvfgCloseDev(&context_);
    if (events_created_)
        GvfgDestroyEvents(&events_);
    events_created_ = false;
    events_ = {};
    if (stop_event_)
        CloseHandle(stop_event_);
    stop_event_ = nullptr;
    if (recovery_event_)
        CloseHandle(recovery_event_);
    recovery_event_ = nullptr;
    if (device_handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(device_handle_);
    device_handle_ = INVALID_HANDLE_VALUE;
}

gvfg_status_t GigabyteCaptureSession::close()
{
    stop_stream();
    close_vendor_channel();
    device_path_.clear();
    configured_ = false;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::set_channel(uint32_t channel)
{
    if (channel > 1)
        return reject(GVFG_EINVAL, "GigabyteLib channel index is invalid");
    if (channel_open_)
        return reject(GVFG_ESTATE, "GigabyteLib channel is already open");
    channel_ = channel;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::set_zero_copy_enabled(bool enabled)
{
    if (channel_open_ || running_)
        return reject(GVFG_ESTATE, "GigabyteLib frame memory mode is already fixed");
    zero_copy_enabled_ = enabled;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::set_audio_enabled(bool enabled)
{
    if (running_)
        return reject(GVFG_ESTATE, "GigabyteLib audio mode cannot change while running");
    if (audio_enabled_ != enabled)
    {
        if (channel_open_)
            close_vendor_channel();
        std::lock_guard<std::mutex> lock(state_mutex_);
        audio_info_valid_ = false;
    }
    audio_enabled_ = enabled;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::set_video_format(gvfg_pixel_format_t format)
{
    if (format != GVFG_PIXFMT_YUY2 && format != GVFG_PIXFMT_Y210)
        return reject(GVFG_EINVAL, "video format is invalid");
    if (running_)
        return reject(GVFG_ESTATE, "video format cannot change while capture is running");
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;

    const ULONG colorDepth = format == GVFG_PIXFMT_Y210
                                 ? GVFG_VIDEO_COLOR_DEPTH_10_BITS
                                 : GVFG_VIDEO_COLOR_DEPTH_8_BITS;
    const gvfg_status_t status = from_vendor(
        GvfgSetVideoColorDepth(context_, channel_, colorDepth));
    if (status == GVFG_OK)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        video_info_valid_ = false;
    }
    return status;
}

gvfg_status_t GigabyteCaptureSession::refresh_video_info()
{
    if (!context_)
        return reject(GVFG_ESTATE, "GigabyteLib video info requested before open");
    GVFG_VIDEO_INFO info{};
    gvfg_status_t status = GVFG_OK;
    {
        std::lock_guard<std::mutex> captureLock(capture_mutex_);
        status = from_vendor(GvfgGetVideoInfo(context_, channel_, &info));
    }
    if (status != GVFG_OK)
        return status;
    gvfg_signal_status_t signal{};
    signal.connected = info.VideoSignalLock ? 1 : 0;
    signal.channel = channel_;
    signal.width = info.Width;
    signal.height = info.Height;
    signal.pixel_format = decode_fourcc(info.Fourcc);
    signal.bit_depth = bit_depth(signal.pixel_format);
    signal.video_interface = static_cast<int>(info.VideoInterface);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cached_signal_ = signal;
        video_info_ = info;
        video_info_valid_ = true;
    }
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::get_signal_status(gvfg_signal_status_t &out)
{
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    const gvfg_status_t status = refresh_video_info();
    if (status != GVFG_OK)
        return status;
    std::lock_guard<std::mutex> lock(state_mutex_);
    out = cached_signal_;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::get_device_capabilities(
    gvfg_device_capabilities_t &out)
{
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    GVFG_DEV_INFO info{};
    const gvfg_status_t status = from_vendor(GvfgGetDevInfo(context_, &info));
    if (status != GVFG_OK)
        return status;
    out = {};
    out.video_channel_count = info.NumVideoChn;
    out.has_audio = info.HasAudio ? 1 : 0;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::get_sdi_info(gvfg_sdi_info_t &out)
{
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    GVFG_SDI_VIDEO_INFO info{};
    gvfg_status_t status = from_vendor(
        GvfgGetSdiVideoInputInfo(context_, channel_, &info));
    if (status != GVFG_OK)
        return status;
    GVFG_SDI_VIDEO_INFO_STR text{};
    status = from_vendor(GvfgStringifySdiVideoInputInfo(context_, &info, &text));
    if (status != GVFG_OK)
        return status;
    out = {};
    out.connected = info.VideoSignalLock ? 1 : 0;
    out.mode = static_cast<gvfg_sdi_mode_t>(info.Mode);
    out.resolution = static_cast<gvfg_sdi_resolution_t>(info.Resol);
    out.fps = static_cast<gvfg_sdi_fps_t>(info.Fps);
    out.progressive = info.Progressive ? 1 : 0;
    out.level_b = info.LevelB ? 1 : 0;
    out.st352_payload = info.St352Payload;
    out.error_count = info.ErrorCount;
    strncpy_s(out.signal_lock_name, text.VideoSignalLock, _TRUNCATE);
    strncpy_s(out.mode_name, text.Mode, _TRUNCATE);
    strncpy_s(out.resolution_name, text.Resol, _TRUNCATE);
    strncpy_s(out.fps_name, text.Fps, _TRUNCATE);
    strncpy_s(out.scan_name, text.Progressive, _TRUNCATE);
    strncpy_s(out.st352_format_name, text.St352PayloadByte0Fmt, _TRUNCATE);
    strncpy_s(out.st352_fps_name, text.St352PayloadByte1Fps, _TRUNCATE);
    strncpy_s(out.st352_chroma_name, text.St352PayloadByte2Chroma, _TRUNCATE);
    strncpy_s(out.st352_bit_depth_name, text.St352PayloadByte3BitDepth, _TRUNCATE);
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::get_audio_format(GigabyteAudioInfo &out)
{
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    bool refresh = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        refresh = !audio_info_valid_;
    }
    if (refresh)
    {
        const gvfg_status_t status = refresh_audio_info();
        if (status != GVFG_OK)
            return status;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    out = {};
    out.sample_rate = audio_info_.SamplesPerSec;
    out.channels = audio_info_.Channels;
    out.bits_per_sample = audio_info_.BitsPerSample;
    out.frame_bytes = audio_info_.cbBufSize;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::refresh_audio_info()
{
    GVFG_AUDIO_INFO info{};
    const gvfg_status_t status = from_vendor(
        GvfgGetAudioInfo(context_, channel_, &info));
    if (status != GVFG_OK)
        return status;
    std::lock_guard<std::mutex> lock(state_mutex_);
    audio_info_ = info;
    audio_info_valid_ = true;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::set_event_callback(
    GigabyteEventCallback callback, void *user)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = callback;
    event_callback_user_ = user;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::configure_stream()
{
    if (running_)
        return reject(GVFG_ESTATE, "GigabyteLib stream is already running");
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    configured_ = true;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::start_stream()
{
    if (running_)
        return GVFG_OK;
    if (!configured_)
        return reject(GVFG_ESTATE, "GigabyteLib stream has not been configured");
    ResetEvent(stop_event_);
    ResetEvent(recovery_event_);
    recovery_pending_ = false;
    recovery_status_ = GVFG_OK;
    std::lock_guard<std::mutex> captureLock(capture_mutex_);
    const gvfg_status_t status = from_vendor(GvfgStartCapture(context_, channel_));
    if (status != GVFG_OK)
        return status;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        frame_held_ = false;
    }
    running_ = true;
    start_event_monitoring();
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::stop_stream()
{
    const bool wasRunning = running_.exchange(false);
    recovery_pending_ = false;
    recovery_status_ = GVFG_OK;
    if (stop_event_)
        SetEvent(stop_event_);
    if (recovery_event_)
        SetEvent(recovery_event_);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        frame_held_ = false;
    }
    recovery_cv_.notify_all();
    stop_event_monitoring();
    gvfg_status_t result = GVFG_OK;
    if (wasRunning && channel_open_ && context_)
    {
        std::lock_guard<std::mutex> captureLock(capture_mutex_);
        const gvfg_status_t stopStatus = from_vendor(GvfgStopCapture(context_, channel_));
        if (result == GVFG_OK)
            result = stopStatus;
    }
    return result;
}

gvfg_status_t GigabyteCaptureSession::wait_frame(uint32_t timeoutMs, gvfg_frame_t &out)
{
    out = {};
    if (!running_ || !context_)
        return reject(GVFG_ESTATE, "GigabyteLib frame read rejected: stream is not running");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (frame_held_)
            return reject(GVFG_ESTATE, "GigabyteLib frame read rejected: previous frame is held");
    }

    std::array<HANDLE, 4> handles{stop_event_, recovery_event_,
                                  events_.hVideoFrameInEvent, events_.hVideoExtraFrame};
    const DWORD count = handles[3] ? 4u : 3u;
    if (!handles[2])
        return reject(GVFG_EIO, "GigabyteLib did not provide a video-frame event");
    const DWORD waitMs = timeoutMs == UINT32_MAX ? INFINITE : timeoutMs;
    const DWORD wait = WaitForMultipleObjects(count, handles.data(), FALSE, waitMs);
    if (wait == WAIT_TIMEOUT)
        return GVFG_ETIMEOUT;
    if (wait == WAIT_OBJECT_0)
        return GVFG_ESTATE;
    if (wait == WAIT_OBJECT_0 + 1)
        return wait_for_recovery(timeoutMs);
    if (wait != WAIT_OBJECT_0 + 2 && wait != WAIT_OBJECT_0 + 3)
        return reject(GVFG_EIO, "GigabyteLib video event wait failed");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (wait == WAIT_OBJECT_0 + 2)
            ++video_dma_event_wakes_;
        else
            ++extra_video_event_wakes_;
    }

    gvfg_signal_status_t signal{};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        signal = cached_signal_;
    }
    if (!signal.connected || signal.width == 0 || signal.height == 0)
        return GVFG_ETIMEOUT;

    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        bytes = video_info_.cbBufSize;
    }
    const void *data = nullptr;
    gvfg_status_t status = GVFG_OK;
    if (zero_copy_enabled_)
    {
        void *buffer = nullptr;
        const auto begin = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> captureLock(capture_mutex_);
            status = from_vendor(GvfgGetVideoFrameZeroCopy(context_, channel_, &buffer));
        }
        const auto end = std::chrono::steady_clock::now();
        record_get_frame_timing(std::chrono::duration<double, std::micro>(end - begin).count());
        data = buffer;
    }
    else
    {
        if (copy_buffer_.size() != bytes)
            copy_buffer_.assign(bytes, 0);
        const auto begin = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> captureLock(capture_mutex_);
            status = from_vendor(GvfgGetVideoFrame(context_, channel_, copy_buffer_.data(),
                                                   static_cast<ULONG>(copy_buffer_.size())));
        }
        const auto end = std::chrono::steady_clock::now();
        record_get_frame_timing(std::chrono::duration<double, std::micro>(end - begin).count());
        data = copy_buffer_.data();
    }
    if (status != GVFG_OK)
        return status;
    if (!data || bytes == 0)
        return reject(GVFG_EIO, "GigabyteLib returned an invalid video frame");

    std::lock_guard<std::mutex> lock(state_mutex_);
    out.data = data;
    out.data_size = bytes;
    out.frame_id = 0;
    out.timestamp_ns = monotonic_ns();
    out.width = signal.width;
    out.height = signal.height;
    out.row_stride_bytes = static_cast<int>(bytes / signal.height);
    out.pixel_format = signal.pixel_format;
    out.bit_depth = signal.bit_depth;
    ++video_frames_from_lib_;
    frame_held_ = true;
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::release_frame()
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!frame_held_)
            return reject(GVFG_ESTATE, "GigabyteLib frame release rejected: no frame is held");
        // Current GigabyteLib zero-copy contract requires no per-frame release call.
        frame_held_ = false;
    }
    recovery_cv_.notify_all();
    return GVFG_OK;
}

void GigabyteCaptureSession::get_cached_signal_status(gvfg_signal_status_t &out) const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    out = cached_signal_;
}

gvfg_status_t GigabyteCaptureSession::wait_audio(
    uint32_t timeoutMs, void *destination, uint32_t destinationCapacity, uint32_t &outBytes)
{
    outBytes = 0;
    if (!running_ || !audio_enabled_ || !destination)
        return reject(GVFG_ESTATE, "GigabyteLib audio read rejected");
    uint32_t frameBytes = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        frameBytes = audio_info_.cbBufSize;
    }
    if (frameBytes == 0 || frameBytes > destinationCapacity)
        return reject(GVFG_EINVAL, "GigabyteLib audio destination is too small");
    if (!events_.hAudioFrameInEvent)
        return reject(GVFG_EIO, "GigabyteLib did not provide an audio-frame event");

    std::array<HANDLE, 4> handles{stop_event_, recovery_event_,
                                  events_.hAudioFrameInEvent, events_.hAudioExtraFrame};
    const DWORD count = handles[3] ? 4u : 3u;
    const DWORD waitMs = timeoutMs == UINT32_MAX ? INFINITE : timeoutMs;
    const DWORD wait = WaitForMultipleObjects(count, handles.data(), FALSE, waitMs);
    if (wait == WAIT_TIMEOUT)
        return GVFG_ETIMEOUT;
    if (wait == WAIT_OBJECT_0)
        return GVFG_ESTATE;
    if (wait == WAIT_OBJECT_0 + 1)
        return wait_for_recovery(timeoutMs);
    if (wait != WAIT_OBJECT_0 + 2 && wait != WAIT_OBJECT_0 + 3)
        return reject(GVFG_EIO, "GigabyteLib audio event wait failed");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (wait == WAIT_OBJECT_0 + 2)
            ++audio_event_wakes_;
        else
            ++extra_audio_event_wakes_;
    }
    gvfg_status_t status = GVFG_OK;
    {
        std::lock_guard<std::mutex> captureLock(capture_mutex_);
        status = from_vendor(GvfgGetAudioFrame(context_, channel_, destination, frameBytes));
    }
    if (status != GVFG_OK)
        return status;
    outBytes = frameBytes;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++audio_frames_from_lib_;
    }
    return GVFG_OK;
}

void GigabyteCaptureSession::start_event_monitoring()
{
    if (monitoring_)
        return;
    monitoring_ = true;
    event_thread_ = std::thread(&GigabyteCaptureSession::event_thread_proc, this);
}

void GigabyteCaptureSession::stop_event_monitoring()
{
    monitoring_ = false;
    if (stop_event_)
        SetEvent(stop_event_);
    if (event_thread_.joinable())
        event_thread_.join();
}

void GigabyteCaptureSession::event_thread_proc()
{
    const std::array<HANDLE, 4> handles{stop_event_, events_.hVideoFormatChangedEvent,
                                        events_.hVideoInputPluginEvent, events_.hVideoInputUnplugEvent};
    while (monitoring_)
    {
        std::array<HANDLE, 4> active{};
        std::array<int, 4> kinds{};
        DWORD count = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (handles[static_cast<size_t>(i)])
            {
                active[count] = handles[static_cast<size_t>(i)];
                kinds[count] = i;
                ++count;
            }
        }
        if (count == 0)
            return;
        const DWORD wait = WaitForMultipleObjects(count, active.data(), FALSE, INFINITE);
        if (wait < WAIT_OBJECT_0 || wait >= WAIT_OBJECT_0 + count)
            return;
        const int kind = kinds[wait - WAIT_OBJECT_0];
        if (kind == 0 || !monitoring_)
            return;
        if (kind == 1)
        {
            refresh_video_info();
            emit_event(GVFG_EVENT_VIDEO_FORMAT_CHANGED);
        }
        else if (kind == 2)
        {
            refresh_video_info();
            if (running_)
            {
                recovery_status_ = GVFG_OK;
                recovery_pending_ = true;
                SetEvent(recovery_event_);
                recover_stream_after_plugin();
            }
            emit_event(GVFG_EVENT_VIDEO_INPUT_PLUGIN);
        }
        else if (kind == 3)
        {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cached_signal_ = {};
                cached_signal_.channel = channel_;
                video_info_valid_ = false;
            }
            emit_event(GVFG_EVENT_VIDEO_INPUT_UNPLUG);
        }
    }
}

void GigabyteCaptureSession::emit_event(gvfg_event_type_t type) const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (event_callback_)
        event_callback_(type, event_callback_user_);
}

void GigabyteCaptureSession::fill_debug_stats(gvfg_debug_backend_stats_t &out) const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    out.video_dma_event_wakes = video_dma_event_wakes_;
    out.extra_video_event_wakes = extra_video_event_wakes_;
    out.video_frames_from_lib = video_frames_from_lib_;
    out.audio_dma_event_wakes = audio_event_wakes_;
    out.extra_audio_event_wakes = extra_audio_event_wakes_;
    out.audio_frames_from_lib = audio_frames_from_lib_;
    out.get_frame_timing_samples = get_frame_timing_samples_;
    out.get_frame_timing_average_us = get_frame_timing_samples_
        ? get_frame_timing_total_us_ / static_cast<double>(get_frame_timing_samples_) : 0.0;
    out.get_frame_timing_max300_us = get_frame_timing_last_max300_us_;
    out.get_frame_timing_max_us = get_frame_timing_lifetime_max_us_;
}

gvfg_status_t GigabyteCaptureSession::debug_read_register(uint32_t offset, uint32_t &outValue)
{
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    if (!gigabyte_read_register(device_handle_, offset, outValue))
        return reject(GVFG_EIO,
                      "GigabyteLib gap: driver extension register read failed");
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::debug_write_register(uint32_t offset, uint32_t value)
{
    const gvfg_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GVFG_OK)
        return openStatus;
    if (!gigabyte_write_register(device_handle_, offset, value))
        return reject(GVFG_EIO,
                      "GigabyteLib gap: driver extension register write failed");
    return GVFG_OK;
}

gvfg_status_t GigabyteCaptureSession::from_vendor(GVFG_HRESULT result)
{
    return static_cast<gvfg_status_t>(result);
}

gvfg_status_t GigabyteCaptureSession::recover_stream_after_plugin()
{
    {
        std::unique_lock<std::mutex> stateLock(state_mutex_);
        recovery_cv_.wait(stateLock, [this]() { return !frame_held_ || !running_; });
    }

    std::lock_guard<std::mutex> captureLock(capture_mutex_);
    if (!recovery_pending_)
    {
        ResetEvent(recovery_event_);
        return GVFG_OK;
    }
    if (!running_)
    {
        recovery_pending_ = false;
        ResetEvent(recovery_event_);
        recovery_cv_.notify_all();
        return GVFG_OK;
    }

    gvfg_status_t status = from_vendor(GvfgStopCapture(context_, channel_));
    if (status == GVFG_OK)
        status = from_vendor(GvfgStartCapture(context_, channel_));
    recovery_status_ = status;
    recovery_pending_ = false;
    ResetEvent(recovery_event_);
    recovery_cv_.notify_all();
    return status;
}

gvfg_status_t GigabyteCaptureSession::wait_for_recovery(uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    const auto complete = [this]() { return !recovery_pending_ || !running_; };
    if (timeoutMs == UINT32_MAX)
        recovery_cv_.wait(lock, complete);
    else if (!recovery_cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), complete))
        return GVFG_ETIMEOUT;
    if (!running_)
        return GVFG_ESTATE;
    const gvfg_status_t status = recovery_status_.load();
    return status == GVFG_OK ? GVFG_ETIMEOUT : status;
}

gvfg_status_t GigabyteCaptureSession::reject(gvfg_status_t status, const char *message) const
{
    error_state_.set(message ? message : "GigabyteLib operation rejected");
    return status;
}

void GigabyteCaptureSession::record_get_frame_timing(double elapsedUs)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++get_frame_timing_samples_;
    get_frame_timing_total_us_ += elapsedUs;
    get_frame_timing_window_max_us_ = (std::max)(get_frame_timing_window_max_us_, elapsedUs);
    get_frame_timing_lifetime_max_us_ = (std::max)(get_frame_timing_lifetime_max_us_, elapsedUs);
    if (++get_frame_timing_window_samples_ == 300)
    {
        get_frame_timing_last_max300_us_ = get_frame_timing_window_max_us_;
        get_frame_timing_window_samples_ = 0;
        get_frame_timing_window_max_us_ = 0.0;
    }
}
}
