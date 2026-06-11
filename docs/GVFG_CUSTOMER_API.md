# GVFG 客戶 API 使用說明

這份文件說明 VFG100 / GVFG capture SDK 對外提供的 C API。公開 API 名稱統一使用 `gvfg_` 前綴。

公開 API 定義在：

```text
sdk/gvfg/include/gvfg_capture.h
```

`gvfg` 是獨立的 GVFG capture SDK，和 `gcapture` 分開。只需要 VFG100 / GVFG 功能的客戶，不需要包含或使用 DirectShow / Media Foundation 那批 API。

## 要提供給客戶的檔案

執行時需要放在客戶程式旁邊：

```text
gvfg.dll
```

開發時需要提供 include 目錄：

```text
sdk/gvfg/include
```

客戶程式只需要 include：

```c
#include <gvfg_capture.h>
```

連結時使用：

```text
gvfg.lib
```

SDK 內部 XDMA backend 標頭檔不提供給客戶使用。

最小 Qt 範例：

```text
samples/gvfg_qt_preview
```

這個範例示範 GVFG API 的基本使用方式。

## CMake 選項

是否建置 GVFG 範例：

```text
BUILD_GVFG_SAMPLES=ON/OFF
```

`gvfg` library 會由 top-level CMake 建置；`BUILD_GVFG_SAMPLES` 只控制是否一起建置 `samples/gvfg_qt_preview`。

## 整體架構

```text
客戶 App
  |
  | include gvfg_capture.h
  | link gvfg.lib
  v
gvfg.dll
  |
  | 公開 GVFG capture API
  | 管理 preview、callback、runtime info
  v
GVFG capture device / driver
```

## Capture / Preview 資料流

SDK 內建 preview 的資料流：

```text
GVFG capture device
-> gvfg.dll
-> SDK preview pipeline
-> D3D swapchain present 到客戶提供的 HWND
```

重點：

- SDK 內建 preview 直接畫到客戶提供的 `HWND`。
- frame callback 是可選功能，給 snapshot、客戶自訂處理或客戶自己的 preview pipeline 使用。
- SDK 內建 preview 不需要客戶啟用 frame callback。
- callback 的 `data` pointer 只在 callback 期間有效；callback return 後要視為失效。

## 建議使用流程

