#ifndef GVFG_OSD_H
#define GVFG_OSD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GVFG_OSD_MAX_TEXT_CHARACTERS 32u

typedef void *gvfg_osd_handle;

typedef enum gvfg_osd_status_t
{
    GVFG_OSD_OK = 0,
    GVFG_OSD_EINVAL = -1,
    GVFG_OSD_ENOMEM = -2,
    GVFG_OSD_ERENDER = -3
} gvfg_osd_status_t;

typedef enum gvfg_osd_pixel_format_t
{
    GVFG_OSD_PIXFMT_YUY2 = 1,
    GVFG_OSD_PIXFMT_Y210 = 2
} gvfg_osd_pixel_format_t;

/*
 * Generates an off-line OSD surface using the Arial system font. Text is
 * restricted to printable ASCII and at most GVFG_OSD_MAX_TEXT_CHARACTERS
 * characters on one line without wrapping. color_rgb is 0xRRGGBB.
 * x/y identify the top-left of the conservative measured text box (including
 * glyph overhang and a one-pixel guard). They are clamped to keep that box
 * inside the frame. Rendering returns EINVAL if the box cannot fit the frame.
 * Output YUV uses BT.709 limited range. The mask is one coverage byte per
 * pixel: 0 is transparent and 255 is fully covered.
 */
typedef struct gvfg_osd_config_t
{
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    const char *text;
    uint32_t text_length;
    int32_t x;
    int32_t y;
    uint32_t font_size_px;
    uint32_t color_rgb;
} gvfg_osd_config_t;

/* All buffers are caller-owned and are used only for the duration of render. */
typedef struct gvfg_osd_output_t
{
    uint32_t struct_size;
    void *yuv_data;
    uint64_t yuv_size;
    uint32_t yuv_stride;
    void *mask_data;
    uint64_t mask_size;
    uint32_t mask_stride;
} gvfg_osd_output_t;

gvfg_osd_status_t gvfg_osd_create(
    gvfg_osd_handle *out_handle);

void gvfg_osd_destroy(
    gvfg_osd_handle handle);

/* Conservative whole-text bounds in pixels, not pixel-tight raster bounds. */
gvfg_osd_status_t gvfg_osd_measure(
    gvfg_osd_handle handle,
    const char *text,
    uint32_t text_length,
    uint32_t font_size_px,
    uint32_t *out_width,
    uint32_t *out_height);

gvfg_osd_status_t gvfg_osd_render(
    gvfg_osd_handle handle,
    const gvfg_osd_config_t *config,
    const gvfg_osd_output_t *output);

const char *gvfg_osd_strerror(
    gvfg_osd_status_t status);

#ifdef __cplusplus
}
#endif

#endif
