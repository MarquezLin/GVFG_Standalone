# GVFG SDK 與 Demo 建置說明

SDK version 由 `GVFG_SDK/CMakeLists.txt` 決定，目前為 2.1.1。
Sample version 由自己的 CMake 決定；目前 checkout 是內部分支，與 customer master 不必相同。

SDK Debug 在原有等待流程實際收到格式事件時，透過 `OutputDebugStringA()`
輸出 `[GVFG] Received VIDEO_FORMAT_CHANGED` 到偵錯器輸出視窗。
未收到時不輸出；不改事件流程、不新增 API，Release 不輸出。
清除事件前亦以零等待檢查 format event；若已觸發，先印出訊息並標示
`before format/start/stop reset`，再執行原有清除。這是 SDK 端觀察，不代表攔截 driver 內部觸發。

## 1. 正確專案根目錄

```text
GVFG_SDK workspace/                  # 不是單一 Git repository
  GVFG_Standalone/GVFG_SDK/          # SDK CMake project
  Costomer_SDK_Release/GVFG_Customer_Sample/ # Qt Demo CMake project
  srcroot/                          # 完整 Viewer CMake project
```

SDK 與 Demo 現在不是同一個頂層 CMake 專案。不存在 `BUILD_GVFG_SAMPLES` 的 SDK 開關。
不要把舊 `samples/gvfg_qt_preview` 路徑、舊 helper DLL target 或其他 checkout 的 build cache 混用。

## 2. 環境與 target

- SDK：Windows x64、CMake 3.16+、相容的 MSVC x64 toolchain 與 vendor `GvfgSdk.lib`。
- Demo：另外需要相容的 Qt 6 Widgets kit；沒有 Qt Multimedia／QAudioSink 相依。
- Viewer：另外依賴自己 `srcroot/CMakeLists.txt` 的 Qt／FFmpeg／DCMTK 等項目。

| SDK target | 類型 | 說明 |
|---|---|---|
| `gvfg` | SHARED | Capture facade、vendor adapter、GPU conversion／OSD |
| `gvfg_playback` | SHARED | 對外播放 DLL，只匯出 Playback 函式 |
| `gvfg_preview` | STATIC | D3D backend，私下連結進 Playback |
| `gvfg_audio_playback` | STATIC | WASAPI backend，私下連結進 Playback |

## 3. 建置 SDK

在 MSVC x64 Developer Command Prompt，於 workspace 執行：

```bat
cmake -S GVFG_Standalone/GVFG_SDK -B GVFG_Standalone/GVFG_SDK/build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build GVFG_Standalone/GVFG_SDK/build_release --target gvfg gvfg_playback
```

Ninja 使用 CMAKE_BUILD_TYPE；Visual Studio 多組態 generator 使用 `--config Release`。
預設 bin/lib 在 build root；多組態可能再有 Release／Debug 子目錄，應確認實際檔案位置。
SDK build 不需要 Qt，也不產生 Demo EXE。

`GVFG_MINGW_DLLTOOL` 是 FILEPATH，可指向 dlltool 以產生客戶端 GNU 匯入庫。
這不表示 MSVC vendor 靜態庫可直接由 MinGW 建置 SDK。
`GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE` 是現有預設 OFF 的實驗選項，不屬於客戶播放設定。

## 4. Sample 的三種 SDK 來源

| 條件 | 使用來源 |
|---|---|
| 未指定 override（Customer 預設） | 只使用 Sample 自己的 include/bin/lib，不搜尋同層 SDK |
| 明確指定 `GVFG_SAMPLE_SDK_SOURCE_DIR`，未指定 build override | add_subdirectory 建置 SDK，連結真正的 gvfg／gvfg_playback target |
| `GVFG_SAMPLE_SDK_BUILD_DIR` 有值 | 使用該目錄 bin/lib；另有指定 source 時 headers 取自該 source，否則使用套件 headers；須版本配套 |

指定 prebuilt SDK 的範例，於 workspace 執行：

