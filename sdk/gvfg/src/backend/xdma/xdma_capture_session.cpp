#include "xdma_capture_session.h"

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
#if GVFG_XDMA_DEBUG_LOG
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

    static xdma_pixel_format_t decode_pixel_format(uint32_t raw)
    {
        switch (raw)
        {
        case fourcc('Y', 'U', 'Y', '2'):
            return XDMA_PIXFMT_YUY2;
        case fourcc('U', 'Y', 'V', 'Y'):
            return XDMA_PIXFMT_UYVY;
        case fourcc('N', 'V', '1', '2'):
            return XDMA_PIXFMT_NV12;
        case fourcc('Y', '2', '1', '0'):
            return XDMA_PIXFMT_Y210;
        case fourcc('P', '0', '1', '0'):
            return XDMA_PIXFMT_P010;
        default:
            break;
        }

        switch (raw & 0x3u)
        {
        case 0:
            return XDMA_PIXFMT_YUY2;
        case 1:
            return XDMA_PIXFMT_RGB24;
        case 2:
            return XDMA_PIXFMT_YUV444;
        case 3:
            return XDMA_PIXFMT_NV12;
        default:
            return XDMA_PIXFMT_UNKNOWN;
        }
    }

    static uint32_t old_style_format_code(xdma_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case XDMA_PIXFMT_RGB24:
            return 1;
        case XDMA_PIXFMT_YUV444:
            return 2;
        case XDMA_PIXFMT_NV12:
        case XDMA_PIXFMT_P010:
            return 3;
        case XDMA_PIXFMT_YUY2:
        case XDMA_PIXFMT_UYVY:
        case XDMA_PIXFMT_Y210:
        default:
            return 0;
        }
    }

    static uint32_t bit_depth_for_pixfmt(xdma_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case XDMA_PIXFMT_P010:
        case XDMA_PIXFMT_Y210:
            return 10;
        default:
            return 8;
        }
    }

    static size_t bytes_per_frame(uint32_t width, uint32_t height, xdma_pixel_format_t fmt, uint32_t bitDepth)
    {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        switch (fmt)
        {
        case XDMA_PIXFMT_NV12:
            return pixels * 3u / 2u;
        case XDMA_PIXFMT_P010:
            return pixels * 3u;
        case XDMA_PIXFMT_RGB24:
        case XDMA_PIXFMT_YUV444:
            return bitDepth > 8u ? pixels * 6u : pixels * 3u;
        case XDMA_PIXFMT_Y210:
            return pixels * 4u;
        case XDMA_PIXFMT_YUY2:
        case XDMA_PIXFMT_UYVY:
        default:
            return pixels * 2u;
        }
    }

    static uint32_t event_mask_for_type(xdma_event_type_t type)
    {
        switch (type)
        {
        case XDMA_EVENT_VIDEO_IRQ:
            return XDMA_EVENT_MASK_VIDEO_IRQ;
        case XDMA_EVENT_PLUG_IN:
            return XDMA_EVENT_MASK_PLUG_IN;
        case XDMA_EVENT_PLUG_OUT:
            return XDMA_EVENT_MASK_PLUG_OUT;
        case XDMA_EVENT_CAPTURE_PAUSED:
            return XDMA_EVENT_MASK_CAPTURE_PAUSED;
        case XDMA_EVENT_CAPTURE_RESUMED:
            return XDMA_EVENT_MASK_CAPTURE_RESUMED;
        default:
            return 0;
        }
    }

    static void reset_stats(xdma_stream_stats_t &stats, xdma_stream_state_t state)
    {
        std::memset(&stats, 0, sizeof(stats));
        stats.state = state;
    }
}

namespace gvfg::internal
{
    std::vector<XdmaDevice> enumerate_xdma_devices()
    {
        std::vector<XdmaDevice> devices;
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

            XdmaDevice device;
            device.interface_path = detail->DevicePath;
            device.friendly_name = L"PcieS2mm Capture Device " + std::to_wstring(devices.size());
            devices.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(info);
        return devices;
    }

    XdmaCaptureSession::XdmaCaptureSession()
    {
        stream_desc_.input = XDMA_INPUT_SDI;
        stream_desc_.width = kDefaultWidth;
        stream_desc_.height = kDefaultHeight;
        stream_desc_.pixel_format = XDMA_PIXFMT_YUY2;
        stream_desc_.buffer_count = kDefaultRingBufferCount;
        stream_bit_depth_ = 8;
        reset_stats(stats_, XDMA_STREAM_STOPPED);
    }

    XdmaCaptureSession::~XdmaCaptureSession()
    {
        close();
    }

