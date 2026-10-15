#include "gvfg_osd.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
struct OsdContext
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    bool ownsComInitialization = false;

    ~OsdContext()
    {
        dwrite.Reset();
        d2d.Reset();
        wic.Reset();
        if (ownsComInitialization)
            CoUninitialize();
    }
};

uint8_t clampByte(double value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

struct Yuv8
{
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

Yuv8 rgbToBt709Limited(uint32_t rgb)
{
    const double r = static_cast<double>((rgb >> 16) & 0xffu);
    const double g = static_cast<double>((rgb >> 8) & 0xffu);
    const double b = static_cast<double>(rgb & 0xffu);
    return {
        clampByte(16.0 + 0.182586 * r + 0.614231 * g + 0.062007 * b),
        clampByte(128.0 - 0.100644 * r - 0.338572 * g + 0.439216 * b),
        clampByte(128.0 + 0.439216 * r - 0.398942 * g - 0.040274 * b)};
}

bool checkedImageBytes(uint32_t stride, uint32_t height, uint64_t &bytes)
{
    bytes = static_cast<uint64_t>(stride) * height;
    return height == 0 || bytes / height == stride;
}

bool validAscii(const char *text, uint32_t length)
{
    if (!text || length == 0 || length > GVFG_OSD_MAX_TEXT_CHARACTERS)
        return false;
    for (uint32_t i = 0; i < length; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c > 0x7e)
            return false;
    }
    return true;
}

std::wstring widenAscii(const char *text, uint32_t length)
{
    std::wstring result;
    result.reserve(length);
    for (uint32_t i = 0; i < length; ++i)
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(text[i])));
    return result;
}

gvfg_osd_status_t renderMask(OsdContext &context,
                             const gvfg_osd_config_t &config,
                             uint8_t *destination,
                             uint32_t destinationStride)
{
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = context.wic->CreateBitmap(config.width, config.height,
                                            GUID_WICPixelFormat32bppPBGRA,
                                            WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr))
        return GVFG_OSD_ERENDER;

    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                         D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> target;
    hr = context.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target);
    if (FAILED(hr))
        return GVFG_OSD_ERENDER;

    const std::wstring text = widenAscii(config.text, config.text_length);

    ComPtr<IDWriteTextFormat> format;
    hr = context.dwrite->CreateTextFormat(
        L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(config.font_size_px), L"en-us", &format);
    if (FAILED(hr))
        return GVFG_OSD_ERENDER;

    ComPtr<ID2D1SolidColorBrush> brush;
    hr = target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush);
    if (FAILED(hr))
        return GVFG_OSD_ERENDER;

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0.0f, 0.0f));
    const D2D1_RECT_F layout = D2D1::RectF(
        static_cast<float>(config.x), static_cast<float>(config.y),
        static_cast<float>(config.width), static_cast<float>(config.height));
    target->DrawText(text.c_str(), config.text_length, format.Get(), layout,
                     brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                     DWRITE_MEASURING_MODE_NATURAL);
    hr = target->EndDraw();
    if (FAILED(hr))
        return GVFG_OSD_ERENDER;

    WICRect lockRect{0, 0, static_cast<INT>(config.width), static_cast<INT>(config.height)};
    ComPtr<IWICBitmapLock> lock;
    hr = bitmap->Lock(&lockRect, WICBitmapLockRead, &lock);
    if (FAILED(hr))
        return GVFG_OSD_ERENDER;

    UINT sourceStride = 0;
    UINT sourceSize = 0;
    BYTE *source = nullptr;
    if (FAILED(lock->GetStride(&sourceStride)) ||
        FAILED(lock->GetDataPointer(&sourceSize, &source)) || !source)
        return GVFG_OSD_ERENDER;

    for (uint32_t y = 0; y < config.height; ++y)
    {
        const uint8_t *sourceRow = source + static_cast<size_t>(y) * sourceStride;
        uint8_t *destinationRow = destination + static_cast<size_t>(y) * destinationStride;
        for (uint32_t x = 0; x < config.width; ++x)
            destinationRow[x] = sourceRow[x * 4u + 3u];
    }
    return GVFG_OSD_OK;
}

void writeYuy2(const gvfg_osd_config_t &config,
               const gvfg_osd_output_t &output,
               const Yuv8 color)
{
    auto *base = static_cast<uint8_t *>(output.yuv_data);
    const auto *mask = static_cast<const uint8_t *>(output.mask_data);
    constexpr Yuv8 black{16, 128, 128};
    for (uint32_t y = 0; y < config.height; ++y)
    {
        uint8_t *row = base + static_cast<size_t>(y) * output.yuv_stride;
        const uint8_t *maskRow = mask + static_cast<size_t>(y) * output.mask_stride;
        for (uint32_t x = 0; x < config.width; x += 2)
        {
            const bool first = maskRow[x] != 0;
            const bool second = maskRow[x + 1] != 0;
            row[x * 2u + 0u] = first ? color.y : black.y;
            row[x * 2u + 1u] = (first || second) ? color.u : black.u;
            row[x * 2u + 2u] = second ? color.y : black.y;
            row[x * 2u + 3u] = (first || second) ? color.v : black.v;
        }
    }
}

