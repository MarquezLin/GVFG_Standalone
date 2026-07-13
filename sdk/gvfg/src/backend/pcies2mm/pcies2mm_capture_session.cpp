#include "pcies2mm_capture_session.h"

#include <setupapi.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

#pragma comment(lib, "SetupAPI.lib")

namespace
{
    // New pcie_s2mm_driver device interface from CaptureDemo/video_card.cpp.
    const GUID GUID_DEVINTERFACE_PcieS2mm =
        {0x8c47b9c3, 0x1faa, 0x4557, {0xbc, 0x1d, 0xf2, 0x25, 0xd2, 0x6c, 0x9e, 0x91}};

    constexpr DWORD kIoctlWriteReg = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kIoctlReadReg = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kIoctlRegisterEvent = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kIoctlGetFrame = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS);
    constexpr DWORD kIoctlUnregisterEvent = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kIoctlWriteRegBar1 = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kIoctlReadRegBar1 = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kIoctlGetVideoDoneIndex = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS);

    constexpr uint32_t kEventTypeVideoDma = 0;
    constexpr uint32_t kEventTypeVideoFormatChange = 1;
    constexpr uint32_t kEventTypeVideoPlugin = 2;
    constexpr uint32_t kEventTypeVideoUnplug = 3;
    constexpr uint32_t kDmaBufferCount = 16;
    constexpr uint32_t kMaxChannels = 2;
    constexpr uint32_t kDefaultWidth = 1920;
    constexpr uint32_t kDefaultHeight = 1080;
    constexpr uint32_t kDefaultRingBufferCount = 3;
    constexpr uint32_t kMaxRingBufferCount = 16;

    constexpr uint32_t kInterruptBase = 0x00000000;
    constexpr uint32_t kCh0VideoBase = 0x00000200;
    constexpr uint32_t kCh1VideoBase = 0x00000400;
    constexpr uint32_t kIrqMaskW1sOffset = 0x004;
    constexpr uint32_t kIrqMaskW1cOffset = 0x008;
    constexpr uint32_t kVideoEnOffset = 0x000;
    constexpr uint32_t kVideoDmaEnOffset = 0x020;
    constexpr uint32_t kVideoHSizeOffset = 0x02c;
    constexpr uint32_t kVideoVSizeOffset = 0x030;
    constexpr uint32_t kVideoFormatOffset = 0x034;

    constexpr uint32_t kCh0VideoDmaIrqMask = 1u << 0;
    constexpr uint32_t kCh1VideoDmaIrqMask = 1u << 4;

    constexpr uint32_t fourcc(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
               (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
    }

    typedef struct _PCIES2MM_REG_ACCESS
    {
        ULONG Offset;
        ULONG Value;
    } PCIES2MM_REG_ACCESS;

    typedef struct _PCIES2MM_EVENT_REG
    {
        ULONG Type;
        ULONG ChannelIndex;
        HANDLE EventHandle;
    } PCIES2MM_EVENT_REG;

    static uint64_t steady_now_ns()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    static bool should_log_counter(uint64_t count)
    {
        return count <= 5 || (count % 60) == 0;
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
#if GVFG_PCIES2MM_DEBUG_LOG
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

    static pcies2mm_pixel_format_t decode_pixel_format(uint32_t raw)
    {
        switch (raw)
        {
        case fourcc('Y', 'U', 'Y', '2'):
            return PCIES2MM_PIXFMT_YUY2;
        case fourcc('U', 'Y', 'V', 'Y'):
            return PCIES2MM_PIXFMT_UYVY;
        case fourcc('N', 'V', '1', '2'):
            return PCIES2MM_PIXFMT_NV12;
        case fourcc('Y', '2', '1', '0'):
            return PCIES2MM_PIXFMT_Y210;
        case fourcc('P', '0', '1', '0'):
            return PCIES2MM_PIXFMT_P010;
        default:
            break;
        }

        switch (raw & 0x3u)
        {
        case 0:
            return PCIES2MM_PIXFMT_YUY2;
        case 1:
            return PCIES2MM_PIXFMT_RGB24;
        case 2:
            return PCIES2MM_PIXFMT_YUV444;
        case 3:
            return PCIES2MM_PIXFMT_NV12;
        default:
            return PCIES2MM_PIXFMT_UNKNOWN;
        }
    }

    static uint32_t old_style_format_code(pcies2mm_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case PCIES2MM_PIXFMT_RGB24:
            return 1;
        case PCIES2MM_PIXFMT_YUV444:
            return 2;
        case PCIES2MM_PIXFMT_NV12:
        case PCIES2MM_PIXFMT_P010:
            return 3;
        case PCIES2MM_PIXFMT_YUY2:
        case PCIES2MM_PIXFMT_UYVY:
        case PCIES2MM_PIXFMT_Y210:
        default:
            return 0;
        }
    }

    static uint32_t bit_depth_for_pixfmt(pcies2mm_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case PCIES2MM_PIXFMT_P010:
        case PCIES2MM_PIXFMT_Y210:
            return 10;
        default:
            return 8;
        }
    }

    static size_t bytes_per_frame(uint32_t width, uint32_t height, pcies2mm_pixel_format_t fmt, uint32_t bitDepth)
    {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        switch (fmt)
        {
        case PCIES2MM_PIXFMT_NV12:
            return pixels * 3u / 2u;
        case PCIES2MM_PIXFMT_P010:
            return pixels * 3u;
        case PCIES2MM_PIXFMT_RGB24:
        case PCIES2MM_PIXFMT_YUV444:
            return bitDepth > 8u ? pixels * 6u : pixels * 3u;
        case PCIES2MM_PIXFMT_Y210:
            return pixels * 4u;
        case PCIES2MM_PIXFMT_YUY2:
        case PCIES2MM_PIXFMT_UYVY:
        default:
            return pixels * 2u;
        }
    }

    static uint32_t event_mask_for_type(pcies2mm_event_type_t type)
    {
        switch (type)
        {
        case PCIES2MM_EVENT_VIDEO_IRQ:
            return PCIES2MM_EVENT_MASK_VIDEO_IRQ;
        case PCIES2MM_EVENT_PLUG_IN:
            return PCIES2MM_EVENT_MASK_PLUG_IN;
        case PCIES2MM_EVENT_PLUG_OUT:
            return PCIES2MM_EVENT_MASK_PLUG_OUT;
        case PCIES2MM_EVENT_CAPTURE_PAUSED:
            return PCIES2MM_EVENT_MASK_CAPTURE_PAUSED;
        case PCIES2MM_EVENT_CAPTURE_RESUMED:
            return PCIES2MM_EVENT_MASK_CAPTURE_RESUMED;
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
    std::vector<PcieS2mmDevice> enumerate_pcies2mm_devices()
    {
        std::vector<PcieS2mmDevice> devices;
        HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_PcieS2mm, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
            return devices;

        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_DEVINTERFACE_PcieS2mm, index, &iface); ++index)
        {
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &required, nullptr);
            if (required == 0)
                continue;

            std::vector<uint8_t> detailBytes(required);
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detailBytes.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, required, nullptr, nullptr))
                continue;

            PcieS2mmDevice device;
            device.interface_path = detail->DevicePath;
            device.friendly_name = L"PcieS2mm Capture Device " + std::to_wstring(devices.size());
            devices.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(info);
        return devices;
    }

    PcieS2mmCaptureSession::PcieS2mmCaptureSession()
    {
        stream_desc_.input = PCIES2MM_INPUT_SDI;
        stream_desc_.width = kDefaultWidth;
        stream_desc_.height = kDefaultHeight;
        stream_desc_.pixel_format = PCIES2MM_PIXFMT_YUY2;
        stream_desc_.buffer_count = kDefaultRingBufferCount;
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

        device_ = CreateFileW(base_path_.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (device_ == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            close_handles();
            return fail(PCIES2MM_EIO, "CreateFile(PcieS2mm)", err);
        }

        opened_ = true;
        configured_ = false;
        clear_last_error();
        PCIES2MM_LOG("open_device: %s", wide_to_utf8(base_path_).c_str());
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::close()
    {
        stop_stream();
        close_handles();
        opened_ = false;
        configured_ = false;
        base_path_.clear();
        friendly_name_.clear();
        reset_stats(stats_, PCIES2MM_STREAM_STOPPED);
        return PCIES2MM_OK;
    }

    void PcieS2mmCaptureSession::close_handles()
    {
        close_event_handles();
        if (device_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
        }
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_input(pcies2mm_input_t input)
    {
        if (!opened_)
            return PCIES2MM_ESTATE;
        input_ = input == PCIES2MM_INPUT_HDMI ? PCIES2MM_INPUT_HDMI : PCIES2MM_INPUT_SDI;
        stream_desc_.input = input_;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::get_signal_status(pcies2mm_signal_status_t &out) const
    {
        if (!opened_)
            return PCIES2MM_ESTATE;

        std::memset(&out, 0, sizeof(out));
        uint32_t rawWidth = 0;
        uint32_t rawHeight = 0;
        uint32_t rawFormat = 0;
        const bool widthOk = read_reg(video_base() + kVideoHSizeOffset, rawWidth);
        const bool heightOk = read_reg(video_base() + kVideoVSizeOffset, rawHeight);
        const bool formatOk = read_reg(video_base() + kVideoFormatOffset, rawFormat);

        pcies2mm_pixel_format_t fmt = formatOk ? decode_pixel_format(rawFormat) : stream_desc_.pixel_format;
        if (fmt == PCIES2MM_PIXFMT_UNKNOWN)
            fmt = stream_desc_.pixel_format == PCIES2MM_PIXFMT_UNKNOWN ? PCIES2MM_PIXFMT_YUY2 : stream_desc_.pixel_format;

        const uint32_t width = (widthOk && rawWidth != 0) ? rawWidth : stream_desc_.width;
        const uint32_t height = (heightOk && rawHeight != 0) ? rawHeight : stream_desc_.height;
        const uint32_t bitDepth = bit_depth_for_pixfmt(fmt);
        const bool haveSize = width != 0 && height != 0;

        out.signal_locked = haveSize ? 1 : 0;
        out.input = input_;
        out.width = width;
        out.height = height;
        out.pixel_format = fmt;
        out.bit_depth = bitDepth;

        // gvfg_capture.cpp still expects the older FPGA status fields. The new
        // sample has no equivalent status register, so synthesize "ready" from
        // the channel resolution contract while preserving raw H/V when present.
        out.fpga_width_valid = haveSize ? 1u : 0u;
        out.fpga_height_valid = haveSize ? 1u : 0u;
        out.fpga_width_raw = width;
        out.fpga_height_raw = height;
        out.fpga_video_format_raw = old_style_format_code(fmt);
        out.fpga_frame_rate_raw = 0x0b; // 60 Hz placeholder until the new driver exposes timing metadata.
        out.fpga_bit_depth_raw = bitDepth;
        out.fpga_status_raw = active_channel() == 0 ? ((1u << 0) | (1u << 1))
                                                    : ((1u << 2) | (1u << 3));
        out.fpga_valid_mask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
        return haveSize ? PCIES2MM_OK : PCIES2MM_ENODEV;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::set_event_callback(pcies2mm_event_callback_t callback, void *user, uint32_t eventMask)
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
        event_callback_ = callback;
        event_callback_user_ = user;
        event_mask_filter_ = eventMask ? eventMask : PCIES2MM_EVENT_MASK_DEFAULT;
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
        case PCIES2MM_PIXFMT_UYVY:
            break;
        default:
            return fail(PCIES2MM_ENOTSUP, "configure_stream(pixel_format)", ERROR_NOT_SUPPORTED);
        }

        stream_desc_ = desc;
        stream_desc_.input = input_;
        stream_desc_.pixel_format = fmt;
        stream_desc_.buffer_count = std::clamp(desc.buffer_count ? desc.buffer_count : kDefaultRingBufferCount,
                                               1u,
                                               kMaxRingBufferCount);
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

        const size_t bytes = frame_size_bytes();
        if (bytes == 0 || bytes > (std::numeric_limits<DWORD>::max)())
            return fail(PCIES2MM_EINVAL, "frame_size_bytes", ERROR_INVALID_PARAMETER);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ring_.assign(stream_desc_.buffer_count, FrameSlot{});
            for (FrameSlot &slot : frame_ring_)
                slot.data.assign(bytes, 0);
            next_write_slot_ = 0;
            active_delivery_slot_ = static_cast<size_t>(-1);
            pending_events_ = 0;
            latest_sequence_ = 0;
            delivered_sequence_ = 0;
            wait_timeout_count_ = 0;
            stream_error_ = false;
            reset_stats(stats_, PCIES2MM_STREAM_RUNNING);
        }

        const uint32_t channel = active_channel();
        if (!create_and_register_events(channel))
        {
            const DWORD err = GetLastError();
            unregister_events(channel);
            close_event_handles();
            return fail(PCIES2MM_EIO, "REGISTER_EVENT", err);
        }

        running_ = true;
        capture_active_ = true;
        try
        {
            capture_thread_ = std::thread(&PcieS2mmCaptureSession::capture_thread_proc, this);
        }
        catch (...)
        {
            running_ = false;
            capture_active_ = false;
            unregister_events(channel);
            close_event_handles();
            return fail(PCIES2MM_EIO, "capture_thread", ERROR_NOT_ENOUGH_MEMORY);
        }

        const bool dmaEnableOk = write_reg(video_base() + kVideoDmaEnOffset, 1);
        const bool videoEnableOk = write_reg(video_base() + kVideoEnOffset, 1);
        if (!dmaEnableOk || !videoEnableOk)
        {
            const DWORD err = GetLastError();
            stop_stream();
            return fail(PCIES2MM_EIO, "enable video capture", err);
        }

        PCIES2MM_LOG("start: channel=%u base=0x%x bytes=%zu", channel, video_base(), bytes);
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::stop_stream()
    {
        if (!running_ && !capture_thread_.joinable())
            return PCIES2MM_OK;

        const uint32_t channel = active_channel();
        running_ = false;
        capture_active_ = false;

        if (device_ != INVALID_HANDLE_VALUE)
        {
            write_reg(video_base() + kVideoDmaEnOffset, 0);
            write_reg(video_base() + kVideoEnOffset, 0);
            write_reg(kInterruptBase + kIrqMaskW1cOffset, video_irq_mask_bit());
        }

        if (dma_event_)
            SetEvent(dma_event_);

        if (capture_thread_.joinable())
            capture_thread_.join();

        if (device_ != INVALID_HANDLE_VALUE)
            unregister_events(channel);

        close_event_handles();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_ = 0;
            active_delivery_slot_ = static_cast<size_t>(-1);
            for (FrameSlot &slot : frame_ring_)
                slot.in_use = false;
            stats_.state = configured_ ? PCIES2MM_STREAM_CONFIGURED : PCIES2MM_STREAM_STOPPED;
        }
        frame_cv_.notify_all();
        data_cv_.notify_all();
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::wait_frame(uint32_t timeoutMs, pcies2mm_frame_t &out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        std::memset(&out, 0, sizeof(out));
        if (!running_)
            return PCIES2MM_ESTATE;

        const auto hasFrame = [this]()
        {
            for (const FrameSlot &slot : frame_ring_)
            {
                if (slot.ready && slot.sequence > delivered_sequence_)
                    return true;
            }
            return stream_error_ || !running_ || !capture_active_.load(std::memory_order_acquire);
        };

        if (timeoutMs == 0)
        {
            frame_cv_.wait(lock, hasFrame);
        }
        else if (!frame_cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasFrame))
        {
            ++wait_timeout_count_;
            return PCIES2MM_ETIMEOUT;
        }

        size_t readySlot = frame_ring_.size();
        uint64_t readySequence = delivered_sequence_;
        for (size_t i = 0; i < frame_ring_.size(); ++i)
        {
            const FrameSlot &slot = frame_ring_[i];
            if (slot.ready && slot.sequence > readySequence)
            {
                readySlot = i;
                readySequence = slot.sequence;
            }
        }

        if (!running_ && readySlot == frame_ring_.size())
            return PCIES2MM_ESTATE;
        if (!capture_active_.load(std::memory_order_acquire) && readySlot == frame_ring_.size())
            return PCIES2MM_ESTATE;
        if (stream_error_ && readySlot == frame_ring_.size())
            return PCIES2MM_EIO;
        if (readySlot == frame_ring_.size())
            return PCIES2MM_ETIMEOUT;

        FrameSlot &slot = frame_ring_[readySlot];
        slot.ready = false;
        slot.in_use = true;
        active_delivery_slot_ = readySlot;
        delivered_sequence_ = slot.sequence;
        ++stats_.frames_delivered;

        out.data = slot.data.data();
        out.data_size_bytes = slot.bytes;
        out.frame_id = delivered_sequence_;
        out.width = stream_desc_.width;
        out.height = stream_desc_.height;
        out.pixel_format = stream_desc_.pixel_format;
        out.bit_depth = stream_bit_depth_;
        return PCIES2MM_OK;
    }

    pcies2mm_status_t PcieS2mmCaptureSession::release_frame(const pcies2mm_frame_t &frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_delivery_slot_ >= frame_ring_.size())
            return PCIES2MM_ESTATE;

        const FrameSlot &slot = frame_ring_[active_delivery_slot_];
        if (frame.data != slot.data.data() ||
            frame.data_size_bytes != slot.bytes ||
            frame.frame_id != delivered_sequence_ ||
            frame.width != stream_desc_.width ||
            frame.height != stream_desc_.height ||
            frame.pixel_format != stream_desc_.pixel_format ||
            frame.bit_depth != stream_bit_depth_)
            return PCIES2MM_EINVAL;

        frame_ring_[active_delivery_slot_].in_use = false;
        active_delivery_slot_ = static_cast<size_t>(-1);
        data_cv_.notify_all();
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
        outDebugState.data_worker_stop = 0;
        outDebugState.pending_events = pending_events_;
        outDebugState.latest_sequence = latest_sequence_;
        outDebugState.delivered_sequence = delivered_sequence_;
        outDebugState.active_delivery_slot = active_delivery_slot_ == static_cast<size_t>(-1)
                                                ? UINT64_MAX
                                                : static_cast<uint64_t>(active_delivery_slot_);
        outDebugState.next_write_slot = static_cast<uint64_t>(next_write_slot_);
        outDebugState.ring_size = static_cast<uint64_t>(frame_ring_.size());
    }

    bool PcieS2mmCaptureSession::read_reg(uint32_t offset, uint32_t &out) const
    {
        PCIES2MM_REG_ACCESS reg{};
        reg.Offset = offset;
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(device_,
                                        kIoctlReadReg,
                                        &reg,
                                        sizeof(reg),
                                        &reg,
                                        sizeof(reg),
                                        &bytesReturned,
                                        nullptr);
        if (!ok)
            return false;
        out = reg.Value;
        return true;
    }

    bool PcieS2mmCaptureSession::write_reg(uint32_t offset, uint32_t value) const
    {
        PCIES2MM_REG_ACCESS reg{};
        reg.Offset = offset;
        reg.Value = value;
        DWORD bytesReturned = 0;
        return DeviceIoControl(device_,
                               kIoctlWriteReg,
                               &reg,
                               sizeof(reg),
                               nullptr,
                               0,
                               &bytesReturned,
                               nullptr) != FALSE;
    }

    bool PcieS2mmCaptureSession::read_reg_bar1(uint32_t offset, uint32_t &out) const
    {
        PCIES2MM_REG_ACCESS reg{};
        reg.Offset = offset;
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(device_,
                                        kIoctlReadRegBar1,
                                        &reg,
                                        sizeof(reg),
                                        &reg,
                                        sizeof(reg),
                                        &bytesReturned,
                                        nullptr);
        if (!ok)
            return false;
        out = reg.Value;
        return true;
    }

    bool PcieS2mmCaptureSession::write_reg_bar1(uint32_t offset, uint32_t value) const
    {
        PCIES2MM_REG_ACCESS reg{};
        reg.Offset = offset;
        reg.Value = value;
        DWORD bytesReturned = 0;
        return DeviceIoControl(device_,
                               kIoctlWriteRegBar1,
                               &reg,
                               sizeof(reg),
                               nullptr,
                               0,
                               &bytesReturned,
                               nullptr) != FALSE;
    }

    bool PcieS2mmCaptureSession::register_event(uint32_t channelIndex, uint32_t eventType, HANDLE eventHandle)
    {
        PCIES2MM_EVENT_REG eventReg{};
        eventReg.Type = eventType;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = eventHandle;
        DWORD bytesReturned = 0;
        return DeviceIoControl(device_,
                               kIoctlRegisterEvent,
                               &eventReg,
                               sizeof(eventReg),
                               nullptr,
                               0,
                               &bytesReturned,
                               nullptr) != FALSE;
    }

    void PcieS2mmCaptureSession::unregister_event(uint32_t channelIndex, uint32_t eventType)
    {
        PCIES2MM_EVENT_REG eventReg{};
        eventReg.Type = eventType;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = nullptr;
        DWORD bytesReturned = 0;
        DeviceIoControl(device_,
                        kIoctlUnregisterEvent,
                        &eventReg,
                        sizeof(eventReg),
                        nullptr,
                        0,
                        &bytesReturned,
                        nullptr);
    }

    bool PcieS2mmCaptureSession::create_and_register_events(uint32_t channelIndex)
    {
        dma_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        format_change_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        plug_in_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        plug_out_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!dma_event_ || !format_change_event_ || !plug_in_event_ || !plug_out_event_)
            return false;

        if (!register_event(channelIndex, kEventTypeVideoDma, dma_event_))
            return false;
        if (!register_event(channelIndex, kEventTypeVideoFormatChange, format_change_event_))
            return false;
        if (!register_event(channelIndex, kEventTypeVideoPlugin, plug_in_event_))
            return false;
        if (!register_event(channelIndex, kEventTypeVideoUnplug, plug_out_event_))
            return false;
        return true;
    }

    void PcieS2mmCaptureSession::unregister_events(uint32_t channelIndex)
    {
        unregister_event(channelIndex, kEventTypeVideoDma);
        unregister_event(channelIndex, kEventTypeVideoFormatChange);
        unregister_event(channelIndex, kEventTypeVideoPlugin);
        unregister_event(channelIndex, kEventTypeVideoUnplug);
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

    bool PcieS2mmCaptureSession::get_video_done_index(uint32_t channelIndex, uint32_t &doneIndex) const
    {
        ULONG input = channelIndex;
        ULONG value = 0;
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(device_,
                                        kIoctlGetVideoDoneIndex,
                                        &input,
                                        sizeof(input),
                                        &value,
                                        sizeof(value),
                                        &bytesReturned,
                                        nullptr);
        if (!ok)
            return false;
        doneIndex = value;
        return true;
    }

    int PcieS2mmCaptureSession::get_frame(uint32_t channelIndex, uint32_t frameIndex, uint8_t *buffer, DWORD bufferSize) const
    {
        ULONG input[2] = {channelIndex, frameIndex};
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(device_,
                                        kIoctlGetFrame,
                                        input,
                                        sizeof(input),
                                        buffer,
                                        bufferSize,
                                        &bytesReturned,
                                        nullptr);
        if (!ok)
            return -1;
        return static_cast<int>(bytesReturned);
    }

    void PcieS2mmCaptureSession::capture_thread_proc()
    {
        const uint32_t channel = active_channel();
        write_reg(kInterruptBase + kIrqMaskW1sOffset, video_irq_mask_bit());
        HANDLE waitHandles[] = {
            dma_event_,
            format_change_event_,
            plug_in_event_,
            plug_out_event_,
        };
        constexpr DWORD waitHandleCount = 4;

        while (running_)
        {
            const DWORD waitResult = WaitForMultipleObjects(waitHandleCount,
                                                            waitHandles,
                                                            FALSE,
                                                            1000);
            if (!running_)
                break;
            if (waitResult == WAIT_TIMEOUT)
                continue;
            if (waitResult < WAIT_OBJECT_0 || waitResult >= WAIT_OBJECT_0 + waitHandleCount)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            switch (waitResult - WAIT_OBJECT_0)
            {
            case 0:
                handle_dma_event(channel);
                break;
            case 1:
                handle_format_change_event(channel);
                break;
            case 2:
                handle_plugin_event(channel);
                break;
            case 3:
                handle_unplug_event(channel);
                break;
            default:
                break;
            }
        }

        write_reg(kInterruptBase + kIrqMaskW1cOffset, video_irq_mask_bit());
    }

    void PcieS2mmCaptureSession::handle_dma_event(uint32_t channel)
    {
        if (!capture_active_.load(std::memory_order_acquire))
            return;

        uint32_t doneIndex = 0;
        if (!get_video_done_index(channel, doneIndex))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.dma_errors;
            frame_cv_.notify_all();
            return;
        }

        emit_event(PCIES2MM_EVENT_VIDEO_IRQ, channel == 0 ? 0 : 4, video_irq_mask_bit());

        const DWORD bytes = static_cast<DWORD>(frame_size_bytes());
        size_t slotIndex = frame_ring_.size();
        uint8_t *slotData = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++pending_events_;
            ++stats_.interrupt_count;

            auto hasFreeSlot = [this]()
            {
                for (const FrameSlot &slot : frame_ring_)
                {
                    if (!slot.in_use)
                        return true;
                }
                return false;
            };

            while (running_ && !hasFreeSlot())
                data_cv_.wait_for(lock, std::chrono::seconds(1));
            if (!running_)
                return;

            for (size_t attempt = 0; attempt < frame_ring_.size(); ++attempt)
            {
                const size_t candidate = (next_write_slot_ + attempt) % frame_ring_.size();
                if (!frame_ring_[candidate].in_use)
                {
                    slotIndex = candidate;
                    break;
                }
            }
            if (slotIndex == frame_ring_.size())
                return;

            FrameSlot &slot = frame_ring_[slotIndex];
            if (slot.ready && slot.sequence > delivered_sequence_)
                ++stats_.frames_dropped;
            slot.ready = false;
            slot.in_use = true;
            if (slot.data.size() < bytes)
                slot.data.resize(bytes);
            slotData = slot.data.data();
            next_write_slot_ = (slotIndex + 1) % frame_ring_.size();
        }

        const uint32_t frameIndex = doneIndex % kDmaBufferCount;
        const int ret = get_frame(channel, frameIndex, slotData, bytes);
        if (!running_)
            return;
        if (ret < 0 || static_cast<DWORD>(ret) != bytes)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slotIndex < frame_ring_.size())
                frame_ring_[slotIndex].in_use = false;
            if (pending_events_ > 0)
                --pending_events_;
            ++stats_.dma_errors;
            frame_cv_.notify_all();
            data_cv_.notify_all();
            PCIES2MM_ERROR_LOG("get_frame failed channel=%u frame=%u ret=%d expected=%lu",
                               channel,
                               frameIndex,
                               ret,
                               bytes);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_events_ > 0)
                --pending_events_;
            publish_frame(slotIndex, static_cast<size_t>(ret));
        }
    }

    void PcieS2mmCaptureSession::handle_format_change_event(uint32_t channel)
    {
        write_reg(video_base() + kVideoDmaEnOffset, 0);
        write_reg(video_base() + kVideoEnOffset, 0);
        capture_active_ = false;
        emit_event(PCIES2MM_EVENT_CAPTURE_PAUSED, channel == 0 ? 0 : 4, video_irq_mask_bit());

        if (refresh_stream_from_signal(true))
        {
            write_reg(video_base() + kVideoDmaEnOffset, 1);
            write_reg(video_base() + kVideoEnOffset, 1);
            capture_active_ = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = false;
                stats_.state = PCIES2MM_STREAM_RUNNING;
            }
            emit_event(PCIES2MM_EVENT_CAPTURE_RESUMED, channel == 0 ? 0 : 4, video_irq_mask_bit());
        }

        frame_cv_.notify_all();
        data_cv_.notify_all();
    }

    void PcieS2mmCaptureSession::handle_plugin_event(uint32_t channel)
    {
        refresh_stream_from_signal(true);
        write_reg(video_base() + kVideoDmaEnOffset, 1);
        write_reg(video_base() + kVideoEnOffset, 1);
        capture_active_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_error_ = false;
            stats_.state = PCIES2MM_STREAM_RUNNING;
        }
        emit_event(PCIES2MM_EVENT_PLUG_IN, channel == 0 ? 0 : 4, video_irq_mask_bit());
        emit_event(PCIES2MM_EVENT_CAPTURE_RESUMED, channel == 0 ? 0 : 4, video_irq_mask_bit());
        frame_cv_.notify_all();
        data_cv_.notify_all();
    }

    void PcieS2mmCaptureSession::handle_unplug_event(uint32_t channel)
    {
        write_reg(video_base() + kVideoDmaEnOffset, 0);
        write_reg(video_base() + kVideoEnOffset, 0);
        capture_active_ = false;
        emit_event(PCIES2MM_EVENT_PLUG_OUT, channel == 0 ? 0 : 4, video_irq_mask_bit());
        emit_event(PCIES2MM_EVENT_CAPTURE_PAUSED, channel == 0 ? 0 : 4, video_irq_mask_bit());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_ = 0;
            stats_.state = PCIES2MM_STREAM_CONFIGURED;
        }
        frame_cv_.notify_all();
        data_cv_.notify_all();
    }

    bool PcieS2mmCaptureSession::refresh_stream_from_signal(bool resizeRing)
    {
        pcies2mm_signal_status_t signal{};
        if (get_signal_status(signal) != PCIES2MM_OK ||
            signal.width == 0 ||
            signal.height == 0 ||
            signal.pixel_format == PCIES2MM_PIXFMT_UNKNOWN)
        {
            PCIES2MM_ERROR_LOG("refresh_stream_from_signal failed");
            return false;
        }

        const uint32_t bitDepth = bit_depth_for_pixfmt(signal.pixel_format);
        const size_t bytes = bytes_per_frame(signal.width, signal.height, signal.pixel_format, bitDepth);
        if (bytes == 0 || bytes > (std::numeric_limits<DWORD>::max)())
        {
            PCIES2MM_ERROR_LOG("refresh_stream_from_signal invalid frame size width=%u height=%u bytes=%zu",
                               signal.width,
                               signal.height,
                               bytes);
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        stream_desc_.width = signal.width;
        stream_desc_.height = signal.height;
        stream_desc_.pixel_format = signal.pixel_format;
        stream_bit_depth_ = bitDepth;
        if (resizeRing)
        {
            frame_ring_.assign(stream_desc_.buffer_count, FrameSlot{});
            for (FrameSlot &slot : frame_ring_)
                slot.data.assign(bytes, 0);
            next_write_slot_ = 0;
            active_delivery_slot_ = static_cast<size_t>(-1);
            pending_events_ = 0;
        }
        return true;
    }

    void PcieS2mmCaptureSession::publish_frame(size_t slotIndex, size_t bytes)
    {
        if (slotIndex >= frame_ring_.size())
            return;

        FrameSlot &slot = frame_ring_[slotIndex];
        slot.bytes = bytes;
        slot.sequence = latest_sequence_ + 1;
        slot.ready = true;
        slot.in_use = false;
        ++latest_sequence_;
        ++stats_.frames_captured;
        stats_.state = PCIES2MM_STREAM_RUNNING;
        if (should_log_counter(latest_sequence_))
        {
            PCIES2MM_LOG("publish_frame: id=%llu slot=%zu bytes=%zu",
                         static_cast<unsigned long long>(latest_sequence_),
                         slotIndex,
                         bytes);
        }
        frame_cv_.notify_one();
        data_cv_.notify_all();
    }

    void PcieS2mmCaptureSession::emit_event(pcies2mm_event_type_t type, uint32_t irqBit, uint32_t irqMask) const
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

        pcies2mm_event_t event{};
        event.type = type;
        event.irq_bit = irqBit;
        event.irq_mask = irqMask;
        event.timestamp_ns = steady_now_ns();
        callback(&event, user);
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
        return input_ == PCIES2MM_INPUT_HDMI ? 1u : 0u;
    }

    uint32_t PcieS2mmCaptureSession::video_event_mask() const
    {
        return video_irq_mask_bit();
    }

    uint32_t PcieS2mmCaptureSession::video_base() const
    {
        return active_channel() == 0 ? kCh0VideoBase : kCh1VideoBase;
    }

    uint32_t PcieS2mmCaptureSession::video_irq_mask_bit() const
    {
        return active_channel() == 0 ? kCh0VideoDmaIrqMask : kCh1VideoDmaIrqMask;
    }

    size_t PcieS2mmCaptureSession::frame_size_bytes() const
    {
        return bytes_per_frame(stream_desc_.width, stream_desc_.height, stream_desc_.pixel_format, stream_bit_depth_);
    }
}
