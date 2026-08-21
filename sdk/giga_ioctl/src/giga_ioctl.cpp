#include "giga_ioctl.h"
#include "giga_ioctl_private.h"
#include <limits>

namespace
{
    BOOL channel_ioctl(HANDLE device, DWORD code, uint32_t channel)
    {
        ULONG input = channel;
        DWORD returned = 0;
        return DeviceIoControl(device, code, &input, sizeof(input), nullptr, 0, &returned, nullptr);
    }
}

extern "C" {
BOOL giga_ioctl_read_register(HANDLE d, uint32_t o, uint32_t *v) {
    if (!v) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GIGA_REG_ACCESS r{ o, 0 }; DWORD n = 0;
    BOOL ok = DeviceIoControl(d, IOCTL_PCIES2MM_READ_REG, &r, sizeof(r), &r, sizeof(r), &n, nullptr);
    if (ok) *v = r.Value;
    return ok;
}
BOOL giga_ioctl_write_register(HANDLE d, uint32_t o, uint32_t v) {
    GIGA_REG_ACCESS r{ o, v }; DWORD n = 0;
    return DeviceIoControl(d, IOCTL_PCIES2MM_WRITE_REG, &r, sizeof(r), nullptr, 0, &n, nullptr);
}
BOOL giga_ioctl_register_event(HANDLE d, uint32_t c, uint32_t t, HANDLE e) {
    GIGA_EVENT_REG r{ t, c, e }; DWORD n = 0;
    return DeviceIoControl(d, IOCTL_PCIES2MM_REGISTER_EVENT, &r, sizeof(r), nullptr, 0, &n, nullptr);
}
BOOL giga_ioctl_unregister_event(HANDLE d, uint32_t c, uint32_t t) {
    GIGA_EVENT_REG r{ t, c, nullptr }; DWORD n = 0;
    return DeviceIoControl(d, IOCTL_PCIES2MM_UNREGISTER_EVENT, &r, sizeof(r), nullptr, 0, &n, nullptr);
}
BOOL giga_ioctl_get_frame(HANDLE d, uint32_t c, uint32_t i, void *b, uint32_t s, uint32_t *n) {
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG input[2] = { c, i }; DWORD returned = 0;
    BOOL ok = DeviceIoControl(d, IOCTL_PCIES2MM_GET_FRAME, input, sizeof(input), b, s, &returned, nullptr);
    *n = returned; return ok;
}
BOOL giga_ioctl_video_start(HANDLE d, uint32_t c) { return channel_ioctl(d, IOCTL_GIGA_VIDEO_START, c); }
BOOL giga_ioctl_video_stop(HANDLE d, uint32_t c) { return channel_ioctl(d, IOCTL_GIGA_VIDEO_STOP, c); }
BOOL giga_ioctl_release_video_frame(HANDLE d, uint32_t c) { return channel_ioctl(d, IOCTL_GIGA_RELEASE_VIDEO_FRAME, c); }
BOOL giga_ioctl_acquire_video_frame_zerocopy(HANDLE d, uint32_t c, const void **p) {
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG input[2] = { c, (std::numeric_limits<ULONG>::max)() }; void *address = nullptr; DWORD n = 0;
    BOOL ok = DeviceIoControl(d, IOCTL_GIGA_ACQUIRE_VIDEO_FRAME_ZEROCOPY, input, sizeof(input), &address, sizeof(address), &n, nullptr);
    if (!ok) return FALSE;
    if (!address) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    *p = address;
    return TRUE;
}
BOOL giga_ioctl_set_frame_zerocopy(HANDLE d, uint32_t c, BOOL enabled) {
    return channel_ioctl(d, enabled ? IOCTL_GIGA_ENABLE_FRAME_ZEROCOPY : IOCTL_GIGA_DISABLE_FRAME_ZEROCOPY, c);
}
}
