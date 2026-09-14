#pragma once

/*
 * Internal GVFG debug API.
 *
 * Do not ship this header in customer/demo packages. It is intended for
 * internal driver, FPGA, hardware bring-up, and diagnostic tools.
 */

#include "gvfg_capture.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int width;        /* Width of the most recent frame returned by gvfg_read_channel_frame(). */
    int height;       /* Height of the most recent frame returned by gvfg_read_channel_frame(). */
    int bit_depth;    /* Bits per color channel of the frame buffer. */
    int pixel_format; /* gvfg_pixel_format_t value: YUY2 or Y210. */
    int valid;        /* Non-zero after a frame is returned during the current running session. */
} gvfg_debug_last_frame_info_t;

typedef struct
{
    int sdk_running;
    int frame_held;
    uint32_t event_queue_depth;

    double runtime_fps;
    uint64_t frames_returned;
    gvfg_debug_last_frame_info_t last_frame;

    int session_state;
    uint64_t session_frames_captured;
    uint64_t session_frames_delivered;
    uint64_t session_dma_errors;
    uint64_t session_interrupt_count;
    uint64_t session_wait_timeouts;

    int session_running;
    int session_capture_active;
    uint64_t session_latest_sequence;
    uint64_t session_delivered_sequence;
    uint64_t audio_dma_event_wakes;
    uint64_t extra_audio_event_wakes;
    uint64_t audio_frames_from_driver;
    uint64_t audio_bytes_from_driver;

    int get_frame_zero_copy;
    uint64_t get_frame_timing_samples;
    double get_frame_timing_average_us;
    double get_frame_timing_max300_us;
    double get_frame_timing_max_us;
} gvfg_debug_channel_stats_t;

/*
 * Query driver-neutral internal session counters.
 *
 * This exposes implementation-level counters for internal tools only. Customer
 * applications should use gvfg_get_channel_runtime_info() instead.
 */
GVFG_API gvfg_status_t gvfg_debug_get_channel_stats(
    _In_ gvfg_handle handle,
    _In_ int channel_index,
    _Out_ gvfg_debug_channel_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
