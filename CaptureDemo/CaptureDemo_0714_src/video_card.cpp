#include <QDebug>
#include "video_card.h"

#include <SetupAPI.h>
#include <string.h>
#include <fstream>

#pragma comment(lib, "SetupAPI.lib")

// {8c47b9c3-1faa-4557-bc1d-f225d26c9e91}
const GUID GUID_DEVINTERFACE_PcieS2mm =
    {0x8c47b9c3, 0x1faa, 0x4557, {0xbc, 0x1d, 0xf2, 0x25, 0xd2, 0x6c, 0x9e, 0x91}};

#define MAX_PATH_LEN 400

/* Static channel configuration table */
static const struct
{
    ULONG video_base;
    ULONG irq_mask_bit;
} g_channel_config[PCIE_S2MM_MAX_CHANNELS] = {
    {CH0_VIDEO_BASE, IRQ_CH0_VIDEO_DMA_CTRL_MASK}, /* CH0: bit0 */
    {CH1_VIDEO_BASE, IRQ_CH1_VIDEO_DMA_CTRL_MASK}, /* CH1: bit4 */
};

int calcFrameBytes(int width, int height, UINT32 fourcc)
{
    if (fourcc == 0x59565955)
    { // UYVY
        return width * height * 2;
    }
    else if (fourcc == 0x76323130) // v210
    {
        return width * height * 20 / 8;
    }
    return 0;
}

VideoCard::VideoCard()
{
    for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
    {
        channels_[i].video_base = g_channel_config[i].video_base;
        channels_[i].irq_mask_bit = g_channel_config[i].irq_mask_bit;
    }
}

VideoCard::~VideoCard()
{
    uninit();
}

int VideoCard::init()
{
    if (0 != openDevice())
    {
        return -1;
    }
    is_initialized_ = true;
    return 0;
}

void VideoCard::uninit()
{
    if (!is_initialized_)
    {
        return;
    }

    // Stop all channels
    for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
    {
        stopCapture(i);
    }

    closeDevice();
    is_initialized_ = false;
}

