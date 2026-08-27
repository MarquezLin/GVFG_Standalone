# GVFG Standalone 內部建置與功能說明

本文件供內部開發使用，說明 `GVFG_Standalone` 的用途、CMake 設定、Debug 診斷功能、輸出檔案，以及如何提供 `qt6/srcroot` 使用。

## 1. 專案定位

`GVFG_Standalone` 是 GVFG 擷取功能的 source of truth，包含：

- `sdk/gvfg`：GVFG 公開 C API、PCIe S2MM backend、GPU conversion。
- `helpers/gvfg_preview`：獨立的 D3D preview helper。
- `samples/gvfg_qt_preview`：直接使用 GVFG API 的 Qt 測試程式。
- `docs`：客戶 API、內部架構及建置文件。

`qt6/srcroot` 不會重新編譯這裡的 source，而是透過 header、LIB 與 DLL 匯入已建置完成的 Standalone SDK。

## 2. 必要環境

- Windows x64。
- Visual Studio 2022 MSVC Build Tools。
- CMake 3.16 以上。
- Qt 6 MSVC x64；只有建置 `gvfg_qt_preview` sample 時需要 Qt。

建議先進入 Visual Studio Developer Command Prompt，避免 `cl.exe` 找不到標準函式庫，或系統同時存在 `Path` 與 `PATH` 時造成 MSBuild 錯誤。

## 3. CMake 選項

| 變數 | 類型／預設 | 功能 |
|---|---|---|
| `BUILD_GVFG_SAMPLES` | BOOL／`ON` | 是否建置 `gvfg_qt_preview.exe`。SDK 與 preview DLL 不受此選項影響。 |
| `CMAKE_PREFIX_PATH` | PATH | Qt MSVC kit 的根目錄，例如 `C:\Qt\6.10.2\msvc2022_64`。 |

`GVFG_INTERNAL_DIAGNOSTICS` 不是給使用者手動設定的 cache option。CMake 會依 configuration 自動決定：

- Debug：`GVFG_INTERNAL_DIAGNOSTICS=1`
- Release：`GVFG_INTERNAL_DIAGNOSTICS=0`

這個設定會同時套用到 `gvfg.dll`、`gvfg_preview.dll` 與 `gvfg_qt_preview.exe`。

## 4. 更新 SDK 版本

版本只有一個來源：根目錄 `CMakeLists.txt` 的 `project()`：

```cmake
project(gvfg_standalone VERSION 0.1.0 LANGUAGES CXX)
```

例如準備 `0.2.0` 時改成：

```cmake
project(gvfg_standalone VERSION 0.2.0 LANGUAGES CXX)
```

`sdk/gvfg` 會直接繼承根 project version，並將它編入 `gvfg.dll`。不再存在 `GVFG_SDK_VERSION` cache option，也不應使用 `-DGVFG_SDK_VERSION=...` 覆寫。修改版本後要重新 configure，再重新建置 DLL。

程式可用下列公開 API 查詢實際載入的 DLL 版本：

```c
const char *version = gvfg_get_version();
```

`qt6_viewer` 啟動時也會把這個 runtime 版本寫入 log，便於發現 header、LIB、DLL 混用舊版本的問題。

## 5. 建置方式

### 5.1 Release：一般整合與效能測試

```bat
cmake -S . -B build_release ^
  -DBUILD_GVFG_SAMPLES=ON ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64

cmake --build build_release --config Release --target gvfg_qt_preview
```

若只需要 SDK，不需要 Qt sample：

```bat
cmake -S . -B build_sdk ^
  -DBUILD_GVFG_SAMPLES=OFF

cmake --build build_sdk --config Release --target gvfg gvfg_preview
```

### 5.2 Debug：內部診斷

```bat
cmake -S . -B build_debug ^
  -DBUILD_GVFG_SAMPLES=ON ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64

cmake --build build_debug --config Debug --target gvfg_qt_preview
```

Debug 版本會增加：

