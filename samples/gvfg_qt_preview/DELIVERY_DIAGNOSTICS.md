# 關鍵交付 Log

畫面保留原本狀態，不顯示完整交付統計，也不增加設定控制。正常的預覽與音訊佇列取捨不輸出 Log。

- 預覽排隊逾時或容量不足是顯示節拍取捨，不輸出 Log。只有提交／渲染失敗或序號異常增加時，才記錄 `ERROR preview delivery`。
- Audio 播放佇列滿而淘汰舊 PCM 區塊不輸出 Log。只有序號異常或寫入失敗時，才記錄 `ERROR audio delivery`。
- 序號缺口、倒退／重複、音訊失敗位元組僅在有異常時附上。
- 錯誤最多每 5 秒彙整一次；Stop 會補記尚未輸出的錯誤。
- Stop 不列印正常統計摘要。只有資料去向核對不一致時才印 `ERROR delivery accounting mismatch`。正常停止清掉的待處理資料不算運行錯誤。

## 拿到 Log 後怎麼處理

`ERROR preview delivery` 表示提交／渲染失敗或 frame ID 異常，需查 Preview API、GPU 與來源序號。APP 持續讀取並釋放每張影像，沒有新增預覽降頻行為。

預覽使用 Present(0, 0)，讓 DXGI 等待呈現相依條件，不做外部重試。SyncInterval 仍為 0，並非每張都保證顯示。預覽改成 FIFO，最多保留 3 張待處理影像（包含正在上傳的容量保留），另有 1 個處理中槽位，共 4 個 GPU 槽。60 FPS 下 3 張約 50 ms；這不是刻意等滿 50 ms 才顯示。從上傳完成入列開始計時，等待超過 50 ms 的影像才丟棄（expired）；容量滿時跳過新影像（busy），不替換仍在期限內的舊影像。50 ms 是開始處理前的等待期限，不含 GPU 執行／DXGI Present 等待，不保證端到端延遲或實體螢幕逐張呈現。Stop／關閉／鎖等待只處理 Windows 同步送達的視窗訊息，避免 DXGI 等待 UI 而 UI 又等待 worker；不處理一般排隊輸入或 Qt callbacks。

Audio 保留原本最多 10 個 PCM 區塊的播放佇列（`kMaxQueuedAudioFrames`）；滿時丟掉最舊區塊，再加入新區塊。丟棄區塊、停滯與收音間隔仍保留為內部診斷計數，但不顯示在一般 Log；audio frame 不保證與 video frame 等長。

播放 write 失敗，或持續回傳 0、兩秒沒有進展，會記錄原因並停止通道；檢查音效裝置後重新 Start。此檢查無法中斷本身永不返回的 write 呼叫。

統計從 APP 成功取得 SDK frame 開始。SDK ID 連續不能證明更上游沒有丟失；DXGI 接受 Present 不代表實體螢幕已顯示，QAudioSink 接受 PCM 也不代表已實際發聲。已丟掉的原始資料無法靠這些處理補回。

擷取 API／驅動不修改。新版 APP 需配套的新版 gvfg_preview.dll。後續 GPU 優化修改了共用的轉換原始碼；若要發佈包含該優化的 SDK GPU buffer converter，也需重建其 DLL。尚待 build、播放／預覽異常測試與硬體長測；靜態檢查不能證明零丟失。