```bat
cmake -S Costomer_SDK_Release/GVFG_Customer_Sample -B sample_release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64 -DGVFG_SAMPLE_SDK_BUILD_DIR=C:/path/to/sdk-build -DGVFG_SAMPLE_SDK_SOURCE_DIR=C:/path/to/GVFG_Standalone/GVFG_SDK
cmake --build sample_release --target gvfg_qt_preview
```

指定 build override 時，兩個路徑要對應同一版本；不要讓 include 仍指向其他 checkout。
Customer 新 build 預設使用套件 include/bin/lib。舊 cache 曾記住 SDK source／build 時，
需清空 `GVFG_SAMPLE_SDK_SOURCE_DIR` 與 `GVFG_SAMPLE_SDK_BUILD_DIR` 才會改用套件。
內部診斷支線有自己的 source 選擇設定，不從 Customer 的新預設推論其行為。
Sample POST_BUILD 只複製匹配的 gvfg.dll／gvfg_playback.dll 到 EXE 旁。

## 5. Customer 與 internal Debug

Customer Demo 使用公開 API、基本狀態與必要錯誤，不輸出同步內部 queue／PTS／dropped。
目前 `internal/audio-diagnostics` 分支的 CMake 另在 Debug 加入 internal_diagnostics 與 private header 路徑，
Release 不編譯該診斷來源。內部版規則以 Sample 的 INTERNAL_BRANCH_WORKFLOW.md 為準；
不要把內部分支或 Debug binary 當成客戶交付版本。

SDK 與 Preview 的 `GVFG_INTERNAL_DIAGNOSTICS` 各由其 CMake 依組態定義，
不是可任意切換所有模組的統一 runtime 設定。不要假設 Debug 有 vendor 未提供的 IRQ／DMA loss counter。

## 6. 安裝與封裝

```bat
cmake --install GVFG_Standalone/GVFG_SDK/build_release --prefix sdk_package
```

乾淨 install tree 只交付 Capture／Playback 的 DLL 與匯入庫、公開標頭及客戶文件。
不交付 backend 靜態 `.lib`、private source、gvfg_debug.h、vendor headers 或舊 helper DLL。
文件保留 `GVFG_Customer_doc/` 子目錄，使使用手冊與 API Reference 的相對連結在安裝後仍成立。
使用新目錄安裝；cmake install 不會自動移除舊套件留下的 helper DLL 或舊文件。

Customer Sample 的 `package_release.bat` 由已建好的 Release EXE 打包：

```bat
package_release.bat "C:\path\to\gvfg_qt_preview.exe" "C:\Qt\6.10.2\msvc2022_64\bin" "C:\path\to\packages"
```

腳本檢查 EXE 同目錄的 gvfg.dll／gvfg_playback.dll，檢查 customer EXE 未匯入 gvfg_debug_*，
再執行 windeployqt。它不會建置或替你挑別的 build directory。
`srcroot` 的封裝腳本有額外 Viewer／FFmpeg runtime，與 Customer Demo 套件不同。

## 7. 常見錯誤

| 問題 | 檢查 |
|---|---|
| LNK1104 無法覆寫 DLL／EXE | 關閉正在使用該 build 的程式，再建置；切分支也不會更新既有 binary |
| 找不到 gvfg_preview.dll | 程式或封裝仍使用舊 ABI，改連結 Playback，勿補回舊 DLL 掩蓋混版 |
| 找不到 Playback symbol | 標頭、匯入庫、DLL 不配套，或誤連內部靜態 backend |
| Sample 沒找到預建 SDK | 確認來源選擇、bin/lib 位置、MSVC／MinGW 匯入庫檔名 |
| 視窗關閉後音訊停止 | 確認沒有停止 video producer 卻保留 audio-video 模式；隱藏預覽仍需影像時間軸 |
| 狀態顯示 running 但無輸出 | 檢查 start／submit 回傳、last_error、buffering 與啟用串流資料 |

本次文件變更只驗證文件／來源、範例與安裝規則；不因文件改版宣稱新增硬體測試。
