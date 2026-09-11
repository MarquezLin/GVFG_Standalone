#include "pcies2mm_video_format.h"

namespace
{
    // Legacy firmware reports YVYU for the 8-bit stream even though the DMA
    // payload is YUY2 (byte order Y0 U0 Y1 V0). Keep accepting that register
    // value until FPGA firmware is corrected.
    constexpr uint32_t kLegacyRegisterDisplayYvyu = 0x59565955u;
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
        if (registerValue == kLegacyRegisterDisplayYvyu)
            return PCIES2MM_PIXFMT_YUY2;
        if (registerValue == kRegisterDisplayV210 || registerValue == kRegisterDisplayY210)
            return PCIES2MM_PIXFMT_Y210;

        // Also accept corrected Windows FOURCC values from future firmware.
        switch (registerValue)
        {
        case fourcc('Y', 'V', 'Y', 'U'):
        case fourcc('Y', 'U', 'Y', '2'):
            return PCIES2MM_PIXFMT_YUY2;
        case fourcc('v', '2', '1', '0'):
        case fourcc('Y', '2', '1', '0'):
            return PCIES2MM_PIXFMT_Y210;
        default:
            return PCIES2MM_PIXFMT_UNKNOWN;
        }
    }

    uint32_t bit_depth_for_pixfmt(pcies2mm_pixel_format_t format)
    {
        if (format == PCIES2MM_PIXFMT_YUY2)
            return 8;
        if (format == PCIES2MM_PIXFMT_Y210)
            return 10;
        return 0;
    }

    size_t bytes_per_frame(uint32_t width, uint32_t height, pcies2mm_pixel_format_t format)
    {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (format == PCIES2MM_PIXFMT_YUY2)
            return pixels * 2u;
        if (format == PCIES2MM_PIXFMT_Y210)
            return pixels * 4u;
        return 0;
    }
}
