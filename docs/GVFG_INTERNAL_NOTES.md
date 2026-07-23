# GVFG Internal Notes

這份文件是 standalone GVFG SDK 的內部工程地圖。客戶端 API 細節請看
`docs/GVFG_CUSTOMER_API.md`。

## 目前方向

GVFG 目前切成幾個清楚的模組：

```text
sdk/gvfg/
  gvfg.dll
  capture API, handle lifecycle, pull frame/event bridge, internal debug API

helpers/gvfg_preview/
  gvfg_preview.dll
  optional display helper; renders gvfg_frame_t after gvfg_read_frame()

helpers/gvfg_convert/
  gvfg_convert.dll
  optional snapshot/export helper; converts native capture frames on request

samples/gvfg_qt_preview/
  internal debug Qt tool
  uses gvfg.dll + gvfg_preview.dll + gvfg_debug.h
```

從 customer 角度看，core SDK 必須維持 driver-neutral。PCIES2MM、IRQ、DMA counters、
backend details 都是 internal。

### PCIES2MM backend 檔案分工

`sdk/gvfg/src/backend/pcies2mm/` 內部依責任拆分如下：

```text
pcies2mm_capture_session.{h,cpp}
  stream lifecycle, single driver-event/DMA worker, event dispatch,
  SDK-owned frame ring and frame ownership

pcies2mm_device.{h,cpp}
  Windows SetupAPI device discovery and device interface path

pcies2mm_ioctl.h
  private ABI contract shared with the PCIE S2MM driver:
  IOCTL codes, driver event IDs and DeviceIoControl structures

pcies2mm_reg.h
  FPGA register offsets and bit masks

pcies2mm_video_format.{h,cpp}
  native YUY2/Y210 layout rules and format-register decoding
```

`pcies2mm_ioctl.h` 與 `pcies2mm_reg.h` 都不是 customer public header，不可放進
SDK customer include package。目前板卡只接受 YUY2 與 Y210；這個 FPGA revision 即使
format register 回報 v210，DMA payload 仍由 `pcies2mm_video_format.cpp` 解讀為 Y210。
上述拆分只分離程式責任，不會增加 capture thread。

## 架構

```mermaid
flowchart TD
    subgraph App["Application / Sample"]
        QtApp["samples/gvfg_qt_preview"]
        CustomerApp["Customer application"]
    end

    subgraph Preview["Optional Preview Helper: gvfg_preview.dll"]
        PreviewApi["helpers/gvfg_preview/include/gvfg_preview.h"]
        PreviewPipe["private D3D preview pipeline"]
    end

    subgraph Convert["Optional Convert Helper: gvfg_convert.dll"]
        ConvertApi["helpers/gvfg_convert/include/gvfg_convert.h"]
        ConvertPipe["private CPU/GPU conversion path"]
    end

    subgraph SDK["Core SDK: gvfg.dll"]
        CaptureApi["sdk/gvfg/include/gvfg_capture.h"]
        DebugApi["sdk/gvfg/include/gvfg_debug.h\ninternal only"]
        Facade["sdk/gvfg/src/gvfg_capture.cpp"]
    end

    subgraph Backend["Internal Backend"]
        PcieS2mm["sdk/gvfg/src/backend/pcies2mm"]
        Ring["frame_ring_"]
        Workers["single driver event + DMA worker"]
    end

    CustomerApp --> CaptureApi
    CustomerApp -. optional display .-> PreviewApi
    CustomerApp -. optional snapshot .-> ConvertApi
    QtApp --> CaptureApi
    QtApp --> DebugApi
    QtApp --> PreviewApi
    PreviewApi --> PreviewPipe
    ConvertApi --> ConvertPipe
    ConvertApi --> CaptureApi
    CaptureApi --> Facade
    DebugApi --> Facade
    Facade --> PcieS2mm
    PcieS2mm --> Workers
    Workers --> Ring
```

## Capture 流程

Core capture API 的基本模式是 FFmpeg-style pull model：

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_open / gvfg_open_channel(CH0 or CH1)
-> gvfg_start
-> loop:
   gvfg_read_frame
   customer processing and/or gvfg_preview_render_frame
   gvfg_release_frame
   gvfg_poll_event optional
