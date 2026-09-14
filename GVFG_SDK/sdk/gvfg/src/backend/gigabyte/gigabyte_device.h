#pragma once

#include <string>
#include <vector>

namespace gvfg::internal
{
    struct GigabyteDevice
    {
        std::wstring interface_path;
        std::wstring friendly_name;
    };

    std::vector<GigabyteDevice> enumerate_gigabyte_devices();
}
