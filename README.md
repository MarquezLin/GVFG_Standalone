# GVFG Standalone SDK

目前 SDK 版本為 2.1.1，來源是 [GVFG_SDK/CMakeLists.txt](GVFG_SDK/CMakeLists.txt)。
擷取使用 `gvfg.dll`；選用播放功能使用 `gvfg_playback.dll`。Audio 與 Preview 是
Playback 內部靜態 library，不再各自部署 DLL。

## 文件入口

| 目的 | 文件 |
|---|---|
| 第一次使用擷取 API | [客戶 API 使用手冊](docs/GVFG_CUSTOMER_API.md) |
| 播放影像、音訊或同步影音 | [統一影音播放使用手冊](docs/GVFG_PLAYBACK_API.md) |
| 查結構、參數、限制與狀態碼 | [公開 API Reference](docs/GVFG_Customer_doc/GVFG_CUSTOMER_API_REFERENCE.md) |
| 理解 source、ownership 與執行緒 | [學習入口](docs/GVFG_LEARNING_INDEX.md) |
| 建置 SDK、連結 Demo、安裝 | [內部建置說明](docs/INTERNAL_BUILD_GUIDE.md) |
| 維護同步、WASAPI、D3D 實作 | [Playback 維護說明](GVFG_SDK/helpers/gvfg_playback/README.md) |

客戶先讀使用手冊，不需要理解 backend、driver 或同步演算法才可呼叫 API。

## 原始碼與責任

```text
GVFG_SDK/
  sdk/gvfg/                 Capture C API、GigabyteLib adapter、OSD／GPU conversion
  helpers/gvfg_playback/    公開播放 API、模式、時鐘、緩衝、同步排程
  helpers/gvfg_audio_playback/  內部 WASAPI 靜態 backend
  helpers/gvfg_preview/     內部 D3D 靜態 backend
  helpers/gvfg_osd/         內部 OSD 元件
docs/                      客戶與維護文件
../Costomer_SDK_Release/GVFG_Customer_Sample/  獨立 Qt Demo 專案
../srcroot/                 完整 Viewer，匯入 Standalone SDK
```

Capture 的 read/release、裝置與 channel lifecycle 由應用程式管理；Playback 只管理收到的播放副本。
GigabyteLib `GvfgSdk.lib` 靜態連結在 Capture DLL，private vendor headers 不屬於客戶 API。

## 建置

SDK 不需要 Qt。在已載入 MSVC x64 環境的命令提示字元，於此目錄執行：

```bat
cmake -S GVFG_SDK -B GVFG_SDK/build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build GVFG_SDK/build_release --target gvfg gvfg_playback
```

SDK 的頂層 CMake 不建置 Qt Sample，也沒有 `BUILD_GVFG_SAMPLES` 選項。
Demo 從 [獨立專案目錄](../Costomer_SDK_Release/GVFG_Customer_Sample/README.md) 建置，僅需要 Qt Widgets。
內附 vendor `.lib` 需要相容的 MSVC x64 SDK 建置環境；MinGW 客戶端可連結產生的 GNU 匯入庫。
若找不到 dlltool，可設定 `GVFG_MINGW_DLLTOOL`，細節見建置說明。

## 客戶檔案

```text
include/gvfg_capture.h
include/gvfg_playback.h
include/gvfg_preview.h
include/gvfg_audio_playback.h
bin/gvfg.dll
bin/gvfg_playback.dll
lib/gvfg.lib
lib/gvfg_playback.lib
lib/libgvfg.dll.a                 # 有啟用 dlltool 時
lib/libgvfg_playback.dll.a        # 有啟用 dlltool 時
```

只擷取、不播放時，只需要 Capture 的 header／匯入庫／DLL。使用 Playback 時，
另外攜帶三個播放相關標頭；後端標頭只為公開資料型別，不代表其函式對外匯出。
不要把建置目錄內的 `gvfg_preview.lib`、`gvfg_audio_playback.lib` 誤當成部署用匯入庫。

使用 `cmake --install <sdk-build> --prefix <new-package-directory>` 取得配套輸出。
Customer Demo 的 Release 打包腳本使用 EXE 同目錄中的 Capture／Playback DLL，
不會代為編譯，也不從其他 build 目錄任意挑 DLL。

## 模式、遷移與驗證

Playback 支援 video-only、audio-only、audio-video；純音訊不需視窗，
單張 RAW／DICOM 可用 `gvfg_playback_present_image()`，不啟動播放時間軸。
同步策略在 SDK，不放在 Demo。普通 Demo 顯示基本狀態及 API 錯誤；內部分支的
Debug 診斷與客戶 Release 套件分開，不能以 Debug 畫面作為客戶 API 契約。

舊程式應移除直接使用 Audio／Preview helper 的程式碼與 DLL 相依，
重新編譯並改用 `gvfg_playback_*`；遷移步驟見播放手冊。
既有擷取錄影流程與即時預覽是不同消費路徑，預覽略過數不等於錄影掉格數。

既有 2 分鐘實機測試與慢速輸出模擬的範圍記在 Playback 維護說明；
短測、建置通過不代表完成聲學嘴型精度、小時級漂移或所有硬體／格式驗證。