-> gvfg_stop
-> gvfg_destroy
```

Pull mode 下，`gvfg.dll` 不擁有 application 的 public read thread。UI app 應該
自己建立 worker thread，並在那個 thread 呼叫 `gvfg_read_frame()`。

`gvfg_start()` 在沒有 input signal 時仍會註冊 driver events 並啟動單一 backend
wait thread，但不 enable DMA。`gvfg_read_frame()` 此時 timeout；收到 signal-connected
event 後，backend 在相同 thread 重讀 width/height/payload format、resize ring，然後
enable DMA 並送出 capture-resumed event。

## Frame 所有權

- `gvfg_read_frame()` 回傳一個 SDK-owned frame buffer。
- `frame.data` 在 `gvfg_release_frame()` 前有效。
- `gvfg_frame_t` 要保持 ABI-stable；不要為了 layout 直接 append fields。
- `gvfg_get_frame_layout()` 回傳 SDK-filled layout metadata：`plane_data`、
  `plane_stride`、`plane_size`、`plane_offset`。目前 PCIES2MM backend 先用
  width/height/format 推導 tightly packed layout；未來 driver 如果能回報真實
  pitch 或 plane offsets，應該更新 layout query path，而不是改既有 frame struct。
- `gvfg_preview.dll` 應優先吃 `gvfg_get_frame_layout()`；layout query 不可用時才
  fallback 到 width-derived stride。
- `gvfg_convert.dll` 負責 explicit snapshot/export conversion。不要把 color
  conversion、image export、GPU conversion policy 搬進 `gvfg.dll`。
- 同一個 handle 一次最多 hold 一個 frame。
- Application 如果 release 後還要用 data，必須自己 copy frame。
- `gvfg_preview_render_frame()` 是 synchronous，應該在 `gvfg_release_frame()` 前呼叫。

Typical two-way use：

```text
gvfg_read_frame
-> customer-owned display / AI / recording / snapshot
-> optional gvfg_preview_render_frame
-> gvfg_release_frame
```

## Event 邊界

Customer event 保持 driver-neutral：

```text
GVFG_EVENT_SIGNAL_CONNECTED
GVFG_EVENT_SIGNAL_DISCONNECTED
GVFG_EVENT_CAPTURE_PAUSED
GVFG_EVENT_CAPTURE_RESUMED
```

舊名稱 `GVFG_EVENT_PLUG_IN/OUT` 僅為 aliases；事件代表 input signal cable，
不是 PCIe capture device hotplug。

Video IRQ handling 是 `sdk/gvfg/src/backend/pcies2mm` 內部細節。IRQ bit numbers、
IRQ masks 與 DMA counters 不應出現在
`gvfg_capture.h`。

## API Surfaces

Customer/demo 可見：

```text
include/gvfg_capture.h
include/gvfg_preview.h when display helper is used
include/gvfg_convert.h when snapshot/export conversion is used
```

Internal debug only：

```text
include/gvfg_debug.h
backend counters
interrupt count
DMA errors
latest backend error detail
PDB symbols
internal diagnostic tools
```

## Package 切分

### Demo / Customer Package

Include：

```text
include/gvfg_capture.h
include/gvfg_preview.h when the demo shows video
include/gvfg_convert.h when the demo exports snapshots
lib/gvfg.lib
lib/gvfg_preview.lib when the demo shows video
lib/gvfg_convert.lib when the demo exports snapshots
bin/gvfg.dll
bin/gvfg_preview.dll when the demo shows video
bin/gvfg_convert.dll when the demo exports snapshots
samples/customer-facing source
docs/GVFG_CUSTOMER_API.md
```

Do not include：

```text
include/gvfg_debug.h
SDK source
helper source
PCIES2MM backend headers
PDB symbols
register / DMA / IRQ debug docs
internal diagnostic tools
```

### Internal Debug Package

Include：

```text
include/gvfg_capture.h
include/gvfg_debug.h
include/gvfg_preview.h
include/gvfg_convert.h
lib/gvfg.lib
lib/gvfg_preview.lib
lib/gvfg_convert.lib
bin/gvfg.dll
bin/gvfg_preview.dll
bin/gvfg_convert.dll
bin/gvfg_qt_preview.exe
PDB symbols
internal debug notes
```

### Full Release Package

Full application release 可以使用 `gvfg.dll`、`gvfg_preview.dll` 和
`gvfg_convert.dll`，再加上 licensing 與 closed-source application integration。
不要把 full release package 當成 daily driver/FPGA bring-up vehicle。

## Build

Top-level CMake 會 build core SDK、helpers 和 optional sample：

```text
BUILD_GVFG_SAMPLES=ON
GVFG_PCIES2MM_DEBUG_LOG=OFF
```

輸出產物：

```text
bin/gvfg.dll
bin/gvfg_preview.dll
bin/gvfg_convert.dll
lib/gvfg.lib
lib/gvfg_preview.lib
lib/gvfg_convert.lib
bin/gvfg_qt_preview.exe
```

`GVFG_PCIES2MM_DEBUG_LOG=ON` 會打開 internal backend 的 verbose PCIES2MM flow logging。

## Draw.io Files

保留 draw.io files 方便討論圖：

```text
docs/GVFG_ARCHITECTURE_OVERVIEW.drawio
docs/GVFG_DMA_RING_DATA_FLOW.drawio
```

如果 code change 只影響 API order、package boundary 或 module ownership，更新這份
Markdown 即可。只有 deeper data flow 或 frame ownership diagram 改變時，才需要更新
draw.io。
