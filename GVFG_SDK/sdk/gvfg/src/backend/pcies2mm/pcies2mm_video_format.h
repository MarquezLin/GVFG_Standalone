#pragma once

#include "pcies2mm_backend_types.h"

#include <cstddef>
#include <cstdint>

namespace gvfg::internal
{
    pcies2mm_pixel_format_t decode_pixel_format(uint32_t registerValue);
    uint32_t bit_depth_for_pixfmt(pcies2mm_pixel_format_t format);
    size_t bytes_per_frame(uint32_t width, uint32_t height, pcies2mm_pixel_format_t format);
}
