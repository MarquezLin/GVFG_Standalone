#include "gvfg_gpu_osd.h"

#include "gvfg_osd.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace gvfg::internal
{
namespace
{
#ifdef GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE
// Permission snapshot only: this does NOT establish exclusive DMA ownership.
bool writableExperimentalRange(const gvfg_frame_t &source, uint64_t offset, uint64_t span,
                               gvfg_debug_osd_page_check_t *result)
{
    gvfg_debug_osd_page_check_t check{};
    const auto reject = [&](gvfg_debug_osd_page_reason_t reason) {
        check.reason = static_cast<uint32_t>(reason);
        if (result)
            *result = check;
        return false;
    };
    const uintptr_t base = reinterpret_cast<uintptr_t>(source.data);
    const uintptr_t limit = (std::numeric_limits<uintptr_t>::max)();
    if (!span || offset > source.data_size || span > source.data_size - offset ||
        offset > limit - base || span > limit - (base + offset))
        return reject(GVFG_DEBUG_OSD_PAGE_INVALID_RANGE);
    uintptr_t cursor = base + static_cast<uintptr_t>(offset);
    const uintptr_t end = cursor + static_cast<uintptr_t>(span);
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        const SIZE_T queried = VirtualQuery(reinterpret_cast<const void *>(cursor), &info, sizeof(info));
        // Capture the error immediately, and only on a failed native call.
        check.win32_error = queried == 0 ? GetLastError() : 0;
        check.query_bytes = static_cast<uint32_t>(queried);
        check.protect = info.Protect;
        check.state = info.State;
        check.type = info.Type;
        if (queried != sizeof(info))
            return reject(GVFG_DEBUG_OSD_PAGE_QUERY_FAILED);
        if (info.State != MEM_COMMIT)
            return reject(GVFG_DEBUG_OSD_PAGE_NOT_COMMITTED);
        if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return reject(GVFG_DEBUG_OSD_PAGE_GUARD_OR_NOACCESS);
        if ((info.Protect & 0xffu) != PAGE_READWRITE)
            return reject(GVFG_DEBUG_OSD_PAGE_NOT_READWRITE);
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (info.RegionSize > limit - regionBase)
            return reject(GVFG_DEBUG_OSD_PAGE_INVALID_REGION);
        const uintptr_t next = regionBase + info.RegionSize;
        if (next <= cursor)
            return reject(GVFG_DEBUG_OSD_PAGE_INVALID_REGION);
        cursor = next;
    }
    if (result)
        *result = {};
    return true;
}
#endif
constexpr char kOsdBlendShader[] = R"(
Texture2D<uint4> sourceTexture : register(t0);
Texture2D<uint4> osdTexture : register(t1);
Texture2D<uint2> maskTexture : register(t2);
RWTexture2D<uint4> outputTexture : register(u0);

cbuffer BlendParameters : register(b0)
{
    uint outputValueMask;
    uint3 padding;
};

uint blendComponent(uint sourceValue, uint osdValue, uint alpha)
{
    return ((sourceValue * (255u - alpha) + osdValue * alpha + 127u) / 255u) &
           outputValueMask;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width = 0;
    uint height = 0;
    outputTexture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;

    const uint2 location = id.xy;
    const uint4 sourceValue = sourceTexture.Load(uint3(location, 0));
    const uint2 maskValue = maskTexture.Load(uint3(location, 0));
    const uint pairAlpha = max(maskValue.x, maskValue.y);
    if (pairAlpha == 0u)
    {
        outputTexture[location] = sourceValue;
        return;
    }
    const uint4 osdValue = osdTexture.Load(uint3(location, 0));

    uint4 result;
    result.x = blendComponent(sourceValue.x, osdValue.x, maskValue.x);
    result.y = blendComponent(sourceValue.y, osdValue.y, pairAlpha);
    result.z = blendComponent(sourceValue.z, osdValue.z, maskValue.y);
    result.w = blendComponent(sourceValue.w, osdValue.w, pairAlpha);
    outputTexture[location] = result;
}
)";

