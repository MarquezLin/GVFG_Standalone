#pragma once

/*
 * Optional GVFG preview helper API.
 *
 * This helper renders frames returned by gvfg_read_frame(). It is intentionally
 * separate from gvfg.dll: applications own the read loop and decide whether to
 * render through this DLL, render themselves, record, snapshot, or process data.
 */

#include <gvfg_capture.h>

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

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_create(
    _Outptr_ gvfg_preview_handle *out_handle);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_destroy(
    _In_opt_ gvfg_preview_handle handle);

/*
 * Attach a native window handle for preview output.
 *
 * On Windows, native_window_handle is an HWND.
 */
GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_attach_window(
    _In_ gvfg_preview_handle handle,
    _In_ void *native_window_handle);

/*
 * Synchronously render one frame.
 *
 * The frame remains owned by gvfg.dll. The caller should call this between
 * gvfg_read_frame() and gvfg_release_frame().
 */
GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_render_frame(
    _In_ gvfg_preview_handle handle,
    _In_ const gvfg_frame_t *frame);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_get_info(
    _In_ gvfg_preview_handle handle,
    _Out_ gvfg_preview_info_t *out_info);

GVFG_PREVIEW_API gvfg_preview_status_t gvfg_preview_shutdown(
    _In_ gvfg_preview_handle handle);

GVFG_PREVIEW_API const char *gvfg_preview_strerror(
    _In_ gvfg_preview_status_t status);

#ifdef __cplusplus
}
#endif
