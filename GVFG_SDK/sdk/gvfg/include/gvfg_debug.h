#pragma once

/*
 * Internal GVFG debug API.
 *
 * Do not ship this header in customer/demo packages. It is intended for
 * internal driver, FPGA, hardware bring-up, and diagnostic tools.
 */

#include "gvfg_capture.h"

#include <stdint.h>

#define GVFG_DEBUG_API GVFG_API

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
    uint64_t audio_driver_frame_count;
    int audio_driver_frame_count_valid;
    int audio_frame_event_valid;
    int audio_extra_event_valid;
    int audio_enabled;
    int stream_running;
    uint64_t get_frame_timing_samples;
    double get_frame_timing_average_us;
    double get_frame_timing_max300_us;
    double get_frame_timing_max_us;
    int32_t last_vendor_error_code;
    char last_vendor_api[64];
    char last_vendor_error_name[64];
} gvfg_debug_backend_stats_t;

typedef struct
{
    uint32_t video_channel_count;
    int has_audio;
} gvfg_debug_device_capabilities_t;

/* CPU wall times around the synchronous OSD delivery path, in milliseconds.
 * SUBMIT is command submission, not GPU execution time. READBACK_WAIT includes
 * waiting for earlier GPU work; FRAME_COPY can overlap that GPU work.
 * PROCESS_TOTAL includes the other process stages, but not BACKEND_RELEASE.
 */
typedef enum
{
    GVFG_DEBUG_OSD_LOCK_WAIT,
    GVFG_DEBUG_OSD_SURFACE,
    GVFG_DEBUG_OSD_PREPARE,
    GVFG_DEBUG_OSD_UPLOAD,
    GVFG_DEBUG_OSD_SUBMIT,
    GVFG_DEBUG_OSD_FRAME_COPY,
    GVFG_DEBUG_OSD_READBACK_WAIT,
    GVFG_DEBUG_OSD_ROI_COPY,
    GVFG_DEBUG_OSD_BACKEND_RELEASE,
    GVFG_DEBUG_OSD_PROCESS_TOTAL,
    GVFG_DEBUG_OSD_STAGE_COUNT
} gvfg_debug_osd_stage_t;

typedef struct
{
    uint64_t frame_id;
    int osd_applied;
    int surface_rebuilt;
    uint32_t roi_width;
    uint32_t roi_height;
    double stage_ms[GVFG_DEBUG_OSD_STAGE_COUNT];
} gvfg_debug_osd_frame_timing_t;

/* Internal diagnostic variant of read_channel_frame, with identical frame
 * ownership/release semantics. Timing is collected only through this entry
 * point; normal customer reads do not perform these extra clock queries.
 * On success timing.frame_id matches out_frame->frame_id. No GPU queries,
 * extra Flush, or additional synchronization are introduced.
 */
GVFG_DEBUG_API gvfg_status_t gvfg_debug_read_channel_frame_timed(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN int channel_index,
    GVFG_PARAM_OUT gvfg_frame_t *out_frame,
    GVFG_PARAM_OUT gvfg_debug_osd_frame_timing_t *out_timing,
    GVFG_PARAM_IN uint32_t timeout_ms);

/* Acquisition starts after a video-frame event is ready, before retrieving
 * or copying its data. Excludes waiting for the next frame. The public frame
 * timestamp remains unchanged (data acquired, before OSD).
 * A separate type/entry point preserves the older timed-read ABI.
 */
typedef struct
{
    gvfg_debug_osd_frame_timing_t osd;
    uint64_t acquisition_begin_ns;
} gvfg_debug_frame_read_timing_t;

enum
{
    GVFG_DEBUG_OSD_EXPERIMENT_ENABLED = 1u,
    GVFG_DEBUG_OSD_EXPERIMENT_USED = 2u,
    GVFG_DEBUG_OSD_EXPERIMENT_REJECTED = 4u
};

typedef enum
{
    GVFG_DEBUG_OSD_PAGE_OK = 0,
    GVFG_DEBUG_OSD_PAGE_NO_POINTER,
    GVFG_DEBUG_OSD_PAGE_INVALID_RANGE,
    GVFG_DEBUG_OSD_PAGE_QUERY_FAILED,
    GVFG_DEBUG_OSD_PAGE_NOT_COMMITTED,
    GVFG_DEBUG_OSD_PAGE_GUARD_OR_NOACCESS,
    GVFG_DEBUG_OSD_PAGE_NOT_READWRITE,
    GVFG_DEBUG_OSD_PAGE_INVALID_REGION
} gvfg_debug_osd_page_reason_t;

/* Separate diagnostic type: existing timing/flags ABI remains unchanged.
 * Native Windows values; win32_error is meaningful only for QUERY_FAILED.
 * Does not expose addresses or imply DMA safety. */
typedef struct
{
    uint32_t reason;
    uint32_t protect;
    uint32_t state;
    uint32_t type;
    uint32_t win32_error;
    uint32_t query_bytes;
} gvfg_debug_osd_page_check_t;

/* Query before releasing the matching successfully delivered frame. */
GVFG_DEBUG_API gvfg_status_t gvfg_debug_get_channel_osd_page_check(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN int channel_index,
    GVFG_PARAM_IN uint64_t frame_id,
    GVFG_PARAM_OUT gvfg_debug_osd_page_check_t *out_check);

/* frame_id=0 queries build configuration only. Otherwise the frame must still
 * be held and its ID must match. Never implies a DMA ownership guarantee.
 * Separate query preserves all existing timing struct layouts/read ABIs.
 */
GVFG_DEBUG_API gvfg_status_t gvfg_debug_get_channel_osd_experiment_flags(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN int channel_index,
    GVFG_PARAM_IN uint64_t frame_id,
    GVFG_PARAM_OUT uint32_t *out_flags);

GVFG_DEBUG_API gvfg_status_t gvfg_debug_read_channel_frame_profiled(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN int channel_index,
    GVFG_PARAM_OUT gvfg_frame_t *out_frame,
    GVFG_PARAM_OUT gvfg_debug_frame_read_timing_t *out_timing,
    GVFG_PARAM_IN uint32_t timeout_ms);

/*
 * Query driver-neutral internal backend counters.
 *
 * This exposes implementation-level counters for internal tools only. Customer
 * applications should use gvfg_get_channel_runtime_info() instead.
 */
GVFG_DEBUG_API gvfg_status_t gvfg_debug_get_channel_backend_stats(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN int channel_index,
    GVFG_PARAM_OUT gvfg_debug_backend_stats_t *out_stats);

/* Query device capabilities for internal diagnostics. At least one channel
 * must already be open on handle. */
GVFG_DEBUG_API gvfg_status_t gvfg_debug_get_device_capabilities(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_OUT gvfg_debug_device_capabilities_t *out_capabilities);

/* GIGABYTELIB GAP: GvfgSdk.lib 1.0.2 has no declared register-access API. */
GVFG_DEBUG_API gvfg_status_t gvfg_debug_read_register(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN uint32_t offset,
    GVFG_PARAM_OUT uint32_t *out_value);

GVFG_DEBUG_API gvfg_status_t gvfg_debug_write_register(
    GVFG_PARAM_IN gvfg_handle handle,
    GVFG_PARAM_IN uint32_t offset,
    GVFG_PARAM_IN uint32_t value);

#ifdef __cplusplus
}
#endif

#undef GVFG_DEBUG_API
