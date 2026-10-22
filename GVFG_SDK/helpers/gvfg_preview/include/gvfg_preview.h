#pragma once

/* Public image and presentation types used by gvfg_playback.h. */
#include <stdint.h>

typedef enum
{
    GVFG_PREVIEW_PIXFMT_YUY2 = 1,
    GVFG_PREVIEW_PIXFMT_Y210 = 2,
    GVFG_PREVIEW_PIXFMT_GRAY16 = 4,

    /* Four interleaved 16-bit UNORM components per pixel: R, G, B, A. */
    GVFG_PREVIEW_PIXFMT_RGBA16 = 5,

    /* DICOM native YBR_FULL_422: four 16-bit words per pixel pair,
     * ordered Y0, Y1, Cb, Cr.  Values are right-aligned to bit_depth. */
    GVFG_PREVIEW_PIXFMT_DICOM_YBR_FULL_422 = 6
} gvfg_preview_pixel_format_t;

typedef struct
{
    const void *data;
    uint64_t data_size;
    int width;
    int height;
    int pixel_format; /* gvfg_preview_pixel_format_t */
    int bit_depth;
    int row_bytes;
    uint64_t frame_id;
} gvfg_preview_frame_t;

typedef struct
{
    int active;
    int width;
    int height;
    int bit_depth;
    char pixel_format[32];
    char adapter_name[160];
    int adapter_index;
} gvfg_preview_info_t;

/*
 * Preview presentation statistics.
 *
 * present_fps counts only frames accepted by DXGI Present, not physical
 * scanout. The rate uses successful presents from the most recent five
 * seconds. Pending preview replacements are excluded from this rate.
 */
typedef struct
{
    double present_fps;
    uint64_t presented_frames;
    uint64_t skipped_presents;
} gvfg_preview_stats_t;

/* Lifetime counters (until shutdown); configure/clear do not reset them.
 * submitted = presented + replaced + busy + failed + cancelled + in_flight.
 * Presented means DXGI accepted Present, not proof of physical scanout.
 * Failed render API calls are counted by the caller, not in submitted.
 */
typedef struct
{
    uint64_t submitted;
    uint64_t presented;
    uint64_t replaced; /* Queued frames discarded after the 50 ms age limit. */
    uint64_t busy;
    uint64_t failed;
    uint64_t cancelled;
    uint64_t in_flight;
    uint64_t last_presented_id;
    uint64_t present_busy;
    uint64_t slots_busy;
    uint64_t last_busy_id;
} gvfg_preview_delivery_stats_t;
