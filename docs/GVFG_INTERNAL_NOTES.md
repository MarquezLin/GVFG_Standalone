# GVFG 內部設計與維護說明

## Driver IOCTL DLL 邊界

`gvfg.dll` 保留裝置列舉、`CreateFile`/`CloseHandle`、channel session、event、
thread 與 frame ring。Driver IOCTL code、request layout 及所有
`DeviceIoControl` 呼叫集中在內部 `giga_ioctl.dll`；GVFG 僅呼叫其具名 C API，
失敗時沿用 `GetLastError()`。`giga_ioctl.h` 是內部相依，不屬於客戶公開 API。

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

CMake install 已只安裝 `gvfg_capture.h` 與客戶文件；不得把 `gvfg_debug.h` 或
`sdk/gvfg/src` 加入 customer package。

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
- `pcies2mm_capture_session.*`：stream lifecycle、driver event/DMA worker、frame
  ring 與 backend statistics。
- `pcies2mm_device.*`：SetupAPI 裝置列舉與 interface path。
- `pcies2mm_ioctl.h`：與 driver 共用的 private ABI。
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

- `open()` 會先 close 舊 backend、建立 session、設定 channel、同步 signal。
- `start()` 先 `configureStream()`，清空 facade event queue，再啟動 backend。
- 無訊號時使用最小 placeholder descriptor 進入 event-monitoring mode；訊號恢復後
  backend 依真實 descriptor 啟用 DMA。
- `stop()` 先清除 running、喚醒 event poll、釋放 held frame，再停止 backend。
- `destroy()` 允許 NULL，並透過 destructor/close 保證 stop。

每個 handle 只有一個 `readInProgress` 與一個 `frameHeld`。公開文件要求 lifecycle
由 caller 序列化；若日後要宣告完整 thread-safe，必須先補足 open/start/stop/destroy
彼此的同步與 handle lifetime 保護。

## 4. DMA 與 frame ring

Copy mode 資料路徑：

```text
source frame
-> hardware/driver DMA buffer
-> DMA complete event
-> capture worker 收到 DMA complete event
-> IOCTL_GET_FRAME(frameIndex=0xFFFFFFFF) 複製到 SDK ring free slot
-> slot ready + sequence 更新 + frame_cv notify
-> wait_frame 選取 ready frame
-> facade 回傳指向 slot.data 的 gvfg_frame_t
-> customer/preview/conversion
-> release_frame
-> slot 回到 free
```

Backend stream descriptor 目前配置 3 個 SDK slots。Slot 邏輯狀態：

```text
Free      ready=false, in_use=false
Writing   ready=false, in_use=true
Ready     ready=true,  in_use=false
Delivered ready=false, in_use=true
```

ring 滿時的行為是即時擷取的 drop/backpressure policy，不是 lossless queue；應透過
internal stats 觀察 `frames_dropped`。不要將 slot 數、driver DMA buffer 數或 done-index
演算法暴露為客戶契約。

Zero-copy mode 資料路徑：

```text
gvfg_create
-> gvfg_set_zero_copy_enabled(1)
-> open channel 時 IOCTL_GIGA_ENABLE_FRAME_ZEROCOPY(channel)
-> DMA complete event
-> IOCTL_GIGA_ACQUIRE_VIDEO_FRAME_ZEROCOPY(channel, 0xFFFFFFFF)
-> facade 回傳 driver-owned pointer
-> customer/preview/conversion
-> gvfg_release_frame
-> IOCTL_GIGA_RELEASE_VIDEO_FRAME(channel)
-> close/destroy 時 IOCTL_GIGA_DISABLE_FRAME_ZEROCOPY(channel)
```

Zero-copy 同時只允許一張 outstanding frame。若下一個 DMA event 到達時 caller 尚未
release，backend 只會跳過該次 acquire，不會覆寫仍由 caller 使用的 driver pointer。
這種情況不計入 `frames_dropped`，也不送出 `GVFG_EVENT_FRAME_LOSS`；driver 目前沒有
提供可用來確認實際遺失 frame 的 sequence/drop counter。

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

- `gvfg_debug_get_channel_backend_stats()`：指定 channel 的 facade/backend state、frame/drop/DMA/IRQ/timeout、
  queue/ring/sequence counters。
- `gvfg_debug_get_channel_last_error_detail()`：複製指定 channel 的 UTF-8 backend 詳細錯誤。
- `gvfg_debug_read_register()`：讀取 4-byte aligned BAR-relative register。
- `gvfg_debug_write_register()`：寫入 register。

Register write 可能中斷 DMA、interrupt 或 capture。工具必須確認裝置、offset 與當前
stream 狀態，且不得將 register API 包裝成客戶功能。

客戶診斷只應使用 `gvfg_get_channel_signal_status()`、`gvfg_get_channel_runtime_info()`、event 與
`gvfg_strerror()`。

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