int VideoCard::startCapture(ULONG channelIndex, int width, int height, UINT32 fourcc, const std::function<void(uchar *, int, int)> &cb)
{
    if (!is_initialized_)
    {
        return -1;
    }

    if (channelIndex < 0 || channelIndex >= PCIE_S2MM_MAX_CHANNELS)
    {
        qWarning() << "VideoCard: invalid channel index" << channelIndex;
        return -1;
    }

    ChannelState &ch = channels_[channelIndex];

    if (ch.capturing)
    {
        return 0;
    }

    ch.capturing = true;
    ch.quit_module_event_thread = false;
    ch.frame_count = 0;
    ch.first_frame_timestamp_ms = 0;
    ch.video_width = width;
    ch.video_height = height;
    ch.fourcc = fourcc;

    // Allocate frame buffer for user-mode data copy
    DWORD frame_size = calcFrameBytes(width, height, fourcc);
    if (ch.frame_buffer)
    {
        delete[] ch.frame_buffer;
        ch.frame_buffer = nullptr;
    }
    ch.frame_buffer = new unsigned char[frame_size];
    memset(ch.frame_buffer, 0, frame_size);

    // Store callback
    ch.recv_data_cb = std::make_shared<std::function<void(uchar *, int, int)>>(cb);

    // Create and register interrupt event for this channel
    if (ch.dma_interrupt_event == NULL)
    {
        ch.dma_interrupt_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (ch.dma_interrupt_event == NULL)
        {
            qWarning() << "VideoCard: CreateEvent failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }

        // Register the event with the driver
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_DMA;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = ch.dma_interrupt_event;
        BOOL ok = DeviceIoControl(device_,
                                  IOCTL_PCIES2MM_REGISTER_EVENT,
                                  &eventReg, sizeof(eventReg),
                                  NULL, 0,
                                  &bytesReturned, NULL);
        if (!ok)
        {
            qWarning() << "VideoCard: REGISTER_EVENT failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            CloseHandle(ch.dma_interrupt_event);
            ch.dma_interrupt_event = NULL;
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }
    }

    if (ch.video_format_change_event == NULL)
    {
        ch.video_format_change_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (ch.video_format_change_event == NULL)
        {
            qWarning() << "VideoCard: CreateEvent failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }

        // Register the event with the driver
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_FORMAT_CHANGE;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = ch.video_format_change_event;
        BOOL ok = DeviceIoControl(device_,
                                  IOCTL_PCIES2MM_REGISTER_EVENT,
                                  &eventReg, sizeof(eventReg),
                                  NULL, 0,
                                  &bytesReturned, NULL);
        if (!ok)
        {
            qWarning() << "VideoCard: REGISTER_EVENT failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            CloseHandle(ch.video_format_change_event);
            ch.video_format_change_event = NULL;
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }
    }

    if (ch.video_plugin_event == NULL)
    {
        ch.video_plugin_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (ch.video_plugin_event == NULL)
        {
            qWarning() << "VideoCard: CreateEvent failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }

        // Register the event with the driver
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_PLUGIN;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = ch.video_plugin_event;
        BOOL ok = DeviceIoControl(device_,
                                  IOCTL_PCIES2MM_REGISTER_EVENT,
                                  &eventReg, sizeof(eventReg),
                                  NULL, 0,
                                  &bytesReturned, NULL);
        if (!ok)
        {
            qWarning() << "VideoCard: REGISTER_EVENT failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            CloseHandle(ch.video_plugin_event);
            ch.video_plugin_event = NULL;
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }
    }

    if (ch.video_unplug_event == NULL)
    {
        ch.video_unplug_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (ch.video_unplug_event == NULL)
        {
            qWarning() << "VideoCard: CreateEvent failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }

        // Register the event with the driver
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_UNPLUG;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = ch.video_unplug_event;
        BOOL ok = DeviceIoControl(device_,
                                  IOCTL_PCIES2MM_REGISTER_EVENT,
                                  &eventReg, sizeof(eventReg),
                                  NULL, 0,
                                  &bytesReturned, NULL);
        if (!ok)
        {
            qWarning() << "VideoCard: REGISTER_EVENT failed for ch" << channelIndex
                       << ", error:" << GetLastError();
            CloseHandle(ch.video_unplug_event);
            ch.video_unplug_event = NULL;
            ch.capturing = false;
            delete[] ch.frame_buffer;
            ch.frame_buffer = nullptr;
            return -1;
        }
    }

    // Enable video and DMA for this channel
    writeReg(ch.video_base + VIDEO_DMA_EN_OFFSET, 1);
    writeReg(ch.video_base + VIDEO_EN_OFFSET, 1);

    // Start capture thread for this channel
    ch.capture_thread = std::make_shared<std::thread>(std::bind(&VideoCard::captureThread, this, channelIndex));
    ch.video_module_thread = std::make_shared<std::thread>(std::bind(&VideoCard::videoModuleThread, this, channelIndex));
    return 0;
}

void VideoCard::stopCapture(ULONG channelIndex)
{
    if (channelIndex < 0 || channelIndex >= PCIE_S2MM_MAX_CHANNELS)
    {
        return;
    }

    ChannelState &ch = channels_[channelIndex];

    if (!ch.capturing)
    {
        return;
    }
    ch.capturing = false;
    ch.quit_module_event_thread = true;

    // Disable video and DMA for this channel
    writeReg(ch.video_base + VIDEO_DMA_EN_OFFSET, 0);
    writeReg(ch.video_base + VIDEO_EN_OFFSET, 0);

    // Signal the event to unblock the capture thread if it's waiting
    if (ch.dma_interrupt_event != NULL)
    {
        SetEvent(ch.dma_interrupt_event);
    }

    // Wait for capture thread to finish
    if (ch.capture_thread != nullptr)
    {
        ch.capture_thread->join();
        ch.capture_thread.reset();
        ch.capture_thread = nullptr;
    }

    if (ch.video_module_thread != nullptr)
    {
        ch.video_module_thread->join();
        ch.video_module_thread = nullptr;
    }

    // Unregister event and close handle
    if (ch.dma_interrupt_event != NULL)
    {
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_DMA;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = NULL;
        DeviceIoControl(device_,
                        IOCTL_PCIES2MM_UNREGISTER_EVENT,
                        &eventReg, sizeof(eventReg),
                        NULL, 0,
                        &bytesReturned, NULL);
        CloseHandle(ch.dma_interrupt_event);
        ch.dma_interrupt_event = NULL;
    }

    if (ch.video_format_change_event != NULL)
    {
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_FORMAT_CHANGE;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = NULL;
        DeviceIoControl(device_,
                        IOCTL_PCIES2MM_UNREGISTER_EVENT,
                        &eventReg, sizeof(eventReg),
                        NULL, 0,
                        &bytesReturned, NULL);
        CloseHandle(ch.video_format_change_event);
        ch.video_format_change_event = NULL;
    }

    if (ch.video_plugin_event != NULL)
    {
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_PLUGIN;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = NULL;
        DeviceIoControl(device_,
                        IOCTL_PCIES2MM_UNREGISTER_EVENT,
                        &eventReg, sizeof(eventReg),
                        NULL, 0,
                        &bytesReturned, NULL);
        CloseHandle(ch.video_plugin_event);
        ch.video_plugin_event = NULL;
    }

    if (ch.video_unplug_event != NULL)
    {
        DWORD bytesReturned = 0;
        PCIES2MM_EVENT_REG eventReg;
        eventReg.Type = PCIES2MM_EVENT_TYPE_VIDEO_UNPLUG;
        eventReg.ChannelIndex = channelIndex;
        eventReg.EventHandle = NULL;
        DeviceIoControl(device_,
                        IOCTL_PCIES2MM_UNREGISTER_EVENT,
                        &eventReg, sizeof(eventReg),
                        NULL, 0,
                        &bytesReturned, NULL);
        CloseHandle(ch.video_unplug_event);
        ch.video_unplug_event = NULL;
    }

    // Free frame buffer
    if (ch.frame_buffer)
    {
        delete[] ch.frame_buffer;
        ch.frame_buffer = nullptr;
    }
}

void VideoCard::captureThread(ULONG channelIndex)
{
    ChannelState &ch = channels_[channelIndex];
    ULONG frame_index = 0;

    // Enable interrupt mask for this channel
    writeReg(INTERRUPT_BASE + IRQ_MASK_W1S_OFFSET, ch.irq_mask_bit);

    while (ch.capturing)
    {
        // Wait for interrupt notification from driver (DPC signals our event)
        DWORD waitResult = WaitForSingleObject(ch.dma_interrupt_event, 1000);

        if (!ch.capturing)
        {
            break;
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            // Timeout or error, continue waiting
            continue;
        }

        // Read video done index for this channel
        if (getVideoDoneIndex(channelIndex, frame_index))
        {
            // qDebug() << "ch" << channelIndex << "video done index:" << frame_index;
            if (ch.first_frame_timestamp_ms == 0)
            {
                ch.first_frame_timestamp_ms = GetTickCount();
            }
            ch.frame_count++;
            ULONG duration_ms = GetTickCount() - ch.first_frame_timestamp_ms;
            if (duration_ms > 0)
            {
                if (ch.frame_count % 600 == 0)
                {
                    qDebug() << "ch" << channelIndex << "frame count:" << ch.frame_count << "duration(ms):" << duration_ms
                             << "fps:" << (ch.frame_count * 1000.0 / duration_ms);
                }
            }
        }

        if (!ch.capturing)
        {
            break;
        }

        // Retrieve the frame data from driver's DMA buffer
        frame_index = frame_index % PCIE_S2MM_DMA_BUFFER_COUNT;
        DWORD frame_size = calcFrameBytes(ch.video_width, ch.video_height, ch.fourcc);
        int ret = getFrame(channelIndex, ch.frame_buffer, frame_size, frame_index);
        if (ret < 0)
        {
            qWarning() << "VideoCard: getFrame failed on ch" << channelIndex;
            continue;
        }

        // Invoke the registered callback with frame data
        if (ch.fourcc == 0x76323130)
        {
            // qDebug() << "is v210 format";
            if (ch.frame_count == 10)
            {
                std::ofstream of("./v210.yuv", std::ios::out);
                of.write((const char*)ch.frame_buffer, frame_size);
                of.close();
                qDebug() << "save v210.yuv done";
            }
        }
        else
        {
            if (ch.recv_data_cb)
            {
                (*ch.recv_data_cb)(ch.frame_buffer, (int)ch.video_width, (int)ch.video_height);
            }
        }
    }

    // Disable interrupt mask for this channel
    writeReg(INTERRUPT_BASE + IRQ_MASK_W1C_OFFSET, ch.irq_mask_bit);
}

void VideoCard::videoModuleThread(ULONG channelIndex)
{
    ChannelState &ch = channels_[channelIndex];
    PVOID waitObjects[3];

    waitObjects[0] = ch.video_format_change_event;
    waitObjects[1] = ch.video_plugin_event;
    waitObjects[2] = ch.video_unplug_event;
    qDebug() << "start wait video:" << channelIndex << " events";
    while (!ch.quit_module_event_thread)
    {
        // Wait for interrupt notification from driver (DPC signals our event)
        DWORD waitResult = WaitForMultipleObjects(3, waitObjects, FALSE, 500);

        if (ch.quit_module_event_thread)
        {
            qDebug() << "ch" << channelIndex << "video module thread exiting";
            break;
        }

        // qDebug() << "ch" << channelIndex << "video module thread waitResult:" << waitResult;

        if (waitResult == WAIT_OBJECT_0) // 视频格式变化中断
        {
            // Timeout or error, continue waiting
            writeReg(ch.video_base + VIDEO_DMA_EN_OFFSET, 0);
            writeReg(ch.video_base + VIDEO_EN_OFFSET, 0);
            readReg(ch.video_base + VIDEO_HSIZE_OFFSET, ch.video_width);
            readReg(ch.video_base + VIDEO_VSIZE_OFFSET, ch.video_height);
            ULONG frame_size = ch.video_width * ch.video_height * 2;
            ch.frame_buffer = new unsigned char[ch.video_width * ch.video_height * 2];
            memset(ch.frame_buffer, 0, frame_size);
            qDebug() << "ch" << channelIndex << "video format change event received, new resolution:" << ch.video_width << "x" << ch.video_height;
            continue;
        }

        if (waitResult == WAIT_OBJECT_0 + 1)
        {
            writeReg(ch.video_base + VIDEO_DMA_EN_OFFSET, 0);
            writeReg(ch.video_base + VIDEO_EN_OFFSET, 0);
            readReg(ch.video_base + VIDEO_HSIZE_OFFSET, ch.video_width);
            readReg(ch.video_base + VIDEO_VSIZE_OFFSET, ch.video_height);
            ULONG frame_size = ch.video_width * ch.video_height * 2;
            ch.frame_buffer = new unsigned char[ch.video_width * ch.video_height * 2];
            memset(ch.frame_buffer, 0, frame_size);
            qDebug() << "ch" << channelIndex << "video plugin event received, width:" << ch.video_width << ", height:" << ch.video_height;
            writeReg(ch.video_base + VIDEO_DMA_EN_OFFSET, 1);
            writeReg(ch.video_base + VIDEO_EN_OFFSET, 1);
            continue;
        }

        if (waitResult == WAIT_OBJECT_0 + 2)
        {
            writeReg(ch.video_base + VIDEO_DMA_EN_OFFSET, 0);
            writeReg(ch.video_base + VIDEO_EN_OFFSET, 0);
            qDebug() << "ch" << channelIndex << "video unplug event received, stopping capture";
            continue;
        }

        // Handle video module interrupt here
        // qDebug() << "ch" << channelIndex << "video module interrupt received";
        // // Disable interrupt mask for this channel
        // writeReg(INTERRUPT_BASE + IRQ_MASK_W1C_OFFSET, ch.irq_mask_bit);
        // ch.capturing = false; // For demonstration, stop capturing after one interrupt
    }
    qDebug() << "end wait video:" << channelIndex << " events";
}

bool VideoCard::writeReg(ULONG offset, ULONG value)
{
    PCIES2MM_REG_ACCESS regAccess;
    regAccess.Offset = offset;
    regAccess.Value = value;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              IOCTL_PCIES2MM_WRITE_REG,
                              &regAccess, sizeof(regAccess),
                              NULL, 0,
                              &bytesReturned, NULL);
    if (!ok)
    {
        qWarning() << "VideoCard: writeReg offset=0x" << Qt::hex << offset
                   << " value=0x" << value << " failed, error:" << GetLastError();
        return false;
    }
    return true;
}

bool VideoCard::readReg(ULONG offset, ULONG &value)
{
    PCIES2MM_REG_ACCESS regAccess;
    regAccess.Offset = offset;
    regAccess.Value = 0;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              IOCTL_PCIES2MM_READ_REG,
                              &regAccess, sizeof(regAccess),
                              &regAccess, sizeof(regAccess),
                              &bytesReturned, NULL);
    if (!ok)
    {
        qWarning() << "VideoCard: readReg offset=0x" << Qt::hex << offset
                   << " failed, error:" << GetLastError();
        return false;
    }
    value = regAccess.Value;
    return true;
}

bool VideoCard::writeRegBar1(ULONG offset, ULONG value)
{
    PCIES2MM_REG_ACCESS regAccess;
    regAccess.Offset = offset;
    regAccess.Value = value;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              IOCTL_PCIES2MM_WRITE_REG_BAR1,
                              &regAccess, sizeof(regAccess),
                              NULL, 0,
                              &bytesReturned, NULL);
    if (!ok)
    {
        qWarning() << "VideoCard: writeRegBar1 offset=0x" << Qt::hex << offset
                   << " value=0x" << value << " failed, error:" << GetLastError();
        return false;
    }
    return true;
}

bool VideoCard::readRegBar1(ULONG offset, ULONG &value)
{
    PCIES2MM_REG_ACCESS regAccess;
    regAccess.Offset = offset;
    regAccess.Value = 0;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              IOCTL_PCIES2MM_READ_REG_BAR1,
                              &regAccess, sizeof(regAccess),
                              &regAccess, sizeof(regAccess),
                              &bytesReturned, NULL);
    if (!ok)
    {
        qWarning() << "VideoCard: readRegBar1 offset=0x" << Qt::hex << offset
                   << " failed, error:" << GetLastError();
        return false;
    }
    value = regAccess.Value;
    return true;
}

