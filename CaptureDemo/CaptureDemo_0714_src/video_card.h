#ifndef VIDEO_CARD_H_
#define VIDEO_CARD_H_

#include <memory>
#include <Windows.h>
#include <functional>
#include <thread>
#include "reg.h"

/* ========================================================================
 * New Driver Interface Definitions (mirrors pcie_s2mm_driver/Public.h)
 * ======================================================================== */

// {8c47b9c3-1faa-4557-bc1d-f225d26c9e91}
extern const GUID GUID_DEVINTERFACE_PcieS2mm;

/* IOCTL Codes */
#define IOCTL_PCIES2MM_WRITE_REG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PCIES2MM_READ_REG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PCIES2MM_REGISTER_EVENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PCIES2MM_UNREGISTER_EVENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PCIES2MM_GET_FRAME CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_PCIES2MM_WRITE_REG_BAR1 CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PCIES2MM_READ_REG_BAR1 CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PCIES2MM_GET_VIDEO_DONE_INDEX CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Register Access Structure (shared with driver) */
typedef struct _PCIES2MM_REG_ACCESS
{
    ULONG Offset; // Register byte offset from BAR0 base
    ULONG Value;  // Value to write / value read back
} PCIES2MM_REG_ACCESS, *PPCIES2MM_REG_ACCESS;

/* Event Registration Structure (shared with driver) */
#define PCIES2MM_EVENT_TYPE_VIDEO_DMA 0
#define PCIES2MM_EVENT_TYPE_VIDEO_FORMAT_CHANGE 1
#define PCIES2MM_EVENT_TYPE_VIDEO_PLUGIN 2
#define PCIES2MM_EVENT_TYPE_VIDEO_UNPLUG 3
#define PCIES2MM_EVENT_TYPE_AUDIO_DMA 4
typedef struct _PCIES2MM_EVENT_REG
{
    ULONG Type;         // Event type: 0=VideoDma, 1=VideoFormatChange, 2=VideoPlugin, 3=VideoUnplug, 4=AudioDma
    ULONG ChannelIndex; // Channel index (0 or 1)
    HANDLE EventHandle; // User-mode event handle to register
} PCIES2MM_EVENT_REG, *PPCIES2MM_EVENT_REG;

/* ========================================================================
 * DMA Buffer Configuration (must match driver Device.h)
 * ======================================================================== */

#define PCIE_S2MM_DMA_BUFFER_COUNT 16
#define PCIE_S2MM_DMA_BUFFER_SIZE (1920 * 1080 * 2) // 1080p YUV422

/* ========================================================================
 * Max number of video capture channels
 * ======================================================================== */
#define PCIE_S2MM_MAX_CHANNELS 2

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                  \
((ULONG)(UCHAR)(ch0)        |                       \
 ((ULONG)(UCHAR)(ch1) << 8)  |                       \
 ((ULONG)(UCHAR)(ch2) << 16) |                       \
 ((ULONG)(UCHAR)(ch3) << 24))
#endif

int calcFrameBytes(int width, int height, UINT32 fourcc);

/* ========================================================================
 * Per-channel capture state
 * ======================================================================== */
struct ChannelState
{
    ULONG video_width = 1920;
    ULONG video_height = 1080;
    UINT32 fourcc = 0;
    unsigned char *frame_buffer = nullptr;
    std::shared_ptr<std::function<void(uchar *, int, int)>> recv_data_cb = nullptr;
    std::shared_ptr<std::thread> capture_thread = nullptr;
    std::shared_ptr<std::thread> video_module_thread = nullptr;
    HANDLE dma_interrupt_event = NULL;
    HANDLE video_format_change_event = NULL;
    HANDLE video_plugin_event = NULL;
    HANDLE video_unplug_event = NULL;
    HANDLE audio_dma_event = NULL;
    bool capturing = false;
    bool quit_module_event_thread = false;

    /* Config BRAM base address for this channel video registers */
    ULONG video_base = 0;

    /* Interrupt mask bit for this channel */
    ULONG irq_mask_bit = 0;
    ULONG frame_count = 0;
    ULONG first_frame_timestamp_ms = 0;
};

class VideoCard
{
public:
    VideoCard();
    VideoCard(const VideoCard &card) = delete;
    VideoCard &operator=(const VideoCard &card) = delete;
    virtual ~VideoCard();

    int init();
    void uninit();

    /*
     * @fun: Start capture on specified channel
     * @param[in] channelIndex: channel index (0 or 1)
     * @param[in] width: video width in pixels
     * @param[in] height: video height in pixels
     * @param[in] cb: callback function receiving YUV422 frame data
     * @return 0: success, non-0: failure
     */
    int startCapture(ULONG channelIndex, int width, int height, UINT32 fourcc, const std::function<void(uchar *, int, int)> &cb);

    /*
     * @fun: Stop capture on specified channel
     * @param[in] channelIndex: channel index (0 or 1)
     */
    void stopCapture(ULONG channelIndex);

public:
    /* Device management */
    int openDevice();
    void closeDevice();
    static int getDevicePath(GUID guid, char *devpath, size_t len_devpath);

    /* Register access via IOCTL (BAR0) */
    bool writeReg(ULONG offset, ULONG value);
    bool readReg(ULONG offset, ULONG &value);

    /* Register access via IOCTL (BAR1) */
    bool writeRegBar1(ULONG offset, ULONG value);
    bool readRegBar1(ULONG offset, ULONG &value);

    /* Frame data retrieval via IOCTL */
    int getFrame(ULONG channelIndex, BYTE *buffer, DWORD buffer_size, ULONG frame_index);

    /* Get video DMA done index from driver */
    bool getVideoDoneIndex(ULONG channelIndex, ULONG &doneIndex);

    /* Per-channel capture thread entry */
    void captureThread(ULONG channelIndex);
    void videoModuleThread(ULONG channelIndex);

    /* Single device handle (new driver uses one device interface) */
    HANDLE device_ = INVALID_HANDLE_VALUE;

    /* Per-channel state array */
    ChannelState channels_[PCIE_S2MM_MAX_CHANNELS];

    bool is_initialized_ = false;
};

#endif // VIDEO_CARD_H
