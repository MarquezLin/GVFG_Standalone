#pragma once

#include <windows.h>
#include <cstdint>

namespace gvfg::internal
{
    // GIGABYTELIB GAP: GvfgSdk.lib 1.0.2 has no declared register-access API.
    // Keep this driver extension isolated until matching Gvfg* APIs are supplied.
    bool gigabyte_read_register(HANDLE device, uint32_t offset, uint32_t &value);
    bool gigabyte_write_register(HANDLE device, uint32_t offset, uint32_t value);
}
