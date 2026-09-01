# giga_ioctl

`giga_ioctl` is the private DLL boundary between GVFG and the Windows driver.
It owns only the driver IOCTL codes, request layouts, and thin
`DeviceIoControl` wrappers. Device discovery, capture state, threads, buffers,
and frame lifetime remain in `gvfg`.

When adding a driver operation:

1. Add its private IOCTL code and request layout to `src/giga_ioctl_private.h`.
2. Add a small C function to `include/giga_ioctl.h`.
3. Implement that function in `src/giga_ioctl.cpp` with no capture state.
4. Preserve `GetLastError()` on driver failures; use `ERROR_INVALID_PARAMETER`
   for invalid caller output pointers.

The header is an internal dependency of `gvfg`; it is not a customer SDK API.

## Current audio ABI

The implemented audio path is copy-out, not zero-copy:

- Register `GIGA_IOCTL_EVENT_AUDIO_DMA` and
  `GIGA_IOCTL_EVENT_EXTRA_AUDIO_FRAME` as wake-up notifications.
- Start and stop capture with `giga_ioctl_start_video_audio()` and
  `giga_ioctl_stop_video_audio()`.
- Query the PCM layout and driver frame size with
  `giga_ioctl_get_audio_info()`.
- Read the next available driver frame with `giga_ioctl_get_audio_frame()` and
  `frame_index == UINT32_MAX`. The returned byte count is the valid copy size.

The extra-frame events mean that more frames are ready and the consumer should
drain promptly. They do not transfer ownership of a frame. Audio zero-copy is a
future driver capability and is intentionally not exposed here until its
acquire/release ABI is complete.
