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
