# GVFG Standalone SDK

這是 GVFG capture SDK 的 standalone source tree。

這個 repo 以 GVFG 為 source of truth。其他專案應該透過 public header、
import library、runtime DLL 來使用 GVFG，不要直接把 GVFG source 編進去。

## 內容

- `sdk/gvfg`：GVFG customer C API、internal debug API、PCIES2MM backend。
- `helpers/gvfg_preview`：可選的 preview helper DLL，在 `gvfg_read_frame()` 後使用。
- `samples/gvfg_qt_preview`：Qt preview sample；診斷功能由 build option 控制。
- `docs`：API 與整合說明。

客戶使用指南在 `docs/GVFG_CUSTOMER_API.md`，完整函式與結構參考在
`docs/GVFG_CUSTOMER_API_REFERENCE.md`。內部架構、package
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
```

公司內部 diagnostic build 使用：

```bat
cmake -S . -B build_internal ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64
cmake --build build_internal --target gvfg_qt_preview --config Debug
```

Build 產物：

```text
build/.../bin/gvfg.dll
build/.../bin/gvfg_preview.dll
build/.../lib/gvfg.lib
build/.../lib/gvfg_preview.lib
build/.../bin/gvfg_qt_preview.exe
```

## Driver compatibility

此版本只支援新版 PCIE S2MM driver，不再 backward compatible 舊版 driver。
新版 driver 必須支援：

- `IOCTL_GIGA_VIDEO_START`（function `0x808`）。
- `IOCTL_GIGA_VIDEO_STOP`（function `0x809`）。
- `IOCTL_PCIES2MM_GET_FRAME` 接受 `frameIndex = MAXULONG`（`0xFFFFFFFF`），
  由 driver 自行選擇已完成的 frame。

SDK 不再呼叫 `IOCTL_PCIES2MM_GET_VIDEO_DONE_INDEX`，也不會在新 IOCTL
不支援時退回直接寫入 `VIDEO_DMA_EN_OFFSET`、`VIDEO_EN_OFFSET` 或
`IRQ_MASK_W1S_OFFSET`。若搭配舊版 driver，stream start 或 frame capture
可能失敗。

### Zero-copy selection

上層程式可在 `gvfg_create()` 後、`gvfg_open_channel()` 前選擇 zero-copy：

```c
gvfg_handle handle = NULL;
gvfg_create(&handle);
gvfg_set_zero_copy_enabled(handle, 1); /* 0: copy, 1: zero-copy */
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_0);
```

預設為 copy mode。Device open 後不可切換模式；如需切換，必須 destroy
並重建 session。Zero-copy mode 仍使用相同的 `gvfg_read_frame()` /
`gvfg_release_frame()` ownership contract，每次成功 read 都必須 release。
SDK 會在 open 時 enable zero-copy，並在 close/destroy 時 disable。

`gvfg_qt_preview.exe` 只使用 public API，主畫面只顯示 input 與 Preview
狀態；IRQ、DMA、ring slot 等資訊只存在 internal diagnostic tool。

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

GPU snapshot/export conversion 已整合在 `gvfg.dll`，不需要額外 helper DLL。

Customer/demo package 不應包含 `gvfg_debug.h`、SDK source、PCIES2MM backend
headers、IRQ details 或 helper source。這些只屬於 internal
debug/full application package。
