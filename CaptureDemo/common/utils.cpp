#include "utils.h"

BYTE* Utils::allocate_buffer(size_t size, size_t alignment)
{
    if (size == 0)
    {
        size = 4;
    }

    if (alignment == 0)
    {
        SYSTEM_INFO sys_info;
        GetSystemInfo(&sys_info);
        alignment = sys_info.dwPageSize;
    }

    return (BYTE *)_aligned_malloc(size, alignment);
}
