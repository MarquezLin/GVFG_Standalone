# GVFG 文件閱讀入口

更新於 2026-10-22；SDK 2.1.1。以下連結以目前 source tree 為準。

## 客戶與 Demo 使用者

1. [客戶 Capture 使用手冊](GVFG_CUSTOMER_API.md)：裝置、channel、影音 read/release 與事件。
2. [統一 Playback 使用手冊](GVFG_PLAYBACK_API.md)：一個播放 DLL、三種模式、單張顯示與舊版遷移。
3. [公開 API Reference](GVFG_Customer_doc/GVFG_CUSTOMER_API_REFERENCE.md)：所有 Capture API，以及 Playback config、輸入、狀態、stats、函式契約。
4. [Customer Sample README](../../Costomer_SDK_Release/GVFG_Customer_Sample/README.md)：Qt Creator／CMake 建置與 Demo 操作。

不需先讀 driver、WASAPI 或 D3D 才能使用公開 API。Demo 只展示 API，不實作同步演算法。

## SDK 維護與學習

1. [物件架構](GVFG_Overview.md)：Capture／Playback 兩種 handle、內部物件及 DLL／靜態 library。
2. [資料流與狀態](GVFG_FRAME_FLOW_AND_STATE.md)：借用 pointer、播放副本、read/release、重設與停止。
3. [Capture 錯誤處理](GVFG_ERROR_HANDLING.md)：Capture status／detail 與 ownership；播放錯誤另看 API Reference 第 4 節。
4. [Playback 實作](../GVFG_SDK/helpers/gvfg_playback/README.md)：PTS、master、共同緩衝、WASAPI 進度、漂移與過期預覽追趕。
5. [內部建置說明](INTERNAL_BUILD_GUIDE.md)：實際 target、Sample 三種 SDK 來源、install 與版本配套。
6. [內部設計筆記](GVFG_INTERNAL_NOTES.md)：Capture facade、vendor adapter 與內部診斷邊界。
7. [Viewer backend 邊界](../../srcroot/docs/BACKEND_BOUNDARIES.md)：GVFG、DirectShow、錄影各自的責任。

## 建議的原始碼追蹤順序

| 問題 | 從哪裡看 |
|---|---|
| 公開型別與函式 | `GVFG_SDK/sdk/gvfg/include/gvfg_capture.h`、`helpers/gvfg_playback/include/gvfg_playback.h` |
| Capture token／channel lifecycle | `GVFG_SDK/sdk/gvfg/src/gvfg_capture.cpp` |
| vendor frame／event | `GVFG_SDK/sdk/gvfg/src/gigabyte/gigabyte_capture_session.cpp` |
| 開始播放、queue、master、過期退休 | `GVFG_SDK/helpers/gvfg_playback/src/gvfg_playback.cpp` |
| Clock／sample-to-PTS／DriftFilter | `GVFG_SDK/helpers/gvfg_playback/src/playback_timing.h` |
| WASAPI 與 raw frame position | `GVFG_SDK/helpers/gvfg_audio_playback/src/` |
| GPU 上傳、呈現與完成通知 | `GVFG_SDK/helpers/gvfg_preview/src/gvfg_preview.cpp` |
| Demo 如何使用 API | `Costomer_SDK_Release/GVFG_Customer_Sample/src/playback.cpp`、`capture_controller.cpp` |

表內路徑分別相對 Standalone repo 或 SDK workspace；不是可依賴的公開 ABI。

## 怎麼判讀驗證結果

- source／文件核對：證明描述與所讀取的程式一致。
- build／export／相依檢查：證明指定環境可編譯連結，不等於硬體正常。
- 既有 120 秒 Qt 實機測試：驗證該情境的 FPS 與 queue 穩定，不等於小時級漂移或嘴型精度。
- 慢速輸出模擬：驗證過期預覽追趕與計數，不等於所有 GPU 的效能。

內部測試摘要放在 Playback 維護文件，客戶使用手冊不堆疊歷次除錯 log。
既有 PDF 是產生當時的快照；未依本次 Markdown 重製的 PDF 不可視為目前版本的最新說明。

讀任何成員時先問：誰擁有它、誰修改它、哪把鎖保護、資料何時失效、錯誤時是否還需 release。
