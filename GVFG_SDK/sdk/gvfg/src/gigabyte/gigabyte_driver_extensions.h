#pragma once

#include <windows.h>
#include <cstdint>

namespace gvfg::internal
{
    // GIGABYTELIB GAP: GvfgSdk.lib 1.0.0 does not expose register access or
    // output-format selection. Keep these driver extensions isolated here so
    // they can be deleted when equivalent Gvfg* APIs are supplied.
    bool gigabyte_read_register(HANDLE device, uint32_t offset, uint32_t &value);
    bool gigabyte_write_register(HANDLE device, uint32_t offset, uint32_t value);
}