一般客戶程式照這個順序：

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_set_callbacks
-> gvfg_set_frame_callback_interval 可選
-> gvfg_set_event_callback 可選
-> gvfg_set_preview 可選
-> gvfg_open
-> gvfg_start
-> capture running
-> gvfg_get_signal_status / gvfg_get_runtime_info / gvfg_get_preview_info
-> gvfg_stop
-> gvfg_destroy
```

簡單說：

1. 列出 GVFG / XDMA 裝置。
2. 建立 `gvfg_handle`。
3. 設定 frame / error callback。
4. 視需求設定 frame callback 頻率。
5. 視需求設定 event callback。
6. 視需求設定 SDK 內建 preview。
7. 開啟裝置。
8. 開始 capture。
9. 停止並釋放 handle。

如果客戶要自己畫畫面，就不要呼叫 `gvfg_set_preview()`，直接使用 frame callback 取得原生 buffer。

## 公開型別

### 資料方向速查表

| 型別 | 方向 | 誰填資料 | 用在哪裡 |
| --- | --- | --- | --- |
| `gvfg_handle` | handle | SDK 建立，客戶保存 | 所有 session API |
| `gvfg_status_t` | 回傳值 | SDK 回傳 | 大部分 API 的成功/失敗結果 |
| `gvfg_preview_bitdepth_t` | 設定值 | 客戶選擇 | `gvfg_preview_desc_t::swapchain_bitdepth` |
| `gvfg_pixel_format_t` | 描述值 | SDK 填入 | `gvfg_frame_t::pixel_format` |
| `gvfg_device_info_t` | 輸出 | SDK 填入 | `gvfg_enumerate_devices()` |
| `gvfg_preview_desc_t` | 輸入設定 | 客戶填入 | `gvfg_set_preview()` |
| `gvfg_fpga_signal_raw_t` | 輸出 | SDK 填入 | `gvfg_signal_status_t::raw` |
| `gvfg_signal_status_t` | 輸出 | SDK 填入 | `gvfg_get_signal_status()` |
| `gvfg_callback_frame_info_t` | 輸出 | SDK 填入 | `gvfg_runtime_info_t::callback_frame` |
| `gvfg_preview_output_info_t` | 輸出 | SDK 填入 | `gvfg_runtime_info_t::preview_output` |
| `gvfg_runtime_info_t` | 輸出 | SDK 填入 | `gvfg_get_runtime_info()` |
| `gvfg_preview_info_t` | 輸出 | SDK 填入 | `gvfg_get_preview_info()` |
| `gvfg_frame_t` | callback payload | SDK 填入 | `gvfg_on_frame_cb` |
| `gvfg_event_t` | callback payload | SDK 填入 | `gvfg_on_event_cb` |

讀法很簡單：

- 「輸入設定」代表客戶建立 struct、填欄位，再傳給 SDK。
- 「輸出」代表客戶準備空 struct，SDK 會把查詢結果寫進去。
- 「callback payload」代表 SDK 在 callback 發生時傳進來，客戶只讀，不要改。

### gvfg_handle：handle

```c
typedef struct gvfg_handle_t *gvfg_handle;
```

這是 SDK session handle。客戶不要直接存取內容，只能透過公開 API 操作。

生命週期：

```text
gvfg_create
-> 使用 handle
-> gvfg_destroy
```

### gvfg_status_t：回傳值

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

| 值               | 意義                                  |
| --------------- | ----------------------------------- |
| `GVFG_OK`       | 成功                                  |
| `GVFG_EINVAL`   | 參數錯誤，例如 handle 或輸出 pointer 是 `NULL` |
| `GVFG_ENODEV`   | 找不到裝置、裝置開啟失敗，或沒有有效 signal             |
| `GVFG_ESTATE`   | 狀態錯誤，例如尚未 open 就 start              |
| `GVFG_EIO`      | driver/backend I/O 錯誤               |
| `GVFG_ENOTSUP`  | 功能或格式尚未支援                           |
| `GVFG_ETIMEOUT` | 等待 frame 或 event 逾時                 |

如果要轉成字串，使用：

```c
gvfg_strerror(status)
```

### gvfg_preview_bitdepth_t：設定值

```c
typedef enum
{
    GVFG_PREVIEW_BITDEPTH_AUTO = 0,
    GVFG_PREVIEW_BITDEPTH_10BIT = 10,
    GVFG_PREVIEW_BITDEPTH_8BIT = 8
} gvfg_preview_bitdepth_t;
```

| 值                             | 意義                                     |
| ----------------------------- | -------------------------------------- |
| `GVFG_PREVIEW_BITDEPTH_AUTO`  | 讓 SDK 自動選擇 preview swapchain bit depth |
| `GVFG_PREVIEW_BITDEPTH_10BIT` | 優先要求 10-bit preview swapchain          |
| `GVFG_PREVIEW_BITDEPTH_8BIT`  | 強制使用 8-bit preview swapchain           |

`AUTO` 會依照 signal bit depth、preview pipeline 能力與 swapchain 支援度選擇。

### gvfg_pixel_format_t：描述值

```c
typedef enum
{
    GVFG_PIXFMT_UNKNOWN = 0,
    GVFG_PIXFMT_YUY2 = 1,
    GVFG_PIXFMT_UYVY = 2,
    GVFG_PIXFMT_RGB24 = 3,
    GVFG_PIXFMT_BGRX32 = 4,
    GVFG_PIXFMT_NV12 = 5,
    GVFG_PIXFMT_P010 = 6,
    GVFG_PIXFMT_Y210 = 7,
    GVFG_PIXFMT_YUV444 = 8,
    GVFG_PIXFMT_BGRA8 = 100
} gvfg_pixel_format_t;
```

| 值                     | 意義                                |
| --------------------- | --------------------------------- |
| `GVFG_PIXFMT_UNKNOWN` | 未知格式                              |
| `GVFG_PIXFMT_YUY2`    | YUY2 packed 4:2:2                 |
| `GVFG_PIXFMT_UYVY`    | UYVY packed 4:2:2                 |
| `GVFG_PIXFMT_RGB24`   | RGB 24-bit packed                 |
| `GVFG_PIXFMT_BGRX32`  | BGRX 32-bit packed                |
| `GVFG_PIXFMT_NV12`    | NV12 8-bit 4:2:0                  |
| `GVFG_PIXFMT_P010`    | P010 10-bit 4:2:0                 |
| `GVFG_PIXFMT_Y210`    | Y210 10-bit 4:2:2                 |
| `GVFG_PIXFMT_YUV444`  | YUV 4:4:4                         |
| `GVFG_PIXFMT_BGRA8`   | BGRA 8-bit，主要用於 preview output 描述 |

### gvfg_device_info_t：輸出

```c
typedef struct
{
    int index;
    char name[128];
} gvfg_device_info_t;
```

| 欄位      | 意義                            |
| ------- | ----------------------------- |
| `index` | 裝置 index，傳給 `gvfg_open()` 使用  |
| `name`  | 顯示名稱，UTF-8 null-terminated 字串 |

### gvfg_preview_desc_t：輸入設定

```c
typedef struct
{
    void *hwnd;
    int enable_preview;
    int swapchain_bitdepth;
} gvfg_preview_desc_t;
```

| 欄位                   | 意義                             |
| -------------------- | ------------------------------ |
| `hwnd`               | SDK 要畫 preview 的 Win32 `HWND`  |
| `enable_preview`     | 非 0 代表啟用 SDK 內建 preview；0 代表關閉 |
| `swapchain_bitdepth` | `gvfg_preview_bitdepth_t` 的值   |

注意：

- `hwnd` 必須是真正可用的 native Win32 window handle。
- capture running 期間，這個 window 必須保持有效。
- 如果 app 是 Qt / WinForms / WPF，要取得底層 native `HWND`。

### gvfg_fpga_signal_raw_t：輸出

```c
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t video_format;
    uint32_t frame_rate;
    uint32_t bit_depth;
    uint32_t status;
} gvfg_fpga_signal_raw_t;
```

| 欄位 | 意義 |
| --- | --- |
| `width` | FPGA width 原始值 |
| `height` | FPGA height 原始值 |
| `video_format` | FPGA video format 原始值 |
| `frame_rate` | FPGA frame-rate 原始值 |
| `bit_depth` | FPGA bit-depth 原始值 |
| `status` | FPGA lock/status 原始值 |

`gvfg_fpga_signal_raw_t` 由 SDK 填入。它提供 FPGA 原始數值，適合寫入 log 或提供給技術支援分析。客戶不需要用 raw 欄位判斷讀取是否成功；成功或失敗由 `gvfg_get_signal_status()` 的回傳值與 SDK 整理後的欄位表示。

### gvfg_signal_status_t：輸出

```c
typedef struct
{
    int width;
    int height;
    int video_format_code;
    char video_format[16];
    int frame_rate_code;
    char frame_rate_bits[5];
    char frame_rate_name[16];
    int bit_depth;
    int sdi_locked;
    int sdi_ddr_ok;
    int hdmi_locked;
    int hdmi_ddr_ok;
    gvfg_fpga_signal_raw_t raw;
} gvfg_signal_status_t;
```

| 欄位                  | 意義                                                            |
| ------------------- | ------------------------------------------------------------- |
| `width`             | FPGA 回報的 signal 寬度                                            |
| `height`            | FPGA 回報的 signal 高度                                            |
| `video_format_code` | FPGA video format 代碼，`0=yuv422`、`1=rgb`、`2=yuv444`、`3=yuv420` |
| `video_format`      | 已解碼的 signal format 名稱                                         |
| `frame_rate_code`   | FPGA frame-rate 代碼                                            |
| `frame_rate_bits`   | frame-rate 代碼的 4-bit 字串，例如 `0110`                             |
| `frame_rate_name`   | 已解碼的 frame-rate 名稱，例如 `29.97`；不支援時為 `--`                      |
| `bit_depth`         | FPGA 回報的 signal bit depth                                     |
| `sdi_locked`        | SDI lock 狀態                                                   |
| `sdi_ddr_ok`        | SDI DDR 狀態                                                    |
| `hdmi_locked`       | HDMI lock 狀態                                                  |
| `hdmi_ddr_ok`       | HDMI DDR 狀態                                                   |
| `raw`               | FPGA 原始數值，供記錄與分析使用，不用來判斷 API 成功失敗 |

FPGA frame-rate code 對照：

| bits   | 名稱    |
| ------ | ----- |
| `0000` | None  |
| `0010` | 23.98 |
| `0011` | 24    |
| `0100` | 47.95 |
| `0101` | 25    |
| `0110` | 29.97 |
| `0111` | 30    |
| `1000` | 48    |
| `1001` | 50    |
| `1010` | 59.94 |
| `1011` | 60    |

其他 code，包含 `0001`，顯示為 `--`。

注意：

- `gvfg_signal_status_t` 描述的是 FPGA 回報的 input signal。
- 客戶不需要判斷底層欄位是否讀取成功；`gvfg_get_signal_status()` 會用回傳值表示整體成功或失敗。
- 若部分非必要欄位不可用，SDK 會填入 `--`、`-1` 或 `0` 這類中性值。
- `raw` 保留 FPGA 原始數值；它不是成功判斷機制。
- callback buffer 的實際格式看 `gvfg_runtime_info_t::callback_frame` 或 `gvfg_frame_t::pixel_format`。
- SDK preview swapchain 的輸出格式看 `gvfg_runtime_info_t::preview_output` 或 `gvfg_preview_info_t`。

### gvfg_callback_frame_info_t：輸出

```c
typedef struct
{
    int width;
    int height;
    int bit_depth;
    char pixel_format[32];
    int valid;
} gvfg_callback_frame_info_t;
```

| 欄位             | 意義                             |
| -------------- | ------------------------------ |
| `width`        | 最近一次送到 app callback 的 frame 寬度 |
| `height`       | 最近一次送到 app callback 的 frame 高度 |
| `bit_depth`    | callback buffer 的 bit depth    |
| `pixel_format` | callback buffer 的原生格式名稱        |
| `valid`        | 非 0 代表已有 callback frame 資訊       |

### gvfg_preview_output_info_t：輸出

```c
typedef struct
{
    int enabled;
    int active;
    int width;
    int height;
    int bit_depth;
    char pixel_format[32];
} gvfg_preview_output_info_t;
```

| 欄位             | 意義                                         |
| -------------- | ------------------------------------------ |
| `enabled`      | app 是否要求啟用 SDK 內建 preview                  |
| `active`       | preview pipeline 是否正在運作                    |
| `width`        | preview output 寬度                          |
| `height`       | preview output 高度                          |
| `bit_depth`    | preview output bit depth                   |
| `pixel_format` | preview output 格式名稱，例如 `BGRA8` 或 `RGB10A2` |

### gvfg_runtime_info_t：輸出

```c
typedef struct
{
    gvfg_signal_status_t input_signal;
    gvfg_preview_output_info_t preview_output;
    gvfg_callback_frame_info_t callback_frame;
    double capture_fps;
    uint64_t delivered_frames;
} gvfg_runtime_info_t;
```

| 欄位                 | 意義                               |
| ------------------ | -------------------------------- |
| `input_signal`     | FPGA 回報的 input signal 狀態         |
| `preview_output`   | SDK 內建 preview 輸出狀態              |
| `callback_frame`   | app callback 收到的 frame buffer 狀態 |
| `capture_fps`      | SDK capture worker 測到的 FPS       |
| `delivered_frames` | SDK 已送到 app callback 的 frame 數   |

使用判斷：

- 要看 input signal，使用 `input_signal`。
- 要看 SDK preview 狀態，使用 `preview_output`。
- 要看 callback buffer 狀態，使用 `callback_frame`。
- 要看 D3D swapchain / render path 細節，使用 `gvfg_get_preview_info()`。

### gvfg_preview_info_t：輸出

```c
typedef struct
{
    int enabled;
    int active;
    int width;
    int height;
    int swapchain_bitdepth;
    int swapchain_10bit;
    char render_path[128];
    char backbuffer_format[64];
} gvfg_preview_info_t;
```

| 欄位                   | 意義                                  |
| -------------------- | ----------------------------------- |
| `enabled`            | app 是否要求啟用 SDK 內建 preview           |
| `active`             | preview pipeline / swapchain 是否已建立  |
| `width`              | preview render 寬度                   |
| `height`             | preview render 高度                   |
| `swapchain_bitdepth` | preview swapchain bit depth       |
| `swapchain_10bit`    | 非 0 代表 swapchain 是 10-bit       |
| `render_path`        | SDK preview helper 的 render path 描述 |
| `backbuffer_format`  | D3D swapchain backbuffer format     |

這個 struct 只描述 SDK 內建 preview helper，不描述客戶 app 自己的 rendering pipeline。

### gvfg_frame_t：callback payload

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

| 欄位             | 意義                       |
| -------------- | ------------------------ |
| `data`         | 原生 frame buffer pointer  |
| `data_size`    | `data` 可讀取的總 byte 數      |
| `width`        | frame 寬度                 |
| `height`       | frame 高度                 |
| `pixel_format` | `gvfg_pixel_format_t` 的值 |
| `bit_depth`    | 原生 frame bit depth       |
| `frame_id`     | backend 遞增 frame id      |

重要：

- `data` 只在 callback 當下有效。
- callback return 後不能再使用這個 pointer。
- 如果要存圖、snapshot、或交給其他 thread，要在 callback 裡 copy。
- SDK 不在 `gvfg_frame_t` 提供 stride / plane offset；客戶依 `pixel_format`、`width`、`height`、`bit_depth`、`data_size` 自己計算 layout。
- callback 由 SDK worker thread 呼叫，不是在 UI thread。

### gvfg_event_type_t：描述值

```c
typedef enum
{
    GVFG_EVENT_VIDEO_IRQ = 1,
    GVFG_EVENT_PLUG_IN = 2,
    GVFG_EVENT_PLUG_OUT = 3,
    GVFG_EVENT_CAPTURE_PAUSED = 4,
    GVFG_EVENT_CAPTURE_RESUMED = 5
} gvfg_event_type_t;
```

| 值                            | 意義                     |
| ---------------------------- | ---------------------- |
| `GVFG_EVENT_VIDEO_IRQ`       | 收到 video interrupt     |
| `GVFG_EVENT_PLUG_IN`         | 收到 plug in event       |
| `GVFG_EVENT_PLUG_OUT`        | 收到 plug out event      |
| `GVFG_EVENT_CAPTURE_PAUSED`  | SDK 已自動 pause capture  |
| `GVFG_EVENT_CAPTURE_RESUMED` | SDK 已自動 resume capture |

### gvfg_event_t：callback payload

```c
typedef struct
{
    gvfg_event_type_t type;
    uint32_t irq_bit;
    uint32_t irq_mask;
    uint64_t timestamp_ns;
} gvfg_event_t;
```

| 欄位             | 意義                            |
| -------------- | ----------------------------- |
| `type`         | event 類型                      |
| `irq_bit`      | backend 回報的 IRQ bit           |
| `irq_mask`     | backend 回報的 IRQ mask          |
| `timestamp_ns` | SDK monotonic timestamp，單位 ns |

### Event mask

```c
GVFG_EVENT_MASK_VIDEO_IRQ
GVFG_EVENT_MASK_PLUG_IN
GVFG_EVENT_MASK_PLUG_OUT
GVFG_EVENT_MASK_CAPTURE_PAUSED
GVFG_EVENT_MASK_CAPTURE_RESUMED
GVFG_EVENT_MASK_HOTPLUG
GVFG_EVENT_MASK_DEFAULT
GVFG_EVENT_MASK_ALL
```

| mask                              | 意義                                              |
| --------------------------------- | ----------------------------------------------- |
| `GVFG_EVENT_MASK_VIDEO_IRQ`       | 啟用 video IRQ event                              |
| `GVFG_EVENT_MASK_PLUG_IN`         | 啟用 plug in event                                |
| `GVFG_EVENT_MASK_PLUG_OUT`        | 啟用 plug out event                               |
| `GVFG_EVENT_MASK_CAPTURE_PAUSED`  | 啟用 capture paused event                         |
| `GVFG_EVENT_MASK_CAPTURE_RESUMED` | 啟用 capture resumed event                        |
| `GVFG_EVENT_MASK_HOTPLUG`         | hotplug / capture state events                  |
| `GVFG_EVENT_MASK_DEFAULT`         | 預設 hotplug / capture state events，不包含 video IRQ |
| `GVFG_EVENT_MASK_ALL`             | 所有 event，包含 video IRQ                           |

`VIDEO_IRQ` 可能每張 frame 都觸發，只有在 app 明確需要逐張 event 時才建議打開。

### Callback 型別

```c
typedef void (*gvfg_on_frame_cb)(const gvfg_frame_t *frame, void *user);
typedef void (*gvfg_on_event_cb)(const gvfg_event_t *event, void *user);
typedef void (*gvfg_on_error_cb)(gvfg_status_t status, const char *message, void *user);
```

| 型別                 | 呼叫時機                                                    |
| ------------------ | ------------------------------------------------------- |
| `gvfg_on_frame_cb` | SDK capture worker 收到 frame，且符合 callback interval 設定時呼叫 |
| `gvfg_on_event_cb` | SDK 收到 backend event 時呼叫                                |
| `gvfg_on_error_cb` | SDK 有非同步訊息或錯誤時呼叫                                        |

callback 由 SDK worker thread 呼叫。GUI app 不要在 callback 裡直接更新 UI，應該 marshal 到 UI thread。

## 函式說明

### gvfg_enumerate_devices

```c
int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);
```

列出系統上的 GVFG capture device。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `out_devices` | 輸出 | SDK 寫入 device list。如果只想查詢數量，可傳 `NULL` |
| `max_devices` | 輸入 | `out_devices` 最多可寫入幾筆；SDK 上限是 `GVFG_MAX_DEVICES` |

| 回傳值   | 意義              |
| ----- | --------------- |
| `> 0` | 找到的裝置數量，或實際寫入數量 |
| `0`   | 沒找到裝置           |
| `< 0` | enumerate 失敗    |

### gvfg_create

```c
gvfg_status_t gvfg_create(gvfg_handle *out_handle);
```

建立一個 GVFG capture session。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `out_handle` | 輸出 | SDK 寫入新 handle，不可為 `NULL` |

成功回傳 `GVFG_OK`。建立完成後只是 session，尚未 open device，也尚未 start stream。

### gvfg_destroy

```c
gvfg_status_t gvfg_destroy(gvfg_handle handle);
```

釋放 GVFG capture session。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | `gvfg_create()` 回傳的 handle |

如果 capture 還在跑，destroy 時會先自動 stop / close。正常流程仍建議明確呼叫：

```c
gvfg_stop(handle);
gvfg_destroy(handle);
```

### gvfg_set_callbacks

```c
gvfg_status_t gvfg_set_callbacks(gvfg_handle handle,
                                 gvfg_on_frame_cb on_frame,
                                 gvfg_on_error_cb on_error,
                                 void *user);