- backend/DMA/IRQ 統計資訊。
- preview submit、render、Present 慢幀診斷。
- held-frame 與 slow-release 診斷。
- 詳細 backend error。
- `gvfg_qt_preview` 的 diagnostic status log。

Debug 診斷會增加 log 與檢查成本，不應拿來判斷正式版本效能。

## 6. 輸出檔案

輸出位置由 CMake 統一設定：

```text
<build>/bin/gvfg.dll
<build>/bin/giga_ioctl.dll
<build>/bin/gvfg_preview.dll
<build>/bin/gvfg_qt_preview.exe
<build>/lib/gvfg.lib
<build>/lib/giga_ioctl.lib
<build>/lib/gvfg_preview.lib
```

不同 generator 的 configuration 可能位於 `bin/Release`、`lib/Release`，或 Qt Creator 自己建立的 configuration build 目錄。整合到 `srcroot` 時，`GVFG_STANDALONE_BUILD_DIR` 必須指向實際包含 `bin` 與 `lib` 的那一層。

## 7. 主要功能

### `gvfg.dll`

- 列舉裝置、開啟 channel、開始與停止擷取。
- 取得原生 YUY2 8-bit 或 Y210 10-bit frame。
- 使用 `gvfg_get_version()` 查詢實際載入的 runtime DLL 版本。
- signal status、runtime FPS 與 channel event。
- GPU conversion：BGRA8、RGB10A2、NV12。
- 公開詳細錯誤 API：`gvfg_get_channel_last_error_detail()`。
- 內部 debug API：backend stats、register read/write。

目前 FPGA 的 8-bit format register 仍可能回報舊 `YVYU` 值；SDK 只把它當作 legacy register identifier，對外格式與實際 DMA layout一律是 YUY2（`Y0 U0 Y1 V0`）。

### `gvfg_preview.dll`

- 將 YUY2/Y210 frame 顯示到指定 HWND。
- 自動依來源 bit depth 選擇 preview pipeline。
- preview worker 與 capture frame delivery 分離；preview 落後時可淘汰舊 preview frame，不阻塞擷取交付。

### `gvfg_qt_preview.exe`

- 最小化的 GVFG 整合範例。
- 視窗標題與啟動 log 顯示 `gvfg_get_version()` 回傳的實際 DLL 版本。
- 裝置／channel 選擇、start/stop、preview window。
- 顯示輸入格式、bit depth、capture FPS、preview FPS 與目前診斷狀態。
- Debug build 顯示額外 backend diagnostics。

## 8. 提供給 srcroot 使用

建置 Standalone 後，在 `qt6/srcroot` configure 時指定：

```bat
-DBUILD_GVFG_SDK=ON
-DGVFG_STANDALONE_ROOT=C:\Users\mark\Desktop\GcaptureSDK\GVFG_Standalone
-DGVFG_STANDALONE_BUILD_DIR=C:\path\to\standalone-build
```

`GVFG_STANDALONE_BUILD_DIR` 應包含：

```text
bin/gvfg.dll
bin/gvfg_preview.dll
lib/gvfg.lib
lib/gvfg_preview.lib
```

修改公開 enum、struct 或函式後，必須先重建 Standalone，再重新 configure/build `srcroot`，否則 header、LIB、DLL 可能版本不一致。

## 9. 常見問題

### `GVFG_PIXFMT_*` 未宣告

上層正在使用舊 enum，或 include 到另一份舊 header。確認 include path 指向本專案的 `sdk/gvfg/include`，並同步修改上層。

### `Path`／`PATH` duplicate key

這是啟動 Codex/PowerShell 環境時同時存在兩種大小寫造成的 MSBuild 問題。改從 Visual Studio Developer Command Prompt 建置，或清理重複環境變數。

### 找不到 `type_traits` 或 `utility`

表示 MSVC developer environment 尚未載入。先執行 `VsDevCmd.bat -arch=x64`，再呼叫 CMake build。

### DLL 更新但 viewer 行為仍舊

確認 viewer 執行目錄的 DLL timestamp，並關閉正在執行的 viewer 後重新建置。Windows 會鎖住使用中的 EXE/DLL。
