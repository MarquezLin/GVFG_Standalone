#pragma once

/*
 * Optional GVFG frame conversion helper.
 *
 * Link this helper only when the application needs snapshot/export-friendly
 * buffers. Core capture frames from gvfg.dll remain native hardware buffers.
 */

#include <gvfg_capture.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef GVFG_CONVERT_BUILD
#define GVFG_CONVERT_API __declspec(dllexport)
#else
#define GVFG_CONVERT_API __declspec(dllimport)
#endif
#else
#define GVFG_CONVERT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GVFG_CONVERT_FMT_BGRA8 = 1,  /* 8-bit BGRA, alpha 255. */
    GVFG_CONVERT_FMT_RGB48 = 2,  /* 16-bit RGB container, useful for 10-bit snapshots. */
    GVFG_CONVERT_FMT_RGBA64 = 3  /* 16-bit RGBA container, alpha 65535. */
} gvfg_convert_format_t;

typedef struct gvfg_convert_frame_t *gvfg_convert_frame;

typedef struct
{
    int width;            /* Frame width in pixels. Use 0 to size from source during conversion. */
    int height;           /* Frame height in pixels. Use 0 to size from source during conversion. */
    int pixel_format;     /* gvfg_convert_format_t destination format. */
    int row_bytes;        /* Destination row stride in bytes. Use 0 for helper default. */
    uint64_t data_size;   /* Output buffer size in bytes, filled after create/convert. */
} gvfg_convert_frame_desc_t;

GVFG_CONVERT_API gvfg_status_t gvfg_convert_create_frame(
    _In_ const gvfg_convert_frame_desc_t *desc,
    _Outptr_ gvfg_convert_frame *out_frame);

GVFG_CONVERT_API gvfg_status_t gvfg_convert_destroy_frame(
    _In_opt_ gvfg_convert_frame frame);

GVFG_CONVERT_API gvfg_status_t gvfg_convert_frame_from_capture(
    _In_ const gvfg_frame_t *src,
    _In_ gvfg_convert_frame dst_frame);

GVFG_CONVERT_API gvfg_status_t gvfg_convert_get_frame_desc(
    _In_ gvfg_convert_frame frame,
    _Out_ gvfg_convert_frame_desc_t *out_desc);

GVFG_CONVERT_API gvfg_status_t gvfg_convert_get_buffer(
    _In_ gvfg_convert_frame frame,
    _Out_ const void **out_data,
    _Out_ uint64_t *out_size);

GVFG_CONVERT_API gvfg_status_t gvfg_convert_get_layout(
    _In_ gvfg_convert_frame frame,
    _Out_ gvfg_frame_layout_t *out_layout);

GVFG_CONVERT_API const char *gvfg_convert_strerror(
    _In_ gvfg_status_t status);

#ifdef __cplusplus
}
#endif
