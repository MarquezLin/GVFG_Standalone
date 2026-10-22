# Playback 維護與同步實作

SDK 2.1.1；客戶整合從 [公開播放手冊](../../../docs/GVFG_PLAYBACK_API.md) 開始，
逐參數契約見 [API Reference](../../../docs/GVFG_Customer_doc/GVFG_CUSTOMER_API_REFERENCE.md)。
本文件供 SDK 維護者，說明目前實作，不作為 Customer Demo 的除錯畫面需求。

`signal_card.cpp` 管理 UI 執行緒上的原生 child overlay；圖片以 bitmap resource
內嵌 DLL，不相依 Qt 或外部圖片。`set_signal_connected()` 只轉交訊號狀態，
不介入播放 worker。Parent subclass 處理 resize／銷毀，detach/destroy 回收 overlay。
預設有訊號；未呼叫時不建立圖卡視窗。相關 API 與 HWND lifecycle 由 UI 執行緒序列化。

## 1. 模組與二進位邊界

```text
Application: read Capture frame → submit Playback → release Capture token
                              ↓
gvfg_playback.dll
  gvfg_playback_t: 模式、共同狀態、queue、Clock、AudioTimeline、DriftFilter
        ↓                                   ↓
gvfg_preview STATIC                   gvfg_audio_playback STATIC
  D3D upload / conversion / Present     PCM / WASAPI / raw frame position
```

Playback 是 SHARED target；兩個 backend 是 PRIVATE 連結的 STATIC target，只有 Playback 函式匯出。
`.def` 在本目錄。客戶標頭包含 backend header 只是重用資料型別，不對外匯出 backend 函式。
Capture DLL 與 Playback DLL 沒有互相接管 handle，SDK source 仍依 target 各自維護。

## 2. 物件與執行緒

| 位置 | 責任 |
|---|---|
| `src/gvfg_playback.cpp` | C ABI、參數驗證、queue、共同 worker、排程與控制 |
| `src/playback_timing.h` | sample_time、Clock、AudioSourceClock、AudioTimeline、DriftFilter |
| `../gvfg_audio_playback/src/audio_output_internal.h` | 同 DLL 內的私有 raw position／pause／rate bridge |
| `../gvfg_preview/src/gvfg_preview.cpp` | D3D upload slots、呈現 worker、完成通知與統計 |

應用程式可有一個 video 與一個 audio producer，submit 時短暫持有 core mutex 並複製。
共用 worker 決定送給 WASAPI 的 PCM 與到期影像；各 backend 另有自己的 worker。
控制 API 不由 Demo 重新實作時鐘。caller 必須序列化控制操作，協調 producer 與 handle／HWND lifetime。

## 3. SDK timestamp 與來源 PTS

Capture video／audio timestamp 來自 SDK 的 steady_clock 交付時刻，時鐘域相同；
不是硬體曝光／收音 timestamp，也不是 UTC。兩路各自嚴格遞增。
影像沿用此值；第一包音訊扣除包長估起點，後續由 AudioSourceClock 建立連續 spans。
名義 sample 數仍提供基礎時長；與 delivery clock 的誤差使用約 500 ms 平滑、5 ms 容忍區，
每包來源時長最多伸縮 0.25%。此 mapping 不丟棄 PCM，也不把 callback jitter 直接套到每包 PTS。
AudioTimeline 將實際 consumed frames 依 span 的起訖 PTS 插值，輸出調速仍由 DriftFilter 處理。
若忽略來源時鐘偏差，只把第一包 PTS 加上永久累計 sample 數，較慢供應會讓 audio horizon
逐漸落後 delivery timestamp，進入共用 buffering 並累積影像佇列；較快供應則可能堆積音訊。
起點並不是把第一筆音影相減後設為永久零誤差，未知 pipeline 延遲仍存在。

`sample_time` 先做整數商與餘數，避免長時間 frames × 1e9 先溢位。
AudioTimeline 在 PCM 成功送入低階輸出時保存來源 spans；先配置 mapping，拒收則 rollback。
Audio backend 只回報 played_frames 與 queued_frames，不知道 video 或來源 PTS。

## 4. Clock 與 master

| 模式 | master |
|---|---|
| Video-only | 強制 External |
| Audio-only | 強制 Audio |
| Audio-video | config 選 External／Audio／Video，預設 External |

Clock 保存 PTS 錨點、steady wall time 與 paused。External 在錨點上加單調經過時間；
Video 在成功呈現或隱藏消化後更新，再於影格間推進。
Audio 以 written_frames 減 WASAPI padding 估算 consumed frames，再由 AudioTimeline 映射來源位置。
它不是實體 speaker 的聲學時鐘；Present 也不代表 scanout 已完成。

共同 PTS 只定義座標；master 決定追趕哪個播放進度。設定 Video master 不會讓缺少的 PCM 提早產生，
也不保證任意 GPU 能以輸入速率呈現全部 frame。

