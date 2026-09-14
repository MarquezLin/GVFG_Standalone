#include "gigabyte_capture_session.h"
#include "gigabyte_driver_extensions.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

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

gigabyte_pixel_format_t decode_fourcc(uint32_t fourcc)
{
    if (fourcc == kFourccY210)
        return GIGABYTE_PIXFMT_Y210;
    if (fourcc == kFourccYuy2 || fourcc == kFourccYuyv || fourcc == kFourccUyvy)
        return GIGABYTE_PIXFMT_YUY2;
    return GIGABYTE_PIXFMT_UNKNOWN;
}

uint32_t bit_depth(gigabyte_pixel_format_t format)
{
    return format == GIGABYTE_PIXFMT_Y210 ? 10u :
           format == GIGABYTE_PIXFMT_YUY2 ? 8u : 0u;
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

gigabyte_status_t GigabyteCaptureSession::open_device_index(size_t deviceIndex)
{
    const auto devices = enumerate_gigabyte_devices();
    if (deviceIndex >= devices.size())
        return reject(GIGABYTE_ENODEV, "GigabyteLib open rejected: device index is unavailable");
    return open_device(devices[deviceIndex]);
}

gigabyte_status_t GigabyteCaptureSession::open_device_connection(
    const std::shared_ptr<GigabyteDeviceConnection> &connection)
{
    if (!connection || connection->interface_path.empty())
        return reject(GIGABYTE_EINVAL, "GigabyteLib open rejected: device selection is invalid");
    close();
    device_connection_ = connection;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::open_device(const GigabyteDevice &device)
{
    close();
    device_connection_ = std::make_shared<GigabyteDeviceConnection>();
    device_connection_->interface_path = device.interface_path;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::ensure_vendor_channel_open() const
{
    if (channel_open_)
        return GIGABYTE_OK;
    if (!device_connection_ || device_connection_->interface_path.empty())
        return reject(GIGABYTE_ESTATE, "GigabyteLib channel open rejected: no device is selected");

    device_handle_ = CreateFileW(device_connection_->interface_path.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device_handle_ == INVALID_HANDLE_VALUE)
        return reject(GIGABYTE_ENODEV, "CreateFile for GigabyteLib failed");

    const ULONG memoryMode = zero_copy_enabled_
                                 ? GVFG_VIDEO_FRAME_ZERO_COPY | GVFG_AUDIO_FRAME_BUF_COPY
                                 : GVFG_VIDEO_FRAME_BUF_COPY | GVFG_AUDIO_FRAME_BUF_COPY;
    gigabyte_status_t status = from_vendor(
        GvfgOpenDev(device_handle_, &context_, memoryMode), "GvfgOpenDev");
    if (status != GIGABYTE_OK)
    {
        CloseHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
        context_ = nullptr;
        return status;
    }

    status = from_vendor(GvfgCreateEvents(&events_, audio_enabled_ ? FALSE : TRUE),
                         "GvfgCreateEvents");
    if (status != GIGABYTE_OK)
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
        return reject(GIGABYTE_EIO, "CreateEvent for GigabyteLib cancellation failed");
    }

    status = from_vendor(GvfgOpenVideoChn(context_, channel_, &events_),
                         "GvfgOpenVideoChn");
    if (status != GIGABYTE_OK)
    {
        close_vendor_channel();
        return status;
    }
    channel_open_ = true;
    return refresh_video_info();
}

void GigabyteCaptureSession::close_vendor_channel() const
{
    if (channel_open_ && context_)
        GvfgCloseVideoChn(context_, channel_);
    channel_open_ = false;
    if (context_)
        GvfgCloseDev(&context_);
    if (events_created_)
        GvfgDestroyEvents(&events_);
    events_created_ = false;
    events_ = {};
    if (stop_event_)
        CloseHandle(stop_event_);
    stop_event_ = nullptr;
    if (device_handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(device_handle_);
    device_handle_ = INVALID_HANDLE_VALUE;
}

gigabyte_status_t GigabyteCaptureSession::close()
{
    stop_stream();
    close_vendor_channel();
    device_connection_.reset();
    configured_ = false;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::set_channel(uint32_t channel)
{
    if (channel > 1)
        return reject(GIGABYTE_EINVAL, "GigabyteLib channel index is invalid");
    if (channel_open_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib channel is already open");
    channel_ = channel;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::set_zero_copy_enabled(bool enabled)
{
    if (channel_open_ || running_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib frame memory mode is already fixed");
    zero_copy_enabled_ = enabled;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::set_audio_enabled(bool enabled)
{
    if (running_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib audio mode cannot change while running");
    if (channel_open_ && audio_enabled_ != enabled)
        close_vendor_channel();
    audio_enabled_ = enabled;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::set_video_format(gigabyte_pixel_format_t format)
{
    if (format != GIGABYTE_PIXFMT_YUY2 && format != GIGABYTE_PIXFMT_Y210)
        return reject(GIGABYTE_EINVAL, "video format is invalid");
    if (running_)
        return reject(GIGABYTE_ESTATE, "video format cannot change while capture is running");
    const gigabyte_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GIGABYTE_OK)
        return openStatus;

    constexpr uint32_t kVideoOutputFormatRegister = 0x080;
    if (!gigabyte_write_register(device_handle_, kVideoOutputFormatRegister,
                                  format == GIGABYTE_PIXFMT_Y210 ? 1u : 0u))
        return reject(GIGABYTE_EIO,
                      "GigabyteLib gap: driver extension failed to set the output format register");
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::refresh_video_info() const
{
    if (!context_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib video info requested before open");
    GVFG_VIDEO_INFO info{};
    const gigabyte_status_t status = from_vendor(
        GvfgGetVideoInfo(context_, channel_, &info), "GvfgGetVideoInfo");
    if (status != GIGABYTE_OK)
        return status;
    gigabyte_signal_status_t signal{};
    signal.connected = info.VideoSignalLock ? 1 : 0;
    signal.channel = channel_;
    signal.width = info.Width;
    signal.height = info.Height;
    signal.pixel_format = decode_fourcc(info.Fourcc);
    signal.bit_depth = bit_depth(signal.pixel_format);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cached_signal_ = signal;
    }
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::get_signal_status(gigabyte_signal_status_t &out) const
{
    const gigabyte_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GIGABYTE_OK)
        return openStatus;
    const gigabyte_status_t status = refresh_video_info();
    if (status == GIGABYTE_OK)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        out = cached_signal_;
    }
    return status;
}

gigabyte_status_t GigabyteCaptureSession::get_audio_format(gigabyte_audio_format_t &out) const
{
    const gigabyte_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GIGABYTE_OK)
        return openStatus;
    const gigabyte_status_t status = from_vendor(
        GvfgGetAudioInfo(context_, channel_, &audio_info_), "GvfgGetAudioInfo");
    if (status != GIGABYTE_OK)
        return status;
    out = {};
    out.sample_rate = audio_info_.SamplesPerSec;
    out.channels = audio_info_.Channels;
    out.bits_per_sample = audio_info_.BitsPerSample;
    out.frames_per_second = audio_info_.FramesPerSec;
    out.frame_bytes = audio_info_.cbBufSize;
    out.block_align = static_cast<uint32_t>(audio_info_.Channels) *
                      static_cast<uint32_t>(audio_info_.BitsPerSample) / 8u;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::set_event_callback(
    gigabyte_event_callback_t callback, void *user, uint32_t eventMask)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = callback;
    event_callback_user_ = user;
    event_mask_filter_ = eventMask;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::configure_stream()
{
    if (running_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib stream is already running");
    const gigabyte_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GIGABYTE_OK)
        return openStatus;
    configured_ = true;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::start_stream()
{
    if (running_)
        return GIGABYTE_OK;
    if (!configured_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib stream has not been configured");
    ResetEvent(stop_event_);
    const gigabyte_status_t status = from_vendor(
        GvfgStartCapture(context_, channel_), "GvfgStartCapture");
    if (status != GIGABYTE_OK)
        return status;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        held_frame_ = {};
        frame_held_ = false;
        frame_id_ = 0;
    }
    running_ = true;
    start_event_monitoring();
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::stop_stream()
{
    const bool wasRunning = running_.exchange(false);
    if (stop_event_)
        SetEvent(stop_event_);
    stop_event_monitoring();

    gigabyte_status_t result = GIGABYTE_OK;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (frame_held_ && zero_copy_enabled_ && context_)
            result = from_vendor(GvfgReleaseVideoFrameZeroCopy(context_, channel_),
                                 "GvfgReleaseVideoFrameZeroCopy during stop");
        held_frame_ = {};
        frame_held_ = false;
    }
    if (wasRunning && channel_open_ && context_)
    {
        const gigabyte_status_t stopStatus = from_vendor(
            GvfgStopCapture(context_, channel_), "GvfgStopCapture");
        if (result == GIGABYTE_OK)
            result = stopStatus;
    }
    return result;
}

gigabyte_status_t GigabyteCaptureSession::wait_frame(uint32_t timeoutMs, gigabyte_frame_t &out)
{
    out = {};
    if (!running_ || !context_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib frame read rejected: stream is not running");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (frame_held_)
            return reject(GIGABYTE_ESTATE, "GigabyteLib frame read rejected: previous frame is held");
    }

    std::array<HANDLE, 3> handles{stop_event_, events_.hVideoFrameInEvent, events_.hVideoExtraFrame};
    const DWORD count = handles[2] ? 3u : 2u;
    if (!handles[1])
        return reject(GIGABYTE_EIO, "GigabyteLib did not provide a video-frame event");
    const DWORD waitMs = timeoutMs == UINT32_MAX ? INFINITE : timeoutMs;
    const DWORD wait = WaitForMultipleObjects(count, handles.data(), FALSE, waitMs);
    if (wait == WAIT_TIMEOUT)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++wait_timeout_count_;
        return GIGABYTE_ETIMEOUT;
    }
    if (wait == WAIT_OBJECT_0)
        return GIGABYTE_ESTATE;
    if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_OBJECT_0 + 2)
        return reject(GIGABYTE_EIO, "GigabyteLib video event wait failed");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++video_event_wakes_;
    }

    const auto begin = std::chrono::steady_clock::now();
    const gigabyte_status_t infoStatus = refresh_video_info();
    if (infoStatus != GIGABYTE_OK)
        return infoStatus;
    gigabyte_signal_status_t signal{};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        signal = cached_signal_;
    }
    if (!signal.connected || signal.width == 0 || signal.height == 0)
        return GIGABYTE_ETIMEOUT;

    const size_t bytes = frame_size_bytes();
    const void *data = nullptr;
    gigabyte_status_t status = GIGABYTE_OK;
    if (zero_copy_enabled_)
    {
        void *buffer = nullptr;
        status = from_vendor(GvfgGetVideoFrameZeroCopy(context_, channel_, &buffer),
                             "GvfgGetVideoFrameZeroCopy");
        data = buffer;
    }
    else
    {
        if (copy_buffer_.size() != bytes)
            copy_buffer_.assign(bytes, 0);
        status = from_vendor(GvfgGetVideoFrame(context_, channel_, copy_buffer_.data(),
                                               static_cast<ULONG>(copy_buffer_.size())),
                             "GvfgGetVideoFrame");
        data = copy_buffer_.data();
    }
    const auto end = std::chrono::steady_clock::now();
    record_get_frame_timing(std::chrono::duration<double, std::micro>(end - begin).count());
    if (status != GIGABYTE_OK)
        return status;
    if (!data || bytes == 0)
        return reject(GIGABYTE_EIO, "GigabyteLib returned an invalid video frame");

    std::lock_guard<std::mutex> lock(state_mutex_);
    out.data = data;
    out.data_size_bytes = bytes;
    out.frame_id = ++frame_id_;
    out.timestamp_ns = monotonic_ns();
    out.width = signal.width;
    out.height = signal.height;
    out.pixel_format = signal.pixel_format;
    out.bit_depth = signal.bit_depth;
    held_frame_ = out;
    frame_held_ = true;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::release_frame(const gigabyte_frame_t &frame)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!frame_held_)
        return reject(GIGABYTE_ESTATE, "GigabyteLib frame release rejected: no frame is held");
    if (frame.data != held_frame_.data || frame.frame_id != held_frame_.frame_id ||
        frame.data_size_bytes != held_frame_.data_size_bytes)
        return reject(GIGABYTE_EINVAL, "GigabyteLib frame release token does not match");
    if (zero_copy_enabled_)
    {
        const gigabyte_status_t status = from_vendor(
            GvfgReleaseVideoFrameZeroCopy(context_, channel_), "GvfgReleaseVideoFrameZeroCopy");
        if (status != GIGABYTE_OK)
            return status;
    }
    held_frame_ = {};
    frame_held_ = false;
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::wait_audio(
    uint32_t timeoutMs, void *destination, uint32_t destinationCapacity, uint32_t &outBytes)
{
    outBytes = 0;
    if (!running_ || !audio_enabled_ || !destination)
        return reject(GIGABYTE_ESTATE, "GigabyteLib audio read rejected");
    gigabyte_audio_format_t format{};
    const gigabyte_status_t formatStatus = get_audio_format(format);
    if (formatStatus != GIGABYTE_OK)
        return formatStatus;
    if (format.frame_bytes == 0 || format.frame_bytes > destinationCapacity)
        return reject(GIGABYTE_EINVAL, "GigabyteLib audio destination is too small");
    if (!events_.hAudioFrameInEvent)
        return reject(GIGABYTE_EIO, "GigabyteLib did not provide an audio-frame event");

    std::array<HANDLE, 3> handles{stop_event_, events_.hAudioFrameInEvent, events_.hAudioExtraFrame};
    const DWORD count = handles[2] ? 3u : 2u;
    const DWORD waitMs = timeoutMs == UINT32_MAX ? INFINITE : timeoutMs;
    const DWORD wait = WaitForMultipleObjects(count, handles.data(), FALSE, waitMs);
    if (wait == WAIT_TIMEOUT)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++wait_timeout_count_;
        return GIGABYTE_ETIMEOUT;
    }
    if (wait == WAIT_OBJECT_0)
        return GIGABYTE_ESTATE;
    if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_OBJECT_0 + 2)
        return reject(GIGABYTE_EIO, "GigabyteLib audio event wait failed");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (wait == WAIT_OBJECT_0 + 1)
            ++audio_event_wakes_;
        else
            ++extra_audio_event_wakes_;
    }
    const gigabyte_status_t status = from_vendor(
        GvfgGetAudioFrame(context_, channel_, destination, format.frame_bytes), "GvfgGetAudioFrame");
    if (status != GIGABYTE_OK)
        return status;
    outBytes = format.frame_bytes;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++audio_frames_from_driver_;
        audio_bytes_from_driver_ += outBytes;
    }
    return GIGABYTE_OK;
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
            emit_event(GIGABYTE_EVENT_FORMAT_CHANGE_BEGIN);
            refresh_video_info();
            emit_event(GIGABYTE_EVENT_STREAM_READY);
        }
        else if (kind == 2)
        {
            refresh_video_info();
            emit_event(GIGABYTE_EVENT_PLUG_IN);
            emit_event(GIGABYTE_EVENT_STREAM_READY);
        }
        else if (kind == 3)
        {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cached_signal_ = {};
                cached_signal_.channel = channel_;
            }
            emit_event(GIGABYTE_EVENT_PLUG_OUT);
        }
    }
}

