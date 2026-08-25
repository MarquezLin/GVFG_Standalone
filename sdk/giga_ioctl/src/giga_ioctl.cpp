#include "giga_ioctl.h"
#include "giga_ioctl_private.h"

#include <limits>

namespace
{
    BOOL send_ioctl(HANDLE device,
                    DWORD code,
                    void *input,
                    DWORD input_size,
                    void *output = nullptr,
                    DWORD output_size = 0,
                    DWORD *bytes_returned = nullptr)
    {
        DWORD ignored = 0;
        return DeviceIoControl(device, code,
                               input, input_size,
                               output, output_size,
                               bytes_returned ? bytes_returned : &ignored,
                               nullptr);
    }

    BOOL send_channel_ioctl(HANDLE device, DWORD code, uint32_t channel)
    {
        ULONG channel_index = channel;
        return send_ioctl(device, code, &channel_index, sizeof(channel_index));
    }

    BOOL invalid_output_pointer()
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

extern "C"
{
    BOOL giga_ioctl_read_register(HANDLE device, uint32_t offset, uint32_t *value)
    {
        if (!value)
            return invalid_output_pointer();

        GIGA_REG_ACCESS request{offset, 0};
        const BOOL ok = send_ioctl(device, IOCTL_PCIES2MM_READ_REG,
                                   &request, sizeof(request),
                                   &request, sizeof(request));
        if (ok)
            *value = request.Value;
        return ok;
    }

    BOOL giga_ioctl_write_register(HANDLE device, uint32_t offset, uint32_t value)
    {
        GIGA_REG_ACCESS request{offset, value};
        return send_ioctl(device, IOCTL_PCIES2MM_WRITE_REG, &request, sizeof(request));
    }

    BOOL giga_ioctl_register_event(HANDLE device, uint32_t channel,
                                   uint32_t type, HANDLE event_handle)
    {
        GIGA_EVENT_REG request{type, channel, event_handle};
        return send_ioctl(device, IOCTL_PCIES2MM_REGISTER_EVENT, &request, sizeof(request));
    }

    BOOL giga_ioctl_unregister_event(HANDLE device, uint32_t channel, uint32_t type)
    {
        GIGA_EVENT_REG request{type, channel, nullptr};
        return send_ioctl(device, IOCTL_PCIES2MM_UNREGISTER_EVENT, &request, sizeof(request));
    }

    BOOL giga_ioctl_get_frame(HANDLE device, uint32_t channel, uint32_t frame_index,
                              void *buffer, uint32_t buffer_size, uint32_t *bytes_returned)
    {
        if (!bytes_returned)
            return invalid_output_pointer();

        GIGA_FRAME_REQUEST request{channel, frame_index};
        DWORD returned = 0;
        const BOOL ok = send_ioctl(device, IOCTL_PCIES2MM_GET_FRAME,
                                   &request, sizeof(request),
                                   buffer, buffer_size, &returned);
        *bytes_returned = returned;
        return ok;
    }

    BOOL giga_ioctl_video_start(HANDLE device, uint32_t channel)
    {
        return send_channel_ioctl(device, IOCTL_GIGA_VIDEO_START, channel);
    }

    BOOL giga_ioctl_video_stop(HANDLE device, uint32_t channel)
    {
        return send_channel_ioctl(device, IOCTL_GIGA_VIDEO_STOP, channel);
    }

    BOOL giga_ioctl_release_video_frame(HANDLE device, uint32_t channel)
    {
        return send_channel_ioctl(device, IOCTL_GIGA_RELEASE_VIDEO_FRAME, channel);
    }

    BOOL giga_ioctl_acquire_video_frame_zerocopy(HANDLE device, uint32_t channel,
                                                 const void **frame_address)
    {
        if (!frame_address)
            return invalid_output_pointer();

        GIGA_FRAME_REQUEST request{channel, (std::numeric_limits<ULONG>::max)()};
        void *address = nullptr;
        const BOOL ok = send_ioctl(device, IOCTL_GIGA_ACQUIRE_VIDEO_FRAME_ZEROCOPY,
                                   &request, sizeof(request),
                                   &address, sizeof(address));
        if (!ok)
            return FALSE;
        if (!address)
        {
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }

        *frame_address = address;
        return TRUE;
    }

    BOOL giga_ioctl_set_frame_zerocopy(HANDLE device, uint32_t channel, BOOL enabled)
    {
        const DWORD code = enabled
                               ? IOCTL_GIGA_ENABLE_FRAME_ZEROCOPY
                               : IOCTL_GIGA_DISABLE_FRAME_ZEROCOPY;
        return send_channel_ioctl(device, code, channel);
    }
}
