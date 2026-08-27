# GVFG 內部設計與維護說明

## Driver IOCTL DLL 邊界

`gvfg.dll` 保留裝置列舉、`CreateFile`/`CloseHandle`、雙 channel session、event、
event thread 與每 channel 的單一 frame buffer。Driver IOCTL code、request layout 及所有
`DeviceIoControl` 呼叫集中在獨立維護的內部 `giga_ioctl.dll`；GVFG 僅呼叫其具名 C API，
失敗時沿用 `GetLastError()`。此 DLL 只負責 driver IOCTL code、request layout 與薄封裝，
不持有 capture state。`giga_ioctl.h` 是內部相依，不屬於客戶公開 API。

本文件只供 GVFG SDK、driver、FPGA 與內部診斷工具維護者使用。客戶行為與公開
契約請以 `GVFG_CUSTOMER_API.md`、`GVFG_CUSTOMER_API_REFERENCE.md` 和
`gvfg_capture.h` 為準。本文件中的 IOCTL、
register、ring、counter 與執行緒模型都不是公開 ABI。

## 1. 客戶／內部邊界

| 類別 | 客戶套件 | 僅內部 |
|---|---|---|
| Header | `gvfg_capture.h`、選用 `gvfg_preview.h` | `gvfg_debug.h`、backend headers |
| Binary | `gvfg.dll/.lib`、選用 preview DLL/lib | driver/FPGA 診斷工具 |
| 文件 | `GVFG_CUSTOMER_API.md`、`GVFG_CUSTOMER_API_REFERENCE.md` | 本文件 |
| 資訊 | lifecycle、frame、event、signal、runtime FPS | IOCTL、IRQ、register、DMA/ring、debug counters |

CMake install 會安裝公開的 `gvfg_capture.h`、選用的 `gvfg_preview.h`、客戶文件，
以及 `gvfg.dll`、`giga_ioctl.dll`、`gvfg_preview.dll` 等 target 產物；不得把
`gvfg_debug.h`、private `giga_ioctl.h` 或 `sdk/gvfg/src` 加入 customer package。

## 2. 元件責任

```text
customer/sample
  -> gvfg_capture.h
  -> gvfg.dll facade (sdk/gvfg/src/gvfg_capture.cpp)
  -> PcieS2mmCaptureSession
  -> Windows device / IOCTL / FPGA registers / DMA

customer/sample (optional)
  -> gvfg_preview.h
  -> gvfg_preview.dll
  -> D3D preview pipeline
```

- `gvfg_capture.cpp`：公開 handle 狀態、狀態碼轉換、frame token 驗證、event
  queue、runtime counter 與 debug API facade。
- `pcies2mm_capture_session.*`：每 channel 的 stream lifecycle、driver event、DMA
  wait/read、單一 outstanding frame 與 backend statistics；不包含 SDK frame ring。
- `pcies2mm_device.*`：SetupAPI 裝置列舉與 interface path。
- `sdk/giga_ioctl`：獨立 DLL；集中管理與 driver 共用的 private ABI 與 `DeviceIoControl` 薄封裝。
- `pcies2mm_reg.h`：FPGA register offsets/masks。
- `pcies2mm_video_format.*`：format register 解碼與 YUY2/Y210 layout；相容舊 FPGA 回報的 YVYU register 值。
- `src/gpu/*`：D3D11 同步轉換及 readback 到 caller buffer。

## 3. 公開 facade 狀態

概念狀態：

```text
Created/Closed -> Opened -> Running -> Opened -> Destroyed
                    ^          |
                    +----------+ stop
```

- `open()` 會先 close 舊 backend、建立 session、設定 channel 並建立 event monitoring；
  signal descriptor 在 start/configure 階段重新查詢。
- `start()` 先 `configureStream()`，清空 facade event queue，再啟動 backend。
- 無訊號時使用最小 placeholder descriptor 進入 event-monitoring mode；訊號恢復後
  backend 依真實 descriptor 啟用 DMA。
- `stop()` 先清除 running、釋放 facade held frame、停止 backend，最後清空 event queue
  並喚醒 event poll。
- `destroy()` 允許 NULL，並透過 destructor/close 保證 stop。

每個 `gvfg_channel_session_t` 各有自己的 `readInProgress`、`frameHeld` 與
`heldBackendFrame`；同一 handle 的 CH0、CH1 可由兩條 worker thread 分別 read。
Open/start/stop/destroy 等 handle lifecycle 仍應由 caller 序列化；若日後要宣告完整
thread-safe，必須先補足這些操作彼此的同步與 handle lifetime 保護。

