# GVFG 客戶 API Reference

本文件是 GVFG 客戶公開 API 的逐項參考。快速使用流程、執行緒、frame ownership、
錯誤處理與範例，請先閱讀 [`GVFG_CUSTOMER_API.md`](GVFG_CUSTOMER_API.md)。

客戶核心 API 以 `sdk/gvfg/include/gvfg_capture.h` 為準；選用 preview API 以
`helpers/gvfg_preview/include/gvfg_preview.h` 為準。內部 debug、driver、IOCTL、
register 與 DMA 實作不屬於本文件。

## 1. Core API 完整參考

本節逐一說明 `gvfg_capture.h` 的所有公開常數、型別、結構與函式。除非特別註明，
所有輸出結構都建議先用 `{0}` 初始化。

### 1.1 公開常數

| 常數                      | 值            | 說明                            |
| ----------------------- | ------------:| ----------------------------- |
| `GVFG_MAX_DEVICES`      | 16           | 一次最多列舉的裝置數                    |
| `GVFG_TIMEOUT_INFINITE` | `UINT32_MAX` | 無限等待；`timeout_ms == 0` 則完全不等待 |

### 1.2 `gvfg_status_t`

所有回傳 `gvfg_status_t` 的函式都使用以下狀態碼：

| 成員              | 值   | 說明                                             |
| --------------- | ---:| ---------------------------------------------- |
| `GVFG_OK`       | 0   | 成功                                             |
| `GVFG_EINVAL`   | -1  | NULL pointer、無效 channel、buffer 大小或 token 等參數錯誤 |
| `GVFG_ENODEV`   | -2  | 找不到裝置或裝置無法開啟                                   |
| `GVFG_ESTATE`   | -3  | 呼叫順序或目前 session 狀態不允許此操作                       |
| `GVFG_EIO`      | -4  | Driver、backend 或 GPU I/O 失敗                    |
| `GVFG_ENOTSUP`  | -5  | 不支援指定功能或格式                                     |
| `GVFG_ETIMEOUT` | -6  | 在期限內等不到 frame、event 或 backend 工作               |

### 1.3 `gvfg_pixel_format_t`

| 成員                    | 值   | 說明                                        |
| --------------------- | ---:| ----------------------------------------- |
| `GVFG_PIXFMT_UNKNOWN` | 0   | 未知或尚無有效訊號格式                               |
| `GVFG_PIXFMT_Y210`    | 2   | 10-bit packed YUV 4:2:2；目前每 pixel 4 bytes |
| `GVFG_PIXFMT_YVYU`    | 3   | 8-bit packed YUV 4:2:2；目前每 pixel 2 bytes  |

### 1.4 `gvfg_channel_t`

| 成員               | 值   | 說明       |
| ---------------- | ---:| -------- |
| `GVFG_CHANNEL_0` | 0   | 裝置輸入通道 0 |
| `GVFG_CHANNEL_1` | 1   | 裝置輸入通道 1 |

### 1.5 `gvfg_device_info_t`

由 `gvfg_enumerate_devices()` 填入。

| 欄位      | 型別          | 說明                                      |
| ------- | ----------- | --------------------------------------- |
| `index` | `int`       | 傳給 `gvfg_open_channel()` 的裝置索引          |
| `name`  | `char[128]` | 供 UI／log 顯示的 UTF-8、null-terminated 裝置名稱 |

### 1.6 `gvfg_signal_status_t`

由 `gvfg_get_signal_status()` 填入。

| 欄位             | 型別    | 說明                                         |
| -------------- | ----- | ------------------------------------------ |
| `connected`    | `int` | 非 0 表示 selected channel 目前有有效輸入訊號          |
| `channel`      | `int` | 目前選擇的 `gvfg_channel_t` 值                   |
| `width`        | `int` | 連線時的輸入寬度；未連線時為 0                           |
| `height`       | `int` | 連線時的輸入高度；未連線時為 0                           |
| `pixel_format` | `int` | 實際 frame payload 的 `gvfg_pixel_format_t` 值 |
| `bit_depth`    | `int` | 從 payload 格式取得的每色彩 channel bit depth       |

### 1.7 `gvfg_runtime_info_t`

由 `gvfg_get_runtime_info()` 填入。統計在 `gvfg_start()` 時重設。