void writeY210(const gvfg_osd_config_t &config,
               const gvfg_osd_output_t &output,
               const Yuv8 color)
{
    auto *base = static_cast<uint8_t *>(output.yuv_data);
    const auto *mask = static_cast<const uint8_t *>(output.mask_data);
    const uint16_t blackY = static_cast<uint16_t>(16u * 4u) << 6u;
    const uint16_t blackC = static_cast<uint16_t>(128u * 4u) << 6u;
    const uint16_t colorY = static_cast<uint16_t>(color.y * 4u) << 6u;
    const uint16_t colorU = static_cast<uint16_t>(color.u * 4u) << 6u;
    const uint16_t colorV = static_cast<uint16_t>(color.v * 4u) << 6u;
    for (uint32_t y = 0; y < config.height; ++y)
    {
        auto *row = reinterpret_cast<uint16_t *>(base + static_cast<size_t>(y) * output.yuv_stride);
        const uint8_t *maskRow = mask + static_cast<size_t>(y) * output.mask_stride;
        for (uint32_t x = 0; x < config.width; x += 2)
        {
            const bool first = maskRow[x] != 0;
            const bool second = maskRow[x + 1] != 0;
            row[x * 2u + 0u] = first ? colorY : blackY;
            row[x * 2u + 1u] = (first || second) ? colorU : blackC;
            row[x * 2u + 2u] = second ? colorY : blackY;
            row[x * 2u + 3u] = (first || second) ? colorV : blackC;
        }
    }
}
} // namespace

extern "C"
{
gvfg_osd_status_t gvfg_osd_create(gvfg_osd_handle *out_handle)
{
    if (!out_handle)
        return GVFG_OSD_EINVAL;
    *out_handle = nullptr;

    auto *context = new (std::nothrow) OsdContext();
    if (!context)
        return GVFG_OSD_ENOMEM;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        delete context;
        return GVFG_OSD_ERENDER;
    }
    context->ownsComInitialization = SUCCEEDED(hr);
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&context->wic));
    if (SUCCEEDED(hr))
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                               context->d2d.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(
                                     context->dwrite.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        delete context;
        return GVFG_OSD_ERENDER;
    }

    *out_handle = context;
    return GVFG_OSD_OK;
}

void gvfg_osd_destroy(gvfg_osd_handle handle)
{
    delete static_cast<OsdContext *>(handle);
}

gvfg_osd_status_t gvfg_osd_render(gvfg_osd_handle handle,
                                  const gvfg_osd_config_t *config,
                                  const gvfg_osd_output_t *output)
{
    if (!handle || !config || !output ||
        config->struct_size != sizeof(gvfg_osd_config_t) ||
        output->struct_size != sizeof(gvfg_osd_output_t) ||
        !validAscii(config->text, config->text_length) ||
        config->width == 0 || config->height == 0 || (config->width & 1u) != 0 ||
        config->font_size_px == 0 || !output->yuv_data || !output->mask_data)
        return GVFG_OSD_EINVAL;

    uint32_t minimumYuvStride = 0;
    if (config->pixel_format == GVFG_OSD_PIXFMT_YUY2)
        minimumYuvStride = config->width * 2u;
    else if (config->pixel_format == GVFG_OSD_PIXFMT_Y210)
        minimumYuvStride = config->width * 4u;
    else
        return GVFG_OSD_EINVAL;

    if (output->yuv_stride < minimumYuvStride || output->mask_stride < config->width)
        return GVFG_OSD_EINVAL;

    uint64_t requiredYuv = 0;
    uint64_t requiredMask = 0;
    if (!checkedImageBytes(output->yuv_stride, config->height, requiredYuv) ||
        !checkedImageBytes(output->mask_stride, config->height, requiredMask) ||
        output->yuv_size < requiredYuv || output->mask_size < requiredMask)
        return GVFG_OSD_EINVAL;

    std::memset(output->mask_data, 0, static_cast<size_t>(requiredMask));
    const gvfg_osd_status_t maskStatus = renderMask(
        *static_cast<OsdContext *>(handle), *config,
        static_cast<uint8_t *>(output->mask_data), output->mask_stride);
    if (maskStatus != GVFG_OSD_OK)
        return maskStatus;

    const Yuv8 color = rgbToBt709Limited(config->color_rgb);
    if (config->pixel_format == GVFG_OSD_PIXFMT_YUY2)
        writeYuy2(*config, *output, color);
    else
        writeY210(*config, *output, color);
    return GVFG_OSD_OK;
}

const char *gvfg_osd_strerror(gvfg_osd_status_t status)
{
    switch (status)
    {
    case GVFG_OSD_OK: return "success";
    case GVFG_OSD_EINVAL: return "invalid argument";
    case GVFG_OSD_ENOMEM: return "out of memory";
    case GVFG_OSD_ERENDER: return "text rendering failed";
    default: return "unknown gvfg_osd status";
    }
}
}