bool copyRowsToMapped(const uint8_t *source, int sourceStride,
                      uint32_t rowBytes, uint32_t height,
                      const D3D11_MAPPED_SUBRESOURCE &mapped)
{
    if (!source || sourceStride < 0 || static_cast<uint32_t>(sourceStride) < rowBytes ||
        mapped.RowPitch < rowBytes)
        return false;
    if (static_cast<uint32_t>(sourceStride) == rowBytes && mapped.RowPitch == rowBytes)
    {
        std::memcpy(mapped.pData, source, static_cast<size_t>(rowBytes) * height);
        return true;
    }
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(static_cast<uint8_t *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    source + static_cast<size_t>(y) * static_cast<size_t>(sourceStride),
                    rowBytes);
    }
    return true;
}

struct OsdRegion
{
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool empty() const { return width == 0 || height == 0; }
};

OsdRegion findCoveredRegion(const std::vector<uint8_t> &mask,
                            uint32_t frameWidth, uint32_t frameHeight)
{
    uint32_t left = frameWidth;
    uint32_t top = frameHeight;
    uint32_t right = 0;
    uint32_t bottom = 0;
    for (uint32_t row = 0; row < frameHeight; ++row)
    {
        const uint8_t *maskRow = mask.data() + static_cast<size_t>(row) * frameWidth;
        for (uint32_t column = 0; column < frameWidth; column += 2u)
        {
            // Keep both luma samples and their shared U/V inside the region.
            if (maskRow[column] == 0 && maskRow[column + 1u] == 0)
                continue;
            left = (std::min)(left, column);
            right = (std::max)(right, column + 2u);
            top = (std::min)(top, row);
            bottom = row + 1u;
        }
    }
    return right > left ? OsdRegion{left, top, right - left, bottom - top}
                        : OsdRegion{};
}

void copyFrameRows(const gvfg_frame_t &source, uint8_t *destination,
                   uint32_t rowBytes)
{
    const auto *sourceData = static_cast<const uint8_t *>(source.data);
    if (static_cast<uint32_t>(source.row_stride_bytes) == rowBytes)
    {
        std::memcpy(destination, sourceData,
                    static_cast<size_t>(rowBytes) * static_cast<size_t>(source.height));
        return;
    }
    for (int row = 0; row < source.height; ++row)
    {
        std::memcpy(destination + static_cast<size_t>(row) * rowBytes,
                    sourceData + static_cast<size_t>(row) * source.row_stride_bytes,
                    rowBytes);
    }
}
}

struct GpuOsdProcessor::Impl
{
    gvfg_status_t configure(const gvfg_channel_osd_config_t &value)
    {
        if (!value.text || value.text_length == 0 ||
            value.text_length > GVFG_OSD_MAX_TEXT_CHARACTERS ||
            value.font_size_px == 0)
            return GVFG_EINVAL;
        for (uint32_t i = 0; i < value.text_length; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(value.text[i]);
            if (c < 0x20 || c > 0x7e)
                return GVFG_EINVAL;
        }

        std::lock_guard<std::mutex> lock(mutex);
        text.assign(value.text, value.text + value.text_length);
        x = value.x;
        y = value.y;
        fontSize = value.font_size_px;
        colorRgb = value.color_rgb & 0x00ffffffu;
        active = true;
        surfaceDirty = true;
        return GVFG_OK;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        active = false;
        text.clear();
        surfaceDirty = true;
    }

    bool ensureDevice()
    {
        if (device && context && shader && blendParameters)
            return true;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL actual{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       flags, levels, static_cast<UINT>(std::size(levels)),
                                       D3D11_SDK_VERSION, &device, &actual, &context);
#ifdef _DEBUG
        if (FAILED(hr))
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, &device, &actual, &context);
        }
