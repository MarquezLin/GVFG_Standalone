#pragma once

#include <stdint.h>
#include <stddef.h>

typedef enum gigabyte_status_t
{
    GIGABYTE_OK = 0,
    GIGABYTE_EINVAL,
    GIGABYTE_ENODEV,
    GIGABYTE_ESTATE,
    GIGABYTE_ENOTSUP,
    GIGABYTE_ETIMEOUT,
    GIGABYTE_EIO
} gigabyte_status_t;

typedef enum gigabyte_pixel_format_t
{
    GIGABYTE_PIXFMT_UNKNOWN = 0,
    GIGABYTE_PIXFMT_Y210 = 2,
    GIGABYTE_PIXFMT_YUY2 = 3
} gigabyte_pixel_format_t;

typedef enum gigabyte_stream_state_t
{
    GIGABYTE_STREAM_STOPPED = 0,
    GIGABYTE_STREAM_CONFIGURED = 1,
    GIGABYTE_STREAM_RUNNING = 2
} gigabyte_stream_state_t;

typedef struct gigabyte_signal_status_t
{
    int connected;
    uint32_t channel;
    uint32_t width;
    uint32_t height;
    gigabyte_pixel_format_t pixel_format;
    uint32_t bit_depth;
} gigabyte_signal_status_t;

typedef struct gigabyte_frame_t
{
    const void *data; /* Valid until the next GigabyteCaptureSession::wait_frame() or close(). */
    size_t data_size_bytes;
    uint64_t frame_id;
    uint64_t timestamp_ns;
    uint32_t width;
    uint32_t height;
    gigabyte_pixel_format_t pixel_format;
    uint32_t bit_depth;
} gigabyte_frame_t;

typedef struct gigabyte_audio_format_t
{
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t frames_per_second;
    uint32_t frame_bytes;
    uint32_t block_align;
} gigabyte_audio_format_t;

typedef struct gigabyte_stream_stats_t
{
    gigabyte_stream_state_t state;
    uint64_t frames_captured;
    uint64_t frames_delivered;
    uint64_t dma_errors;
    uint64_t interrupt_count;
} gigabyte_stream_stats_t;

typedef struct gigabyte_debug_state_t
{
    int running;
    int capture_active;
    uint64_t latest_sequence;
    uint64_t delivered_sequence;
    uint64_t audio_dma_event_wakes;
    uint64_t extra_audio_event_wakes;
    uint64_t audio_frames_from_driver;
    uint64_t audio_bytes_from_driver;
    int get_frame_zero_copy;
    uint64_t get_frame_timing_samples;
    double get_frame_timing_average_us;
    double get_frame_timing_max300_us;
    double get_frame_timing_max_us;
} gigabyte_debug_state_t;

typedef enum gigabyte_event_type_t
{
    GIGABYTE_EVENT_PLUG_IN = 1,
    GIGABYTE_EVENT_PLUG_OUT = 2,
    GIGABYTE_EVENT_STREAM_READY = 3,
    GIGABYTE_EVENT_FORMAT_CHANGE_BEGIN = 4
} gigabyte_event_type_t;

enum
{
    GIGABYTE_EVENT_MASK_PLUG_IN = 1u << 0,
    GIGABYTE_EVENT_MASK_PLUG_OUT = 1u << 1,
    GIGABYTE_EVENT_MASK_STREAM_READY = 1u << 2,
    GIGABYTE_EVENT_MASK_FORMAT_CHANGE_BEGIN = 1u << 3,
    GIGABYTE_EVENT_MASK_DEFAULT = GIGABYTE_EVENT_MASK_PLUG_IN |
                                 GIGABYTE_EVENT_MASK_PLUG_OUT |
                                 GIGABYTE_EVENT_MASK_STREAM_READY |
                                 GIGABYTE_EVENT_MASK_FORMAT_CHANGE_BEGIN
};

typedef void (*gigabyte_event_callback_t)(gigabyte_event_type_t event, void *user);