```

設定 frame callback 和 error callback。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `on_frame` | 輸入 | frame callback，可為 `NULL` |
| `on_error` | 輸入 | error callback，可為 `NULL` |
| `user` | 輸入 | 客戶自訂 pointer，callback 時原樣帶回 |

### gvfg_set_frame_callback_interval

```c
gvfg_status_t gvfg_set_frame_callback_interval(gvfg_handle handle,
                                               uint32_t frame_interval);
```

設定 frame callback 的輸出頻率。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `frame_interval` | 輸入 | `0` 或 `1` 代表每張 frame 都 callback；`N > 1` 代表每 N 張 backend frame callback 一張 |

這只影響 `on_frame` callback，不影響 SDK 內建 preview、不影響 capture 速率，也不影響 backend 收 frame。

### gvfg_set_event_callback

```c
gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                      gvfg_on_event_cb on_event,
                                      void *user,
                                      uint32_t event_mask);
```

註冊 capture event callback。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `on_event` | 輸入 | event callback，可為 `NULL` |
| `user` | 輸入 | 客戶自訂 pointer，callback 時原樣帶回 |
| `event_mask` | 輸入 | `GVFG_EVENT_MASK_*` bit mask；傳 `0` 使用 `GVFG_EVENT_MASK_DEFAULT` |

event callback 是通知用途。熱插拔時的 pause/resume 由 SDK 自己處理，客戶不需要在 event callback 裡重啟 capture。

### gvfg_set_preview

```c
gvfg_status_t gvfg_set_preview(gvfg_handle handle, const gvfg_preview_desc_t *desc);
```

設定 SDK 內建 D3D preview helper。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `desc` | 輸入 | 客戶填好的 preview 設定，不可為 `NULL` |

這個 API 不是 core capture 必要流程。客戶如果自己畫畫面，可以省略。

一般建議在 `gvfg_start()` 前呼叫。capture running 中也可呼叫，用來更新 preview target 或 bit-depth request。

### gvfg_open

```c
gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);
```

開啟指定的 GVFG device。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `device_index` | 輸入 | `gvfg_enumerate_devices()` 得到的 device index |

呼叫成功後，SDK 會開啟指定裝置並讀取 input signal 資訊。

stream 解析度會依照 signal 自動調整。

### gvfg_start

```c
gvfg_status_t gvfg_start(gvfg_handle handle);
```

開始 GVFG streaming。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | 已經 open 的 session handle |

呼叫成功後，SDK 會啟動 capture worker。若 app 已設定 SDK 內建 preview，SDK 會建立 preview pipeline；若 app 已設定 frame callback，SDK 會依 callback interval 送出 frame。

preview 與 frame callback 彼此獨立；不使用 SDK 內建 preview 的 app 仍可透過 frame callback 取得 frame。

### gvfg_stop

```c
gvfg_status_t gvfg_stop(gvfg_handle handle);
```

停止 GVFG streaming。

即使 capture 沒有在執行，也可以呼叫；SDK 會做狀態檢查。

### gvfg_get_signal_status

```c
gvfg_status_t gvfg_get_signal_status(gvfg_handle handle,
                                     gvfg_signal_status_t *out_status);
