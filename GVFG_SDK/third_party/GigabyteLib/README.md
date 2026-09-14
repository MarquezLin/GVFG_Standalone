# GigabyteLib dependency

This directory contains the supervisor-provided static MSVC library used by the
GVFG backend.

- Upstream version macro: `GVFG_SDK_VER 1.0.0`
- Library: `GvfgSdk.lib`
- SHA-256: `B8D1ED04C2A8F1857F43B81E68B17DAE362D576D2156AF666A2B5C3286CEBDAA`
- Original source: `_SDK/ex/_demo/GigabyteLib/GigabyteLib`

`GvfgSdk.lib` is linked into `gvfg.dll`; it is not distributed as a separate
customer runtime dependency. The supplied package did not contain source,
symbols, or a license file. Replace the library and headers together when a new
upstream build is delivered, and update the checksum above.
