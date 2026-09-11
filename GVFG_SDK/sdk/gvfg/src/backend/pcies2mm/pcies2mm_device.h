#pragma once

#include <string>
#include <vector>

namespace gvfg::internal
{
    struct PcieS2mmDevice
    {
        std::wstring interface_path;
        std::wstring friendly_name;
    };

    std::vector<PcieS2mmDevice> enumerate_pcies2mm_devices();
}