```

取得 input signal 狀態。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `out_status` | 輸出 | SDK 寫入 signal status，不可為 `NULL` |

如果有有效 width / height，回傳 `GVFG_OK`。如果沒有有效 signal，回傳錯誤狀態。

### gvfg_get_runtime_info

```c
gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle,
                                    gvfg_runtime_info_t *out_info);
```

取得 core capture runtime 資訊。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `out_info` | 輸出 | SDK 寫入 runtime info，不可為 `NULL` |

適合顯示：

- input signal。
- SDK preview output 狀態。
- callback buffer 狀態。
- runtime FPS。
- 已送到 app callback 的 frame 數。

### gvfg_get_preview_info

```c
gvfg_status_t gvfg_get_preview_info(gvfg_handle handle,
                                    gvfg_preview_info_t *out_info);
```

取得 SDK 內建 preview helper 的詳細狀態。

| 參數 | 方向 | 說明 |
| --- | --- | --- |
| `handle` | 輸入 | session handle |
| `out_info` | 輸出 | SDK 寫入 preview info，不可為 `NULL` |

只有在客戶使用 `gvfg_set_preview()` 時才需要呼叫。

### gvfg_strerror

```c
const char *gvfg_strerror(gvfg_status_t status);
```

把 `gvfg_status_t` 轉成 static string pointer。客戶不需要也不能 free。

## 最小使用範例

```c
#include "gvfg_capture.h"
#include <stdio.h>