| 欄位                 | 型別         | 說明                                                      |
| ------------------ | ---------- | ------------------------------------------------------- |
| `capture_fps`      | `double`   | 依 `gvfg_read_frame()` 成功交付時間估算並平滑化的 FPS；尚無足夠 frame 時為 0 |
| `delivered_frames` | `uint64_t` | 此次 running session 中成功交付給 caller 的 frame 數              |
| `lost_frames`      | `uint64_t` | SDK 能確定在交付前遺失的 frame 數；目前包含 capture FIFO 覆蓋       |

### 1.8 `gvfg_frame_t`

由 `gvfg_read_frame()` 填入，也是稍後傳給 `gvfg_release_frame()` 的完整 token。

| 欄位                 | 型別             | 說明                                           |
| ------------------ | -------------- | -------------------------------------------- |
| `data`             | `const void *` | SDK-owned 原生 frame buffer；release 或 stop 後失效 |
| `data_size`        | `uint64_t`     | `data` 指向的總 byte 數                           |
| `width`            | `int`          | frame 寬度，單位 pixel                            |
| `height`           | `int`          | frame 高度，單位 pixel                            |
| `row_stride_bytes` | `int`          | 相鄰兩列起點間距，單位 byte；處理每列時必須使用此值                 |
| `pixel_format`     | `int`          | `gvfg_pixel_format_t` 值                      |
| `bit_depth`        | `int`          | 原生 frame 每色彩 channel 的 bit depth             |
| `frame_id`         | `uint64_t`     | 此次 start/stop run 中單調遞增的 frame ID            |

Caller 不得修改任何欄位再 release。SDK 會將完整 token 與目前 held frame 比對。

### 1.9 `gvfg_gpu_output_format_t`

| 成員                        | 值   | 說明                                                         |
| ------------------------- | ---:| ---------------------------------------------------------- |
| `GVFG_GPU_OUTPUT_BGRA8`   | 1   | DXGI `B8G8R8A8_UNORM` byte layout，alpha 為 255              |
| `GVFG_GPU_OUTPUT_RGB10A2` | 2   | DXGI `R10G10B10A2_UNORM` packed `uint32_t`，alpha 為 3       |
| `GVFG_GPU_OUTPUT_NV12`    | 3   | 8-bit BT.709 limited-range 4:2:0；Y plane 後接 interleaved UV |

### 1.10 `gvfg_gpu_output_buffer_t`

由 caller 建立並傳給 `gvfg_gpu_convert_to_buffer()`。

| 欄位             | 型別         | 說明                                              |
| -------------- | ---------- | ----------------------------------------------- |
| `data`         | `void *`   | Caller-owned destination buffer，不可為 NULL        |
| `data_size`    | `uint64_t` | Destination buffer 可用 byte 數                    |
| `row_bytes`    | `int`      | Destination stride；NV12 的 Y/UV plane 共用此 stride |
| `pixel_format` | `int`      | 要求的 `gvfg_gpu_output_format_t` 值                |

### 1.11 `gvfg_event_type_t`

| 成員                               | 值   | 說明                            |
| -------------------------------- | ---:| ----------------------------- |
| `GVFG_EVENT_UNKNOWN`             | 0   | 未知事件；正常流程不應依賴此值               |
| `GVFG_EVENT_SIGNAL_CONNECTED`    | 1   | Selected channel 偵測到輸入訊號      |
| `GVFG_EVENT_SIGNAL_DISCONNECTED` | 2   | 輸入訊號中斷                        |
| `GVFG_EVENT_STREAM_READY`        | 3   | 啟動、重新接線或格式恢復後，第一個完整 frame 已就緒 |
| `GVFG_EVENT_FORMAT_CHANGE_BEGIN` | 4   | 輸入格式正在改變，應暫停使用舊格式資源           |
| `GVFG_EVENT_FRAME_LOSS`           | 5   | SDK 確定一個或多個 frame 在交付前遺失          |

`gvfg_event_t` 是 `gvfg_poll_event()` 的輸出。呼叫前將結構清零並把
`struct_size` 設為 `sizeof(gvfg_event_t)`。`type` 是上述事件型別；只有
`GVFG_EVENT_FRAME_LOSS` 使用 `count`，表示這筆事件合併的已知 loss 數量。

### 1.12 `gvfg_handle`

```c
typedef struct gvfg_handle_t *gvfg_handle;
```

只能由 `gvfg_create()` 建立、由 `gvfg_destroy()` 銷毀；caller
不可取得、配置或修改其內部內容。

### 1.13 `gvfg_enumerate_devices`

```c
int gvfg_enumerate_devices(gvfg_device_info_t *out_devices,
                           int max_devices);
```

參數：

