# GVFG Standalone SDK

這個 repository 將 GVFG SDK 與 Qt Sample 分成兩個獨立 CMake project。

這個 repo 以 GVFG 為 source of truth。其他專案應該透過 public header、
import library、runtime DLL 來使用 GVFG，不要直接把 GVFG source 編進去。

## Driver compatibility

此開發版本僅支援新 driver `vfg100_0821c`。舊版 driver、舊 IOCTL、
done-index 與舊 SDK frame-ring 流程不在相容範圍內。

## 內容

- `GVFG_SDK`：獨立 SDK project，使用內嵌的 `GvfgSdk.lib` 建立 `gvfg.dll` 與選用的 `gvfg_preview.dll`。
- `GVFG_Qt_Sample`：獨立 Qt APP project，只連結預先建好的 SDK，不會編譯 SDK source。
- `docs`：API 與整合說明。

Qt Sample 的 source boundary：

- `mainwindow.cpp`：Widget 建立、signal/slot、畫面狀態與使用者操作。
- `capture_controller.cpp/.h`：不依賴 UI widget 的裝置/channel lifecycle、video/audio worker、preview submission、狀態與 log 資料流。
- `capture_controller.cpp/.h` 直接示範公開的 GVFG C API，並明確管理 `gvfg_create()`／停止流程／`gvfg_destroy()`，不建立重複轉呼叫層。

客戶使用指南在 `docs/GVFG_CUSTOMER_API.md`，完整函式與結構參考在
`docs/GVFG_CUSTOMER_API_REFERENCE.md`。內部架構、package
切分、threading、frame ownership 說明在 `docs/GVFG_INTERNAL_NOTES.md`。
主管 library 尚缺少的 API 與目前暫時補法整理在 `docs/GIGABYTELIB_GAPS.md`。

## Build

先使用 MSVC 建立 SDK，再使用需要的 Qt kit 建立 Sample。兩個 project
使用不同 build directory，切換 Qt compiler 不會重新編譯 SDK source。
SDK build 會保留 MSVC `.lib`，並在找到 MinGW `dlltool` 時從固定的
DLL export 清單同步產生 MinGW `.dll.a`；兩種上層共用同一組 MSVC DLL。

範例：

```bat
cmake -S GVFG_SDK -B GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release
cmake --build GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release --config Release

cmake -S GVFG_Qt_Sample -B GVFG_Qt_Sample/build-release ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64 ^
  -DGVFG_SDK_RELEASE_DIR=%CD%\GVFG_SDK\build\Desktop_Qt_6_10_2_MSVC2022_64bit-Release
cmake --build GVFG_Qt_Sample/build-release --config Release
```

Qt Sample 只從一個固定的 `GVFG_SDK_RELEASE_DIR` 找 DLL 與對應 compiler
的 import library，不再依 Sample configuration 搜尋不同 SDK build。
它不會使用 `add_subdirectory()` 將 SDK source 編入 APP。

```text
GVFG_SDK_ROOT=<path-to-GVFG_SDK>
GVFG_SDK_RELEASE_DIR=<path-to-one-Release-SDK-build>
```

目前內部開發也統一使用上述 Release 流程，不另外維護 SDK/Sample 的
Debug 配對。內部 header、診斷程式碼與 source 仍保留在 repository；等正式
發布客戶套件時，再整理實際交付的 public header、LIB、DLL 與文件。

Build 產物：

```text
GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/bin/gvfg.dll
GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/bin/gvfg_preview.dll
GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/lib/gvfg.lib
GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/lib/gvfg_preview.lib
GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/lib/libgvfg.dll.a
GVFG_SDK/build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/lib/libgvfg_preview.dll.a
GVFG_Qt_Sample/build-release/bin/gvfg_qt_preview.exe
```

MSVC 上層連結 `gvfg.lib` 與 `gvfg_preview.lib`；MinGW 上層連結
`libgvfg.dll.a` 與 `libgvfg_preview.dll.a`。如果 CMake 找不到 `dlltool`，
可在 SDK configure 時指定 `-DGVFG_MINGW_DLLTOOL=<path-to-dlltool.exe>`。

## Driver compatibility

GVFG SDK 不再直接定義 driver IOCTL 或 register layout。底層 driver 相容性由
主管提供的 `GvfgSdk.lib` 負責，GVFG 只使用其 `Gvfg*` API。

### Zero-copy selection

上層程式可在 `gvfg_create()` 後、`gvfg_open_channel()` 前選擇 zero-copy：

```c
gvfg_handle handle = NULL;
gvfg_create(&handle);
gvfg_set_channel_zero_copy_enabled(handle, GVFG_CHANNEL_0, 1);
gvfg_open_channel(handle, device_index, GVFG_CHANNEL_0);
```

SDK 直接使用 GigabyteLib 建立的 video frame、format changed、input plug-in 與
input unplug events；audio 開啟時才使用 audio frame events。Application 收到
`GVFG_EVENT_VIDEO_INPUT_UNPLUG` 後應暫停 video/audio read，等待
`GVFG_EVENT_VIDEO_INPUT_PLUGIN` 或 stop 再喚醒 worker，不應在已知無訊號時持續
輪詢 timeout。

每次明確呼叫 `gvfg_start_channel()` 都會重新查詢一次 signal；沒有 lock 時回傳
`GVFG_ETIMEOUT`，channel 不會進入 running。Running 期間的 signal status 由 SDK
cache 提供，format changed、input plug-in 與 input unplug event 會先同步 cache，
再讓 Application poll 到對應事件，因此一般狀態顯示不會重複讀取硬體。

每個成功取得的 video/audio frame 都包含 `timestamp_ns`。兩者使用相同的
monotonic SDK delivery clock，可供 Application 做 soft A/V sync；它不是硬體
capture timestamp，也不能跨 process 或 session 比較。

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

Qt sample 的 monitoring playback 與 capture reader 分開。若 `QAudioSink` write
失敗或連續兩秒沒有寫入進度，只會清除過期的播放 queue 並在 audio thread 重新取得
預設 output device、重建 sink；video/audio capture 不會因此停止。Playback recovery
不影響 SDK frame release contract。

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
Sample 的 Start 會在需要時 open device；Stop 指定 channel 後，若另一個 channel 也未
running，Sample 會 destroy handle 並釋放 device，但保留 UI 的裝置選擇，供下次 Start
重新 open。這是 Sample lifecycle policy；公開 `gvfg_stop_channel()` 本身不會 destroy handle。
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
```

`GvfgSdk.lib` 已靜態連結進 `gvfg.dll`，Application 不需另外部署
`giga_ioctl.dll` 或主管 library 的額外 runtime DLL。

需要 optional display helper 時，再加：

```text
include/gvfg_preview.h
lib/gvfg_preview.lib
bin/gvfg_preview.dll
```

GPU snapshot/export conversion 已整合在 `gvfg.dll`，不需要額外 helper DLL。

Customer/demo package 不應包含 `gvfg_debug.h`、SDK source、GigabyteLib session
headers、IRQ details 或 helper source。這些只屬於 internal
debug/full application package。