#endif
        if (FAILED(hr) || !device || !context)
            return false;

        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        hr = D3DCompile(kOsdBlendShader, sizeof(kOsdBlendShader) - 1,
                        "gvfg_osd_blend", nullptr, nullptr, "main", "cs_5_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
        if (FAILED(hr) || !bytecode)
            return false;
        if (FAILED(device->CreateComputeShader(bytecode->GetBufferPointer(),
                                                bytecode->GetBufferSize(),
                                                nullptr, &shader)))
            return false;

        D3D11_BUFFER_DESC parameterDesc{};
        parameterDesc.ByteWidth = 16;
        parameterDesc.Usage = D3D11_USAGE_DEFAULT;
        parameterDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device->CreateBuffer(&parameterDesc, nullptr, &blendParameters));
    }

    bool ensureTexture(ComPtr<ID3D11Texture2D> &texture,
                       uint32_t textureWidth, uint32_t textureHeight,
                       DXGI_FORMAT format, D3D11_USAGE usage,
                       UINT bindFlags, UINT cpuAccess)
    {
        if (texture)
        {
            D3D11_TEXTURE2D_DESC current{};
            texture->GetDesc(&current);
            if (current.Width == textureWidth && current.Height == textureHeight &&
                current.Format == format && current.Usage == usage &&
                current.BindFlags == bindFlags && current.CPUAccessFlags == cpuAccess)
                return true;
            texture.Reset();
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = textureWidth;
        desc.Height = textureHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bindFlags;
        desc.CPUAccessFlags = cpuAccess;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture));
    }

    bool ensureResources(uint32_t regionWidth, uint32_t regionHeight, int pixelFormat)
    {
        if ((regionWidth & 1u) != 0 || regionWidth == 0 || regionHeight == 0)
            return false;
        const DXGI_FORMAT packedFormat = pixelFormat == GVFG_PIXFMT_YUY2
                                             ? DXGI_FORMAT_R8G8B8A8_UINT
                                             : pixelFormat == GVFG_PIXFMT_Y210
                                                   ? DXGI_FORMAT_R16G16B16A16_UINT
                                                   : DXGI_FORMAT_UNKNOWN;
        if (packedFormat == DXGI_FORMAT_UNKNOWN)
            return false;
        if (resourceWidth != regionWidth || resourceHeight != regionHeight ||
            resourceFormat != pixelFormat || !sourceSrv || !osdSrv || !maskSrv || !outputUav)
        {
            // Failed partial recreation must never leave views cached against
            // old textures, including when the next request returns to an old size.
            sourceSrv.Reset();
            osdSrv.Reset();
            maskSrv.Reset();
            outputUav.Reset();
            resourceWidth = 0;
            resourceHeight = 0;
            resourceFormat = GVFG_PIXFMT_UNKNOWN;
        }
        const uint32_t pairWidth = regionWidth / 2u;
        if (!ensureTexture(sourceTexture, pairWidth, regionHeight, packedFormat,
                           D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                           D3D11_CPU_ACCESS_WRITE) ||
            !ensureTexture(osdTexture, pairWidth, regionHeight, packedFormat,
                           D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                           D3D11_CPU_ACCESS_WRITE) ||
            !ensureTexture(maskTexture, pairWidth, regionHeight, DXGI_FORMAT_R8G8_UINT,
                           D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                           D3D11_CPU_ACCESS_WRITE) ||
            !ensureTexture(outputTexture, pairWidth, regionHeight, packedFormat,
                           D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0) ||
            !ensureTexture(readbackTexture, pairWidth, regionHeight, packedFormat,
                           D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ))
            return false;

        if (!sourceSrv || resourceWidth != regionWidth || resourceHeight != regionHeight ||
            resourceFormat != pixelFormat)
        {
            sourceSrv.Reset();
            osdSrv.Reset();
            maskSrv.Reset();
            outputUav.Reset();
            if (FAILED(device->CreateShaderResourceView(sourceTexture.Get(), nullptr, &sourceSrv)) ||
                FAILED(device->CreateShaderResourceView(osdTexture.Get(), nullptr, &osdSrv)) ||
                FAILED(device->CreateShaderResourceView(maskTexture.Get(), nullptr, &maskSrv)) ||
                FAILED(device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &outputUav)))
                return false;
            resourceWidth = regionWidth;
            resourceHeight = regionHeight;
            resourceFormat = pixelFormat;
        }
        return true;
    }

    bool rebuildSurface(uint32_t frameWidth, uint32_t frameHeight, int pixelFormat)
    {
        // Any failed rebuild must be retried, even if input dimensions then
        // return to the previously cached frame size.
        surfaceDirty = true;
        const uint32_t yuvStride = frameWidth * (pixelFormat == GVFG_PIXFMT_YUY2 ? 2u : 4u);
        const uint32_t maskStride = frameWidth;
        gvfg_osd_handle rawHandle = nullptr;
        if (gvfg_osd_create(&rawHandle) != GVFG_OSD_OK)
            return false;
        std::unique_ptr<void, decltype(&gvfg_osd_destroy)> handle(rawHandle, &gvfg_osd_destroy);
        uint32_t textWidth = 0;
        uint32_t textHeight = 0;
        const gvfg_osd_status_t measureStatus = gvfg_osd_measure(
            handle.get(), text.c_str(), static_cast<uint32_t>(text.size()),
            fontSize, &textWidth, &textHeight);
        // Stored input already passed configure() validation. EINVAL here
        // means the measured box cannot be represented by uint32_t either.
        if (measureStatus != GVFG_OSD_OK && measureStatus != GVFG_OSD_EINVAL)
            return false;
        if (measureStatus == GVFG_OSD_EINVAL ||
            textWidth > frameWidth || textHeight > frameHeight)
        {
            // Retain the configuration and retry only after settings or the
            // input dimensions/format change. Capture must continue normally.
            region = {};
            surfaceWidth = frameWidth;
            surfaceHeight = frameHeight;
            surfaceFormat = pixelFormat;
            surfaceDirty = false;
            return true;
        }
        yuvSurface.resize(static_cast<size_t>(yuvStride) * frameHeight);
        maskSurface.resize(static_cast<size_t>(maskStride) * frameHeight);

        gvfg_osd_config_t config{};
        config.struct_size = sizeof(config);
        config.width = frameWidth;
        config.height = frameHeight;
        config.pixel_format = pixelFormat == GVFG_PIXFMT_YUY2
                                  ? GVFG_OSD_PIXFMT_YUY2
                                  : GVFG_OSD_PIXFMT_Y210;
        config.text = text.c_str();
        config.text_length = static_cast<uint32_t>(text.size());
        config.x = x;
        config.y = y;
        config.font_size_px = fontSize;
        config.color_rgb = colorRgb;

        gvfg_osd_output_t output{};
        output.struct_size = sizeof(output);
        output.yuv_data = yuvSurface.data();
        output.yuv_size = yuvSurface.size();
        output.yuv_stride = yuvStride;
        output.mask_data = maskSurface.data();
        output.mask_size = maskSurface.size();
        output.mask_stride = maskStride;
        const gvfg_osd_status_t status = gvfg_osd_render(handle.get(), &config, &output);
        handle.reset();
        if (status != GVFG_OSD_OK)
            return false;

        region = findCoveredRegion(maskSurface, frameWidth, frameHeight);
        if (region.empty())
        {
            surfaceWidth = frameWidth;
            surfaceHeight = frameHeight;
            surfaceFormat = pixelFormat;
            surfaceDirty = false;
            return true;
        }
        if (!ensureDevice() || !ensureResources(region.width, region.height, pixelFormat))
            return false;

        const uint32_t bytesPerPixel = pixelFormat == GVFG_PIXFMT_YUY2 ? 2u : 4u;
        const uint32_t regionRowBytes = region.width * bytesPerPixel;
        const size_t yuvOffset = static_cast<size_t>(region.top) * yuvStride +
                                 static_cast<size_t>(region.left) * bytesPerPixel;
        const size_t maskOffset = static_cast<size_t>(region.top) * maskStride + region.left;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(osdTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        const bool copiedYuv = copyRowsToMapped(
            yuvSurface.data() + yuvOffset, static_cast<int>(yuvStride),
            regionRowBytes, region.height, mapped);
        context->Unmap(osdTexture.Get(), 0);
        if (!copiedYuv)
            return false;

        if (FAILED(context->Map(maskTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        const bool copiedMask = copyRowsToMapped(
            maskSurface.data() + maskOffset, static_cast<int>(maskStride),
            region.width, region.height, mapped);
        context->Unmap(maskTexture.Get(), 0);
        if (!copiedMask)
            return false;

        const uint32_t blendValues[4] = {
            pixelFormat == GVFG_PIXFMT_Y210 ? 0xffc0u : 0x00ffu,
            0u, 0u, 0u};
        context->UpdateSubresource(blendParameters.Get(), 0, nullptr, blendValues, 0, 0);
        surfaceWidth = frameWidth;
        surfaceHeight = frameHeight;
        surfaceFormat = pixelFormat;
        surfaceDirty = false;
        return true;
    }

    gvfg_status_t process(const gvfg_frame_t &source,
                          std::vector<uint8_t> &destination,
                          int &destinationStride,
                          bool &applied,
                          gvfg_debug_osd_frame_timing_t *timing,
                          uint8_t *writableSource,
                          bool experimentalZeroCopy,
                          bool *inPlaceApplied,
                          uint32_t *experimentFlags,
                          gvfg_debug_osd_page_check_t *pageCheck)
    {
        if (pageCheck)
            *pageCheck = {};
        if (inPlaceApplied)
            *inPlaceApplied = false;
#ifndef GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE
        if (experimentalZeroCopy)
            return GVFG_EINVAL;
        (void)experimentFlags;
#endif
        using Clock = std::chrono::steady_clock;
        auto stageStart = timing ? Clock::now() : Clock::time_point{};
        const auto finishStage = [timing, &stageStart](gvfg_debug_osd_stage_t stage) {
            if (timing)
            {
                const auto end = Clock::now();
                timing->stage_ms[stage] = std::chrono::duration<double, std::milli>(end - stageStart).count();
                stageStart = end;
            }
        };
        std::lock_guard<std::mutex> lock(mutex);
        finishStage(GVFG_DEBUG_OSD_LOCK_WAIT);
        applied = false;
        if (!active)
            return GVFG_OK;
        if (!source.data || source.width <= 0 || source.height <= 0 ||
            (source.width & 1) != 0 || source.row_stride_bytes <= 0 ||
            (source.pixel_format != GVFG_PIXFMT_YUY2 &&
             source.pixel_format != GVFG_PIXFMT_Y210))
            return GVFG_EINVAL;
        const uint64_t rowBytes64 = static_cast<uint64_t>(source.width) *
                                    (source.pixel_format == GVFG_PIXFMT_YUY2 ? 2u : 4u);
        if (rowBytes64 > static_cast<uint64_t>((std::numeric_limits<int>::max)()) ||
            static_cast<uint64_t>(source.row_stride_bytes) < rowBytes64)
            return GVFG_EINVAL;
        const uint64_t requiredSource =
            static_cast<uint64_t>(source.row_stride_bytes) *
                static_cast<uint64_t>(source.height - 1) +
            rowBytes64;
        if (source.data_size < requiredSource)
            return GVFG_EINVAL;
        if (writableSource && static_cast<const void *>(writableSource) != source.data)
            return GVFG_EINVAL;

        const bool rebuild = surfaceDirty || surfaceWidth != static_cast<uint32_t>(source.width) ||
                             surfaceHeight != static_cast<uint32_t>(source.height) ||
                             surfaceFormat != source.pixel_format;
        if (timing)
            timing->surface_rebuilt = rebuild ? 1 : 0;
        if (rebuild &&
            !rebuildSurface(static_cast<uint32_t>(source.width),
                            static_cast<uint32_t>(source.height), source.pixel_format))
            return GVFG_EIO;
        finishStage(GVFG_DEBUG_OSD_SURFACE);
        // Invisible text needs no processed buffer or GPU work.
        if (region.empty())
            return GVFG_OK;

        const uint32_t rowBytes = static_cast<uint32_t>(rowBytes64);
        const uint32_t bytesPerPixel = source.pixel_format == GVFG_PIXFMT_YUY2 ? 2u : 4u;
        const uint32_t regionRowBytes = region.width * bytesPerPixel;
        const uint64_t sourceOffset64 = static_cast<uint64_t>(region.top) * source.row_stride_bytes +
                                        static_cast<uint64_t>(region.left) * bytesPerPixel;
        if (sourceOffset64 > (std::numeric_limits<size_t>::max)())
            return GVFG_EINVAL;
        const size_t sourceOffset = static_cast<size_t>(sourceOffset64);
#ifdef GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE
        const uint64_t writeSpan = static_cast<uint64_t>(region.height - 1u) *
                                       source.row_stride_bytes + regionRowBytes;
        if (experimentalZeroCopy &&
            (!writableSource || !writableExperimentalRange(source, sourceOffset, writeSpan, pageCheck)))
        {
            if (!writableSource && pageCheck)
                pageCheck->reason = GVFG_DEBUG_OSD_PAGE_NO_POINTER;
            writableSource = nullptr;
            if (experimentFlags)
                *experimentFlags |= GVFG_DEBUG_OSD_EXPERIMENT_REJECTED;
        }
#endif
        if (!writableSource)
            destination.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(source.height));
        uint8_t *outputData = writableSource ? writableSource : destination.data();
        const uint32_t outputStride = writableSource ? static_cast<uint32_t>(source.row_stride_bytes) : rowBytes;
        if (timing)
        {
            timing->roi_width = region.width;
            timing->roi_height = region.height;
        }
        finishStage(GVFG_DEBUG_OSD_PREPARE);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(sourceTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return GVFG_EIO;
        const bool copiedSource = copyRowsToMapped(
            static_cast<const uint8_t *>(source.data) + sourceOffset,
            source.row_stride_bytes, regionRowBytes, region.height, mapped);
        context->Unmap(sourceTexture.Get(), 0);
        if (!copiedSource)
            return GVFG_EIO;
        finishStage(GVFG_DEBUG_OSD_UPLOAD);

        ID3D11ShaderResourceView *srvs[] = {sourceSrv.Get(), osdSrv.Get(), maskSrv.Get()};
        ID3D11UnorderedAccessView *uavs[] = {outputUav.Get()};
        context->CSSetShader(shader.Get(), nullptr, 0);
        ID3D11Buffer *constantBuffers[] = {blendParameters.Get()};
        context->CSSetConstantBuffers(0, 1, constantBuffers);
        context->CSSetShaderResources(0, 3, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((region.width / 2u + 15u) / 16u,
                          (region.height + 15u) / 16u, 1);
        ID3D11ShaderResourceView *nullSrvs[] = {nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView *nullUav = nullptr;
        context->CSSetShaderResources(0, 3, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        ID3D11Buffer *nullBuffer = nullptr;
        context->CSSetConstantBuffers(0, 1, &nullBuffer);
        context->CSSetShader(nullptr, nullptr, 0);

        context->CopyResource(readbackTexture.Get(), outputTexture.Get());
        finishStage(GVFG_DEBUG_OSD_SUBMIT);
        // Driver zero-copy input requires an independent frame by default.
        // SDK-owned copy input already contains the untouched full frame, so
        // only its ROI is overwritten after GPU readback; no full copy/resize.
        if (!writableSource)
        {
            copyFrameRows(source, outputData, rowBytes);
            finishStage(GVFG_DEBUG_OSD_FRAME_COPY);
        }
        if (FAILED(context->Map(readbackTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return GVFG_EIO;
        finishStage(GVFG_DEBUG_OSD_READBACK_WAIT);
        if (mapped.RowPitch < regionRowBytes)
        {
            context->Unmap(readbackTexture.Get(), 0);
            return GVFG_EIO;
        }
        const size_t destinationOffset = static_cast<size_t>(region.top) * outputStride +
                                         static_cast<size_t>(region.left) * bytesPerPixel;
#ifdef GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE
        // Recheck immediately before writing. Permissions can still change after
        // this check; neither this nor the SDK hold prevents concurrent DMA.
        if (experimentalZeroCopy && writableSource &&
            !writableExperimentalRange(source, sourceOffset, writeSpan, pageCheck))
        {
            context->Unmap(readbackTexture.Get(), 0);
            return GVFG_EIO;
        }
#endif
        for (uint32_t row = 0; row < region.height; ++row)
        {
            std::memcpy(outputData + destinationOffset + static_cast<size_t>(row) * outputStride,
                        static_cast<const uint8_t *>(mapped.pData) +
                            static_cast<size_t>(row) * mapped.RowPitch,
                        regionRowBytes);
        }
        context->Unmap(readbackTexture.Get(), 0);
        finishStage(GVFG_DEBUG_OSD_ROI_COPY);
        destinationStride = static_cast<int>(outputStride);
        applied = true;
        if (inPlaceApplied)
            *inPlaceApplied = writableSource != nullptr;
        if (experimentalZeroCopy && writableSource && experimentFlags)
            *experimentFlags |= GVFG_DEBUG_OSD_EXPERIMENT_USED;
        if (timing)
            timing->osd_applied = 1;
        return GVFG_OK;
    }

    mutable std::mutex mutex;
    bool active = false;
    bool surfaceDirty = true;
    std::string text;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t fontSize = 0;
    uint32_t colorRgb = 0xffffffu;
    OsdRegion region;
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    int surfaceFormat = GVFG_PIXFMT_UNKNOWN;
    uint32_t resourceWidth = 0;
    uint32_t resourceHeight = 0;
    int resourceFormat = GVFG_PIXFMT_UNKNOWN;
    std::vector<uint8_t> yuvSurface;
    std::vector<uint8_t> maskSurface;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> blendParameters;
    ComPtr<ID3D11Texture2D> sourceTexture;
    ComPtr<ID3D11Texture2D> osdTexture;
    ComPtr<ID3D11Texture2D> maskTexture;
    ComPtr<ID3D11Texture2D> outputTexture;
    ComPtr<ID3D11Texture2D> readbackTexture;
    ComPtr<ID3D11ShaderResourceView> sourceSrv;
    ComPtr<ID3D11ShaderResourceView> osdSrv;
    ComPtr<ID3D11ShaderResourceView> maskSrv;
    ComPtr<ID3D11UnorderedAccessView> outputUav;
};

GpuOsdProcessor::GpuOsdProcessor() : impl_(std::make_unique<Impl>()) {}
GpuOsdProcessor::~GpuOsdProcessor() = default;
gvfg_status_t GpuOsdProcessor::configure(const gvfg_channel_osd_config_t &config)
{
    try
    {
        return impl_->configure(config);
    }
    catch (...)
    {
        return GVFG_EIO;
    }
}
void GpuOsdProcessor::clear() { impl_->clear(); }
gvfg_status_t GpuOsdProcessor::process(const gvfg_frame_t &source,
                                       std::vector<uint8_t> &destination,
                                       int &destinationStride,
                                       bool &applied,
                                       gvfg_debug_osd_frame_timing_t *timing,
                                       uint8_t *writableSource,
                                       bool experimentalZeroCopy,
                                       bool *inPlaceApplied,
                                       uint32_t *experimentFlags,
                                       gvfg_debug_osd_page_check_t *pageCheck)
{
    try
    {
        return impl_->process(source, destination, destinationStride, applied, timing, writableSource,
                              experimentalZeroCopy, inPlaceApplied, experimentFlags, pageCheck);
    }
    catch (...)
    {
        applied = false;
        if (inPlaceApplied)
            *inPlaceApplied = false;
        destinationStride = 0;
        return GVFG_EIO;
    }
}
}