    xdma_status_t XdmaCaptureSession::open_device_index(size_t deviceIndex)
    {
        const auto devices = enumerate_xdma_devices();
        if (deviceIndex >= devices.size())
            return fail(XDMA_ENODEV, "enumerate_pcies2mm_devices", ERROR_NOT_FOUND);
        return open_device(devices[deviceIndex]);
    }

    xdma_status_t XdmaCaptureSession::open_device(const XdmaDevice &device)
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
            return fail(XDMA_EIO, "CreateFile(PcieS2mm)", err);
        }

        opened_ = true;
        configured_ = false;
        clear_last_error();
        PCIES2MM_LOG("open_device: %s", wide_to_utf8(base_path_).c_str());
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::close()
    {
        stop_stream();
        close_handles();
        opened_ = false;
        configured_ = false;
        base_path_.clear();
        friendly_name_.clear();
        reset_stats(stats_, XDMA_STREAM_STOPPED);
        return XDMA_OK;
    }

    void XdmaCaptureSession::close_handles()
    {
        if (interrupt_event_)
        {
            CloseHandle(interrupt_event_);
            interrupt_event_ = nullptr;
        }
        if (device_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
        }
    }

    xdma_status_t XdmaCaptureSession::set_input(xdma_input_t input)
    {
        if (!opened_)
            return XDMA_ESTATE;
        input_ = input == XDMA_INPUT_HDMI ? XDMA_INPUT_HDMI : XDMA_INPUT_SDI;
        stream_desc_.input = input_;
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::get_signal_status(xdma_signal_status_t &out) const
    {
        if (!opened_)
            return XDMA_ESTATE;

        std::memset(&out, 0, sizeof(out));
        uint32_t rawWidth = 0;
        uint32_t rawHeight = 0;
        uint32_t rawFormat = 0;
        const bool widthOk = read_reg(video_base() + kVideoHSizeOffset, rawWidth);
        const bool heightOk = read_reg(video_base() + kVideoVSizeOffset, rawHeight);
        const bool formatOk = read_reg(video_base() + kVideoFormatOffset, rawFormat);

        xdma_pixel_format_t fmt = formatOk ? decode_pixel_format(rawFormat) : stream_desc_.pixel_format;
        if (fmt == XDMA_PIXFMT_UNKNOWN)
            fmt = stream_desc_.pixel_format == XDMA_PIXFMT_UNKNOWN ? XDMA_PIXFMT_YUY2 : stream_desc_.pixel_format;

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
        return haveSize ? XDMA_OK : XDMA_ENODEV;
    }

    xdma_status_t XdmaCaptureSession::set_event_callback(xdma_event_callback_t callback, void *user, uint32_t eventMask)
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
        event_callback_ = callback;
        event_callback_user_ = user;
        event_mask_filter_ = eventMask ? eventMask : XDMA_EVENT_MASK_DEFAULT;
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::configure_stream(const xdma_stream_desc_t &desc)
    {
        if (!opened_)
            return XDMA_ESTATE;
        if (running_)
            return XDMA_ESTATE;
        if (desc.width == 0 || desc.height == 0)
            return XDMA_EINVAL;

        xdma_pixel_format_t fmt = desc.pixel_format == XDMA_PIXFMT_UNKNOWN ? XDMA_PIXFMT_YUY2 : desc.pixel_format;
        switch (fmt)
        {
        case XDMA_PIXFMT_YUY2:
        case XDMA_PIXFMT_UYVY:
            break;
        default:
            return fail(XDMA_ENOTSUP, "configure_stream(pixel_format)", ERROR_NOT_SUPPORTED);
        }

        stream_desc_ = desc;
        stream_desc_.input = input_;
        stream_desc_.pixel_format = fmt;
        stream_desc_.buffer_count = std::clamp(desc.buffer_count ? desc.buffer_count : kDefaultRingBufferCount,
                                               1u,
                                               kMaxRingBufferCount);
        stream_bit_depth_ = bit_depth_for_pixfmt(fmt);
        configured_ = true;
        reset_stats(stats_, XDMA_STREAM_CONFIGURED);
        clear_last_error();
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::start_stream()
    {
        if (!opened_ || !configured_)
            return XDMA_ESTATE;
        if (running_)
            return XDMA_OK;

        const size_t bytes = frame_size_bytes();
        if (bytes == 0 || bytes > (std::numeric_limits<DWORD>::max)())
            return fail(XDMA_EINVAL, "frame_size_bytes", ERROR_INVALID_PARAMETER);

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
            reset_stats(stats_, XDMA_STREAM_RUNNING);
        }

        interrupt_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!interrupt_event_)
            return fail(XDMA_EIO, "CreateEvent");

        const uint32_t channel = active_channel();
        if (!register_event(channel))
        {
            const DWORD err = GetLastError();
            CloseHandle(interrupt_event_);
            interrupt_event_ = nullptr;
            return fail(XDMA_EIO, "REGISTER_EVENT", err);
        }

        running_ = true;
        capture_active_ = true;
        try
        {
            capture_thread_ = std::thread(&XdmaCaptureSession::capture_thread_proc, this);
        }
        catch (...)
        {
            running_ = false;
            capture_active_ = false;
            unregister_event(channel);
            CloseHandle(interrupt_event_);
            interrupt_event_ = nullptr;
            return fail(XDMA_EIO, "capture_thread", ERROR_NOT_ENOUGH_MEMORY);
        }

        const bool dmaEnableOk = write_reg(video_base() + kVideoDmaEnOffset, 1);
        const bool videoEnableOk = write_reg(video_base() + kVideoEnOffset, 1);
        if (!dmaEnableOk || !videoEnableOk)
        {
            const DWORD err = GetLastError();
            stop_stream();
            return fail(XDMA_EIO, "enable video capture", err);
        }

        PCIES2MM_LOG("start: channel=%u base=0x%x bytes=%zu", channel, video_base(), bytes);
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::stop_stream()
    {
        if (!running_ && !capture_thread_.joinable())
            return XDMA_OK;

        const uint32_t channel = active_channel();
        running_ = false;
        capture_active_ = false;

        if (device_ != INVALID_HANDLE_VALUE)
        {
            write_reg(video_base() + kVideoDmaEnOffset, 0);
            write_reg(video_base() + kVideoEnOffset, 0);
            write_reg(kInterruptBase + kIrqMaskW1cOffset, video_irq_mask_bit());
        }

        if (interrupt_event_)
            SetEvent(interrupt_event_);

        if (capture_thread_.joinable())
            capture_thread_.join();

        if (device_ != INVALID_HANDLE_VALUE && interrupt_event_)
            unregister_event(channel);

        if (interrupt_event_)
        {
            CloseHandle(interrupt_event_);
            interrupt_event_ = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_ = 0;
            active_delivery_slot_ = static_cast<size_t>(-1);
            for (FrameSlot &slot : frame_ring_)
                slot.in_use = false;
            stats_.state = configured_ ? XDMA_STREAM_CONFIGURED : XDMA_STREAM_STOPPED;
        }
        frame_cv_.notify_all();
        data_cv_.notify_all();
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::wait_frame(uint32_t timeoutMs, xdma_frame_t &out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        std::memset(&out, 0, sizeof(out));
        if (!running_)
            return XDMA_ESTATE;

        const auto hasFrame = [this]()
        {
            for (const FrameSlot &slot : frame_ring_)
            {
                if (slot.ready && slot.sequence > delivered_sequence_)
                    return true;
            }
            return stream_error_ || !running_;
        };

        if (timeoutMs == 0)
        {
            frame_cv_.wait(lock, hasFrame);
        }
        else if (!frame_cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasFrame))
        {
            ++wait_timeout_count_;
            return XDMA_ETIMEOUT;
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
            return XDMA_ESTATE;
        if (stream_error_ && readySlot == frame_ring_.size())
            return XDMA_EIO;
        if (readySlot == frame_ring_.size())
            return XDMA_ETIMEOUT;

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
        return XDMA_OK;
    }

    xdma_status_t XdmaCaptureSession::release_frame(const xdma_frame_t &frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_delivery_slot_ >= frame_ring_.size())
            return XDMA_ESTATE;

        const FrameSlot &slot = frame_ring_[active_delivery_slot_];
        if (frame.data != slot.data.data() ||
            frame.data_size_bytes != slot.bytes ||
            frame.frame_id != delivered_sequence_ ||
            frame.width != stream_desc_.width ||
            frame.height != stream_desc_.height ||
            frame.pixel_format != stream_desc_.pixel_format ||
            frame.bit_depth != stream_bit_depth_)
            return XDMA_EINVAL;

        frame_ring_[active_delivery_slot_].in_use = false;
        active_delivery_slot_ = static_cast<size_t>(-1);
        data_cv_.notify_all();
        return XDMA_OK;
    }

    const char *XdmaCaptureSession::last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_.c_str();
    }

    void XdmaCaptureSession::get_debug_stats(xdma_stream_stats_t &outStats,
                                             uint64_t &outWaitTimeouts,
                                             xdma_debug_state_t &outDebugState) const
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

    bool XdmaCaptureSession::read_reg(uint32_t offset, uint32_t &out) const
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

    bool XdmaCaptureSession::write_reg(uint32_t offset, uint32_t value) const
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

    bool XdmaCaptureSession::read_reg_bar1(uint32_t offset, uint32_t &out) const
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

    bool XdmaCaptureSession::write_reg_bar1(uint32_t offset, uint32_t value) const
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

    bool XdmaCaptureSession::register_event(uint32_t channelIndex)
    {
        PCIES2MM_EVENT_REG eventReg{};
        eventReg.Type = kEventTypeVideoDma;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = interrupt_event_;
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

    void XdmaCaptureSession::unregister_event(uint32_t channelIndex)
    {
        PCIES2MM_EVENT_REG eventReg{};
        eventReg.Type = kEventTypeVideoDma;
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

    bool XdmaCaptureSession::get_video_done_index(uint32_t channelIndex, uint32_t &doneIndex) const
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

    int XdmaCaptureSession::get_frame(uint32_t channelIndex, uint32_t frameIndex, uint8_t *buffer, DWORD bufferSize) const
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

    void XdmaCaptureSession::capture_thread_proc()
    {
        const uint32_t channel = active_channel();
        write_reg(kInterruptBase + kIrqMaskW1sOffset, video_irq_mask_bit());

        while (running_)
        {
            const DWORD waitResult = WaitForSingleObject(interrupt_event_, 1000);
            if (!running_)
                break;
            if (waitResult == WAIT_TIMEOUT)
                continue;
            if (waitResult != WAIT_OBJECT_0)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            uint32_t doneIndex = 0;
            if (!get_video_done_index(channel, doneIndex))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                break;
            }

            emit_event(XDMA_EVENT_VIDEO_IRQ, channel == 0 ? 0 : 4, video_irq_mask_bit());

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
                    break;

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
                    continue;

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
                break;
            if (ret < 0 || static_cast<DWORD>(ret) != bytes)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (slotIndex < frame_ring_.size())
                    frame_ring_[slotIndex].in_use = false;
                stream_error_ = true;
                ++stats_.dma_errors;
                frame_cv_.notify_all();
                data_cv_.notify_all();
                PCIES2MM_ERROR_LOG("get_frame failed channel=%u frame=%u ret=%d expected=%lu",
                                   channel,
                                   frameIndex,
                                   ret,
                                   bytes);
                break;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_events_ > 0)
                    --pending_events_;
                publish_frame(slotIndex, static_cast<size_t>(ret));
            }
        }

        write_reg(kInterruptBase + kIrqMaskW1cOffset, video_irq_mask_bit());
    }

    void XdmaCaptureSession::publish_frame(size_t slotIndex, size_t bytes)
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
        stats_.state = XDMA_STREAM_RUNNING;
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

    void XdmaCaptureSession::emit_event(xdma_event_type_t type, uint32_t irqBit, uint32_t irqMask) const
    {
        xdma_event_callback_t callback = nullptr;
        void *user = nullptr;
        {
            std::lock_guard<std::mutex> lock(event_callback_mutex_);
            const uint32_t typeMask = event_mask_for_type(type);
            if (!event_callback_ || typeMask == 0 || (event_mask_filter_ & typeMask) == 0)
                return;
            callback = event_callback_;
            user = event_callback_user_;
        }

        xdma_event_t event{};
        event.type = type;
        event.irq_bit = irqBit;
        event.irq_mask = irqMask;
        event.timestamp_ns = steady_now_ns();
        callback(&event, user);
    }

    xdma_status_t XdmaCaptureSession::fail(xdma_status_t status, const char *where, DWORD winerr) const
    {
        std::ostringstream oss;
        oss << (where ? where : "PcieS2mm") << " failed";
        if (winerr != NO_ERROR)
            oss << ": " << win32_error(winerr) << " (" << winerr << ")";
        set_last_error(oss.str());
        PCIES2MM_ERROR_LOG("%s", oss.str().c_str());
        return status;
    }

    void XdmaCaptureSession::set_last_error(const std::string &message) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = message;
    }

    void XdmaCaptureSession::clear_last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_.clear();
    }

    uint32_t XdmaCaptureSession::active_channel() const
    {
        return input_ == XDMA_INPUT_HDMI ? 1u : 0u;
    }

    uint32_t XdmaCaptureSession::video_event_mask() const
    {
        return video_irq_mask_bit();
    }

    uint32_t XdmaCaptureSession::video_base() const
    {
        return active_channel() == 0 ? kCh0VideoBase : kCh1VideoBase;
    }

    uint32_t XdmaCaptureSession::video_irq_mask_bit() const
    {
        return active_channel() == 0 ? kCh0VideoDmaIrqMask : kCh1VideoDmaIrqMask;
    }

    size_t XdmaCaptureSession::frame_size_bytes() const
    {
        return bytes_per_frame(stream_desc_.width, stream_desc_.height, stream_desc_.pixel_format, stream_bit_depth_);
    }
}