- `out_devices`：接收裝置資料的陣列；只查數量時傳 `NULL`。
- `max_devices`：陣列可容納的項目數；超過 `GVFG_MAX_DEVICES` 會被截斷。當
  `out_devices == NULL` 時應傳 0。

回傳值：有提供 output array 時為實際寫入數量；只查數量時為可用裝置數；沒有裝置
時為 0。此函式回傳數量，不回傳 `gvfg_status_t`。

### 1.14 `gvfg_create`

```c
gvfg_status_t gvfg_create(gvfg_handle *out_handle);
```

- `out_handle`：接收新 handle，不可為 NULL。
- 成功：`GVFG_OK`，handle 處於 closed 狀態。
- 失敗：`GVFG_EINVAL`（output pointer 為 NULL）。

### 1.15 `gvfg_destroy`

```c
gvfg_status_t gvfg_destroy(gvfg_handle handle);
```

- `handle`：`gvfg_create()` 回傳的 handle；可為 NULL。
- 回傳：`GVFG_OK`。若仍在 running，SDK 會先停止。返回後 handle 不得再使用。

### 1.16 `gvfg_open_channel`

```c
gvfg_status_t gvfg_open_channel(gvfg_handle handle,
                                int device_index,
                                int channel_index);
```

- `handle`：已建立的 session handle。
- `device_index`：`gvfg_device_info_t.index`。
- `channel_index`：`GVFG_CHANNEL_0` 或 `GVFG_CHANNEL_1`。
- 可能回傳：`GVFG_OK`、`GVFG_EINVAL`、`GVFG_ENODEV`、`GVFG_EIO`。

對同一 handle 再次 open 會先關閉原 backend session；應用程式仍應依正常 lifecycle
先 stop，再切換裝置或 channel。

### 1.17 `gvfg_start`

```c
gvfg_status_t gvfg_start(gvfg_handle handle);
```

- `handle`：已成功 open 的 session handle。
- `GVFG_OK`：開始擷取、已經 running，或無訊號但成功進入訊號監看模式。
- `GVFG_EINVAL`：handle 為 NULL。
- `GVFG_ESTATE`：尚未 open 裝置。
- 也可能回傳 backend 的 `GVFG_EIO`、`GVFG_ENOTSUP` 等錯誤。

### 1.18 `gvfg_read_frame`

```c
gvfg_status_t gvfg_read_frame(gvfg_handle handle,
                              gvfg_frame_t *out_frame,
                              uint32_t timeout_ms);
```

- `handle`：running session。
- `out_frame`：接收 frame descriptor，不可為 NULL；呼叫前建議清零。
- `timeout_ms`：0 為 non-blocking；`GVFG_TIMEOUT_INFINITE` 為無限等待；其他值為毫秒。
- `GVFG_OK`：成功取得 frame，caller 現在持有一個必須 release 的 token。
- `GVFG_EINVAL`：handle 或 output pointer 為 NULL。
- `GVFG_ESTATE`：未 running、已有 held frame、已有另一個 read，或等待時被 stop。
- `GVFG_ETIMEOUT`：期限內沒有 frame。
- `GVFG_EIO`／`GVFG_ENOTSUP`：frame/backend 無效或格式不支援。

### 1.19 `gvfg_release_frame`

```c
gvfg_status_t gvfg_release_frame(gvfg_handle handle,
                                 const gvfg_frame_t *frame);
```

- `handle`：取得該 frame 的同一個 handle。
- `frame`：`gvfg_read_frame()` 原封不動回傳的完整 descriptor。
- `GVFG_OK`：成功歸還 frame，slot 可再次使用。
- `GVFG_EINVAL`：NULL 或 token 內容與 held frame 不符。
- `GVFG_ESTATE`：沒有 backend、目前沒有 held frame，或 backend release 狀態不正確。

### 1.20 `gvfg_gpu_convert_to_buffer`

```c
gvfg_status_t gvfg_gpu_convert_to_buffer(
    const gvfg_frame_t *source,
    const gvfg_gpu_output_buffer_t *output);
```

- `source`：有效的 YVYU/Y210 frame；若來自 read，必須尚未 release。
- `output`：caller 填好的 destination descriptor。
- `GVFG_OK`：同步轉換及 copy 完成。
- `GVFG_EINVAL`：NULL、尺寸、stride、buffer size、奇偶尺寸或 overflow 錯誤。
- `GVFG_ENOTSUP`：input/output format 不支援。
- `GVFG_EIO`：D3D/GPU resource、dispatch 或 readback 失敗。

