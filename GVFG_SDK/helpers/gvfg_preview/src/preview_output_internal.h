#pragma once

/* Private static backend; not part of the customer API. */
#include "gvfg_preview.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GVFG_PREVIEW_OK = 0,
    GVFG_PREVIEW_EINVAL = -1,
    GVFG_PREVIEW_ESTATE = -2,
    GVFG_PREVIEW_ENOTSUP = -3,
    GVFG_PREVIEW_ERENDER = -4
} gvfg_preview_status_t;

typedef struct gvfg_preview_handle_t *gvfg_preview_handle;

gvfg_preview_status_t gvfg_preview_get_delivery_stats(
    gvfg_preview_handle handle, gvfg_preview_delivery_stats_t *out_stats);

/* Call after stopping submissions. Wait at most timeout_ms for queued work.
 * Returns ESTATE if work remains; does not discard or reset counters. */
gvfg_preview_status_t gvfg_preview_wait_idle(
    gvfg_preview_handle handle, uint32_t timeout_ms);

gvfg_preview_status_t gvfg_preview_create(
    gvfg_preview_handle *out_handle);

gvfg_preview_status_t gvfg_preview_destroy(
    gvfg_preview_handle handle);

/*
 * Attach a native window handle for preview output.
 *
 * On Windows, native_window_handle is an HWND.
 */
gvfg_preview_status_t gvfg_preview_attach_window(
    gvfg_preview_handle handle,
    void *native_window_handle);

/* Pre-create GPU resources before the first captured frame is held. */
gvfg_preview_status_t gvfg_preview_prepare(
    gvfg_preview_handle handle,
    int width,
    int height,
    int bit_depth);

/*
 * Upload one frame to an internal GPU slot and queue it for preview.
 *
 * The frame memory remains owned by the caller and must remain valid until
 * this function returns. GPU conversion and Present continue on the internal
 * preview worker. Up to three pending frames are retained in FIFO order.
 * Frames waiting longer than 50 ms are discarded; if the queue is full,
 * the incoming frame is skipped. No SDK frame is retained for queueing.
 * The age limit excludes time spent rendering or waiting inside Present.
 */
gvfg_preview_status_t gvfg_preview_render_frame(
    gvfg_preview_handle handle,
    const gvfg_preview_frame_t *frame);

/* Replace the last presented frame with a solid black frame. */
gvfg_preview_status_t gvfg_preview_clear(
    gvfg_preview_handle handle);

gvfg_preview_status_t gvfg_preview_get_info(
    gvfg_preview_handle handle,
    gvfg_preview_info_t *out_info);

gvfg_preview_status_t gvfg_preview_get_stats(
    gvfg_preview_handle handle,
    gvfg_preview_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