static void on_frame(const gvfg_frame_t *frame, void *user)
{
    (void)user;

    if (!frame || !frame->data)
        return;

    printf("frame %llu: %dx%d format=%d size=%llu\n",
           (unsigned long long)frame->frame_id,
           frame->width,
           frame->height,
           frame->pixel_format,
           (unsigned long long)frame->data_size);

    /*
      如果要存圖或交給其他 thread，這裡要 copy frame->data。
      callback return 後 frame->data 就不能再使用。
    */
}

static void on_error(gvfg_status_t status, const char *message, void *user)
{
    (void)user;
    printf("gvfg message %d: %s\n",
           (int)status,
           message ? message : "");
}

int start_xdma_preview(void *hwnd)
{
    gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {0};
    int device_count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);

    if (device_count <= 0) {
        printf("No GVFG device found\n");
        return -1;
    }

    gvfg_handle handle = NULL;
    gvfg_status_t st = gvfg_create(&handle);

    if (st != GVFG_OK) {
        printf("gvfg_create failed: %s\n", gvfg_strerror(st));
        return -1;
    }

    gvfg_set_callbacks(handle, on_frame, on_error, NULL);

    if (hwnd) {
        gvfg_preview_desc_t preview = {0};
        preview.hwnd = hwnd;
        preview.enable_preview = 1;
        preview.swapchain_bitdepth = GVFG_PREVIEW_BITDEPTH_AUTO;
        gvfg_set_preview(handle, &preview);
    }

    st = gvfg_open(handle, devices[0].index);
    if (st != GVFG_OK) {
        printf("gvfg_open failed: %s\n", gvfg_strerror(st));
        gvfg_destroy(handle);
        return -1;
    }

    st = gvfg_start(handle);
    if (st != GVFG_OK) {
        printf("gvfg_start failed: %s\n", gvfg_strerror(st));
        gvfg_destroy(handle);
        return -1;
    }

    /*
      handle 必須保存起來。
      capture running 期間不能 destroy。

      停止時：

      gvfg_stop(handle);
      gvfg_destroy(handle);
    */

    return 0;
}
```
