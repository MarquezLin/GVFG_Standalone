# GVFG 統一影音播放使用手冊

適用於 SDK 2.1.1。應用程式使用 `gvfg_capture.h` 擷取資料，使用
`gvfg_playback.h` 播放；時鐘、共同緩衝及影音同步由 Playback 管理。

## 1. 連結與部署

| 用途 | MSVC 匯入庫 | 執行期 DLL |
|---|---|---|
| 裝置、channel、影音擷取 | `gvfg.lib` | `gvfg.dll` |
| 影像、音訊、影音同步、單張影像顯示 | `gvfg_playback.lib` | `gvfg_playback.dll` |

MinGW 使用對應的 `libgvfg.dll.a`、`libgvfg_playback.dll.a`。
播放功能只有一個 DLL；擷取仍有自己的 DLL。Windows 與 C/C++ runtime 相依仍須滿足。

公開標頭需一起交付：`gvfg_capture.h`、`gvfg_playback.h`、`gvfg_preview.h`、
`gvfg_audio_playback.h`。後兩個由 Playback 標頭引入，提供影像、PCM 格式及統計型別。

## 2. 選擇模式

先以 `gvfg_playback_default_config()` 初始化 config，再修改需要的欄位。

同步預設開啟；可在 create 後、start 前，或播放途中選擇：

```cpp
gvfg_playback_set_sync_enabled(player, 0); // 影音各自播放，不自動回到同步。
gvfg_playback_set_sync_enabled(player, 1); // 啟用同步；播放途中會先等待來源與輸出穩定。
```

兩行代表不同選擇，擇一呼叫並檢查回傳值。此設定會保留到後續 stop/start 與 reset_timeline。
關閉同步時仍有有界緩衝與過時資料清理；不需要由應用程式調整底層影音輸出。

| `mode` | 資料需求 | 有效 master | 音訊輸出 |
|---|---|---|---|
| `GVFG_PLAYBACK_VIDEO_ONLY` | 持續提交影像 | 強制 External | 不建立 |
| `GVFG_PLAYBACK_AUDIO_ONLY` | 持續提交 PCM | 強制 Audio | 建立 |
| `GVFG_PLAYBACK_AUDIO_VIDEO` | 兩路都須持續提交 | External／Audio／Video，預設 External | 建立 |

純音訊不需 HWND。`attach_window(nullptr)` 是隱藏畫面，仍須提交啟用模式所需的影像；
它不會把影音模式改成純音訊。`set_volume(0)` 是靜音，仍須提交 PCM，音訊時間軸繼續運行。

共同 PTS 說明資料屬於哪個時間；master 決定「現在播到哪裡」。相同時間軸不代表兩個
實際輸出裝置的速度與延遲相同。External 用單調系統時鐘排程；Audio 用估計音訊輸出進度；
Video 用最近成功呈現的影像時間及經過時間。Video master 不保證 GPU 能呈現每張輸入影格。

## 3. 建立與啟動

以下為 C++ 初始化範例；`format` 應由 Capture API 查詢取得，不應假設所有輸入都是 48 kHz。

```cpp
#include "gvfg_capture.h"
#include "gvfg_playback.h"

gvfg_playback_status_t BeginPlayback(
    gvfg_playback *out, void *hwnd, const gvfg_audio_format_t &format,
    bool enableAudio)
{
    if (!out) return GVFG_PLAYBACK_EINVAL;
    *out = nullptr;
    gvfg_playback p = nullptr;
    auto st = gvfg_playback_create(&p);
    if (st != GVFG_PLAYBACK_OK) return st;
    if (hwnd) st = gvfg_playback_attach_window(p, hwnd);
    if (st == GVFG_PLAYBACK_OK) {
        gvfg_playback_config_t c{};
        gvfg_playback_default_config(&c);
        c.mode = enableAudio ? GVFG_PLAYBACK_AUDIO_VIDEO : GVFG_PLAYBACK_VIDEO_ONLY;
        if (enableAudio)
            c.audio_format = {format.sample_rate,
                static_cast<uint16_t>(format.channels),
                static_cast<uint16_t>(format.bits_per_sample)};
        st = gvfg_playback_start(p, &c);
    }
    if (st != GVFG_PLAYBACK_OK) {
        gvfg_playback_destroy(p);
        return st;
    }
    *out = p;
    return GVFG_PLAYBACK_OK;
}
```

