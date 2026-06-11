# GVFG Standalone SDK

This is the standalone source tree for the GVFG capture SDK.

GVFG is the source of truth here. Other projects should consume GVFG through
the exported public header, import library, and runtime DLL instead of compiling
the GVFG source directly.

## Contents

- `sdk/gvfg`: GVFG public C API and internal XDMA backend.
- `sdk/gvfg/src/render`: GVFG internal D3D preview/render pipeline.
- `samples/gvfg_qt_preview`: customer-facing Qt preview sample.
- `docs`: API and integration notes.

Start with `docs/GVFG_PROJECT_MAP.md` for the project layer map, API
lifecycle, and frame/event data-flow overview.

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
build/.../lib/gvfg.lib
build/.../bin/gvfg_qt_preview.exe
```

## Consumer Layout

Applications should consume the SDK with:

```text
include/gvfg_capture.h
lib/gvfg.lib
bin/gvfg.dll
```

The XDMA backend headers are implementation details and should not be included by
customer applications.