## 5. 緩衝與排程

1. 等所有啟用串流的第一筆資料，取較早來源起點建立 paused clocks。
2. 在仍暫停時完成首次 GPU prepare，避免資源建立消耗正在播放的小段 audio lead。
3. 以 video 最後來源時間及 audio 已接受 sample 結尾的共同 horizon 判斷 preroll，預設 100 ms。
4. 覆蓋足夠後開始；video 保留一張 lookahead，不假設有限資料最後一幀的 duration。
5. 每輪把 audio 補到約 40 ms lead；不是每張影像只准送一包 10 ms PCM。
6. 依 master 提交到期影像；若下一張也已到期，External／Audio master 退休舊張，保留最新到期影格。
7. 若 horizon 用盡或音訊真正 starvation，短暫重新緩衝；超時後改為獨立播放。

過期退休也可在 producer 送入時執行，避免共用 worker 暫時等 GPU 時讓舊預覽堆滿 queue。
正常 buffering 或 Video master 不執行此自動過期退休。
queue 壓力、超過 250 ms 的影音／來源映射誤差、正向音訊交付缺口，或等待超過
max(250 ms, 2 × preroll)，都會自動降級。初始永遠收不到音訊也不會永久卡住影像。

降級時 video 立即消化最新可用影格，各種 master 都可退休舊預覽；GPU in-flight 已佔滿
byte budget 時可直接計數略過新輸入，而不超額配置或讓擷取停止。
audio 取消舊 queue，透過 backend 的既有 reset 清除輸出 padding，完成 reset 後才接受新的 frame mapping。
backend position 的 resetting 旗標與 generation 在 mutex 下協調，避免舊 consumed frames 套到新 spans。
音訊以原速獨立播放，保留有界的近期 PCM；軟體 queue 過時資料可丟棄，無資料時不輸出聲音。
降級輸出的短 reserve 最多 20 ms，等待稀疏封包最多 40 ms，不要求影音同時 preroll。

恢復用兩秒 delivery／nominal PCM 的線性回歸估計來源速率，允許 ±0.25%；
使用窗口內所有觀測，避免單一批次交付的端點 jitter 誤判來源不穩。
輸入新鮮、音訊進度與 video clock 差在 150 ms 內且條件持續兩秒後，採納共同位置恢復同步。
恢復不重啟擷取、不 flush 正在輸出的 PCM，但重新對齊可能短暫保留影像。
`gvfg_playback_set_sync_enabled()` 控制 handle 的同步選擇，預設 true，保留到 stop/start 與 reset。
關閉時使用同一套獨立播放排程並禁止恢復；重開會重新觀察穩定條件，不直接跳回 master。
此函式不改 config layout，切換時透過 core mutex 協調；主動關閉不計為 sync_fallbacks。
單筆資料大於設定容量是 EINVAL；普通 queue 壓力不再產生 EQUEUE_FULL。

## 6. GPU 完成與略過

呈現使用 Preview 的自有 upload resources；CPU source 只需維持到 render 返回。
共用 worker 等待該工作退休後才送下一張，因此不把每次 render 接受誤認為成功 Present。
背景 thread 用 `idleCv_` 等待工作完成通知，不逐幀輪詢 Windows 1 ms 計時器。
HWND owner 的同步等待仍服務 sent messages，避免 DXGI 與 UI thread 互等。

初次 prepare 在緩衝期間做；正常每張不再重複 prepare 與前置 idle 等待。
真正後端 failure 導致 EOUTPUT；GPU busy／後端過期是計數略過，不是 Capture failure。
`video_processed` 包含呈現、隱藏消化與略過；`video_dropped` 是過期／busy 略過，不能當 driver drop。
底層 Preview delivery 計數不包含 core 在送 GPU 前已退休的所有 frame。

## 7. 音訊補償

Audio master 不調整音訊速率。其他 master 每約 20 ms 以 audio PTS 減 master PTS 更新 DriftFilter：

- 平滑誤差，暖機 20 次；3 ms 以內的目標速率為 1.0。
- 目標比例限制 0.995～1.005，每次比例變化最多 0.0001。
- 超過 250 ms 的誤差會清 filter 並回到 1.0；不是大幅失步的自動修復機制。

WASAPI 使用 shared-mode AUTOCONVERTPCM、SRC_DEFAULT_QUALITY、RATEADJUST，
透過 `IAudioClockAdjustment::SetSampleRate()` 執行指令；不支援或操作失敗會回報輸出錯誤。
本實作沒有舊 QAudioSink 的兩秒自動重建裝置策略，也沒有無界加速補回未知遺失 samples。

## 8. 與 FFplay 的對應及差異

