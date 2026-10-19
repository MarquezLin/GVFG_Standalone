#pragma once

#include "gvfg_capture.h"
#include "gvfg_debug.h"

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
    // writableSource must equal source.data. Only the backend can grant this
    // writable SDK-owned storage, held until the caller releases the frame.
    // The default-OFF experimental override admits driver storage only after
    // page checks; this is not a driver/DMA ownership guarantee.
    // nullptr preserves the separate-output path used for driver zero-copy.
    gvfg_status_t process(const gvfg_frame_t &source,
                          std::vector<uint8_t> &destination,
                          int &destinationStride,
                          bool &applied,
                          gvfg_debug_osd_frame_timing_t *timing = nullptr,
                          uint8_t *writableSource = nullptr,
                          bool experimentalZeroCopy = false,
                          bool *inPlaceApplied = nullptr,
                          uint32_t *experimentFlags = nullptr,
                          gvfg_debug_osd_page_check_t *pageCheck = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
