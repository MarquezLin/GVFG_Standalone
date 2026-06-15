#pragma once

#include <gvfg_capture.h>

#include <memory>
#include <mutex>

namespace gvfg::internal
{
class SharedScenePipeline;
}

class SamplePreviewRenderer
{
public:
    SamplePreviewRenderer();
    ~SamplePreviewRenderer();

    bool configure(void *hwnd);
    bool render(const gvfg_frame_t &frame);
    void shutdown();

    bool active() const;
    int width() const;
    int height() const;
    int bitDepth() const;
    const char *pixelFormat() const;

private:
    bool ensureDevice();
    bool ensurePipeline(int width, int height, int sourceBitDepth);

    mutable std::mutex mutex_;
    void *hwnd_ = nullptr;
    bool configured_ = false;
    int width_ = 0;
    int height_ = 0;
    int bitDepth_ = 0;
    bool swapchain10Bit_ = false;

    struct D3DState;
    std::unique_ptr<D3DState> d3d_;
    std::unique_ptr<gvfg::internal::SharedScenePipeline> pipeline_;
};
