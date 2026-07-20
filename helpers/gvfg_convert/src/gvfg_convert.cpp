#include "gvfg_convert.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    bool checked_mul_u64(uint64_t a, uint64_t b, uint64_t &out)
    {
        if (a != 0 && b > UINT64_MAX / a)
            return false;
        out = a * b;
        return true;
    }

    int convert_format_bytes_per_pixel(int fmt)
    {
        switch (fmt)
        {
        case GVFG_CONVERT_FMT_BGRA8:
            return 4;
        case GVFG_CONVERT_FMT_RGB48:
            return 6;
        case GVFG_CONVERT_FMT_RGBA64:
            return 8;
        default:
            return 0;
        }
    }

    uint8_t clamp_u8(double v)
    {
        if (v <= 0.0)
            return 0;
        if (v >= 255.0)
            return 255;
        return static_cast<uint8_t>(v + 0.5);
    }

    uint16_t clamp_u16(double v)
    {
        if (v <= 0.0)
            return 0;
        if (v >= 65535.0)
            return 65535;
        return static_cast<uint16_t>(v + 0.5);
    }

    void yuv709_limited_to_rgb01(double y01,
                                 double u01,
                                 double v01,
                                 double &r,
                                 double &g,
                                 double &b)
    {
        const double y = y01 * 255.0;
        const double u = (u01 - 0.5) * 255.0;
        const double v = (v01 - 0.5) * 255.0;
        const double c = y - 16.0;
        r = (1.164383 * c + 1.792741 * v) / 255.0;
        g = (1.164383 * c - 0.213249 * u - 0.532909 * v) / 255.0;
        b = (1.164383 * c + 2.112402 * u) / 255.0;
    }

    void store_converted_pixel(uint8_t *dst,
                               int dstFormat,
                               double r,
                               double g,
                               double b)
    {
        switch (dstFormat)
        {
        case GVFG_CONVERT_FMT_BGRA8:
            dst[0] = clamp_u8(b * 255.0);
            dst[1] = clamp_u8(g * 255.0);
            dst[2] = clamp_u8(r * 255.0);
            dst[3] = 255;
            break;
        case GVFG_CONVERT_FMT_RGB48:
        {
            auto *out = reinterpret_cast<uint16_t *>(dst);
            out[0] = clamp_u16(r * 65535.0);
            out[1] = clamp_u16(g * 65535.0);
            out[2] = clamp_u16(b * 65535.0);
            break;
        }
        case GVFG_CONVERT_FMT_RGBA64:
        {
            auto *out = reinterpret_cast<uint16_t *>(dst);
            out[0] = clamp_u16(r * 65535.0);
            out[1] = clamp_u16(g * 65535.0);
            out[2] = clamp_u16(b * 65535.0);
            out[3] = 65535;
            break;
        }
        default:
            break;
        }
    }

    uint16_t y210_word_to_10bit(uint16_t value)
    {
        return static_cast<uint16_t>((value >> 6) & 0x03ffu);
    }
}

struct gvfg_convert_frame_t
{
    gvfg_convert_frame_desc_t request{};
    gvfg_convert_frame_desc_t desc{};
    gvfg_frame_layout_t layout{};
    std::vector<uint8_t> buffer;

