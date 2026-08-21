#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef GIGA_IOCTL_BUILD
#define GIGA_IOCTL_API __declspec(dllexport)
#else
#define GIGA_IOCTL_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum giga_ioctl_event_type
{
    GIGA_IOCTL_EVENT_VIDEO_DMA = 0,
    GIGA_IOCTL_EVENT_VIDEO_FORMAT_CHANGE = 1,
    GIGA_IOCTL_EVENT_VIDEO_PLUGIN = 2,
    GIGA_IOCTL_EVENT_VIDEO_UNPLUG = 3,
    GIGA_IOCTL_EVENT_AUDIO_DMA = 4
};

GIGA_IOCTL_API BOOL giga_ioctl_read_register(HANDLE device, uint32_t offset, uint32_t *value);
GIGA_IOCTL_API BOOL giga_ioctl_write_register(HANDLE device, uint32_t offset, uint32_t value);
GIGA_IOCTL_API BOOL giga_ioctl_register_event(HANDLE device, uint32_t channel, uint32_t type, HANDLE event_handle);
GIGA_IOCTL_API BOOL giga_ioctl_unregister_event(HANDLE device, uint32_t channel, uint32_t type);
GIGA_IOCTL_API BOOL giga_ioctl_get_frame(HANDLE device, uint32_t channel, uint32_t frame_index,
                                         void *buffer, uint32_t buffer_size, uint32_t *bytes_returned);
GIGA_IOCTL_API BOOL giga_ioctl_video_start(HANDLE device, uint32_t channel);
GIGA_IOCTL_API BOOL giga_ioctl_video_stop(HANDLE device, uint32_t channel);
GIGA_IOCTL_API BOOL giga_ioctl_release_video_frame(HANDLE device, uint32_t channel);
GIGA_IOCTL_API BOOL giga_ioctl_acquire_video_frame_zerocopy(HANDLE device, uint32_t channel,
                                                            const void **frame_address);
GIGA_IOCTL_API BOOL giga_ioctl_set_frame_zerocopy(HANDLE device, uint32_t channel, BOOL enabled);

#ifdef __cplusplus
}
#endif
