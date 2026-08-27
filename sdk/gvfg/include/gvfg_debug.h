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

    int backend_state;
    uint64_t backend_frames_captured;
    uint64_t backend_frames_delivered;
    uint64_t backend_dma_errors;
    uint64_t backend_interrupt_count;
    uint64_t backend_wait_timeouts;

    int backend_running;
    int backend_capture_active;
    uint64_t backend_latest_sequence;
    uint64_t backend_delivered_sequence;

    int get_frame_zero_copy;
    uint64_t get_frame_timing_samples;
    double get_frame_timing_average_us;
    double get_frame_timing_max300_us;
    double get_frame_timing_max_us;
} gvfg_debug_backend_stats_t;

/*
 * Query driver-neutral internal backend counters.
 *
 * This exposes implementation-level counters for internal tools only. Customer
 * applications should use gvfg_get_channel_runtime_info() instead.
 */
GVFG_API gvfg_status_t gvfg_debug_get_channel_backend_stats(
    _In_ gvfg_handle handle,
    _In_ int channel_index,
    _Out_ gvfg_debug_backend_stats_t *out_stats);

/*
 * Read or write one 32-bit BAR-relative hardware register.
 *
 * The offset must be 4-byte aligned. Register writes can disrupt active DMA,
 * interrupt handling, or video capture and are intended for internal tools.
 */
GVFG_API gvfg_status_t gvfg_debug_read_register(
    _In_ gvfg_handle handle,
    _In_ uint32_t offset,
    _Out_ uint32_t *out_value);

GVFG_API gvfg_status_t gvfg_debug_write_register(
    _In_ gvfg_handle handle,
    _In_ uint32_t offset,
    _In_ uint32_t value);

#ifdef __cplusplus
}
#endif