    gvfg_status_t configure(int width, int height, int pixelFormat, int rowBytes)
    {
        if (width <= 0 || height <= 0 || convert_format_bytes_per_pixel(pixelFormat) <= 0)
            return GVFG_EINVAL;

        const int bytesPerPixel = convert_format_bytes_per_pixel(pixelFormat);
        uint64_t minStride64 = 0;
        if (!checked_mul_u64(static_cast<uint64_t>(width),
                             static_cast<uint64_t>(bytesPerPixel),
                             minStride64) ||
            minStride64 > static_cast<uint64_t>(INT_MAX))
            return GVFG_EINVAL;

        const int minStride = static_cast<int>(minStride64);
        const int actualStride = rowBytes > 0 ? rowBytes : minStride;
        if (actualStride < minStride)
            return GVFG_EINVAL;

        uint64_t size = 0;
        if (!checked_mul_u64(static_cast<uint64_t>(actualStride),
                             static_cast<uint64_t>(height),
                             size) ||
            size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
            return GVFG_EINVAL;

        desc = request;
        desc.width = width;
        desc.height = height;
        desc.pixel_format = pixelFormat;
        desc.row_bytes = actualStride;
        desc.data_size = size;

        try
        {
            buffer.resize(static_cast<size_t>(size));
        }
        catch (...)
        {
            buffer.clear();
            desc.data_size = 0;
            return GVFG_EIO;
        }

        layout = {};
        layout.struct_size = sizeof(layout);
        layout.layout_flags = GVFG_FRAME_LAYOUT_CONTIGUOUS | GVFG_FRAME_LAYOUT_SDK_DERIVED;
        layout.row_bytes = actualStride;
        layout.plane_count = 1;
        layout.plane_data[0] = buffer.data();
        layout.plane_stride[0] = actualStride;
        layout.plane_size[0] = size;
        layout.plane_offset[0] = 0;
        return GVFG_OK;
    }
};

namespace
{
    gvfg_status_t create_or_reconfigure_frame(const gvfg_frame_t &src,
                                              gvfg_convert_frame_t &dst)
    {
        const int requestedWidth = dst.request.width;
        const int requestedHeight = dst.request.height;
        const int targetWidth = requestedWidth > 0 ? requestedWidth : src.width;
        const int targetHeight = requestedHeight > 0 ? requestedHeight : src.height;

        if (targetWidth != src.width || targetHeight != src.height)
            return GVFG_ENOTSUP;

        if (!dst.buffer.empty() &&
            dst.desc.width == targetWidth &&
            dst.desc.height == targetHeight &&
            dst.desc.pixel_format == dst.request.pixel_format &&
            dst.desc.row_bytes > 0)
            return GVFG_OK;

        return dst.configure(targetWidth, targetHeight, dst.request.pixel_format, dst.request.row_bytes);
    }

    gvfg_status_t convert_yuy2_to_frame(const gvfg_frame_t &src,
                                        const gvfg_frame_layout_t &srcLayout,
                                        gvfg_convert_frame_t &dst)
    {
        if (srcLayout.plane_count < 1 || !srcLayout.plane_data[0] || srcLayout.plane_stride[0] < src.width * 2)
            return GVFG_EINVAL;

        const auto *srcBase = static_cast<const uint8_t *>(srcLayout.plane_data[0]);
        const int srcStride = srcLayout.plane_stride[0];
        const int bytesPerPixel = convert_format_bytes_per_pixel(dst.desc.pixel_format);
        if (bytesPerPixel <= 0)
            return GVFG_ENOTSUP;

        for (int y = 0; y < src.height; ++y)
        {
            const uint8_t *srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(srcStride);
            uint8_t *dstRow = dst.buffer.data() + static_cast<size_t>(y) * static_cast<size_t>(dst.desc.row_bytes);

            for (int x = 0; x < src.width; x += 2)
            {
                const int sx = x * 2;
                const uint8_t y0 = srcRow[sx + 0];
                const uint8_t u = srcRow[sx + 1];
                const uint8_t y1 = (x + 1 < src.width) ? srcRow[sx + 2] : y0;
                const uint8_t v = (x + 1 < src.width) ? srcRow[sx + 3] : srcRow[sx + 1];

                double r = 0.0, g = 0.0, b = 0.0;
                yuv709_limited_to_rgb01(static_cast<double>(y0) / 255.0,
                                        static_cast<double>(u) / 255.0,
                                        static_cast<double>(v) / 255.0,
                                        r,
                                        g,
                                        b);
                store_converted_pixel(dstRow + static_cast<size_t>(x) * static_cast<size_t>(bytesPerPixel),
                                      dst.desc.pixel_format,
                                      r,
                                      g,
                                      b);

                if (x + 1 < src.width)
                {
                    yuv709_limited_to_rgb01(static_cast<double>(y1) / 255.0,
                                            static_cast<double>(u) / 255.0,
                                            static_cast<double>(v) / 255.0,
                                            r,
                                            g,
                                            b);
                    store_converted_pixel(dstRow + static_cast<size_t>(x + 1) * static_cast<size_t>(bytesPerPixel),
                                          dst.desc.pixel_format,
                                          r,
                                          g,
                                          b);
                }
            }
        }

        return GVFG_OK;
    }