void GigabyteCaptureSession::emit_event(gigabyte_event_type_t type) const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    uint32_t bit = 0;
    if (type == GIGABYTE_EVENT_PLUG_IN) bit = GIGABYTE_EVENT_MASK_PLUG_IN;
    if (type == GIGABYTE_EVENT_PLUG_OUT) bit = GIGABYTE_EVENT_MASK_PLUG_OUT;
    if (type == GIGABYTE_EVENT_STREAM_READY) bit = GIGABYTE_EVENT_MASK_STREAM_READY;
    if (type == GIGABYTE_EVENT_FORMAT_CHANGE_BEGIN) bit = GIGABYTE_EVENT_MASK_FORMAT_CHANGE_BEGIN;
    if (event_callback_ && (event_mask_filter_ & bit) != 0)
        event_callback_(type, event_callback_user_);
}

void GigabyteCaptureSession::get_debug_stats(
    gigabyte_stream_stats_t &outStats, uint64_t &outWaitTimeouts,
    gigabyte_debug_state_t &outDebugState) const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    outStats = {};
    outStats.state = running_ ? GIGABYTE_STREAM_RUNNING :
                     configured_ ? GIGABYTE_STREAM_CONFIGURED : GIGABYTE_STREAM_STOPPED;
    outStats.frames_captured = video_event_wakes_;
    outStats.frames_delivered = frame_id_;
    outStats.interrupt_count = video_event_wakes_;
    outWaitTimeouts = wait_timeout_count_;
    outDebugState = {};
    outDebugState.running = running_ ? 1 : 0;
    outDebugState.capture_active = running_ ? 1 : 0;
    outDebugState.latest_sequence = video_event_wakes_;
    outDebugState.delivered_sequence = frame_id_;
    outDebugState.audio_dma_event_wakes = audio_event_wakes_;
    outDebugState.extra_audio_event_wakes = extra_audio_event_wakes_;
    outDebugState.audio_frames_from_driver = audio_frames_from_driver_;
    outDebugState.audio_bytes_from_driver = audio_bytes_from_driver_;
    outDebugState.get_frame_zero_copy = zero_copy_enabled_ ? 1 : 0;
    outDebugState.get_frame_timing_samples = get_frame_timing_samples_;
    outDebugState.get_frame_timing_average_us = get_frame_timing_samples_
        ? get_frame_timing_total_us_ / static_cast<double>(get_frame_timing_samples_) : 0.0;
    outDebugState.get_frame_timing_max300_us = get_frame_timing_last_max300_us_;
    outDebugState.get_frame_timing_max_us = get_frame_timing_lifetime_max_us_;
}

