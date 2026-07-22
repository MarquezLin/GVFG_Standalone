# GVFG 客戶端 API

本文說明客戶端使用的 GVFG capture API，主要 public header 是
`sdk/gvfg/include/gvfg_capture.h`。

Customer SDK 的 core API 只負責 capture。它不 expose SDK-managed preview、
driver registers、DMA internals 或 FPGA debug controls。Preview rendering、
snapshot conversion 這類功能放在 helper DLL，使用者需要時再另外 link。

## 基本流程

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_open
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

### `gvfg_frame_t`

`gvfg_read_frame()` 回傳的 frame descriptor。

```c
typedef struct
{
    const void *data;
    uint64_t data_size;
    int width;
    int height;
    int pixel_format;
    int bit_depth;
    uint64_t frame_id;
} gvfg_frame_t;
```

### `gvfg_frame_layout_t`

由 `gvfg_get_frame_layout()` 回傳的 optional per-plane layout。

```c
typedef struct
{
    uint32_t struct_size;
    uint32_t layout_flags;
    int row_bytes;
    int plane_count;
    const void *plane_data[GVFG_MAX_PLANES];
    int plane_stride[GVFG_MAX_PLANES];
    uint64_t plane_size[GVFG_MAX_PLANES];
    uint64_t plane_offset[GVFG_MAX_PLANES];
    uint64_t reserved[8];
} gvfg_frame_layout_t;
```

需要 row pitch 或 plane offset 的 application，應該在 `gvfg_read_frame()` 後呼叫
`gvfg_get_frame_layout()`，並使用 `plane_data[]`、`plane_stride[]`、
`plane_size[]`，不要自己用 width 和 pixel format 猜 stride。

`gvfg_frame_t::data` 和 `data_size` 仍然描述整個 native buffer。`plane_offset[]`
是從 `data` 開始算的 byte offset，未來 driver 如果回報 padded 或 non-default
DMA layout，也可以在不改 `gvfg_frame_t` 的情況下支援。

`layout_flags` 是 `gvfg_frame_layout_flags_t` bitmask。目前 backend 會設
`GVFG_FRAME_LAYOUT_CONTIGUOUS` 和 `GVFG_FRAME_LAYOUT_SDK_DERIVED`，表示 planes
位在同一個 native DMA buffer 裡，而且 pitch 是 SDK 依照 known native format
推導出來的。Application 必須忽略 `reserved[]`。

目前 tightly packed layout：

- `YUY2` / `UYVY`：one plane，`plane_stride[0] = width * 2`。
- `Y210`：one plane，`plane_stride[0] = width * 4`。
- `RGB24`：one plane，`plane_stride[0] = width * 3`。
- `YUV444`：one plane，8-bit 時 `width * 3`，>8-bit 時 `width * 6`。
- `NV12`：two planes，Y 後接 interleaved UV，
  `plane_stride[0] = plane_stride[1] = width`。
- `P010`：two planes，Y 後接 interleaved UV，
  `plane_stride[0] = plane_stride[1] = width * 2`。

### `gvfg_convert_frame`

Optional `gvfg_convert.dll` 使用的 helper-owned destination frame。

```c
typedef struct gvfg_convert_frame_t *gvfg_convert_frame;

typedef struct
{
    uint32_t struct_size;
    int width;
    int height;
    int pixel_format;
    int row_bytes;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t data_size;
    uint64_t reserved[8];
} gvfg_convert_frame_desc_t;
```

這個設計接近 Blackmagic-style conversion：application 先建立 destination
frame，再明確把 source frame convert 進去。Core capture API 仍然回傳
hardware/native buffer，不會在 `gvfg_read_frame()` 裡偷偷轉格式。

Snapshot 用途可以把 `width`、`height`、`row_bytes` 設成 0，讓 helper 依照
captured source frame 自動決定大小。

第一版 destination formats：

- `GVFG_CONVERT_FMT_BGRA8`：8-bit BGRA，alpha 255。
- `GVFG_CONVERT_FMT_RGB48`：16-bit RGB container，適合 10-bit-friendly snapshot。
- `GVFG_CONVERT_FMT_RGBA64`：16-bit RGBA container，alpha 65535。

