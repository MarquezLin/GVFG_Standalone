# GVFG Standalone SDK

This is the standalone source tree for the GVFG capture SDK.

GVFG is the source of truth here. Other projects should consume GVFG through
the exported public header, import library, and runtime DLL instead of compiling
the GVFG source directly.

## Contents

- `sdk/gvfg`: GVFG customer C API, internal debug API, and XDMA backend.
- `helpers/gvfg_preview`: optional preview helper DLL used after gvfg_read_frame().
- `samples/gvfg_qt_preview`: internal debug Qt preview tool.
- `docs`: API and integration notes.

Customer-facing API details are in `docs/GVFG_CUSTOMER_API.md`. Internal
architecture, package split, threading, and frame ownership notes are in
`docs/GVFG_INTERNAL_NOTES.md`.

## Build

Open this folder's `CMakeLists.txt` in Qt Creator with a Windows MSVC Qt kit, or
configure from a Visual Studio developer shell.

Example:

```bat
cmake -S . -B build -DBUILD_GVFG_SAMPLES=ON -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64
cmake --build build --target gvfg_qt_preview --config Release
```

Useful CMake options:

```text
BUILD_GVFG_SAMPLES=ON
GVFG_XDMA_DEBUG_LOG=OFF
```

Build outputs:

```text
build/.../bin/gvfg.dll
build/.../bin/gvfg_preview.dll
build/.../lib/gvfg.lib
build/.../lib/gvfg_preview.lib
build/.../bin/gvfg_qt_preview.exe
```

## Consumer Layout

Applications should consume the SDK with:

```text
include/gvfg_capture.h
lib/gvfg.lib
bin/gvfg.dll
```

Applications that want the optional display helper can also consume:

```text
include/gvfg_preview.h
lib/gvfg_preview.lib
bin/gvfg_preview.dll
```

Customer/demo packages should not include `gvfg_debug.h`, SDK source, XDMA
backend headers, IRQ details, raw FPGA values, or preview helper source. Those
belong in the internal debug/full application packages.

