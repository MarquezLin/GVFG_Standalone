# GVFG Standalone SDK

這是 GVFG capture SDK 的 standalone source tree。

這個 repo 以 GVFG 為 source of truth。其他專案應該透過 public header、
import library、runtime DLL 來使用 GVFG，不要直接把 GVFG source 編進去。

## 內容

- `sdk/gvfg`：GVFG customer C API、internal debug API、PCIES2MM backend。
- `helpers/gvfg_preview`：可選的 preview helper DLL，在 `gvfg_read_frame()` 後使用。
- `helpers/gvfg_convert`：可選的 snapshot/export conversion helper DLL。
- `samples/gvfg_qt_preview`：內部 debug Qt preview tool。
- `docs`：API 與整合說明。

客戶端 API 細節在 `docs/GVFG_CUSTOMER_API.md`。內部架構、package
切分、threading、frame ownership 說明在 `docs/GVFG_INTERNAL_NOTES.md`。

## Build

可以用 Windows MSVC Qt kit 在 Qt Creator 打開此資料夾的 `CMakeLists.txt`，
或在 Visual Studio developer shell 裡 configure。

範例：

```bat
cmake -S . -B build -DBUILD_GVFG_SAMPLES=ON -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64
cmake --build build --target gvfg_qt_preview --config Release
```

常用 CMake options：

```text
BUILD_GVFG_SAMPLES=ON
GVFG_PCIES2MM_DEBUG_LOG=OFF
```

Build 產物：

```text
build/.../bin/gvfg.dll
build/.../bin/gvfg_preview.dll
build/.../bin/gvfg_convert.dll
build/.../lib/gvfg.lib
build/.../lib/gvfg_preview.lib
build/.../lib/gvfg_convert.lib
build/.../bin/gvfg_qt_preview.exe
```

## Consumer Layout

一般 application 使用 core SDK 只需要：

```text
include/gvfg_capture.h
lib/gvfg.lib
bin/gvfg.dll
```

需要 optional display helper 時，再加：

```text
include/gvfg_preview.h
lib/gvfg_preview.lib
bin/gvfg_preview.dll
```

需要 snapshot/export conversion 時，再加：

```text
include/gvfg_convert.h
lib/gvfg_convert.lib
bin/gvfg_convert.dll
```

Customer/demo package 不應包含 `gvfg_debug.h`、SDK source、PCIES2MM backend
headers、IRQ details 或 helper source。這些只屬於 internal
debug/full application package。
