#include "gvfg_capture.h"

#include "d3d_conversion_pipeline.h"

#include <d3d11_4.h>
#include <dxgi.h>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(gvfg_gpu_output_buffer_t) == 24,
              "gvfg_gpu_output_buffer_t x64 ABI must remain frozen");
static_assert(offsetof(gvfg_gpu_output_buffer_t, data) == 0);
static_assert(offsetof(gvfg_gpu_output_buffer_t, data_size) == 8);
static_assert(offsetof(gvfg_gpu_output_buffer_t, row_bytes) == 16);
static_assert(offsetof(gvfg_gpu_output_buffer_t, pixel_format) == 20);
#endif

namespace
{
class GpuBufferConverter
{
public:
    gvfg_status_t convert(const gvfg_frame_t &source,
                          const gvfg_gpu_output_buffer_t &output)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDevice())
            return GVFG_EIO;

        if (!pipeline_)
            pipeline_ = std::make_unique<gvfg::internal::D3DPreviewPipeline>();
        if (!pipeline_->initialize(device_.Get(), context_.Get()) ||
            !pipeline_->ensure_rt_and_pipeline(source.width, source.height))
            return GVFG_EIO;

        gvfg::internal::gvfg_render_pixfmt_t renderFormat{};
        bool uploaded = false;
        const auto *data = static_cast<const uint8_t *>(source.data);
        switch (source.pixel_format)
        {
        case GVFG_PIXFMT_YVYU:
            renderFormat = gvfg::internal::GVFG_RENDER_FMT_YVYU;
            uploaded = pipeline_->upload_packed_422_frame(data,
                                                     source.row_stride_bytes,
                                                     source.width,
                                                     source.height);
            break;
        case GVFG_PIXFMT_Y210:
            renderFormat = gvfg::internal::GVFG_RENDER_FMT_Y210;
            uploaded = pipeline_->upload_y210_frame(data,
                                                     source.row_stride_bytes,
                                                     source.width,
                                                     source.height);
            break;
        default:
            return GVFG_ENOTSUP;
        }

        if (!uploaded ||
            !pipeline_->render_uploaded_yuv_to_fp16(renderFormat, source.width, source.height) ||
            !pipeline_->copy_fp16_to_scene())
            return GVFG_EIO;

        DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
        bool rendered = false;
        switch (output.pixel_format)
        {
        case GVFG_GPU_OUTPUT_BGRA8:
            outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            rendered = pipeline_->blit_fp16_to_rgba8(source.width, source.height);
            break;
        case GVFG_GPU_OUTPUT_RGB10A2:
            outputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            rendered = pipeline_->blit_fp16_to_rgb10a2(source.width, source.height);
            break;
        case GVFG_GPU_OUTPUT_NV12:
            rendered = pipeline_->blit_fp16_to_nv12(source.width, source.height);
            if (!rendered ||
                !pipeline_->readback_nv12_to_buffer(output.data,
                                                     output.data_size,
                                                     output.row_bytes,
                                                     source.width,
                                                     source.height))
                return GVFG_EIO;
            return GVFG_OK;
        default:
            return GVFG_ENOTSUP;
        }

        if (!rendered ||
            !pipeline_->readback_to_buffer(output.data,
                                           output.data_size,
                                           output.row_bytes,
                                           outputFormat,
                                           source.width,
                                           source.height))
            return GVFG_EIO;
        return GVFG_OK;
    }

private:
    bool ensureDevice()
    {
        if (device_ && context_)
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
        HRESULT hr = D3D11CreateDevice(nullptr,
                                       D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       flags,
                                       levels,
                                       static_cast<UINT>(std::size(levels)),
                                       D3D11_SDK_VERSION,
                                       &device_,
                                       &actual,
                                       &context_);
#ifdef _DEBUG
        if (FAILED(hr))
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   flags,
                                   levels,
                                   static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION,
                                   &device_,
                                   &actual,
                                   &context_);
        }
#endif
        if (FAILED(hr) || !device_ || !context_)
            return false;

        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(device_.As(&multithread)) && multithread)
            multithread->SetMultithreadProtected(TRUE);
        return true;
    }

    std::mutex mutex_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::unique_ptr<gvfg::internal::D3DPreviewPipeline> pipeline_;
};

