# GVFG 客戶端 API

本文說明客戶端使用的 GVFG capture API，主要 public header 是
`sdk/gvfg/include/gvfg_capture.h`。

Customer SDK 的 core `gvfg.dll` 負責 capture 與 explicit GPU buffer conversion。
它不 expose SDK-managed preview、driver registers、DMA internals 或 FPGA debug
controls。Preview rendering 仍由 optional `gvfg_preview.dll` 提供。

Customer 交付內容只包含 public headers 與 public DLL functions。Customer
sample 只記錄 signal events、API failures、capture stall/recovery 與
preview failures，不顯示硬體或 backend implementation details。

## 基本流程

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_open_channel
-> gvfg_start
-> loop:
   gvfg_read_frame
   use or copy frame data
   gvfg_release_frame
   gvfg_poll_event optional
-> gvfg_stop
-> gvfg_destroy
```

## 執行緒模型

Frame API 是 pull-based。Public API layer 不會替 customer 啟動 frame thread
或 preview thread。

Application 自己決定在哪裡呼叫 `gvfg_read_frame()`：

```text
UI app:
  create an app-owned worker thread
  call gvfg_read_frame() in that worker
  marshal UI updates back to the UI thread

console/service app:
  call gvfg_read_frame() directly from its processing loop
```

Backend 在 capture running 時可以有自己的 internal driver I/O workers，但那是
`gvfg.dll` 背後的 implementation detail。

同一個 handle 允許一條 frame-reading thread；query API 與 `gvfg_poll_event()`
可以和它同時執行。Application 必須自行 serialize lifecycle API。`gvfg_stop()` 可以
喚醒 blocked read/poll，但 stop/destroy 讓 frame 失效時，其他 thread 不得繼續存取它。

所有等待 API 使用一致的 timeout：`0` 表示 non-blocking，
`GVFG_TIMEOUT_INFINITE` 表示無限等待，其他值是 milliseconds。

## Frame 所有權

`gvfg_read_frame()` 會回傳一個 SDK-owned frame buffer。

規則：

- `frame.data` 在呼叫 `gvfg_release_frame()` 前有效。
- 同一個 handle 一次最多只能 hold 一個 frame。
- Application 如果需要在 release 後繼續使用資料，必須自己 copy frame。
- `gvfg_stop()` 會讓尚未 release 的 frame 失效。

## 公開型別

### `gvfg_handle`

Opaque session handle。

```c
typedef struct gvfg_handle_t *gvfg_handle;
```

### `gvfg_status_t`

共用 return status。

```c
typedef enum
{
    GVFG_OK = 0,
    GVFG_EINVAL = -1,
    GVFG_ENODEV = -2,
    GVFG_ESTATE = -3,
    GVFG_EIO = -4,
    GVFG_ENOTSUP = -5,
    GVFG_ETIMEOUT = -6
} gvfg_status_t;
```

### `gvfg_device_info_t`

`gvfg_enumerate_devices()` 回傳的 device entry。

### `gvfg_signal_status_t`

Customer-readable input signal status：

- connected state 與 CH0 / CH1
- width / height
- 實際 DMA pixel format
- bit depth

新 driver 未提供 frame-rate、SDI/HDMI lock、DDR status 或舊 FPGA validity
register，因此 SDK 不再合成或公開這些欄位。

沒有 input signal 是正常狀態：API 回傳 `GVFG_OK`，同時 `connected == 0`。

```c
gvfg_signal_status_t signal = {};
gvfg_get_signal_status(h, &signal);
```

### `gvfg_frame_t`

`gvfg_read_frame()` 回傳的 frame descriptor。

```c
typedef struct
{
    const void *data;
    uint64_t data_size;
    int width;
    int height;
    int row_stride_bytes;
    int pixel_format;
    int bit_depth;
    uint64_t frame_id;
} gvfg_frame_t;
```

`row_stride_bytes` 是相鄰兩列起點之間的 byte 距離。Application 應使用此欄位
逐列存取影像，不要自行用 width 和 pixel format 猜 stride。`data`、
`data_size` 與 `row_stride_bytes` 都由 SDK 填入。

### ABI 與 metadata 擴充規則

- `gvfg_handle` 永遠保持 opaque。
- `gvfg_frame_t` 保持極小，stable release 後凍結。
- 第一版 API 不做跨版本 struct 相容；DLL、header 與 application 必須整套更新。
- 現行 native formats 都是 single-plane packed buffers；row stride 直接放在
  `gvfg_frame_t`。
- 未來真的加入 multi-plane format 時，再新增獨立 plane-layout API。
- 未來 color metadata 使用獨立的 `gvfg_get_frame_color_info()`。
- 未來 timestamp metadata 使用獨立的 `gvfg_get_frame_timestamp_info()`。
- Metadata 類型真的大量增加時，才考慮 side data。
- 只有 major version 可以破壞既有 ABI。

板子只提供兩種 tightly packed native capture layout：

- `YVYU`：`row_stride_bytes = width * 2`。
- `Y210`：`row_stride_bytes = width * 4`。

其他 native input format 不在 SDK 支援範圍，會回傳 `GVFG_ENOTSUP`。

### `gvfg_gpu_output_buffer_t`

`gvfg.dll` 的 GPU conversion API 使用 caller-owned destination buffer：

```c
typedef struct
{
    void *data;
    uint64_t data_size;
    int row_bytes;
    int pixel_format;
} gvfg_gpu_output_buffer_t;
```

支援 `GVFG_GPU_OUTPUT_BGRA8` 與 packed `GVFG_GPU_OUTPUT_RGB10A2`；兩者皆為
每像素四 bytes。Core capture API 仍回傳 hardware/native buffer，不會在
`gvfg_read_frame()` 裡自動轉換。

### `gvfg_event_type_t`

`gvfg_poll_event()` 直接回傳 capture event 類型。

Customer event 保持 driver-neutral：

- `GVFG_EVENT_SIGNAL_CONNECTED`
- `GVFG_EVENT_SIGNAL_DISCONNECTED`

Frame interrupts、IRQ bit numbers、IRQ masks 都是 internal details，不應出現在
`gvfg_capture.h`。

## 主要 API

Public header 內的 function prototype 會使用 Windows SAL annotation，例如
`_In_`、`_Out_`、`_Inout_` 來標示參數方向。以下文件中的 prototype 為了閱讀性會省略
SAL；實際呼叫 API 時，application 不需要也不能額外傳入這些 annotation。

### `gvfg_enumerate_devices`

```c
int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);
```

列舉 GVFG capture devices。傳 `NULL, 0` 可以只查詢 device count。

### `gvfg_create` / `gvfg_destroy`

```c
gvfg_status_t gvfg_create(gvfg_handle *out_handle);
gvfg_status_t gvfg_destroy(gvfg_handle handle);
```

建立或銷毀 session handle。銷毀 running handle 時會先 stop capture。

### `gvfg_open_channel`

```c
gvfg_status_t gvfg_open_channel(gvfg_handle handle,
                                int device_index,
                                int channel_index);
