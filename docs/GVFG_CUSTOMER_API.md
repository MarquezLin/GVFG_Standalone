# GVFG 客戶 API 使用手冊

本文件說明客戶可使用的 GVFG 公開介面。唯一的公開核心標頭是
`sdk/gvfg/include/gvfg_capture.h`；選用預覽功能時才需要
`helpers/gvfg_preview/include/gvfg_preview.h`。

客戶套件不應包含 `gvfg_debug.h`、`sdk/gvfg/src/`、driver IOCTL、FPGA
register、IRQ 或 DMA ring 實作細節。

## 1. 套件內容

核心擷取：

```text
include/gvfg_capture.h
lib/gvfg.lib
bin/gvfg.dll
```

選用預覽：

```text
include/gvfg_preview.h
lib/gvfg_preview.lib
bin/gvfg_preview.dll
```

目前支援 Windows x64。API 使用 C ABI，可由 C 或 C++ 呼叫。

## 2. 最小擷取流程

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_set_zero_copy_enabled (optional, before open)
-> gvfg_open_channel
-> gvfg_start
-> 重複：
   gvfg_read_frame
   使用／複製／轉換 frame
   gvfg_release_frame
-> gvfg_stop
-> gvfg_destroy
```

完整範例：

```c
#include <gvfg_capture.h>