## 4. DMA 與單一 frame buffer

Copy mode 資料路徑：

```text
source frame
-> hardware/driver DMA buffer
-> DMA complete event
-> 呼叫 gvfg_read_channel_frame() 的 thread 等待 DMA complete event
-> IOCTL_GET_FRAME(frameIndex=0xFFFFFFFF) 直接複製到 SDK 的單一 buffer
-> facade 回傳指向該 buffer 的 gvfg_frame_t
-> customer/preview/conversion
-> gvfg_release_channel_frame(channel)
-> buffer 可供下一次 read 使用
```

Backend 不做 frame queue、slot 切換或 done-index 查詢。每個 channel 同一時間只允許
一個 read，且成功取得的 frame 必須 release 後才能讀下一張。SDK 不把 driver DMA
buffer 數或 done-index 演算法暴露為客戶契約。

Zero-copy mode 資料路徑：

```text
gvfg_create
-> gvfg_set_channel_zero_copy_enabled(channel, 1)
-> open channel 時 IOCTL_GIGA_ENABLE_FRAME_ZEROCOPY(channel)
-> DMA complete event
-> IOCTL_GIGA_ACQUIRE_VIDEO_FRAME_ZEROCOPY(channel, 0xFFFFFFFF)
-> facade 回傳 driver-owned pointer
-> customer/preview/conversion
-> gvfg_release_channel_frame(channel)
-> IOCTL_GIGA_RELEASE_VIDEO_FRAME(channel)
-> close/destroy 時 IOCTL_GIGA_DISABLE_FRAME_ZEROCOPY(channel)
```

Zero-copy 同時只允許一張 outstanding frame。Caller 必須 release 目前的
driver pointer，才能再次 read。SDK 不根據 read 間隔推算 frame loss；driver 目前也
沒有提供可用來確認實際遺失 frame 的 sequence/drop counter。

## 5. Frame token 與 ABI

Facade 在 release 時驗證原 token 的 data、size、width、height、stride、format、
bit depth 與 frame ID。任何欄位遭修改都回傳 `GVFG_EINVAL`。

x64 `gvfg_frame_t` ABI 已在 `gvfg_capture.cpp` 以 static assertions 固定為 48 bytes
及明確欄位 offsets。修改公開 struct 時必須視為 ABI 變更，不能只重新編譯 DLL。
新增 metadata 優先考慮新 query API 或帶 size/version 的新 struct。

## Driver ABI requirement

目前 SDK 只支援新版 PCIE S2MM driver，必要 private ABI 包含：

- `IOCTL_GIGA_VIDEO_START` (`0x830`) / `IOCTL_GIGA_VIDEO_STOP` (`0x831`)。
- `IOCTL_GIGA_RELEASE_VIDEO_FRAME` (`0x832`)。
- `IOCTL_GIGA_ACQUIRE_VIDEO_FRAME_ZEROCOPY` (`0x833`)。
- `IOCTL_GIGA_ENABLE_FRAME_ZEROCOPY` (`0x834`) /
  `IOCTL_GIGA_DISABLE_FRAME_ZEROCOPY` (`0x835`)。
- Copy mode 的 `IOCTL_PCIES2MM_GET_FRAME` 接受 `frameIndex=0xFFFFFFFF`。

不再呼叫 `GET_VIDEO_DONE_INDEX`，也不再 fallback 直接寫入 video enable、DMA enable
或 IRQ mask registers。舊 driver 不屬於此 revision 的支援範圍。

目前原生格式：

- YUY2：2 bytes/pixel，8-bit packed 4:2:2，byte order 為 Y0 U0 Y1 V0。
- Y210：4 bytes/pixel，10-bit packed 4:2:2。

目前皆為 single-plane。加入 multi-plane 格式前，必須先設計 plane count、offset、
stride、buffer lifetime 與向後相容 API，不能直接重新解釋既有 `data`。

## 6. Event 流程

Backend event 映射：

| Backend | Public |
|---|---|
| `PCIES2MM_EVENT_PLUG_IN` | `GVFG_EVENT_SIGNAL_CONNECTED` |
| `PCIES2MM_EVENT_PLUG_OUT` | `GVFG_EVENT_SIGNAL_DISCONNECTED` |
| `PCIES2MM_EVENT_STREAM_READY` | `GVFG_EVENT_STREAM_READY` |
| `PCIES2MM_EVENT_FORMAT_CHANGE_BEGIN` | `GVFG_EVENT_FORMAT_CHANGE_BEGIN` |

