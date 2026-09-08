# GVFG Standalone SDK

這是 GVFG capture SDK 的 standalone source tree。

這個 repo 以 GVFG 為 source of truth。其他專案應該透過 public header、
import library、runtime DLL 來使用 GVFG，不要直接把 GVFG source 編進去。

## Driver compatibility

此開發版本僅支援新 driver `vfg100_0821c`。舊版 driver、舊 IOCTL、
done-index 與舊 SDK frame-ring 流程不在相容範圍內。

## 內容

- `sdk/gvfg`：GVFG customer C API、internal debug API、PCIES2MM backend。
- `helpers/gvfg_preview`：可選的 preview helper DLL，在 `gvfg_read_channel_frame()` 後使用。
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
build/.../bin/giga_ioctl.dll
build/.../bin/gvfg_preview.dll
build/.../lib/gvfg.lib
build/.../lib/giga_ioctl.lib
build/.../lib/gvfg_preview.lib
build/.../bin/gvfg_qt_preview.exe
```

## Driver compatibility

此版本只支援新版 PCIE S2MM driver，不再 backward compatible 舊版 driver。
新版 driver 必須支援：

- `IOCTL_GIGA_VIDEO_START`（function `0x830`）。
- `IOCTL_GIGA_VIDEO_STOP`（function `0x831`）。
- `IOCTL_GIGA_RELEASE_VIDEO_FRAME` 到 `IOCTL_GIGA_CLOSE_VIDEO`
  使用 function `0x832` 到 `0x837`。
- `IOCTL_PCIES2MM_GET_FRAME` 接受 `frameIndex = MAXULONG`（`0xFFFFFFFF`），
  由 driver 自行選擇已完成的 frame。
- `IOCTL_PCIES2MM_GET_AUDIO_FRAME`（function `0x809`）同樣接受
  `frameIndex = MAXULONG`。
- `IOCTL_GIGA_START_VIDEO_AUDIO`、`IOCTL_GIGA_STOP_VIDEO_AUDIO` 與
  `IOCTL_GIGA_GET_AUDIO_INFO`（function `0x839` 到 `0x83B`）。
- Extra video/audio frame event（event type `5`、`6`）表示 driver 仍有
  frame 待取；SDK 將它們當成額外的 ready notification。

SDK 不再呼叫 `IOCTL_PCIES2MM_GET_VIDEO_DONE_INDEX`，也不會在新 IOCTL
不支援時退回直接寫入 `VIDEO_DMA_EN_OFFSET`、`VIDEO_EN_OFFSET` 或
`IRQ_MASK_W1S_OFFSET`。若搭配舊版 driver，stream start 或 frame capture
可能失敗。

### Zero-copy selection

上層程式可在 `gvfg_create()` 後、`gvfg_open_channel()` 前選擇 zero-copy：

```c
gvfg_handle handle = NULL;
gvfg_create(&handle);
gvfg_set_channel_zero_copy_enabled(handle, GVFG_CHANNEL_0, 1);
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_0);
```

每個 channel 可在 open 前選擇要註冊／接收的事件：

```c
uint32_t events = GVFG_EVENT_MASK_SIGNAL_CONNECTED |
                  GVFG_EVENT_MASK_SIGNAL_DISCONNECTED |
                  GVFG_EVENT_MASK_FORMAT_CHANGE_BEGIN;
gvfg_set_channel_event_mask(handle, GVFG_CHANNEL_0, events);
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_0);
```

`VIDEO_DMA` 為 frame capture 必需，SDK 固定註冊。Plug-in、Unplug 與
Format-change driver event 則依 mask 註冊；關閉它們也會停用對應的自動訊號恢復。
Application 收到 `GVFG_EVENT_SIGNAL_DISCONNECTED` 後應暫停 video/audio read，
等待 `GVFG_EVENT_SIGNAL_CONNECTED` 或 stop 再喚醒 worker，不應在已知無訊號時持續
輪詢 timeout。

每個 channel 預設為 copy mode。指定 channel open 後不可切換該路模式；
另一條尚未 open 的 channel 仍可獨立選擇。Zero-copy mode 仍使用相同的 `gvfg_read_channel_frame()` /
`gvfg_release_channel_frame()` ownership contract，每次成功 read 都必須 release。
SDK 會在 open 時 enable zero-copy，並在 close/destroy 時 disable。

### Audio capture

Channel 預設只啟動 video。CH0 可在 start 前改成 video + audio：

```c
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_0);
gvfg_set_channel_audio_enabled(handle, GVFG_CHANNEL_0, 1);

gvfg_audio_format_t audio = {0};
gvfg_get_channel_audio_format(handle, GVFG_CHANNEL_0, &audio);
gvfg_start_channel(handle, GVFG_CHANNEL_0);

gvfg_audio_frame_t pcm = {0};
if (gvfg_read_channel_audio_frame(handle, GVFG_CHANNEL_0, &pcm, 1000) == GVFG_OK) {
    /* Consume pcm.data / pcm.data_size here. */
    gvfg_release_channel_audio_frame(handle, GVFG_CHANNEL_0, &pcm);
}
```

Video-only 使用 `IOCTL_GIGA_VIDEO_START/STOP`；video + audio 使用
`IOCTL_GIGA_START/STOP_VIDEO_AUDIO`。PCM 由 driver DMA buffer 複製到
SDK-owned frame buffer，再以和 video 相同的 read/release ownership 交付。
SDK 不建立 audio ring buffer。Audio zero-copy
尚未納入，CH1 audio 也尚未正式支援。

### 同一裝置雙 channel

同一個 `gvfg_handle` 可對同一個 device index 開啟 CH0、CH1。SDK 只建立一個
Windows device handle；兩個 channel 各自保有 event、單一 frame buffer
以及 held-frame/zero-copy ownership：

```c
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_0);
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_1);
gvfg_start_channel(handle, GVFG_CHANNEL_0);
gvfg_start_channel(handle, GVFG_CHANNEL_1);

gvfg_frame_t frame0 = {0};
if (gvfg_read_channel_frame(handle, GVFG_CHANNEL_0, &frame0, 1000) == GVFG_OK)
    gvfg_release_channel_frame(handle, GVFG_CHANNEL_0, &frame0);

gvfg_stop(handle); /* stop both channels */
```

兩個 channel 應由不同 worker thread 讀取。所有 stream、frame、event、signal 與
runtime API 都明確要求 `channel_index`；不再保留隱含 selected-channel 的舊 API。
目前硬體不允許 CH0、CH1 同時使用 Y210；SDK 會拒絕第二個 Y210 設定。

`gvfg_qt_preview.exe` 只使用 public API，主畫面只顯示 input 與 Preview
狀態；CH0、CH1 各自有 Start、Stop 與獨立 Preview 視窗，可單獨測試，
也可同時啟動。第二個 channel open/start 失敗時不會停止已運作的 channel。
關閉獨立 Preview 視窗只會停止該視窗的 frame submission；capture/audio 仍可繼續，
未顯示的 frame 不計為 preview delivery failure。重新開啟 Preview 或 Fullscreen 會
恢復 frame submission。
IRQ、DMA 等資訊只存在 internal diagnostic tool。

## Consumer Layout

一般 application 使用 core SDK 只需要：

```text
include/gvfg_capture.h
lib/gvfg.lib
bin/gvfg.dll
bin/giga_ioctl.dll
```

Application 不直接 link `giga_ioctl.lib`，但 `gvfg.dll` 會在 runtime 載入
`giga_ioctl.dll`，因此部署時兩個 DLL 必須一起提供。

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
