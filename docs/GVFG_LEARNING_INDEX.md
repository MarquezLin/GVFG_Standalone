# GVFG SDK 學習文件入口

這組文件以目前 `GVFG_Standalone` 工作目錄的實作為準，目的是協助自己理解 C++、追蹤 SDK 架構，以及記錄目前已完成與尚未驗證的內容。

## 建議閱讀順序

1. [`GVFG_Overview.md`](GVFG_Overview.md)
   - 先認識 `gvfg_handle_t`、`gvfg_channel_session_t`、`PcieS2mmCaptureSession`。
   - 理解 `unique_ptr`、`shared_ptr`、宣告與實際配置的差異。
   - 理解 CH0、CH1 如何共用同一個 Windows device handle。
2. [`GVFG_ERROR_HANDLING.md`](GVFG_ERROR_HANDLING.md)
   - 理解公開 status、詳細錯誤字串與 Win32 `GetLastError()` 的分工。
   - 理解 facade 與 backend 如何寫入同一份 `ChannelErrorState`。
   - 理解哪些失敗可以 retry，哪些必須立即回傳。
3. [`GVFG_FRAME_FLOW_AND_STATE.md`](GVFG_FRAME_FLOW_AND_STATE.md)
   - 從 `gvfg_start_channel()` 一路追到 driver/DMA，再回到 application。
   - 比較 copy 與 zero-copy 的資料位置及 ownership。
   - 理解 read/release、stop、signal event、format change 的狀態變數。

其他用途不同的既有文件：

- [`GVFG_CUSTOMER_API.md`](GVFG_CUSTOMER_API.md)：客戶使用流程與規則。
- [`GVFG_CUSTOMER_API_REFERENCE.md`](GVFG_CUSTOMER_API_REFERENCE.md)：公開 API 逐項參考。
- [`GVFG_INTERNAL_NOTES.md`](GVFG_INTERNAL_NOTES.md)：內部模組邊界、打包與維護筆記。
- [`INTERNAL_BUILD_GUIDE.md`](INTERNAL_BUILD_GUIDE.md)：建置與診斷方式。

## 目前程式改到哪裡

更新日期：2026-08-27。

目前已實作：

- 一個 `gvfg_handle_t` 可管理 CH0、CH1。
- CH0、CH1 共用一個 `PcieS2mmDeviceConnection`，底層只有一個 Windows device `HANDLE`。
- 每條 channel 各自擁有 backend、event、capture thread、copy buffer 與 frame lifetime state。
- 公開 API 已改為 channel-aware：open/start/read/release/poll/stop 都帶 `channel_index`。
- copy mode 直接把 driver frame 寫入該 channel 的 `copy_buffer_`。
- zero-copy mode 保存 driver-owned pointer，成功 read 必須配對一次 release。
- `ChannelErrorState` 由 `gvfg_handle_t` 逐 channel 持有，facade/backend 共用同一份詳細錯誤。
- `firstOpenChannel()` 與「第二條 channel 從第一條借 backend」的 ownership 已移除。
- 重複的 `device_`、`opened_`、device path/name session 狀態已移除；`device_connection_` 是 device handle 的唯一來源。
- `wait_frame()` 只對 transient not-ready error 在原始 deadline 內重試，不使用無界 retry 或 `Sleep()`。

目前驗證範圍：

- 已做 source 搜尋與 `git diff --check`。
- 本輪未執行 build。
- 尚未在硬體上驗證 CH0、CH1 同時 start/read/release。
- 尚未用實際拔插、格式切換、stop-with-held-frame 覆蓋所有狀態轉移。

## 閱讀程式碼的方式

遇到一個成員變數時，依序問：

1. 它屬於哪個物件？
2. 該物件由誰擁有？
3. 它只是管理物件，還是真的影像資料？
4. 哪個函式第一次設定它？
5. 哪個函式清除它？
6. 多執行緒存取時由 mutex、atomic 或其他規則保護？
7. 發生錯誤時，狀態會保留供 retry，還是立即清除？

閱讀 frame lifetime 時，最重要的是追蹤：

```text
read 成功
    -> data pointer 交給 application
    -> frameHeld/frame_held_ = true
    -> application 同步使用或複製
    -> release 成功
    -> held state 清除
```

不要只看 pointer 指向哪裡，也要看誰有權決定它何時失效。
