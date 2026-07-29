#pragma once

#include <stdint.h>
#include <stddef.h>

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

typedef enum pcies2mm_pixel_format_t
{
    PCIES2MM_PIXFMT_UNKNOWN = 0,
    PCIES2MM_PIXFMT_YUY2 = 1,
    PCIES2MM_PIXFMT_Y210 = 2
} pcies2mm_pixel_format_t;

typedef enum pcies2mm_stream_state_t
{
    PCIES2MM_STREAM_STOPPED = 0,
    PCIES2MM_STREAM_CONFIGURED = 1,
    PCIES2MM_STREAM_RUNNING = 2
} pcies2mm_stream_state_t;

typedef struct pcies2mm_stream_desc_t
{
    uint32_t channel;
    uint32_t width;
    uint32_t height;
    pcies2mm_pixel_format_t pixel_format;
    uint32_t buffer_count;
    uint32_t flags;
} pcies2mm_stream_desc_t;

typedef struct pcies2mm_signal_status_t
{
    int connected;
    uint32_t channel;
    uint32_t width;
    uint32_t height;
    pcies2mm_pixel_format_t pixel_format;
    uint32_t bit_depth;
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
    uint32_t pending_events;
    uint64_t latest_sequence;
    uint64_t delivered_sequence;
    uint64_t active_delivery_slot;
    uint64_t next_write_slot;
    uint64_t ring_size;
} pcies2mm_debug_state_t;

typedef enum pcies2mm_event_type_t
{
    PCIES2MM_EVENT_PLUG_IN = 1,
    PCIES2MM_EVENT_PLUG_OUT = 2
} pcies2mm_event_type_t;

enum
{
    PCIES2MM_EVENT_MASK_PLUG_IN = 1u << 0,
    PCIES2MM_EVENT_MASK_PLUG_OUT = 1u << 1,
    PCIES2MM_EVENT_MASK_DEFAULT = PCIES2MM_EVENT_MASK_PLUG_IN |
                                 PCIES2MM_EVENT_MASK_PLUG_OUT
};

typedef struct pcies2mm_event_t
{
    pcies2mm_event_type_t type;
    uint64_t timestamp_ns;
} pcies2mm_event_t;

typedef void (*pcies2mm_event_callback_t)(const pcies2mm_event_t *event, void *user);