int VideoCard::getFrame(ULONG channelIndex, BYTE *buffer, DWORD buffer_size, ULONG frame_index)
{
    ULONG inputBuf[2] = {(ULONG)channelIndex, frame_index};
    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              IOCTL_PCIES2MM_GET_FRAME,
                              inputBuf, sizeof(inputBuf),
                              buffer, buffer_size,
                              &bytesReturned, NULL);
    if (!ok)
    {
        qWarning() << "VideoCard: GET_FRAME failed, ch=" << channelIndex
                   << " frame_index=" << frame_index
                   << " error:" << GetLastError();
        return -1;
    }
    return (int)bytesReturned;
}

bool VideoCard::getVideoDoneIndex(ULONG channelIndex, ULONG &doneIndex)
{
    ULONG chIdx = (ULONG)channelIndex;
    ULONG value = 0;
    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(device_,
                              IOCTL_PCIES2MM_GET_VIDEO_DONE_INDEX,
                              &chIdx, sizeof(ULONG),
                              &value, sizeof(ULONG),
                              &bytesReturned, NULL);
    if (!ok)
    {
        qWarning() << "VideoCard: GET_VIDEO_DONE_INDEX ch=" << channelIndex
                   << " failed, error:" << GetLastError();
        return false;
    }
    doneIndex = value;
    return true;
}