一般順序是：建立 Capture handle、選擇／開啟 channel、設定 audio enable、查詢格式、
成功 start capture、建立／綁定／start Playback，最後啟動應用程式的影音讀取執行緒。
任何步驟失敗都須回收已建立資源。Playback 不會代替應用程式開啟或停止擷取裝置。

## 4. 提交影像與 release

`gvfg_frame_t` 與 `gvfg_preview_frame_t` 是不同型別，pixel format 必須明確對應。
不要直接假設兩個 enum 的整數值相同。

```cpp
bool SubmitOneVideo(gvfg_handle capture, int channel, gvfg_playback player)
{
    gvfg_frame_t source{};
    const auto read = gvfg_read_channel_frame(capture, channel, &source, 100);
    if (read == GVFG_ETIMEOUT) return true; // 本次無 frame，沒有 release
    if (read != GVFG_OK) return false;

    gvfg_playback_video_frame_t v{};
    v.image.data = source.data;
    v.image.data_size = source.data_size;
    v.image.width = source.width;
    v.image.height = source.height;
    v.image.bit_depth = source.bit_depth;
    v.image.row_bytes = source.row_stride_bytes;
    v.image.frame_id = source.frame_id;
    v.timestamp_ns = source.timestamp_ns;
    switch (source.pixel_format) {
    case GVFG_PIXFMT_YUY2: v.image.pixel_format = GVFG_PREVIEW_PIXFMT_YUY2; break;
    case GVFG_PIXFMT_Y210: v.image.pixel_format = GVFG_PREVIEW_PIXFMT_Y210; break;
    default: v.image.pixel_format = 0; break;
    }
    const auto submit = gvfg_playback_submit_video(player, &v);
    const auto release = gvfg_release_channel_frame(capture, channel, &source);
    return submit == GVFG_PLAYBACK_OK && release == GVFG_OK;
}
```

成功 read 的原始 token 必須恰好 release 一次，包含 submit 失敗路徑。
submit 成功表示已完成複製並接受排程，不表示已經顯示。Playback 不長期持有 Capture frame。

## 5. 提交 PCM 與 release

```cpp
bool SubmitOneAudio(gvfg_handle capture, int channel, gvfg_playback player)
{
    gvfg_audio_frame_t source{};
    const auto read = gvfg_read_channel_audio_frame(capture, channel, &source, 100);
    if (read == GVFG_ETIMEOUT) return true;
    if (read != GVFG_OK) return false;
    const auto submit = gvfg_playback_submit_audio(
        player, source.data, source.data_size, source.timestamp_ns);
    const auto release = gvfg_release_channel_audio_frame(capture, channel, &source);
    return submit == GVFG_PLAYBACK_OK && release == GVFG_OK;
}
```

目前接受 signed interleaved PCM16；bytes 必須對齊 `channels * 2`，每包最多 100 ms。
應用程式可像範例一樣直接 submit 後 release，也可先複製、release，再 submit 自有副本。
兩種方式都必須在 release 前完成對 SDK 記憶體的使用。

## 6. 時間戳、緩衝與落後

- 直接傳入 SDK 的 `timestamp_ns`；它是同一單調交付時鐘的奈秒值，不是 UTC，也不是已知的硬體曝光／收音時間。
- 各路時間必須大於 0、嚴格遞增且不超過 `INT64_MAX`。不要把第一筆音影差值歸零來掩蓋未知延遲。
- 第一包 PCM 以交付時間扣除包長估計起點，後續維持連續時間軸，並平滑追蹤與交付時鐘的長期偏差。無法辨認驅動內的未知舊資料或遺失 samples。
- 預設共同 preroll 為 100 ms，以已收到的啟用串流時間覆蓋範圍判斷。缺資料時只短暫等待，超時會降級為獨立播放；正常排程的影像保留一張 lookahead。
- 正常同步時 External／Audio master 會略過已過播放期限的預覽影格，保留最新到期影格；GPU busy 的略過也會計數。Video master 在正常同步時不啟用這個自動過期退休策略。
- `video_dropped` 是預覽略過數，不是 Capture／driver 遺失數，也不是錄影掉格數。
- 微幅音訊速率補償有上限；持續缺資料、未知底層延遲或大幅落後，不能保證由補償自動修復。

