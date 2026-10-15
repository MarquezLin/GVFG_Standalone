#pragma once

#include "gvfg_capture.h"

#include <memory>
#include <string>
#include <vector>

namespace gvfg::internal
{
class GpuOsdProcessor
{
public:
    GpuOsdProcessor();
    ~GpuOsdProcessor();

    gvfg_status_t configure(const gvfg_channel_osd_config_t &config);
    void clear();
    gvfg_status_t process(const gvfg_frame_t &source,
                          std::vector<uint8_t> &destination,
                          int &destinationStride,
                          bool &applied);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
