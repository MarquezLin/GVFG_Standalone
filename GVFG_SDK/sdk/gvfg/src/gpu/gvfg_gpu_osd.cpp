#include "gvfg_gpu_osd.h"

#include "gvfg_osd.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
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
        yuvSurface.resize(static_cast<size_t>(yuvStride) * frameHeight);
        maskSurface.resize(static_cast<size_t>(maskStride) * frameHeight);

        gvfg_osd_handle handle = nullptr;
        if (gvfg_osd_create(&handle) != GVFG_OSD_OK)
            return false;
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
        const gvfg_osd_status_t status = gvfg_osd_render(handle, &config, &output);
        gvfg_osd_destroy(handle);
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
                          bool &applied)
    {
        std::lock_guard<std::mutex> lock(mutex);
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

        if ((surfaceDirty || surfaceWidth != static_cast<uint32_t>(source.width) ||
             surfaceHeight != static_cast<uint32_t>(source.height) ||
             surfaceFormat != source.pixel_format) &&
            !rebuildSurface(static_cast<uint32_t>(source.width),
                            static_cast<uint32_t>(source.height), source.pixel_format))
            return GVFG_EIO;
        // Invisible text needs no processed buffer or GPU work.
        if (region.empty())
            return GVFG_OK;

        const uint32_t rowBytes = static_cast<uint32_t>(rowBytes64);
        const uint32_t bytesPerPixel = source.pixel_format == GVFG_PIXFMT_YUY2 ? 2u : 4u;
        const uint32_t regionRowBytes = region.width * bytesPerPixel;
        const size_t sourceOffset = static_cast<size_t>(region.top) * source.row_stride_bytes +
                                    static_cast<size_t>(region.left) * bytesPerPixel;
        destination.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(source.height));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(sourceTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return GVFG_EIO;
        const bool copiedSource = copyRowsToMapped(
            static_cast<const uint8_t *>(source.data) + sourceOffset,
            source.row_stride_bytes, regionRowBytes, region.height, mapped);
        context->Unmap(sourceTexture.Get(), 0);
        if (!copiedSource)
            return GVFG_EIO;

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
        // CPU only preserves the native frame; all blending remains on the GPU.
        // Copy before Map READ so it can overlap queued GPU work.
        copyFrameRows(source, destination.data(), rowBytes);
        if (FAILED(context->Map(readbackTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return GVFG_EIO;
        if (mapped.RowPitch < regionRowBytes)
        {
            context->Unmap(readbackTexture.Get(), 0);
            return GVFG_EIO;
        }
        const size_t destinationOffset = static_cast<size_t>(region.top) * rowBytes +
                                         static_cast<size_t>(region.left) * bytesPerPixel;
        for (uint32_t row = 0; row < region.height; ++row)
        {
            std::memcpy(destination.data() + destinationOffset + static_cast<size_t>(row) * rowBytes,
                        static_cast<const uint8_t *>(mapped.pData) +
                            static_cast<size_t>(row) * mapped.RowPitch,
                        regionRowBytes);
        }
        context->Unmap(readbackTexture.Get(), 0);
        destinationStride = static_cast<int>(rowBytes);
        applied = true;
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
    return impl_->configure(config);
}
void GpuOsdProcessor::clear() { impl_->clear(); }
gvfg_status_t GpuOsdProcessor::process(const gvfg_frame_t &source,
                                       std::vector<uint8_t> &destination,
                                       int &destinationStride,
                                       bool &applied)
{
    return impl_->process(source, destination, destinationStride, applied);
}
}