bool checkedFrameLayout(const gvfg_frame_t &source)
{
    if (!source.data || source.width <= 0 || source.height <= 0 ||
        source.row_stride_bytes <= 0)
        return false;

    uint64_t minimumRowBytes = 0;
    switch (source.pixel_format)
    {
    case GVFG_PIXFMT_YVYU:
        minimumRowBytes = static_cast<uint64_t>(source.width) * 2u;
        break;
    case GVFG_PIXFMT_Y210:
        minimumRowBytes = static_cast<uint64_t>(source.width) * 4u;
        break;
    default:
        return true;
    }
    if (static_cast<uint64_t>(source.row_stride_bytes) < minimumRowBytes)
        return false;
    const uint64_t rowOffset = static_cast<uint64_t>(source.row_stride_bytes) *
                               static_cast<uint64_t>(source.height - 1);
    return rowOffset <= UINT64_MAX - minimumRowBytes &&
           source.data_size >= rowOffset + minimumRowBytes;
}

GpuBufferConverter &sharedConverter()
{
    static GpuBufferConverter converter;
    return converter;
}
}

extern "C" GVFG_API gvfg_status_t gvfg_gpu_convert_to_buffer(
    const gvfg_frame_t *source,
    const gvfg_gpu_output_buffer_t *output)
{
    if (!source || !output || !output->data ||
        source->width <= 0 || source->height <= 0 ||
        output->row_bytes <= 0)
        return GVFG_EINVAL;

    if (source->pixel_format != GVFG_PIXFMT_YVYU &&
        source->pixel_format != GVFG_PIXFMT_Y210)
        return GVFG_ENOTSUP;
    if (output->pixel_format != GVFG_GPU_OUTPUT_BGRA8 &&
        output->pixel_format != GVFG_GPU_OUTPUT_RGB10A2 &&
        output->pixel_format != GVFG_GPU_OUTPUT_NV12)
        return GVFG_ENOTSUP;
    if (!checkedFrameLayout(*source))
        return GVFG_EINVAL;

    if (output->pixel_format == GVFG_GPU_OUTPUT_NV12 &&
        ((source->width & 1) != 0 || (source->height & 1) != 0))
        return GVFG_EINVAL;

    const uint64_t minimumRowBytes = output->pixel_format == GVFG_GPU_OUTPUT_NV12
                                         ? static_cast<uint64_t>(source->width)
                                         : static_cast<uint64_t>(source->width) * 4u;
    if (static_cast<uint64_t>(output->row_bytes) < minimumRowBytes)
        return GVFG_EINVAL;
    const uint64_t outputRows = output->pixel_format == GVFG_GPU_OUTPUT_NV12
                                    ? static_cast<uint64_t>(source->height) +
                                          static_cast<uint64_t>(source->height / 2)
                                    : static_cast<uint64_t>(source->height);
    const uint64_t requiredSize = static_cast<uint64_t>(output->row_bytes) * outputRows;
    if (output->data_size < requiredSize)
        return GVFG_EINVAL;

    return sharedConverter().convert(*source, *output);
}

namespace
{
gvfg_status_t convertToFormat(const gvfg_frame_t *source,
                              void *destination,
                              uint64_t destinationSize,
                              int rowBytes,
                              gvfg_gpu_output_format_t format)
{
    gvfg_gpu_output_buffer_t output{};
    output.data = destination;
    output.data_size = destinationSize;
    output.row_bytes = rowBytes;
    output.pixel_format = static_cast<int>(format);
    return gvfg_gpu_convert_to_buffer(source, &output);
}
}

extern "C" GVFG_API gvfg_status_t gvfg_gpu_convert_to_bgra8(
    const gvfg_frame_t *source,
    void *destination,
    uint64_t destination_size,
    int row_bytes)
{
    return convertToFormat(source, destination, destination_size, row_bytes,
                           GVFG_GPU_OUTPUT_BGRA8);
}

extern "C" GVFG_API gvfg_status_t gvfg_gpu_convert_to_rgb10a2(
    const gvfg_frame_t *source,
    void *destination,
    uint64_t destination_size,
    int row_bytes)
{
    return convertToFormat(source, destination, destination_size, row_bytes,
                           GVFG_GPU_OUTPUT_RGB10A2);
}

extern "C" GVFG_API gvfg_status_t gvfg_gpu_convert_to_nv12(
    const gvfg_frame_t *source,
    void *destination,
    uint64_t destination_size,
    int row_bytes)
{
    return convertToFormat(source, destination, destination_size, row_bytes,
                           GVFG_GPU_OUTPUT_NV12);
}
