#include "gigabyte_driver_extensions.h"

#include <winioctl.h>

namespace
{
    constexpr DWORD kWriteRegister = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kReadRegister = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);

    struct RegisterAccess
    {
        ULONG offset;
        ULONG value;
    };
}

namespace gvfg::internal
{
    bool gigabyte_read_register(HANDLE device, uint32_t offset, uint32_t &value)
    {
        RegisterAccess request{offset, 0};
        DWORD returned = 0;
        if (!DeviceIoControl(device, kReadRegister,
                             &request, sizeof(request), &request, sizeof(request),
                             &returned, nullptr))
            return false;
        value = request.value;
        return true;
    }

    bool gigabyte_write_register(HANDLE device, uint32_t offset, uint32_t value)
    {
        RegisterAccess request{offset, value};
        DWORD returned = 0;
        return DeviceIoControl(device, kWriteRegister,
                               &request, sizeof(request), nullptr, 0,
                               &returned, nullptr) != FALSE;
    }
}