設計參考 [FFplay 原始碼](https://ffmpeg.org/doxygen/trunk/ffplay_8c_source.html)，獨立實作，未引入 FFmpeg／SDL runtime。

| FFplay 概念 | 目前實作 |
|---|---|
| VideoState／Clock／master 選擇 | gvfg_playback_t／Clock／config |
| video_refresh 與 late frame drop | 到期呈現、保留最新到期影格；預設非 Video master 可退休 |
| audio output clock 與 pending buffer | WASAPI consumed frames／padding ＋來源 spans |
| synchronize_audio／swr compensation | DriftFilter ＋ Windows SetSampleRate |

不是完整 FFplay 移植：沒有 decoder／demuxer、seek、EOS drain 或相同的全部門檻與策略。
共同緩衝、有界 queue、音訊來源時間估計與目前補償上限都是本 SDK 的選擇。
不能宣稱 FFplay 可以做的任意格式／裝置恢復，本 SDK 就全部具備。

## 9. 錯誤、ABI 與回收

core 非同步失敗保存在 last_error，worker 暫停，後續 submit 回報同一錯誤；running 可能仍為 1。
stop/reset 取消 queue，不是無損 drain。reset 沿用舊 config，音訊格式改變應明確 stop/start 新設定。
呼叫端先停止／join read producers、release token，再停止 Playback 與 Capture。

2.0.2 stats 尾端新增 video_dropped；2.0.4 追加 sync_degraded、sync_fallbacks、audio_dropped_frames。
getter 明確接受目前大小、2.0.2／2.0.3 大小及 2.0.0／2.0.1 原始大小，
只回填 caller 提供的範圍；不是對任意 struct_size 都相容。config 仍要求目前完整大小。
公開 API／資料變更需同步標頭、匯入庫、DLL、Reference 與 Demo，避免混版。

## 10. 既有驗證範圍

既有 MSVC build／exports／dependencies／C header 檢查通過；這是靜態與連結證據。
既有實際 Qt Sample 測試為 CH0 1080p YUY2、48 kHz stereo PCM16、External master、120 秒，
含視窗／全螢幕切換：約 60 fps，queue 未持續增長；7166 張輸入，processed 7160、presented 7158、dropped 2。
其餘待播 frame 在 stop 時取消。
慢速輸出模擬為 60 fps 輸入、30 ms／張輸出與 busy：300 張輸入，略過 159、呈現 134、最大 queue 8、無拒收；舊 stats guard 通過。

2.0.3 的決定性模擬使用實際 Playback core，僅替換時鐘／worker 步進與輸出後端：
舊版在 audio source 慢 500 ppm 時，910.084 秒出現 buffering、video queue 32、EQUEUE_FULL。
修正後 External master 的 source ±500／±2000 ppm、output ±300 ppm 與組合案例各模擬 7200 秒，
無 queue rejection，video queue 最高 6～7。另驗證 Audio／Video master、video-only、audio-only；
Audio master 遇到來源較慢時仍會耗盡既有緩衝並重新緩衝，未宣稱音訊裝置成為 master 後仍自動調速。
44.1／48／96 kHz 變動包長、delivery jitter、span 單調及邊界映射檢查通過。
另以未修改的 Release DLL 與實際 WASAPI 輸出測試 60 秒，合成 PCM 供應慢 0.2%、
60 fps 隱藏影像：3600 張提交、3594 張處理、queue 最高 9、拒收 0、重新緩衝 0；
結束時 audio queue 77.73 ms、rate ratio 0.997830。此測試不含 Capture 硬體或 GPU 呈現。
模擬不是實際兩小時硬體播放，也不是聲學／嘴型延遲驗證。

2.0.4 決定性 core 模擬通過：完全無音訊、延後出現、五秒斷音、五秒停影、
來源半速／兩倍速及恢復、各 master、video-only／audio-only、GPU worker 暫停且 video queue 只有兩張。
檢查無拒收、容量不超限、影像持續消化、來源恢復後退出降級，以及兩種既有 stats 大小的尾端 guard。
後端模擬 reset 延遲五毫秒，驗證 reset 完成前不會餵入新 mapping。
正常 ±500 ppm 各模擬兩小時，沒有降級，video queue 最大 6～7。

另以實際 Release DLL／WASAPI、合成 PCM 與隱藏影像跑 60 秒，包含三秒斷音、
五秒兩倍音訊、五秒半速音訊、三秒停影；四次降級均恢復，queue 拒收 0、video queue 最大 22。
3420 張提交、3414 張處理；剩餘待播影格於 stop 取消。
這些測試未使用 Capture 硬體、實體 GPU 呈現或聲學同步量測。臨時測試檔於完成後清除。
同步開關另以實際 Release DLL／WASAPI 驗證啟動前關閉、運行中開啟並恢復、再次關閉，
以及 reset／stop-start 保留關閉選擇；非法參數回傳 EINVAL。
