# GigabyteLib 1.0.2 integration gaps

Capture, audio, events, signal information, copy mode, and zero-copy mode use
the supervisor-provided `GvfgSdk.lib` API directly.

## Action required from the GigabyteLib owner

The following existing GVFG capabilities are not exposed by the declared
`GvfgSdk.lib 1.0.2` API:

| GVFG capability | Current temporary implementation | Requested GigabyteLib API |
|---|---|---|
| Read a diagnostic register | Driver read-register request in `gigabyte_driver_extensions.cpp` | Add a supported diagnostic register-read API |
| Write a diagnostic register | Driver write-register request in `gigabyte_driver_extensions.cpp` | Add a supported diagnostic register-write API |

These are marked `GIGABYTELIB GAP` in source. They intentionally do not use
the removed `giga_ioctl.dll`; the minimal fallback is compiled into `gvfg.dll`.
GigabyteLib 1.0.2 supplies `GvfgSetVideoColorDepth()` for YUY2/Y210 selection,
so format switching no longer uses the extension. Once official register functions
exist, replace the remaining two extension calls and delete
`gigabyte_driver_extensions.*`.

The binary also contains a C++-mangled `GvfgGetAVStats` symbol, but the supplied
header declares neither that function nor `GIGA_AV_STATS_INFO`. It cannot be used
safely until its public declaration and structure ABI are supplied.
