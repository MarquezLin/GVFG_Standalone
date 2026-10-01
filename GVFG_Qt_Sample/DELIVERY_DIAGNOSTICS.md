# 關鍵交付 Log

畫面保留原本狀態，不顯示完整交付統計，也不增加設定控制。正常的預覽與音訊佇列取捨不輸出 Log。

- `[LIB]`：GigabyteLib 回傳的 signal 或 event 現象。
- `[SDK]`／`[SDK API]`：SDK read 邊界或 API 回傳的現象。
- `[APP]`：Preview、audio output 或 Sample 自身核對的現象。
- 累計數字最多每 5 秒彙整一次；Stop 會補記尚未輸出的變化。
- Stop 不列印正常統計摘要；只有實際 API、preview 或 audio output 錯誤才記錄。

## 拿到 Log 後怎麼處理

Log 只標示觀測層、現象與數值。Video/audio `frame_id` 是 SDK 每次 Start 從 1 開始的成功交付序號，不再使用 GigabyteLib `FrameCount`；`[APP] Preview ...` 是 Sample 顯示路徑問題。

預覽使用 Present(0, 0)，讓 DXGI 等待呈現相依條件，不做外部重試。SyncInterval 仍為 0，並非每張都保證顯示。預覽改成 FIFO，最多保留 3 張待處理影像（包含正在上傳的容量保留），另有 1 個處理中槽位，共 4 個 GPU 槽。60 FPS 下 3 張約 50 ms；這不是刻意等滿 50 ms 才顯示。從上傳完成入列開始計時，等待超過 50 ms 的影像才丟棄（expired）；容量滿時跳過新影像（busy），不替換仍在期限內的舊影像。50 ms 是開始處理前的等待期限，不含 GPU 執行／DXGI Present 等待，不保證端到端延遲或實體螢幕逐張呈現。Stop／關閉／鎖等待只處理 Windows 同步送達的視窗訊息，避免 DXGI 等待 UI 而 UI 又等待 worker；不處理一般排隊輸入或 Qt callbacks。

Audio 與 Customer Sample 使用相同的 `gvfg_audio_playback.dll` WASAPI helper。APP 先複製 SDK PCM、release SDK frame，再以 SDK timestamp 寫入 helper 的 bounded queue；queue full、輸出失敗、retry、recovery 與 Stop 統計都會寫入內部版 log。audio frame 不保證與 video frame 等長。

Helper create/start/write 失敗時會記錄 status、文字錯誤與 retry；非 queue-full 錯誤會重建 Windows 預設播放端點。APP 不因播放裝置暫時失敗而停止 SDK capture。

統計從 APP 成功取得 SDK frame 開始。SDK ID 連續不能證明更上游沒有丟失；DXGI 接受 Present、audio helper 接受 PCM 也不代表實體螢幕已顯示或喇叭已實際發聲。已丟掉的原始資料無法靠這些處理補回。

擷取 API／驅動不修改。新版 APP 需配套的新版 gvfg_preview.dll。後續 GPU 優化修改了共用的轉換原始碼；若要發佈包含該優化的 SDK GPU buffer converter，也需重建其 DLL。尚待 build、播放／預覽異常測試與硬體長測；靜態檢查不能證明零丟失。