預設上限為影像 32 張／256 MiB、音訊 1000 ms。queue 壓力、長時間缺資料或影音偏差過大時，
Playback 自動降級：影像持續顯示最新可用影格，音訊以正常速率播放並丟棄積壓的待播資料。
沒有音訊資料時保持靜音，不會因為等待音訊而持續凍結影像；資料不足仍可能聽見音訊間斷。
來源與輸出穩定後會自動恢復同步，重新對齊時可能短暫保留目前畫面。
`sync_degraded`、`sync_fallbacks`、`audio_dropped_frames` 可供整合工具查詢；應用程式不需實作降級控制。
submit 返回 OK 表示資料已由播放器管理，包含略過顯示或丟棄待播副本的情況；不保證每筆都輸出。
單張影像／單包 PCM 超出其設定容量屬於 `EINVAL`，應修正 config 或輸入大小。

## 7. 執行緒、重設與停止

一個 handle 可由一個 video producer 與一個 audio producer 同時 submit。
start、stop、reset、attach、present_image、destroy 等控制操作由 caller 序列化，
並在控制操作前停止／協調 producer；HWND 操作在其所屬 UI 執行緒進行。

停止順序：設停止旗標 → join 影音讀取執行緒 → 確認所有成功 read 都已 release →
Playback stop/destroy → Capture stop；最後一個 channel 結束後才 destroy 共用 Capture handle。
視窗應存活到使用它的播放器結束。

遇到時間倒退、重連或切格式，先協調 producer，再檢查 `reset_timeline()` 回傳值。
reset 會清除待播資料並沿用原 config 重新建立時間軸；音訊格式改變時應 stop/start 新 config。
尚未建立呈現表面（swapchain）時，清除空畫面視為成功，reset 後仍會重新啟動原本運行的時間軸。
已建立的影像輸出若真的清除失敗，reset 仍可能回報 `EOUTPUT`，不能假設一定成功。

stop/reset 都會取消待播資料，沒有無損 EOS/drain 語意；有限檔案的尾幀完整輸出不是目前 API 的保證。
影音模式遇到音訊輸出故障時，helper 停用監聽音效並繼續播放影像，
後續音訊提交回傳 `EAUDIO`；caller 可提示一次警告，繼續擷取與錄影音訊。
包括啟動時音效裝置無法開啟的情況，影音 start 仍可成功、由音訊提交回報 `EAUDIO`。
此狀態保留至 handle 銷毀，stop/reset 不會重試音效；重建 handle 才會再嘗試。
純音訊模式沒有影像可保留，輸出故障仍回報 `EOUTPUT`。
其他 `EOUTPUT` 應檢查 GPU／視窗／記憶體等輸出問題，依錯誤停止或重啟。

## 8. 單張 RAW／DICOM 影像

在 create、attach_window 成功後，用 `gvfg_playback_present_image(player, &image)`；
不要 start。它不需要 PTS，也不啟動音訊，會同步等待已提交的預覽工作完成。
輸入資料須保持有效到返回；只允許停止狀態且已綁定視窗的 handle。
GPU busy 可能略過呈現，OK 不等於已完成實體螢幕掃描。

## 9. 範例與 API Reference

NO SIGNAL 圖卡由 Playback helper 內建與顯示，應用端只需轉交 Capture 的訊號狀態：

```cpp
gvfg_signal_status_t signal{};
if (gvfg_get_channel_signal_status(capture, channel, &signal) == GVFG_OK) {
    auto status = gvfg_playback_set_signal_connected(player, signal.connected ? 1 : 0);
    // 檢查 status；失敗時使用 gvfg_playback_strerror(status)。
}
```

在綁定 HWND 的 UI 執行緒呼叫，與 attach/destroy 序列化。0 顯示固定圖卡，1 隱藏。
預設為 1；可先設定再 attach，狀態保留至下次明確設定，stop/start/reset 不改變它。
視窗 resize 與圖卡資源由 helper 處理，不需 Qt 圖片資源或應用端繪圖。
此 API 不讀 Capture、不以 FPS 或黑色像素推測訊號，也不改影格內容、PTS 或錄影。
若未使用此 API，維持一般預覽行為。destroy/detach 也應在 HWND 執行緒執行。

Customer Demo 展示 API 的啟停、提交、release、事件及基本錯誤處理。

應用程式透過 `gvfg_playback_*` 控制播放：獨立預覽使用 video-only，
單張顯示使用 present_image。同步控制由 Playback 處理。
更新 SDK 時，標頭、匯入庫、DLL 與 CMake 應使用同一版本。

逐欄位限制與所有函式見 [公開 API Reference，第 4 節](GVFG_Customer_doc/GVFG_CUSTOMER_API_REFERENCE.md#4-統一-playback-api)。
