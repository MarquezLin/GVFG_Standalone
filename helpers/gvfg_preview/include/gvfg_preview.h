#pragma once

/*
 * Optional GVFG preview helper API.
 *
 * Standalone preview helper. Applications provide the frame buffer, pixel
 * format, and row stride explicitly; gvfg_preview.dll does not depend on
 * gvfg.dll.
 */

#include <stdint.h>

#ifdef _WIN32
#ifdef GVFG_PREVIEW_BUILD
#define GVFG_PREVIEW_API __declspec(dllexport)
#else
#define GVFG_PREVIEW_API __declspec(dllimport)
#endif
#else
#define GVFG_PREVIEW_API
#endif

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

typedef enum
{
    GVFG_PREVIEW_PIXFMT_Y210 = 2,
    GVFG_PREVIEW_PIXFMT_YVYU = 3
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
 * present_fps counts only frames accepted by DXGI Present. A frame skipped
 * because the non-blocking swapchain is busy is excluded. The rate uses
 * successful presents from the most recent five seconds.
 */
typedef struct
{
    double present_fps;
    uint64_t presented_frames;
    uint64_t skipped_presents;
} gvfg_preview_stats_t;

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_create(
    gvfg_preview_handle *out_handle);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_destroy(
    gvfg_preview_handle handle);

/*
 * Attach a native window handle for preview output.
 *
 * On Windows, native_window_handle is an HWND.
 */
GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_attach_window(
    gvfg_preview_handle handle,
    void *native_window_handle);

/* Pre-create GPU resources before the first captured frame is held. */
GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_prepare(
    gvfg_preview_handle handle,
    int width,
    int height,
    int bit_depth);

/*
 * Synchronously render one frame.
 *
 * The frame memory remains owned by the caller and must remain valid until
 * this synchronous function returns.
 */
GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_render_frame(
    gvfg_preview_handle handle,
    const gvfg_preview_frame_t *frame);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_get_info(
    gvfg_preview_handle handle,
    gvfg_preview_info_t *out_info);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_get_stats(
    gvfg_preview_handle handle,
    gvfg_preview_stats_t *out_stats);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_shutdown(
    gvfg_preview_handle handle);

GVFG_PREVIEW_API const char *gvfg_preview_strerror(
    gvfg_preview_status_t status);

#ifdef __cplusplus
}
#endif
