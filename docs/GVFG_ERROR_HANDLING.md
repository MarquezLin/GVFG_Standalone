# GVFG 擷取與播放錯誤處理

依目前公開標頭、gvfg_error_state.h、Capture facade 與 vendor adapter 整理。
Capture 與 Playback 是兩套 status，必須使用各自的 strerror，不可只看相同整數就混用。

## 1. Capture status

| 值 | 名稱 | 意義 |
|---:|---|---|
| 0 | GVFG_OK | 操作成功 |
| -1 | GVFG_EINVAL | 參數或 token 不符 |
| -2 | GVFG_ENODEV | 沒有裝置或無法開啟 |
| -3 | GVFG_ESTATE | channel／read／held token 狀態不允許操作 |
| -4 | GVFG_EIO | SDK／backend I/O、GPU 或內部 extension 失敗 |
| -5 | GVFG_ENOTSUP | 不支援的格式或功能 |
| -6 | GVFG_ETIMEOUT | 等待期限用完或 start 未取得有效 signal |
| -7 | GVFG_EBUSY | 裝置或資源忙碌 |

目前 GigabyteCaptureSession::from_vendor 將 OK 轉 GVFG_OK、DEV_BUSY 轉 EBUSY、
CONTEXT_ERROR 轉 ESTATE、VIDEO_CHN_INVALID 轉 EINVAL，其餘 vendor 失敗轉 EIO。
不要沿用曾經存在的 vendor 正數原樣回傳說明。

## 2. 詳細文字與內部 detail

ChannelErrorState 由 handle 逐 channel 持有，facade／adapter 使用同一份錯誤狀態，
mutex 保護 message 與 vendor detail。公開 detail 是客戶可理解的操作原因；
內部 vendor_detail 另保存 API、原始 code 與名稱，供 private debug API 使用。

公開程式可在 Capture 失敗時查 `gvfg_get_channel_last_sdk_error_detail()`，
搭配原始 API 名稱與 status。SDK 沒有宣告呼叫端 GetLastError() 就等於本次 SDK 錯誤；
不要在 DLL 返回後任意讀 Win32 last-error 作為 SDK 的權威原因。
正常 timeout／stop wake 不應當成永久故障，舊 detail 也不能自動配到下一次 timeout。

```cpp
gvfg_frame_t frame{};
const auto st = gvfg_read_channel_frame(handle, channel, &frame, 100);
if (st == GVFG_OK) {
    // 在這裡完成使用、複製或 submit Playback。
    const auto released = gvfg_release_channel_frame(handle, channel, &frame);
    // released 也需要檢查；不要先修改原 token。
} else if (st != GVFG_ETIMEOUT) {
    char detail[512]{};
    gvfg_get_channel_last_sdk_error_detail(handle, channel, detail, sizeof(detail));
    // 顯示 API 名稱、gvfg_strerror(st) 與公開 detail。
}
```

## 3. Playback status 與非同步錯誤

Playback 的錯誤型別是 gvfg_playback_status_t：OK=0、EINVAL=-1、ESTATE=-2、
EOUTPUT=-3、EQUEUE_FULL=-4、EDISCONTINUITY=-5。
例如 Capture 的 -4 是 EIO，Playback 的 -4 是 queue full；這正是不能混用 strerror 的原因。

submit 是接受／複製，不是實體輸出完成。非同步 GPU／WASAPI 失敗會保存為 last_error，
後續 submit 回報該錯誤；running 仍可能為 1。get_stats 回傳 OK 只是查詢成功。
目前輸出錯誤沒有自動重建裝置的保證，由 caller 協調停止與重啟。

| 情況 | ownership 與後續處理 |
|---|---|
| read timeout／失敗，沒有 frame | 不 release 未取得的 token |
| read 成功，submit 成功 | submit 完成複製，release 原始 token |
| read 成功，submit 拒收／失敗 | 仍 release 原始 token；若要重試，先保存應用程式自有副本 |
| Playback queue 壓力 | 自動進入獨立播放並略過過時資料；不因 queue 滿而停止擷取，單筆超出配置容量則是 EINVAL |
| Playback timestamp 不連續 | 協調 producer 後 reset；音訊格式改變用新 config stop/start |
| 過期預覽或 GPU busy 略過 | 計入 video_dropped，不當成 Capture failure |
| Playback EOUTPUT | 檢查裝置／視窗與輸出路徑，明確決定重啟，不無限重試 |

## 4. 生命週期與 Demo

關閉前先停止並 join producer，完成所有成功 read 的 release，再結束 Playback 與 Capture。
不要讓另一個 thread 在 handle destroy 後繼續呼叫 getter／submit。
reset/stop 是取消待播資料，不是錯誤重試或無損 drain。

Customer Demo 只顯示 API 名稱與必要公開錯誤；queue／PTS／driver counters 屬於另行選用的診斷。
內部分支的 Debug diagnostics 與 Release Demo 分開，不能把內部輸出當成公開 API 契約。
完整欄位與狀態限制見 [API Reference](GVFG_Customer_doc/GVFG_CUSTOMER_API_REFERENCE.md)，
操作流程見 [Playback 手冊](GVFG_PLAYBACK_API.md)。
