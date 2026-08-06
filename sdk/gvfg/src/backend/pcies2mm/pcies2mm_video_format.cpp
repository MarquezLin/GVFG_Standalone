#include "pcies2mm_video_format.h"

namespace
{
    // This board revision can report v210 even though its DMA payload is Y210
    // (16-bit containers with 10 valid bits).
    constexpr uint32_t kRegisterDisplayYvyu = 0x59565955u;
    constexpr uint32_t kRegisterDisplayV210 = 0x76323130u;
    constexpr uint32_t kRegisterDisplayY210 = 0x59323130u;

    constexpr uint32_t fourcc(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
               (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
    }
}

namespace gvfg::internal
{
    pcies2mm_pixel_format_t decode_pixel_format(uint32_t registerValue)
    {
        // Current firmware writes character codes in display order. Normalize
        // those values here so no register naming leaks into the SDK formats.
        if (registerValue == kRegisterDisplayYvyu)
            return PCIES2MM_PIXFMT_YVYU;
        if (registerValue == kRegisterDisplayV210 || registerValue == kRegisterDisplayY210)
            return PCIES2MM_PIXFMT_Y210;

        // Also accept corrected Windows FOURCC values from future firmware.
        switch (registerValue)
        {
        case fourcc('Y', 'V', 'Y', 'U'):
            return PCIES2MM_PIXFMT_YVYU;
        case fourcc('v', '2', '1', '0'):
        case fourcc('Y', '2', '1', '0'):
            return PCIES2MM_PIXFMT_Y210;
        default:
            return PCIES2MM_PIXFMT_UNKNOWN;
        }
    }

    uint32_t bit_depth_for_pixfmt(pcies2mm_pixel_format_t format)
    {
        if (format == PCIES2MM_PIXFMT_YVYU)
            return 8;
        if (format == PCIES2MM_PIXFMT_Y210)
            return 10;
        return 0;
    }

    size_t bytes_per_frame(uint32_t width, uint32_t height, pcies2mm_pixel_format_t format)
    {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (format == PCIES2MM_PIXFMT_YVYU)
            return pixels * 2u;
        if (format == PCIES2MM_PIXFMT_Y210)
            return pixels * 4u;
        return 0;
    }
}
