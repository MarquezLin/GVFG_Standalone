# GVFG 內部設計與維護說明

## GigabyteLib backend 邊界

`gvfg.dll` 保留公開 API facade、裝置列舉、雙 channel session、frame ownership 與
事件轉換。底層 capture lifecycle 與 video/audio frame access 全部透過主管提供的
`GvfgSdk.lib` API；zero-copy 的公開 release 只結束 SDK token lifetime，不再呼叫 Lib
release。舊 `giga_ioctl.dll` 與直接 register/IOCTL
backend 已移除。`GvfgSdk.lib` 靜態連結進 `gvfg.dll`，不是客戶 runtime 相依。

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
以及 `gvfg.dll`、`gvfg_preview.dll` 等 target 產物；不得把
`gvfg_debug.h`、GigabyteLib private header/lib 或 `sdk/gvfg/src` 加入 customer package。

## 2. 元件責任

```text
customer/sample
  -> gvfg_capture.h
  -> gvfg.dll facade (sdk/gvfg/src/gvfg_capture.cpp)
  -> GigabyteLib backend adapter
  -> GvfgSdk.lib

customer/sample (optional)
  -> gvfg_preview.h
  -> gvfg_preview.dll
  -> D3D preview pipeline
```

- `gvfg_capture.cpp`：公開 handle 狀態、狀態碼轉換、frame token 驗證、event
  queue、runtime counter 與 debug API facade。
- `gigabyte/gigabyte_capture_session.*`：把既有 GVFG lifecycle、event、
  copy/zero-copy ownership 契約轉接到 `GvfgSdk.lib`。
- `gigabyte/gigabyte_device.*`：SetupAPI 裝置列舉與 interface path。
- `third_party/GigabyteLib`：主管提供的靜態 library 與其 private headers，只供 SDK build。
- `src/gpu/*`：D3D11 同步轉換及 readback 到 caller buffer。

### 2.1 公開 API 對 GigabyteLib 的包裝

公開 API 不直接暴露 GigabyteLib handle、結構或 HRESULT。Facade 依功能分成以下三類：

| 公開 API／資料 | 底層來源 | SDK 提供給上層前的處理 |
|---|---|---|
| `gvfg_enumerate_devices()` | Windows SetupAPI | 轉成 UTF-8、提供 fallback name，並附加 `#1`、`#2` 顯示序號；不呼叫 GigabyteLib |
| `gvfg_create()`、`gvfg_destroy()` | SDK | 管理 opaque handle、雙 channel session、錯誤狀態與自動 stop/close |
| `gvfg_open_channel()` | SDK + GigabyteLib | 先建立 logical channel；真正的 `GvfgOpenDev()`、events 與 `GvfgOpenVideoChn()` 延遲到第一次需要 vendor channel 時執行 |
| zero-copy enable/getter | `GvfgOpenDev()` memory mode + SDK cache | 使用者只選模式；SDK 在 vendor open 時轉成 Lib memory-mode flags，getter 不詢問 Lib |
| audio enable | `GvfgCreateEvents()` | 使用者只選是否啟用 audio；SDK 轉成 Lib 的 no-audio event 建立參數 |
| `gvfg_set_channel_video_format()` | `GvfgSetVideoColorDepth()` | YUY2/Y210 轉成 8/10-bit color depth，成功後更新 video info |
| `gvfg_get_channel_signal_status()` | `GVFG_VIDEO_INFO` cache | `VideoSignalLock` 轉 connected、FourCC 轉 SDK pixel format，並由格式推導 bit depth；正常 UI refresh 只讀 cache |
| `gvfg_get_device_capabilities()` | `GvfgGetDevInfo()` | 只公開 video channel count 與 audio capability |
| `gvfg_get_channel_sdi_info()` | `GvfgGetSdiVideoInputInfo()`、`GvfgStringifySdiVideoInputInfo()` | 同時提供 Lib 原始欄位的 SDK enum/value 與全部八個未改寫的 Lib 格式化字串，不公開 private struct |
| `gvfg_get_channel_audio_format()` | `GvfgGetAudioInfo()` | 只公開 sample rate、channels、bits per sample；`cbBufSize` 留在 SDK 作為讀取 buffer 大小，忽略 Lib FrameCount |
| `gvfg_start_channel()`、stop APIs | `GvfgStartCapture()`、`GvfgStopCapture()` | 驗證狀態、處理 wait cancellation、held frame、event queue、熱插拔恢復與 SDK counters |
| `gvfg_read_channel_frame()` | Lib video events + GetFrame API | 驗證 read/held 狀態，加入 row stride、SDK delivery frame ID、monotonic timestamp 與 delivery FPS 統計 |
| `gvfg_release_channel_frame()` | SDK | 以 `data + frame_id` 確認 held frame 並結束 pointer lifetime；zero-copy 不呼叫 Lib release |
| `gvfg_read_channel_audio_frame()` | Lib audio events + `GvfgGetAudioFrame()` | SDK 配置 copy buffer，加入格式、SDK delivery frame ID 與 monotonic timestamp |
| `gvfg_release_channel_audio_frame()` | SDK | 以 `data + frame_id` 確認 held audio frame 並解除 SDK copy buffer ownership，不呼叫 Lib release |
| `gvfg_poll_channel_event()` | Lib event handles | Callback 轉成容量 64 的 thread-safe SDK queue，補上 non-blocking、timeout 與 stop wake semantics |
| `gvfg_get_channel_runtime_info()` | SDK | FPS 與 delivered frame count 都在成功交付 frame 的 SDK 邊界計算，不是 driver/Lib counter |
| GPU conversion APIs | SDK D3D11 | 驗證 layout/size，再轉成 BGRA8、RGB10A2 或 NV12；GigabyteLib 不參與 |
| version/name/error APIs | SDK | SDK version、公開名稱與錯誤文字；Lib `GVFG_HRESULT` 正數原樣回傳，不產生 SDK detail |
| register R/W debug APIs | direct driver extension | 不屬於 GigabyteLib 正式 API，只能留在 internal debug surface |