gigabyte_status_t GigabyteCaptureSession::debug_read_register(uint32_t offset, uint32_t &outValue) const
{
    const gigabyte_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GIGABYTE_OK)
        return openStatus;
    if (!gigabyte_read_register(device_handle_, offset, outValue))
        return reject(GIGABYTE_EIO,
                      "GigabyteLib gap: driver extension register read failed");
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::debug_write_register(uint32_t offset, uint32_t value) const
{
    const gigabyte_status_t openStatus = ensure_vendor_channel_open();
    if (openStatus != GIGABYTE_OK)
        return openStatus;
    if (!gigabyte_write_register(device_handle_, offset, value))
        return reject(GIGABYTE_EIO,
                      "GigabyteLib gap: driver extension register write failed");
    return GIGABYTE_OK;
}

gigabyte_status_t GigabyteCaptureSession::from_vendor(GVFG_HRESULT result, const char *operation) const
{
    if (result == GVFG_HRESULT_OK)
        return GIGABYTE_OK;
    gigabyte_status_t status = GIGABYTE_EIO;
    if (result == GVFG_HRESULT_DEV_BUSY || result == GVFG_HRESULT_CONTEXT_ERROR)
        status = GIGABYTE_ESTATE;
    else if (result == GVFG_HRESULT_VIDEO_CHN_INVALID)
        status = GIGABYTE_EINVAL;
    else if (result == GVFG_HRESULT_DEV_ERROR)
        status = GIGABYTE_ENODEV;
    std::ostringstream stream;
    stream << (operation ? operation : "GigabyteLib")
           << " failed with GVFG_HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned int>(result);
    error_state_.set(stream.str());
    return status;
}

gigabyte_status_t GigabyteCaptureSession::reject(gigabyte_status_t status, const char *message) const
{
    error_state_.set(message ? message : "GigabyteLib operation rejected");
    return status;
}

size_t GigabyteCaptureSession::frame_size_bytes() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    const uint32_t width = cached_signal_.width;
    const uint32_t height = cached_signal_.height;
    const gigabyte_pixel_format_t format = cached_signal_.pixel_format;
    const size_t bytesPerPixel = format == GIGABYTE_PIXFMT_Y210 ? 4u : 2u;
    return static_cast<size_t>(width) * height * bytesPerPixel;
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