第一版 conversions：

- `YUY2 -> BGRA8 / RGB48 / RGBA64`
- `Y210 -> BGRA8 / RGB48 / RGBA64`

Conversion 使用 BT.709 limited-range YUV to RGB，和 preview path 對齊。其他
source 或 destination format 會回傳 `GVFG_ENOTSUP`。

### `gvfg_event_t`

`gvfg_poll_event()` 回傳的 capture event。

Customer event 保持 driver-neutral：

- `GVFG_EVENT_SIGNAL_CONNECTED`
- `GVFG_EVENT_SIGNAL_DISCONNECTED`
- `GVFG_EVENT_CAPTURE_PAUSED`
- `GVFG_EVENT_CAPTURE_RESUMED`

`GVFG_EVENT_PLUG_IN` 與 `GVFG_EVENT_PLUG_OUT` 保留為 source-compatible aliases；
它們表示所選 channel 的 input signal 插拔，不是 PCIe capture device 的實體插拔。

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

### `gvfg_open` / `gvfg_open_channel`

```c
gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);
gvfg_status_t gvfg_open_channel(gvfg_handle handle,
                                int device_index,
                                int channel_index);
```

用 `gvfg_enumerate_devices()` 得到的 device index 開啟 device。`gvfg_open()` 預設
開啟 CH0；需要明確選擇 CaptureDemo 的 CH0/CH1 時使用 `gvfg_open_channel()`。
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

### Callback mode

```c
typedef void (*gvfg_frame_callback_t)(
    gvfg_handle handle,
    const gvfg_frame_t *frame,
    void *user_data);

typedef void (*gvfg_event_callback_t)(
    gvfg_handle handle,
    const gvfg_event_t *event,
    void *user_data);

gvfg_status_t gvfg_set_frame_callback(gvfg_handle handle,
                                      gvfg_frame_callback_t callback,
                                      void *user_data);
gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                      gvfg_event_callback_t callback,
                                      void *user_data);
gvfg_status_t gvfg_start_callback_mode(gvfg_handle handle);
gvfg_status_t gvfg_stop_callback_mode(gvfg_handle handle);
```

Callback mode 是 optional。它適合想讓 SDK 管理 frame/event dispatch thread 的 application。
同一個 handle 只能使用 pull mode 或 callback mode 其中一種；callback mode active
時，`gvfg_read_frame()` 和 `gvfg_poll_event()` 會回 `GVFG_ESTATE`。

Callback mode 規則：

- `gvfg_start_callback_mode()` 前必須先設定 frame callback。
- Frame 與 event callback 由同一條 SDK-owned dispatch thread 呼叫。
- 同一個 handle 的 frame/event callback 會序列化，不會彼此併發。
- Frame pointer 只在 callback 期間有效；callback return 後 SDK 會自動 release。
- Callback 內不要呼叫 `gvfg_destroy()`；`gvfg_stop_callback_mode()` 也應由其他 thread 呼叫。
- Event callback 依 SDK 收到事件的順序，和 frame callback 在同一條 thread dispatch。
- Heavy work 應 copy/queue 到 application 自己的 worker thread。

最小 callback flow：

```c
static void on_frame(gvfg_handle h, const gvfg_frame_t *frame, void *user)
{
    /* frame is valid only inside this callback. Copy if needed. */
}

gvfg_set_frame_callback(h, on_frame, user);
gvfg_start_callback_mode(h);

/* later, from another thread */
gvfg_stop_callback_mode(h);
```

### `gvfg_read_frame`

```c
gvfg_status_t gvfg_read_frame(gvfg_handle handle,
                              gvfg_frame_t *out_frame,
                              uint32_t timeout_ms);
```

讀取一個 frame。`timeout_ms == 0` 表示 indefinite wait。

回傳值：

- `GVFG_OK`：取得 frame。
- `GVFG_ETIMEOUT`：timeout 前沒有 frame。
- `GVFG_ESTATE`：capture 尚未 running，或上一個 frame 還沒 release。

