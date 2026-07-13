#pragma once

#include <stdint.h>
#include <stddef.h>

#define PCIES2MM_MAX_PLANES 3

typedef enum pcies2mm_status_t
{
    PCIES2MM_OK = 0,
    PCIES2MM_EINVAL,
    PCIES2MM_ENODEV,
    PCIES2MM_ESTATE,
    PCIES2MM_ENOTSUP,
    PCIES2MM_ETIMEOUT,
    PCIES2MM_EIO
} pcies2mm_status_t;

typedef enum pcies2mm_input_t
{
    PCIES2MM_INPUT_UNKNOWN = 0,
    PCIES2MM_INPUT_SDI = 1,
    PCIES2MM_INPUT_HDMI = 2
} pcies2mm_input_t;

typedef enum pcies2mm_pixel_format_t
{
    PCIES2MM_PIXFMT_UNKNOWN = 0,
    PCIES2MM_PIXFMT_YUY2 = 1,
    PCIES2MM_PIXFMT_UYVY = 2,
    PCIES2MM_PIXFMT_RGB24 = 3,
    PCIES2MM_PIXFMT_BGRX32 = 4,
    PCIES2MM_PIXFMT_NV12 = 5,
    PCIES2MM_PIXFMT_P010 = 6,
    PCIES2MM_PIXFMT_Y210 = 7,
    PCIES2MM_PIXFMT_YUV444 = 8
} pcies2mm_pixel_format_t;

typedef enum pcies2mm_stream_state_t
{
    PCIES2MM_STREAM_STOPPED = 0,
    PCIES2MM_STREAM_CONFIGURED = 1,
    PCIES2MM_STREAM_RUNNING = 2
} pcies2mm_stream_state_t;

typedef struct pcies2mm_stream_desc_t
{
    pcies2mm_input_t input;
    uint32_t width;
    uint32_t height;
    pcies2mm_pixel_format_t pixel_format;
    uint32_t buffer_count;
    uint32_t flags;
} pcies2mm_stream_desc_t;

typedef struct pcies2mm_signal_status_t
{
    int signal_locked;
    pcies2mm_input_t input;
    uint32_t width;
    uint32_t height;
    pcies2mm_pixel_format_t pixel_format;
    uint32_t bit_depth;
    uint32_t fpga_valid_mask;       /* bit0:0x0c, bit1:0x18, bit2:0x1c, bit3:0x180 */
    uint32_t fpga_width_valid;      /* Non-zero when FPGA 0x10 read succeeded. */
    uint32_t fpga_height_valid;     /* Non-zero when FPGA 0x14 read succeeded. */
    uint32_t fpga_width_raw;        /* Raw FPGA 0x10 width register. */
    uint32_t fpga_height_raw;       /* Raw FPGA 0x14 height register. */
    uint32_t fpga_video_format_raw; /* 0x0c: 0=yuv422, 1=rgb, 2=yuv444, 3=yuv420 */
    uint32_t fpga_frame_rate_raw;   /* 0x18 low nibble: frame-rate code */
    uint32_t fpga_bit_depth_raw;    /* 0x1c: 8 or 10 */
    uint32_t fpga_status_raw;       /* 0x180: bit0 SDI lock, bit1 SDI DDR, bit2 HDMI lock, bit3 HDMI DDR */
} pcies2mm_signal_status_t;

typedef struct pcies2mm_frame_t
{
    const void *data; /* Valid until the next PcieS2mmCaptureSession::wait_frame() or close(). */
    size_t data_size_bytes;
    uint64_t frame_id;
    uint32_t width;
    uint32_t height;
    pcies2mm_pixel_format_t pixel_format;
    uint32_t bit_depth;
} pcies2mm_frame_t;

typedef struct pcies2mm_stream_stats_t
{
    pcies2mm_stream_state_t state;
    uint64_t frames_captured;
    uint64_t frames_delivered;
    uint64_t frames_dropped;
    uint64_t dma_errors;
    uint64_t interrupt_count;
} pcies2mm_stream_stats_t;

typedef struct pcies2mm_debug_state_t
{
    int running;
    int capture_active;
    int data_worker_stop;
    uint32_t pending_events;
    uint64_t latest_sequence;
    uint64_t delivered_sequence;
    uint64_t active_delivery_slot;
    uint64_t next_write_slot;
    uint64_t ring_size;
} pcies2mm_debug_state_t;

typedef enum pcies2mm_event_type_t
{
    PCIES2MM_EVENT_VIDEO_IRQ = 1,
    PCIES2MM_EVENT_PLUG_IN = 2,
    PCIES2MM_EVENT_PLUG_OUT = 3,
    PCIES2MM_EVENT_CAPTURE_PAUSED = 4,
    PCIES2MM_EVENT_CAPTURE_RESUMED = 5
} pcies2mm_event_type_t;

enum
{
    PCIES2MM_EVENT_MASK_VIDEO_IRQ = 1u << 0,
    PCIES2MM_EVENT_MASK_PLUG_IN = 1u << 1,
    PCIES2MM_EVENT_MASK_PLUG_OUT = 1u << 2,
    PCIES2MM_EVENT_MASK_CAPTURE_PAUSED = 1u << 3,
    PCIES2MM_EVENT_MASK_CAPTURE_RESUMED = 1u << 4,
    PCIES2MM_EVENT_MASK_HOTPLUG = PCIES2MM_EVENT_MASK_PLUG_IN |
                              PCIES2MM_EVENT_MASK_PLUG_OUT |
                              PCIES2MM_EVENT_MASK_CAPTURE_PAUSED |
                              PCIES2MM_EVENT_MASK_CAPTURE_RESUMED,
    PCIES2MM_EVENT_MASK_DEFAULT = PCIES2MM_EVENT_MASK_HOTPLUG,
    PCIES2MM_EVENT_MASK_ALL = PCIES2MM_EVENT_MASK_VIDEO_IRQ | PCIES2MM_EVENT_MASK_HOTPLUG
};

typedef struct pcies2mm_event_t
{
    pcies2mm_event_type_t type;
    uint32_t irq_bit;
    uint32_t irq_mask;
    uint64_t timestamp_ns;
} pcies2mm_event_t;

typedef void (*pcies2mm_event_callback_t)(const pcies2mm_event_t *event, void *user);
