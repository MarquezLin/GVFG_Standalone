# GigabyteLib 1.0.0 integration gaps

Capture, audio, events, signal information, copy mode, and zero-copy mode use
the supervisor-provided `GvfgSdk.lib` API directly.

## Action required from the GigabyteLib owner

The following existing GVFG capabilities are not exposed by `GvfgSdk.lib 1.0.0`:

| GVFG capability | Current temporary implementation | Requested GigabyteLib API |
|---|---|---|
| Select YUY2 or Y210 output | Write global register `0x080` through the isolated driver extension | Add a channel-aware `GvfgSetVideoFormat` or equivalent |
| Read a diagnostic register | Driver read-register request in `gigabyte_driver_extensions.cpp` | Add a supported diagnostic register-read API |
| Write a diagnostic register | Driver write-register request in `gigabyte_driver_extensions.cpp` | Add a supported diagnostic register-write API |

These are marked `GIGABYTELIB GAP` in source. They intentionally do not use
the removed `giga_ioctl.dll`; the minimal fallback is compiled into `gvfg.dll`.
Once official `Gvfg*` functions exist, replace the three extension calls and
delete `gigabyte_driver_extensions.*`.

The library owner should also confirm whether output-format register `0x080`
is global or channel-specific and whether CH0 and CH1 may both select Y210.