### 1.21 `gvfg_gpu_convert_to_bgra8`

```c
gvfg_status_t gvfg_gpu_convert_to_bgra8(
    const gvfg_frame_t *source,
    void *destination,
    uint64_t destination_size,
    int row_bytes);
```

- `source`：有效且尚未 release 的 YVYU/Y210 frame。
- `destination`：caller-owned BGRA8 buffer。
- `destination_size`：buffer byte 數，至少 `row_bytes * source->height`。
- `row_bytes`：destination stride，至少 `source->width * 4`。
- 回傳狀態與通用 GPU conversion 相同。

### 1.22 `gvfg_gpu_convert_to_rgb10a2`

```c
gvfg_status_t gvfg_gpu_convert_to_rgb10a2(
    const gvfg_frame_t *source,
    void *destination,
    uint64_t destination_size,
    int row_bytes);
```

參數與 BGRA8 wrapper 相同，但 destination layout 是 packed RGB10A2；每列仍至少
`source->width * 4` bytes。回傳狀態與通用 GPU conversion 相同。

### 1.23 `gvfg_gpu_convert_to_nv12`

```c
gvfg_status_t gvfg_gpu_convert_to_nv12(
    const gvfg_frame_t *source,
    void *destination,
    uint64_t destination_size,
    int row_bytes);
```

- `source`：有效且尚未 release 的 YVYU/Y210 frame；width、height 必須為偶數。
- `destination`：caller-owned NV12 buffer。
- `destination_size`：至少 `row_bytes * (height + height / 2)`。
- `row_bytes`：Y 與 UV plane 共用的 stride，至少為 `width`。
- 回傳狀態與通用 GPU conversion 相同。

### 1.24 `gvfg_poll_event`

```c
gvfg_status_t gvfg_poll_event(gvfg_handle handle,
                              gvfg_event_t *out_event,
                              uint32_t timeout_ms);
```

- `handle`：running session。
- `out_event`：接收一個事件，不可為 NULL；呼叫前設定 `struct_size`。
- `timeout_ms`：規則與 read 相同。
- `GVFG_OK`：成功取出一個事件。
- `GVFG_EINVAL`：handle/output pointer 為 NULL，或 `struct_size` 不正確。
- `GVFG_ESTATE`：未 running，或等待時被 stop。
- `GVFG_ETIMEOUT`：期限內沒有事件。

### 1.25 `gvfg_stop`

```c
gvfg_status_t gvfg_stop(gvfg_handle handle);
```

- `handle`：session handle，不可為 NULL。
- `GVFG_OK`：成功；已經 stopped 也視為成功。
- `GVFG_EINVAL`：handle 為 NULL。

此函式會停止 backend、喚醒等待中的 read/poll，並使未 release frame 失效。

### 1.26 `gvfg_get_signal_status`

```c
gvfg_status_t gvfg_get_signal_status(
    gvfg_handle handle,
    gvfg_signal_status_t *out_status);
```

- `handle`：已 open 的 session。
- `out_status`：接收 signal status，不可為 NULL。
- `GVFG_OK`：查詢成功；沒有訊號時仍成功，但 `connected == 0`。
- `GVFG_EINVAL`：handle 或 output pointer 為 NULL。
- `GVFG_ESTATE`：尚未 open 裝置。
- 也可能回傳其他 backend I/O 狀態。

### 1.27 `gvfg_get_runtime_info`

```c
gvfg_status_t gvfg_get_runtime_info(
    gvfg_handle handle,
    gvfg_runtime_info_t *out_info);
```

- `handle`：已 open 或 running 的 session。
- `out_info`：接收 runtime statistics，不可為 NULL。
- `GVFG_OK`：查詢成功。
- `GVFG_EINVAL`：handle 或 output pointer 為 NULL。
- `GVFG_ESTATE`：handle 尚無已開啟的 backend 裝置。

### 1.28 `gvfg_pixel_format_name`

```c
const char *gvfg_pixel_format_name(int pixel_format);
```

- `pixel_format`：`gvfg_pixel_format_t` 或其他整數值。
- 回傳：靜態英文字串 `"YVYU"`、`"Y210"` 或 `"UNKNOWN"`。Caller 不可 free。

### 1.29 `gvfg_strerror`

```c
const char *gvfg_strerror(gvfg_status_t status);
```

- `status`：任一 GVFG status code。
- 回傳：靜態、null-terminated 英文說明字串；未知值回傳 unknown 類型說明。
  Caller 不可 free。