```

用 `gvfg_enumerate_devices()` 得到的 device index 開啟 device，並明確選擇
`GVFG_CHANNEL_0` 或 `GVFG_CHANNEL_1`。
選定的 channel 會一致套用到 signal status、signal 插拔事件、DMA done index 與
frame buffer。

### `gvfg_start` / `gvfg_stop`

```c
gvfg_status_t gvfg_start(gvfg_handle handle);
gvfg_status_t gvfg_stop(gvfg_handle handle);
```

開始或停止 capture。`gvfg_start()` 成功後，用 `gvfg_read_frame()` 取 frame。沒有 input
signal 時 `gvfg_start()` 仍會成功並進入 event monitoring；frame read 會 timeout，收到
`GVFG_EVENT_SIGNAL_CONNECTED` 後 SDK 會自動重新讀取格式並啟動 DMA。

### `gvfg_read_frame`

```c
gvfg_status_t gvfg_read_frame(gvfg_handle handle,
                              gvfg_frame_t *out_frame,
                              uint32_t timeout_ms);
```

讀取一個 frame。`timeout_ms == 0` 表示 non-blocking；
`timeout_ms == GVFG_TIMEOUT_INFINITE` 表示 indefinite wait。

回傳值：

- `GVFG_OK`：取得 frame。
- `GVFG_ETIMEOUT`：timeout 前沒有 frame。
- `GVFG_ESTATE`：capture 尚未 running，或上一個 frame 還沒 release。

### `gvfg_gpu_convert_to_buffer`

```c
gvfg_status_t gvfg_gpu_convert_to_buffer(const gvfg_frame_t *source,
                                         const gvfg_gpu_output_buffer_t *output);
