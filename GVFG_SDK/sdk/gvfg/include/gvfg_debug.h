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
    int running;
    int frame_held;
    uint32_t event_queue_depth;
    uint64_t video_dma_event_wakes;
    uint64_t extra_video_event_wakes;
    uint64_t video_frames_from_lib;
    uint64_t audio_dma_event_wakes;
    uint64_t extra_audio_event_wakes;
    uint64_t audio_frames_from_lib;
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
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN int channel_index,
    GVFG_PARAM_OUT gvfg_debug_backend_stats_t *out_stats);

/* GIGABYTELIB GAP: GvfgSdk.lib 1.0.2 has no declared register-access API. */
GVFG_API gvfg_status_t gvfg_debug_read_register(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN uint32_t offset,
    GVFG_PARAM_OUT uint32_t *out_value);

GVFG_API gvfg_status_t gvfg_debug_write_register(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN uint32_t offset,
    GVFG_PARAM_IN uint32_t value);

#ifdef __cplusplus
}
#endif