### 1.30 `gvfg_get_last_error_detail`

```c
gvfg_status_t gvfg_get_last_error_detail(gvfg_handle handle,
                                         char *out_message,
                                         uint32_t out_message_size);
```

- 複製此 handle 最近一次失敗操作的 UTF-8 詳細說明。
- 同一 handle 若被多執行緒同時操作，內容可能被後續錯誤覆蓋；應在失敗後立即取得。
- `GVFG_EINVAL`：handle/message 為 NULL，或 buffer size 為 0。

## 2. Preview API 完整參考

本節說明選用的 `gvfg_preview.h`。Preview status 與 core `gvfg_status_t` 是不同 enum，
不可混用。

### 2.1 Preview 型別

`gvfg_preview_status_t`：

| 成員                     | 說明                           |
| ---------------------- | ---------------------------- |
| `GVFG_PREVIEW_OK`      | 成功                           |
| `GVFG_PREVIEW_EINVAL`  | NULL、尺寸或參數錯誤                 |
| `GVFG_PREVIEW_ESTATE`  | 尚未建立／attach，或狀態不允許操作         |
| `GVFG_PREVIEW_ENOTSUP` | 不支援 frame 格式                 |
| `GVFG_PREVIEW_ERENDER` | D3D/DXGI render 或 Present 失敗 |

`gvfg_preview_handle` 是 opaque pointer，只能由 preview create/destroy 管理。

`gvfg_preview_pixel_format_t` 包含 `GVFG_PREVIEW_PIXFMT_Y210` 與
`GVFG_PREVIEW_PIXFMT_YVYU`，值與對應 core pixel format 相同。

`gvfg_preview_frame_t`：

| 欄位                 | 說明                                                     |
| ------------------ | ------------------------------------------------------ |
| `data`、`data_size` | Caller-owned frame pointer 與 byte 數                    |
| `width`、`height`   | Frame pixel 尺寸                                         |
| `pixel_format`     | `gvfg_preview_pixel_format_t`                          |
| `bit_depth`        | Frame bit depth                                        |
| `row_bytes`        | Source row stride，通常對應 `gvfg_frame_t.row_stride_bytes` |
| `frame_id`         | Frame 識別值，通常對應 `gvfg_frame_t.frame_id`                 |

`gvfg_preview_info_t`：

| 欄位                           | 說明                             |
| ---------------------------- | ------------------------------ |
| `active`                     | 非 0 表示 preview pipeline 已啟用    |
| `width`、`height`、`bit_depth` | 最近使用的 frame 資訊                 |
| `pixel_format[32]`           | Null-terminated 格式名稱           |
| `adapter_name[160]`          | Null-terminated D3D adapter 名稱 |
| `adapter_index`              | 使用的 adapter 索引                 |

`gvfg_preview_stats_t`：

| 欄位                 | 說明                                   |
| ------------------ | ------------------------------------ |
| `present_fps`      | 最近五秒成功 Present 的速率                   |
| `presented_frames` | 成功 Present 的累積 frame 數               |
| `skipped_presents` | Non-blocking swapchain busy 而略過的累積次數 |

### 2.2 Preview 函式

| 函式                                                         | 參數與行為                                           |
| ---------------------------------------------------------- | ----------------------------------------------- |
| `gvfg_preview_create(out_handle)`                          | `out_handle` 接收新 preview handle；不可為 NULL        |
| `gvfg_preview_destroy(handle)`                             | 銷毀 handle 與相關資源；返回後不得再使用                        |
| `gvfg_preview_attach_window(handle, native_window_handle)` | 將 preview 接到 Windows `HWND`；兩參數都必須有效            |
| `gvfg_preview_render_frame(handle, frame)`                 | 同步 render；`frame` 與其 memory 在返回前必須有效            |
| `gvfg_preview_get_info(handle, out_info)`                  | 將目前 pipeline/frame/adapter 資訊寫入 `out_info`      |
| `gvfg_preview_get_stats(handle, out_stats)`                | 將 Present 統計寫入 `out_stats`                      |
| `gvfg_preview_shutdown(handle)`                            | 關閉目前 preview pipeline，但保留 handle 供後續使用或 destroy |
| `gvfg_preview_strerror(status)`                            | 回傳靜態英文錯誤字串；caller 不可 free                       |

除 `gvfg_preview_strerror()` 外，preview 函式都回傳 `gvfg_preview_status_t`。實際錯誤
應以函式回傳值判斷，並使用 `gvfg_preview_strerror()` 記錄說明。