int VideoCard::openDevice()
{
    char device_path[MAX_PATH_LEN + 1] = "";
    DWORD num_devices = getDevicePath(GUID_DEVINTERFACE_PcieS2mm, device_path, sizeof(device_path));
    if (num_devices < 1)
    {
        qWarning() << "VideoCard: no device found";
        return -1;
    }

    device_ = CreateFileA(device_path,
                          GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          NULL);
    if (device_ == INVALID_HANDLE_VALUE)
    {
        qWarning() << "VideoCard: CreateFile failed, error:" << GetLastError();
        return -1;
    }

    return 0;
}

void VideoCard::closeDevice()
{
    if (device_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
    }
}

int VideoCard::getDevicePath(GUID guid, char *devpath, size_t len_devpath)
{
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    SP_DEVICE_INTERFACE_DATA devInterfaceData;
    devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (!SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &guid, 0, &devInterfaceData))
    {
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return 0;
    }

    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetailA(hDevInfo, &devInterfaceData, NULL, 0, &requiredSize, NULL);

    std::unique_ptr<BYTE[]> detailBuf(new BYTE[requiredSize]);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detailBuf.get();
    detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

    if (!SetupDiGetDeviceInterfaceDetailA(hDevInfo, &devInterfaceData, detailData, requiredSize, NULL, NULL))
    {
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return 0;
    }

    strncpy(devpath, detailData->DevicePath, len_devpath - 1);
    devpath[len_devpath - 1] = '\0';

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return 1;
}