每個 channel 的 public event mask 在 open 前設定。Video DMA event 永遠註冊；
format-change、plug-in、unplug driver event 依 mask 選擇性註冊。關閉 driver event
同時代表 backend 不執行該事件所驅動的自動 recovery。

Register read 必須排除純 write-only 位址：global `0x080`，以及每個 video/audio
channel 的 descriptor-write pulse 與 DMA soft-reset register。Interrupt
`0x000/0x004/0x008` 是 read-status/write-control 雙語意位址，讀取其 RO status 合法。

Facade queue 上限為 64；滿時丟棄最舊事件。`pollEvent()` 只允許 running 狀態，支援
non-blocking、有限 timeout 與 infinite wait；stop 透過 `eventCv` 喚醒 waiter。

事件順序的維護目標：format change begin 後停止使用舊 descriptor，完成重新配置且
第一個完整 frame 可用時才送 stream ready。更動 driver event mapping 或 recovery
流程時，要同時驗證無訊號啟動、拔插、解析度切換及 stop-during-wait。

## 7. Internal debug API

`gvfg_debug.h` 僅供內部工具，包含：

- `gvfg_debug_get_channel_backend_stats()`：指定 channel 的 facade/backend state、
  captured/delivered frame、DMA error、IRQ、timeout、sequence 與 GetFrame timing。
- `gvfg_debug_read_register()`：讀取 4-byte aligned BAR-relative register。
- `gvfg_debug_write_register()`：寫入 register。

Register write 可能中斷 DMA、interrupt 或 capture。工具必須確認裝置、offset 與當前
stream 狀態，且不得將 register API 包裝成客戶功能。

客戶診斷使用 `gvfg_get_channel_signal_status()`、`gvfg_get_channel_runtime_info()`、event、
`gvfg_strerror()` 與 `gvfg_get_channel_last_error_detail()`。每個 channel 的 `ChannelErrorState`
由 `gvfg_handle_t` 持有，facade 與 backend 共用同一份；因此 channel open 失敗後也能取得詳細錯誤。

## 8. GPU conversion

公開 GPU conversion 是同步的：建立 D3D pipeline、轉換／readback 到 caller-owned
buffer，返回後不保留指標。輸入只接受目前公開的 YUY2/Y210；輸出為 BGRA8、
RGB10A2 或 BT.709 limited-range NV12。

維護時必測：

- 奇偶尺寸與 overflow 檢查。
- source stride/data size 與 destination row/data size。
- NV12 偶數 width/height、Y/UV plane offset。
- D3D device/resource 建立失敗的狀態碼與資源釋放。
- 在 release 前轉換，及 stop/format change 時沒有持有失效 frame。

## 9. Preview helper

Preview helper 是獨立 DLL，不連結 core capture SDK。它只接受 caller 填入的
`gvfg_preview_frame_t`，同步 render 到 HWND。Sample 可同時使用 capture、preview 與
internal debug header，但客戶 sample/package 不應包含 internal diagnostics。

`present_fps` 只統計 DXGI 成功接受的 Present；non-blocking swapchain busy 的 frame
計入 `skipped_presents`，不可將其解讀為 capture drop。

## 10. 已知限制與發佈檢查表

目前設計限制：Windows only、每 handle 可開同一 device 的兩個 channel、每 channel 最多一個 held
frame、原生格式限 YUY2/Y210、event queue 非持久化且可能淘汰最舊事件。

每次發佈前確認：

1. `gvfg_capture.h` 的所有 symbol 都有 export、實作及客戶文件。
2. 公開 header 不 include private backend/Windows driver header。
3. install/package 不含 `gvfg_debug.h`、backend source、IOCTL/register 文件。
4. 測試 enumerate、CH0/CH1、無訊號 start、plug in/out、format change、stop/destroy。
5. 測試 timeout 0、有限 timeout、infinite wait 被 stop 喚醒。
6. 測試 read/release token、重複 read、錯誤 token、stop with held frame。
7. 測試所有 GPU output 與 buffer size 邊界。
8. 驗證 x64 ABI static assertions、DLL exports、import library 與 customer sample。
9. 用乾淨 install tree 編譯 customer sample，避免意外依賴 source tree。
10. 確認客戶文件沒有 register、IOCTL、IRQ、ring slot 或未承諾的規格。