資料來源必須對上層說清楚：video/audio `frame_id`、`timestamp_ns`、runtime FPS 與
delivered frame count 都由 SDK 產生；width、height、buffer size、signal、audio format
與 SDI info 來自 GigabyteLib。SDK 不應把 delivery counter 描述成 driver/FPGA counter。

## 3. 公開 facade 狀態

概念狀態：

```text
Created/Closed -> Opened -> Running -> Opened -> Destroyed
                    ^          |
                    +----------+ stop
```

- `open()` 會先 close 舊 backend、建立 logical session 並設定 channel；此時尚未必呼叫
  `GvfgOpenDev()`。Vendor channel 與 event handles 在第一次 set-format/query/start 需要時建立。
- `start()` 先執行一次新的 `querySignal()`；沒有 lock 時回 `GVFG_ETIMEOUT`，不進入 running。
  Signal 有效時才以該 descriptor 執行 `configureStream()`、清空 facade event queue 並啟動 backend。
- `stop()` 先清除 running、釋放 facade held frame、停止 backend，最後清空 event queue
  並喚醒 event poll。
- `destroy()` 允許 NULL，並透過 destructor/close 保證 stop。

每個 `gvfg_channel_session_t` 各有自己的 `readInProgress`、`frameHeld` 與
`heldSessionFrame`；同一 handle 的 CH0、CH1 可由兩條 worker thread 分別 read。
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
-> SDK 清除 held token；不送 Lib/driver release
-> close/destroy 時 IOCTL_GIGA_DISABLE_FRAME_ZEROCOPY(channel)
```

Zero-copy 同時只允許一張 outstanding frame。Caller 必須 release 目前的
driver pointer，才能再次 read。SDK 不根據 read 間隔推算 frame loss；driver 目前也
沒有提供可用來確認實際遺失 frame 的 sequence/drop counter。

## 5. Frame token 與 ABI

Facade 在 release 時驗證原 token 的 data、size、width、height、stride、format、
bit depth 與 frame ID。任何欄位遭修改都回傳 `GVFG_EINVAL`。

x64 `gvfg_frame_t` ABI 已在 `gvfg_capture.cpp` 以 static assertions 固定為 56 bytes
及明確欄位 offsets。修改公開 struct 時必須視為 ABI 變更，不能只重新編譯 DLL。
新增 metadata 優先考慮新 query API 或帶 size/version 的新 struct。

## GigabyteLib requirement

Capture、audio、event 與 frame ownership 的 driver ABI 由主管提供的
`GvfgSdk.lib` 負責。Output-format selection 已改用 library 1.0.2 的
`GvfgSetVideoColorDepth()`；目前唯一例外是 diagnostic register R/W，集中在
`gigabyte_driver_extensions.*`，不得擴大成第二套 capture backend。待主管補上正式
register API 後應刪除此 extension。

目前原生格式：

- YUY2：2 bytes/pixel，8-bit packed 4:2:2，byte order 為 Y0 U0 Y1 V0。
- Y210：4 bytes/pixel，10-bit packed 4:2:2。

目前皆為 single-plane。加入 multi-plane 格式前，必須先設計 plane count、offset、
stride、buffer lifetime 與向後相容 API，不能直接重新解釋既有 `data`。

## 6. Event 流程

Backend event 映射：

| Backend | Public |
|---|---|
| `hVideoFormatChangedEvent` | `GVFG_EVENT_VIDEO_FORMAT_CHANGED` |
| `hVideoInputPluginEvent` | `GVFG_EVENT_VIDEO_INPUT_PLUGIN` |
| `hVideoInputUnplugEvent` | `GVFG_EVENT_VIDEO_INPUT_UNPLUG` |

依 GigabyteLib 行為，video、format-change、plug-in 與 unplug events 在 open channel
時固定註冊；使用者只選擇是否啟用 audio，SDK 不再提供重複的 event mask 層。

Facade queue 上限為 64；滿時丟棄最舊事件。`pollEvent()` 只允許 running 狀態，支援
non-blocking、有限 timeout 與 infinite wait；stop 透過 `eventCv` 喚醒 waiter。

目前 backend 在 format-changed／plug-in event 後更新 cached video info；unplug 則先清空
cached signal。Facade callback 會先將 backend cache 同步到 channel cache，再把 public event
放入 queue。Running 期間的 plug-in event 會喚醒 read；SDK event worker
等待 held video frame release，並以同一把 Lib capture lock 避開進行中的 video/audio frame call，
再執行一次 `GvfgStopCapture()` → `GvfgStartCapture()`。Application 只處理事件顯示，不控制重啟；
此恢復只由 plug-in event 驅動，不 polling。

更動 driver event mapping 或 recovery 流程時，要同時驗證無訊號 Start 回傳、拔插、解析度
切換及 stop-during-wait，並確認 consumer 收到事件時對應 cache 已經完成更新。

## 7. Internal debug API

`gvfg_debug.h` 僅供內部工具，包含：

- `gvfg_debug_get_channel_backend_stats()`：指定 channel 的 running/frame-held 狀態、
  event queue depth、frame wait timeout、video/audio event wake、audio 讀取量與
  GigabyteLib GetFrame/acquire timing。

公開的 `gvfg_runtime_info_t` 只保留 application-facing capture FPS 與 delivered frame
數；zero-copy 用公開 getter 查詢。Debug 結構不重複這些公開資訊，也不偽造
GigabyteLib 未提供的 DMA error、IRQ 或 driver sequence counter。

客戶診斷使用 `gvfg_get_channel_signal_status()`、`gvfg_get_channel_runtime_info()`、event、
`gvfg_strerror()` 與 `gvfg_get_channel_last_sdk_error_detail()`。每個 channel 的 `ChannelErrorState`
由 `gvfg_handle_t` 持有，只記錄 SDK 自己產生的負數錯誤；正數 Lib result 不會改寫它。

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

## 11. 完成項目與後續維護

### 本次已完成

1. **Public event ABI 已同步。** 公開 enum、Qt Sample、SDI Info 更新條件與文件使用
   `GVFG_EVENT_VIDEO_FORMAT_CHANGED`、`GVFG_EVENT_VIDEO_INPUT_PLUGIN`、
   `GVFG_EVENT_VIDEO_INPUT_UNPLUG`，不得再引入另一套合成事件名稱。
2. **Zero-copy release 契約已套用。** 正常 release 與 stop-with-held-frame 都只清除
   SDK held state，不呼叫 `GvfgReleaseVideoFrameZeroCopy()`；caller 仍須呼叫公開
   release，作為 pointer lifetime 的結束邊界。

### Lib query 維護規則

1. **VideoInfo query 已快取。** `GvfgSetVideoColorDepth()` 只使 cache 失效，不立即查詢；每次明確
   Start 由 `configureStream()` 前執行一次新的 signal query。Running 期間 status getter 只讀 cache；
   format/plugin/unplug event 先同步 backend 與 facade cache，再通知 consumer。
2. **AudioInfo query 已快取。** 第一次需要 audio format 時更新；Sample getter 與 start
   共用同一份 cache，Stop/Start 不重查。
3. **Sample 的 SDI Info query 以 Start/event 為邊界。** Start 成功後讀一次；之後只在 video
   format changed、input plug-in 或 input unplug event 更新。200 ms UI timer 與一般 status
   render 不得重查；新值與 cache 相同時不重設 UI。

### 後續邊界維護

1. 決定 SetupAPI enumerate 加 `#N` 是否為長期公開顯示契約；若只是 Sample 顯示需求，
   應避免讓排序序號被誤認為硬體穩定 ID。
2. register R/W 持續標記為 GigabyteLib gap 與 internal-only；主管提供正式 Lib API 後
   移除 `gigabyte_driver_extensions.*`，不得讓 direct IOCTL 擴張回第二套 backend。
3. 每次改公開 struct、event enum 或 frame lifetime，都必須同步
   `gvfg_capture.h`、兩份 customer docs、internal notes、流程圖與 sample，再重新驗證
   x64 ABI、DLL exports 及乾淨 install tree build。