    gvfg_status_t convert_y210_to_frame(const gvfg_frame_t &src,
                                        const gvfg_frame_layout_t &srcLayout,
                                        gvfg_convert_frame_t &dst)
    {
        if (srcLayout.plane_count < 1 || !srcLayout.plane_data[0] || srcLayout.plane_stride[0] < src.width * 4)
            return GVFG_EINVAL;

        const auto *srcBase = static_cast<const uint8_t *>(srcLayout.plane_data[0]);
        const int srcStride = srcLayout.plane_stride[0];
        const int bytesPerPixel = convert_format_bytes_per_pixel(dst.desc.pixel_format);
        if (bytesPerPixel <= 0)
            return GVFG_ENOTSUP;

        for (int y = 0; y < src.height; ++y)
        {
            const auto *srcRow = reinterpret_cast<const uint16_t *>(srcBase + static_cast<size_t>(y) * static_cast<size_t>(srcStride));
            uint8_t *dstRow = dst.buffer.data() + static_cast<size_t>(y) * static_cast<size_t>(dst.desc.row_bytes);

            for (int x = 0; x < src.width; x += 2)
            {
                const int sx = x * 2;
                // Standard Y210 is Y0, Cb(U), Y1, Cr(V).  Normalize the
                // current FPGA DMA order Y0, Cr(V), Y1, Cb(U) before RGB
                // conversion so snapshots match the preview path.
                const uint16_t y0 = y210_word_to_10bit(srcRow[sx + 0]);
                const uint16_t v = y210_word_to_10bit(srcRow[sx + 1]);
                const uint16_t y1 = (x + 1 < src.width) ? y210_word_to_10bit(srcRow[sx + 2]) : y0;
                const uint16_t u = (x + 1 < src.width) ? y210_word_to_10bit(srcRow[sx + 3]) : v;

                double r = 0.0, g = 0.0, b = 0.0;
                yuv709_limited_to_rgb01(static_cast<double>(y0) / 1023.0,
                                        static_cast<double>(u) / 1023.0,
                                        static_cast<double>(v) / 1023.0,
                                        r,
                                        g,
                                        b);
                store_converted_pixel(dstRow + static_cast<size_t>(x) * static_cast<size_t>(bytesPerPixel),
                                      dst.desc.pixel_format,
                                      r,
                                      g,
                                      b);

                if (x + 1 < src.width)
                {
                    yuv709_limited_to_rgb01(static_cast<double>(y1) / 1023.0,
                                            static_cast<double>(u) / 1023.0,
                                            static_cast<double>(v) / 1023.0,
                                            r,
                                            g,
                                            b);
                    store_converted_pixel(dstRow + static_cast<size_t>(x + 1) * static_cast<size_t>(bytesPerPixel),
                                          dst.desc.pixel_format,
                                          r,
                                          g,
                                          b);
                }
            }
        }

        return GVFG_OK;
    }
}

extern "C"
{
    gvfg_status_t gvfg_convert_create_frame(const gvfg_convert_frame_desc_t *desc,
                                            gvfg_convert_frame *out_frame)
    {
        if (!desc || !out_frame)
            return GVFG_EINVAL;
        *out_frame = nullptr;

        if (desc->struct_size < sizeof(gvfg_convert_frame_desc_t) ||
            desc->flags != 0 ||
            desc->reserved0 != 0 ||
            convert_format_bytes_per_pixel(desc->pixel_format) <= 0)
            return GVFG_EINVAL;
        if (desc->width < 0 || desc->height < 0 || desc->row_bytes < 0)
            return GVFG_EINVAL;

        auto frame = std::make_unique<gvfg_convert_frame_t>();
        frame->request = {};
        frame->request.struct_size = sizeof(gvfg_convert_frame_desc_t);
        frame->request.width = desc->width;
        frame->request.height = desc->height;
        frame->request.pixel_format = desc->pixel_format;
        frame->request.row_bytes = desc->row_bytes;
        frame->desc = frame->request;

        if (desc->width > 0 || desc->height > 0 || desc->row_bytes > 0)
        {
            if (desc->width <= 0 || desc->height <= 0)
                return GVFG_EINVAL;

            const gvfg_status_t st = frame->configure(desc->width,
                                                      desc->height,
                                                      desc->pixel_format,
                                                      desc->row_bytes);
            if (st != GVFG_OK)
                return st;
        }

        *out_frame = frame.release();
        return GVFG_OK;
    }

    gvfg_status_t gvfg_convert_destroy_frame(gvfg_convert_frame frame)
    {
        delete frame;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_convert_frame_from_capture(const gvfg_frame_t *src,
                                                  gvfg_convert_frame dst_frame)
    {
        if (!src || !dst_frame)
            return GVFG_EINVAL;

        const gvfg_status_t cfg = create_or_reconfigure_frame(*src, *dst_frame);
        if (cfg != GVFG_OK)
            return cfg;

        gvfg_frame_layout_t srcLayout{};
        srcLayout.struct_size = sizeof(srcLayout);
        const gvfg_status_t layoutStatus = gvfg_get_frame_layout(src, &srcLayout);
        if (layoutStatus != GVFG_OK)
            return layoutStatus;

        switch (src->pixel_format)
        {
        case GVFG_PIXFMT_YUY2:
            return convert_yuy2_to_frame(*src, srcLayout, *dst_frame);
        case GVFG_PIXFMT_Y210:
            return convert_y210_to_frame(*src, srcLayout, *dst_frame);
        default:
            return GVFG_ENOTSUP;
        }
    }

    gvfg_status_t gvfg_convert_get_frame_desc(gvfg_convert_frame frame,
                                              gvfg_convert_frame_desc_t *out_desc)
    {
        if (!frame || !out_desc)
            return GVFG_EINVAL;
        if (out_desc->struct_size < sizeof(gvfg_convert_frame_desc_t))
            return GVFG_EINVAL;

        const uint32_t callerSize = out_desc->struct_size;
        std::memset(out_desc, 0, sizeof(*out_desc));
        *out_desc = frame->desc;
        out_desc->struct_size = callerSize;
        return GVFG_OK;
    }

    gvfg_status_t gvfg_convert_get_buffer(gvfg_convert_frame frame,
                                          const void **out_data,
                                          uint64_t *out_size)
    {
        if (!frame || !out_data || !out_size)
            return GVFG_EINVAL;
        *out_data = frame->buffer.empty() ? nullptr : frame->buffer.data();
        *out_size = static_cast<uint64_t>(frame->buffer.size());
        return frame->buffer.empty() ? GVFG_ESTATE : GVFG_OK;
    }

    gvfg_status_t gvfg_convert_get_layout(gvfg_convert_frame frame,
                                          gvfg_frame_layout_t *out_layout)
    {
        if (!frame || !out_layout)
            return GVFG_EINVAL;
        if (out_layout->struct_size < sizeof(gvfg_frame_layout_t))
            return GVFG_EINVAL;
        if (frame->buffer.empty() || frame->layout.plane_count <= 0)
            return GVFG_ESTATE;

        const uint32_t callerSize = out_layout->struct_size;
        std::memset(out_layout, 0, sizeof(*out_layout));
        *out_layout = frame->layout;
        out_layout->struct_size = callerSize;
        return GVFG_OK;
    }

    const char *gvfg_convert_strerror(gvfg_status_t status)
    {
        switch (status)
        {
        case GVFG_OK:
            return "ok";
        case GVFG_EINVAL:
            return "invalid argument";
        case GVFG_ENODEV:
            return "device not found";
        case GVFG_ESTATE:
            return "invalid state";
        case GVFG_EIO:
            return "i/o error";
        case GVFG_ENOTSUP:
            return "not supported";
        case GVFG_ETIMEOUT:
            return "timeout";
        default:
            return "unknown";
        }
    }
}