### `gvfg_get_frame_layout`

```c
gvfg_status_t gvfg_get_frame_layout(const gvfg_frame_t *frame,
                                    gvfg_frame_layout_t *out_layout);
```

回傳 `gvfg_read_frame()` 取得 frame 的 per-plane layout。

呼叫前先初始化 `out_layout->struct_size`：

```c
gvfg_frame_layout_t layout = {};
layout.struct_size = sizeof(layout);
gvfg_get_frame_layout(&frame, &layout);
```

回傳的 `plane_data[]` pointers 只在 `gvfg_release_frame()` 前有效。

### `gvfg_convert_create_frame` / `gvfg_convert_destroy_frame`

```c
gvfg_status_t gvfg_convert_create_frame(const gvfg_convert_frame_desc_t *desc,
                                        gvfg_convert_frame *out_frame);
gvfg_status_t gvfg_convert_destroy_frame(gvfg_convert_frame frame);
```

建立或銷毀 helper-owned destination frame。Caller 主要設定
`desc.pixel_format`；`width`、`height`、`row_bytes` 可以是 0，讓
`gvfg_convert_frame_from_capture()` 依照 source frame 自動 configure。

### `gvfg_convert_frame_from_capture`

```c
gvfg_status_t gvfg_convert_frame_from_capture(const gvfg_frame_t *src,
                                              gvfg_convert_frame dst_frame);
```

把 captured native frame 轉進 helper-owned destination frame。這不會改變
capture output format。

Snapshot buffer flow 範例：

```c
#include <gvfg_capture.h>
#include <gvfg_convert.h>

gvfg_frame_t frame = {};
if (gvfg_read_frame(h, &frame, 1000) == GVFG_OK) {
    gvfg_convert_frame_desc_t desc = {};
    desc.struct_size = sizeof(desc);
    desc.pixel_format = GVFG_CONVERT_FMT_RGB48;

    gvfg_convert_frame image = NULL;
    if (gvfg_convert_create_frame(&desc, &image) == GVFG_OK &&
        gvfg_convert_frame_from_capture(&frame, image) == GVFG_OK) {
        const void *data = NULL;
        uint64_t size = 0;
        gvfg_convert_get_buffer(image, &data, &size);
        /* data contains RGB48 rows; query layout for row_bytes. */
    }

    gvfg_convert_destroy_frame(image);
    gvfg_release_frame(h, &frame);
}
```

### `gvfg_convert_get_frame_desc` / `gvfg_convert_get_buffer` / `gvfg_convert_get_layout`

```c
gvfg_status_t gvfg_convert_get_frame_desc(gvfg_convert_frame frame,
                                          gvfg_convert_frame_desc_t *out_desc);
gvfg_status_t gvfg_convert_get_buffer(gvfg_convert_frame frame,
                                      const void **out_data,
                                      uint64_t *out_size);
gvfg_status_t gvfg_convert_get_layout(gvfg_convert_frame frame,
                                      gvfg_frame_layout_t *out_layout);
```

在 `gvfg_convert_frame_from_capture()` 後，用這些 API 取得 converted image
buffer、metadata 和 row stride。

### `gvfg_release_frame`

```c
gvfg_status_t gvfg_release_frame(gvfg_handle handle,
                                 const gvfg_frame_t *frame);
```

Release `gvfg_read_frame()` 回傳的 frame。

### `gvfg_poll_event`

```c
gvfg_status_t gvfg_poll_event(gvfg_handle handle,
                              gvfg_event_t *out_event,
                              uint32_t timeout_ms);
```

Poll 一個 event。`timeout_ms == 0` 表示 non-blocking poll。

### `gvfg_get_signal_status`

```c
gvfg_status_t gvfg_get_signal_status(gvfg_handle handle,
                                     gvfg_signal_status_t *out_status);
```

查詢目前 input signal metadata。

### `gvfg_get_runtime_info`

```c
gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle,
                                    gvfg_runtime_info_t *out_info);
```

查詢目前 signal status、last read frame format、FPS、delivered frame count。

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
