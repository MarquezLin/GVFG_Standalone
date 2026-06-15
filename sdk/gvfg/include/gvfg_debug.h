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
    int sdk_running;
    int frame_held;
    uint32_t event_queue_depth;

    double runtime_fps;
    uint64_t frames_returned;
    int last_frame_width;
    int last_frame_height;
    int last_frame_pixel_format;
    int last_frame_bit_depth;

    int backend_state;
    uint64_t backend_frames_captured;
    uint64_t backend_frames_delivered;
    uint64_t backend_frames_dropped;
    uint64_t backend_dma_errors;
    uint64_t backend_interrupt_count;
    uint64_t backend_wait_timeouts;
} gvfg_debug_backend_stats_t;

typedef struct
{
    uint32_t valid_mask;
    uint32_t width_valid;
    uint32_t height_valid;
    uint32_t width_raw;
    uint32_t height_raw;
    uint32_t video_format_raw;
    uint32_t frame_rate_raw;
    uint32_t bit_depth_raw;
    uint32_t status_raw;
} gvfg_debug_fpga_signal_raw_t;

/*
 * Query driver-neutral internal backend counters.
 *
 * This exposes implementation-level counters for internal tools only. Customer
 * applications should use gvfg_get_runtime_info() instead.
 */
GVFG_API gvfg_status_t gvfg_debug_get_backend_stats(gvfg_handle handle,
                                                    gvfg_debug_backend_stats_t *out_stats);

/*
 * Query raw FPGA signal values for internal hardware/FPGA debugging.
 *
 * These values are intentionally excluded from gvfg_capture.h.
 */
GVFG_API gvfg_status_t gvfg_debug_get_fpga_signal_raw(gvfg_handle handle,
                                                      gvfg_debug_fpga_signal_raw_t *out_raw);

/*
 * Copy the latest backend error detail into out_message.
 *
 * The message is UTF-8 and null-terminated when out_message_size is non-zero.
 */
GVFG_API gvfg_status_t gvfg_debug_get_last_error_detail(gvfg_handle handle,
                                                        char *out_message,
                                                        uint32_t out_message_size);

#ifdef __cplusplus
}
#endif
