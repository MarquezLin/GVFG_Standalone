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
    uint32_t struct_size; /* Set to sizeof(gvfg_debug_backend_stats_t) before calling. */
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

    int backend_running;
    int backend_capture_active;
    uint32_t backend_pending_events;
    uint64_t backend_latest_sequence;
    uint64_t backend_delivered_sequence;
    uint64_t backend_active_delivery_slot; /* UINT64_MAX when no frame is held by the caller. */
    uint64_t backend_next_write_slot;
    uint64_t backend_ring_size;
} gvfg_debug_backend_stats_t;

/*
 * Query driver-neutral internal backend counters.
 *
 * This exposes implementation-level counters for internal tools only. Customer
 * applications should use gvfg_get_runtime_info() instead.
 */
GVFG_API gvfg_status_t gvfg_debug_get_backend_stats(
    _In_ gvfg_handle handle,
    _Out_ gvfg_debug_backend_stats_t *out_stats);

/*
 * Copy the latest backend error detail into out_message.
 *
 * The message is UTF-8 and null-terminated when out_message_size is non-zero.
 */
GVFG_API gvfg_status_t gvfg_debug_get_last_error_detail(
    _In_ gvfg_handle handle,
    _Out_writes_to_opt_(out_message_size, out_message_size) char *out_message,
    _In_ uint32_t out_message_size);

#ifdef __cplusplus
}
#endif
