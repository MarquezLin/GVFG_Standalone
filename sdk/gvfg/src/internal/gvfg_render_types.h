#pragma once

#include <stdint.h>

namespace gvfg::internal
{
    typedef enum
    {
        GVFG_RENDER_FMT_NV12,
        GVFG_RENDER_FMT_YUY2,
        GVFG_RENDER_FMT_ARGB,
        GVFG_RENDER_FMT_P010,
        GVFG_RENDER_FMT_Y210
    } gvfg_render_pixfmt_t;

    typedef struct
    {
        const void *data[3];
        int stride[3];
        int plane_count;
        int width;
        int height;
        gvfg_render_pixfmt_t format;
        uint64_t pts_ns;
        uint64_t frame_id;
    } gvfg_render_frame_t;

    typedef enum
    {
        GVFG_RENDER_PREVIEW_BITDEPTH_8BIT = 0,
        GVFG_RENDER_PREVIEW_BITDEPTH_10BIT = 1,
        GVFG_RENDER_PREVIEW_BITDEPTH_AUTO = 2
    } gvfg_render_preview_bitdepth_t;

    typedef struct
    {
        void *hwnd;
        int enable_preview;
        int use_fp16_pipeline;
        int swapchain_10bit;
    } gvfg_render_preview_desc_t;

}