int main(void)
{
    gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {0};
    int count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);
    if (count <= 0)
        return 1;

    gvfg_handle h = NULL;
    if (gvfg_create(&h) != GVFG_OK)
        return 2;

    gvfg_status_t st = gvfg_open_channel(h, 0, GVFG_CHANNEL_0);
    if (st == GVFG_OK)
        st = gvfg_start_channel(h, GVFG_CHANNEL_0);

    if (st == GVFG_OK) {
        for (int i = 0; i < 100; ++i) {
            gvfg_frame_t frame = {0};
            st = gvfg_read_channel_frame(h, GVFG_CHANNEL_0, &frame, 1000);
            if (st == GVFG_ETIMEOUT)
                continue;
            if (st != GVFG_OK)
                break;

            /* 在 release 前處理、轉換或複製 frame.data。 */

            st = gvfg_release_channel_frame(h, GVFG_CHANNEL_0, &frame);
            if (st != GVFG_OK)
                break;
        }
    }

    gvfg_stop(h);
    gvfg_destroy(h);
    return st == GVFG_OK ? 0 : 3;
}
```

## 3. 執行緒與 frame 所有權

- API 是 pull model；應用程式自行決定在哪個執行緒呼叫
  `gvfg_read_channel_frame()`。GUI 程式建議每個 channel 使用一個 worker thread，再把 UI 更新送回
  UI thread。
- Copy mode 的 `frame.data` 由 SDK 擁有；zero-copy mode 則指向 driver-owned
  buffer。兩種模式都只在對應的 `gvfg_release_channel_frame()` 前有效，資料若要長期
  保存，必須先複製。
- 每個 channel 同時最多持有一個 frame。尚未 release 又對同一 channel 呼叫
  `gvfg_read_channel_frame()`，會回傳 `GVFG_ESTATE`。
- release 時必須傳回原本的完整 `gvfg_frame_t`，不可修改欄位。
- `gvfg_stop()` 會中止等待並使尚未 release 的 frame 失效。
- 同一個 handle 的 lifecycle 操作應由應用程式自行序列化；不要同時 open、
  start、stop 或 destroy。

`timeout_ms` 的共同規則：`0` 表示不等待；`GVFG_TIMEOUT_INFINITE` 表示無限等待；
其他值的單位為毫秒。

## 4. 狀態碼

| 狀態              | 說明                          |
| --------------- | --------------------------- |
| `GVFG_OK`       | 成功                          |
| `GVFG_EINVAL`   | 參數、channel 或 frame token 無效 |
| `GVFG_ENODEV`   | 找不到裝置或裝置無法開啟                |
| `GVFG_ESTATE`   | 呼叫時機或 handle 狀態不正確          |
| `GVFG_EIO`      | driver/backend I/O 失敗       |
| `GVFG_ENOTSUP`  | 格式或功能不支援                    |
| `GVFG_ETIMEOUT` | 等待逾時                        |

可用 `gvfg_strerror()` 取得靜態英文說明字串；呼叫端不可釋放該字串。

可用 `gvfg_get_version()` 查詢目前實際載入的 `gvfg.dll` 版本。回傳值為
靜態字串，例如 `"0.1.0"`，呼叫端不可釋放。
需要記錄最近一次失敗的詳細原因時，可在 API 失敗後立即呼叫
`gvfg_get_channel_last_error_detail()`；driver/register 等內部診斷仍保留在 debug API。

## 5. 資料格式

### 原生擷取格式

| 格式                | `pixel_format`     | bit depth | 每列大小        |
| ----------------- | ------------------:| ---------:| -----------:|
| YUY2 packed 4:2:2 (`Y0 U0 Y1 V0`) | `GVFG_PIXFMT_YUY2` | 8 | `width * 2` |
| Y210 packed 4:2:2 | `GVFG_PIXFMT_Y210` | 10        | `width * 4` |

實際列距以 `gvfg_frame_t.row_stride_bytes` 為準。
目前不支援的輸入格式會回傳 `GVFG_ENOTSUP`。可用
`gvfg_pixel_format_name()` 取得靜態格式名稱。

### `gvfg_frame_t`

- `data`、`data_size`：SDK buffer 與可用 byte 數。
- `width`、`height`：影像尺寸。
- `row_stride_bytes`：相鄰兩列起點的 byte 距離。
- `pixel_format`、`bit_depth`：原生 payload 格式。
- `frame_id`：同一次 start/stop session 中單調遞增的識別值。

## 6. 公開 API

### 裝置與生命週期

- `gvfg_enumerate_devices(out_devices, max_devices)`：列舉裝置。傳入
  `NULL, 0` 可只查數量；回傳值是寫入數量或可用裝置數，無裝置時為 `0`。
- `gvfg_create(&handle)`：建立 closed session。
- `gvfg_set_zero_copy_enabled(handle, enabled)`：選用 zero-copy；只能在
  `gvfg_open_channel()` 前呼叫，預設為關閉。
- `gvfg_get_zero_copy_enabled(handle, &enabled)`：查詢 session 選擇的模式。
- `gvfg_open_channel(handle, device_index, channel)`：開啟列舉所得裝置，channel
  必須為 `GVFG_CHANNEL_0` 或 `GVFG_CHANNEL_1`。
- `gvfg_set_channel_event_mask(handle, channel, mask)`：在 open 前設定指定 channel
  要註冊及通知的事件；預設 `GVFG_EVENT_MASK_ALL`。DMA event 為擷取必要項目，
  不受 mask 控制。
- `gvfg_start_channel(handle, channel)`：開始指定 channel 擷取。若目前無訊號，成功進入訊號監看模式；此時
  frame read 會 timeout，訊號接上後 SDK 會自動開始擷取。
- `gvfg_stop(handle)`：停止擷取；重複呼叫仍回傳成功。
- `gvfg_destroy(handle)`：必要時先停止，再銷毀 handle。銷毀後不得再使用。

Zero-copy mode 的 driver lifecycle 由 SDK 管理：open 時 enable，每次成功
`gvfg_read_channel_frame()` 後由 `gvfg_release_channel_frame()` 歸還 driver frame，destroy/close
前 disable。Device open 後不可直接切換；應 destroy session、重新 create、設定模式
後再 open。

### Frame

- `gvfg_read_channel_frame(handle, channel, &frame, timeout_ms)`：取得一個 frame；ownership 依 copy/
  zero-copy mode 而定，但 release contract 相同。
- `gvfg_release_channel_frame(handle, channel, &frame)`：釋放原 frame token。

`gvfg_set_channel_video_format()` 只能在指定 stream 尚未開始或已 stop 時呼叫；streaming 中
切換會回傳 `GVFG_ESTATE`。應用程式應在 UI 上同步鎖定格式選項。

### 查詢

- `gvfg_get_channel_signal_status(handle, channel, &status)`：查詢指定 channel 的連線、尺寸、
  原生格式與 bit depth。沒有輸入訊號是正常狀態：回傳 `GVFG_OK` 且
  `connected == 0`。
- `gvfg_get_channel_runtime_info(handle, channel, &info)`：取得指定 channel 已交付 frame 數、SDK 能確定的
  `lost_frames` 與依 read 間隔估算的 `capture_fps`。start 時統計值重設。

### Event

`gvfg_poll_channel_event(handle, channel, &event, timeout_ms)` 一次取出指定 channel 的事件，只能在 running
狀態使用。stop 會喚醒阻塞中的 poll，並回傳 `GVFG_ESTATE`。

| Event                            | 應用程式動作                      |
| -------------------------------- | --------------------------- |
| `GVFG_EVENT_SIGNAL_CONNECTED`    | 訊號已接上；等待 stream ready/frame |
| `GVFG_EVENT_SIGNAL_DISCONNECTED` | 停止使用目前影像內容                  |
| `GVFG_EVENT_FORMAT_CHANGE_BEGIN` | 暫停使用依賴舊尺寸／格式的資源             |
| `GVFG_EVENT_STREAM_READY`        | 第一個完整 frame 已就緒，可依新格式重建資源   |
| `GVFG_EVENT_FRAME_LOSS`          | 記錄錄影內容可能不完整；`event.count` 是本次已知 loss 數量 |

呼叫前應將 `gvfg_event_t` 清零並設定 `struct_size = sizeof(gvfg_event_t)`。
事件是狀態通知，不取代 `gvfg_get_channel_signal_status()`；需要完整 metadata 時應重新查詢。

## 7. GPU 同步轉換

GPU 轉換在 `gvfg.dll` 內完成，呼叫是同步的。必須在
`gvfg_release_channel_frame()` 前轉換；函式返回後 SDK 不保留 source 或 destination
pointer。

支援輸出：

| 格式                        | 配置需求                                    |
| ------------------------- | --------------------------------------- |
| `GVFG_GPU_OUTPUT_BGRA8`   | `row_bytes >= width * 4`；alpha = 255    |
| `GVFG_GPU_OUTPUT_RGB10A2` | `row_bytes >= width * 4`；alpha = 3      |
| `GVFG_GPU_OUTPUT_NV12`    | width/height 必須為偶數；`row_bytes >= width` |

BGRA8/RGB10A2 的 destination 至少為 `row_bytes * height`。NV12 至少為
`row_bytes * (height + height / 2)`，排列為 Y plane，接著是 interleaved UV
plane；使用 BT.709 limited range。

可使用通用函式 `gvfg_gpu_convert_to_buffer()`，或便利函式：

- `gvfg_gpu_convert_to_bgra8()`
- `gvfg_gpu_convert_to_rgb10a2()`
- `gvfg_gpu_convert_to_nv12()`

## 8. 選用預覽 helper

`gvfg_preview.dll` 是同步顯示 helper，不擁有 capture handle，也不依賴
`gvfg.dll`。典型流程：

```text
gvfg_read_frame
-> 將 gvfg_frame_t 欄位對應到 gvfg_preview_frame_t
-> gvfg_preview_render_frame
-> gvfg_release_frame
```

建立後用 `gvfg_preview_attach_window()` 傳入 Windows `HWND`。render 返回前
frame memory 必須保持有效。`gvfg_preview_get_stats()` 的 `present_fps` 只計算
DXGI 成功接受的 Present；swapchain busy 而略過者記在 `skipped_presents`。

## 9. 錯誤處理建議

- `GVFG_ETIMEOUT`：通常可重試，並檢查 signal status/event。
- `GVFG_EVENT_FORMAT_CHANGE_BEGIN`：先停用舊格式資源；等 stream ready 後重建。
- `GVFG_ESTATE`：檢查 lifecycle、是否重複 read、或是否已 stop。
- `GVFG_EIO`／`GVFG_ENODEV`：停止 session，記錄 `gvfg_strerror()`，再由應用程式
  決定是否重新列舉與開啟。
- 不要讀寫硬體 register 或依賴 driver/backend counter；這些不是客戶 API。

## 完整 API Reference

所有公開常數、型別、結構欄位、函式參數與回傳值，請參閱
[`GVFG_CUSTOMER_API_REFERENCE.md`](GVFG_CUSTOMER_API_REFERENCE.md)。