```

用 D3D11 shader 同步轉換 captured YVYU/Y210 frame，並 readback 到 caller buffer。
函式返回前 source 與 output memory 必須保持有效；SDK 不保留兩者的 pointer。

```c
#include <gvfg_capture.h>

gvfg_frame_t frame = {};
if (gvfg_read_frame(h, &frame, 1000) == GVFG_OK) {
    gvfg_gpu_output_buffer_t output = {};
    output.data = canvas_data;
    output.data_size = canvas_size;
    output.row_bytes = frame.width * 4;
    output.pixel_format = GVFG_GPU_OUTPUT_BGRA8;
    gvfg_gpu_convert_to_buffer(&frame, &output);
    gvfg_release_frame(h, &frame);
}
```

### `gvfg_release_frame`

```c
gvfg_status_t gvfg_release_frame(gvfg_handle handle,
                                 const gvfg_frame_t *frame);
```

Release `gvfg_read_frame()` 回傳的 frame。

### `gvfg_poll_event`

```c
gvfg_status_t gvfg_poll_event(gvfg_handle handle,
                              gvfg_event_type_t *out_event,
                              uint32_t timeout_ms);
```

Poll 一個 event。`timeout_ms == 0` 表示 non-blocking poll；
`timeout_ms == GVFG_TIMEOUT_INFINITE` 表示 indefinite wait。
只有 running session 可以 poll；`gvfg_stop()` 會喚醒 blocked poll 並使它回傳
`GVFG_ESTATE`。

### `gvfg_get_signal_status`

```c
gvfg_status_t gvfg_get_signal_status(gvfg_handle handle,
                                     gvfg_signal_status_t *out_status);
```

查詢目前 input signal metadata。

```c
gvfg_signal_status_t status = {};
gvfg_get_signal_status(h, &status);
```

沒有 input signal 時仍回傳 `GVFG_OK`，並設定 `status.connected = 0`。

### `gvfg_get_runtime_info`

```c
gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle,
                                    gvfg_runtime_info_t *out_info);
```

查詢 FPS 與 delivered frame count。Input signal metadata 請另外呼叫
`gvfg_get_signal_status()`；每張實際交付的 frame metadata 由 `gvfg_frame_t`
提供。Last-delivered frame metadata 僅保留於 internal debug API。

```c
gvfg_runtime_info_t info = {};
gvfg_get_runtime_info(h, &info);
```

## Preview 邊界

Preview 不是 core capture SDK API 的一部分。

Application 在 `gvfg_read_frame()` 回傳 frame 後有幾種選擇：

```text
gvfg_read_frame
-> customer-owned display / processing / recording / snapshot
-> optional gvfg_preview_render_frame from gvfg_preview.dll
-> gvfg_release_frame
```

Customer demo source 需要簡單 display path 時，可以 include `gvfg_preview.h`
並 link `gvfg_preview.dll`。Preview helper source 保持 private；customer code
只看到 helper API 和 binary。

`gvfg_preview_get_stats()` 可查詢 preview 顯示速率。`present_fps` 只計算
DXGI `Present` 成功接受的畫面；使用 `DXGI_PRESENT_DO_NOT_WAIT` 時因
swapchain busy 而跳過的畫面不列入。速率以最近五秒的 successful Present
時間戳計算，開始顯示前至少收集兩秒，停止顯示超過一秒後回到 `0`。
