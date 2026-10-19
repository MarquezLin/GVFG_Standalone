# GVFG OSD core

`gvfg_osd_core` is an internal static library that generates a full-frame
YUV422 OSD surface when text settings change. It does not capture, process
live video frames, communicate with the driver, or present a preview. The
library is linked into `gvfg.dll`; it is not installed or shipped as a
separate customer DLL.

The initial API supports:

- YUY2 and Y210 output
- printable ASCII text, up to 32 characters
- Arial-preferred font with DirectWrite fallback, configurable pixel size,
  RGB color, and `(x, y)` position
- one non-wrapping line, with a shared DirectWrite layout for measurement and
  rendering at 96 DPI
- BT.709 limited-range RGB-to-YUV conversion
- an 8-bit coverage mask (`0` transparent, `255` fully covered)

The caller owns all output memory. Width must be even. YUY2 requires at least
`width * 2` bytes per output row; Y210 requires `width * 4`. The mask requires
at least `width` bytes per row.

`gvfg_osd_measure()` returns a conservative complete text box containing logical
spacing, glyph overhang, and a one-pixel guard on each edge. It is not a
pixel-tight raster bound. The public `gvfg_measure_osd_text()` API in `gvfg.dll`
provides the same measurement without creating a capture handle or opening a
device. Font size must be non-zero; the SDK does not impose the viewer's
512-pixel UI limit.

`x` and `y` identify the top-left of this measured box. Rendering clamps them
to `[0, frame width - text width]` and `[0, frame height - text height]` so the
complete box remains visible. The internal render function returns `EINVAL`
if the text box is larger than the frame. For live capture, the GPU processor
detects that case before rendering and temporarily bypasses OSD while keeping
capture and the requested OSD settings active. It retries when the settings,
frame dimensions, or pixel format change. It does not shrink, truncate, or wrap
the text to make it fit.

For live SDK OSD, `GpuOsdProcessor` caches the non-zero mask bounds, aligned
to complete YUV422 pixel pairs. Only this rectangle is uploaded, blended,
and read back each frame. For non-zero-copy acquisition, the backend explicitly
grants writable access to its currently held SDK-owned capture buffer: the
GPU result replaces only that rectangle in place, retaining the original
stride and size. No second full-frame copy or output-buffer resize is needed.
The backend frame stays held until the application's normal release, including
across signal-recovery requests. The application still receives read-only data.

By default, driver zero-copy acquisition grants no writable access. The SDK still
copies the complete native frame into independent output storage, replaces
the processed rectangle there, and releases the backend frame before delivery.
CPU code copies bytes but does not blend pixels. Text with no visible coverage
bypasses processing. The public frame and read/release ABI remain unchanged.

Internal tools can use `gvfg_debug_read_channel_frame_timed()` from
`gvfg_debug.h` to obtain per-frame CPU wall times for OSD lock/surface/preparation,
ROI upload, command submission, full-frame copy, readback Map wait, ROI overwrite,
and pre-delivery backend release. The result is tied to the returned `frame_id`;
`osd_applied` reports whether that frame was actually blended. `process_total`
includes the process sub-stages and must not be added to them again.
Submission time is not GPU execution time, and readback wait includes unfinished
earlier GPU work. Normal public reads do not collect these timings. The timed
path introduces clock reads only, not new GPU queries, Flush, or synchronization.
In-place non-zero-copy OSD reports zero full-frame-copy and pre-delivery backend
release time; the actual backend release is deferred to application release.
`gvfg_debug_read_channel_frame_profiled()` additionally returns
`acquisition_begin_ns`, sampled after the frame event wait succeeds and before
buffer preparation or vendor data retrieval/copy. From this timestamp to the
returned frame timestamp is acquisition cost (including required capture copies);
to application receipt is acquisition plus OSD/delivery cost. Waiting for the
next frame is excluded. The public frame timestamp and old timed-read ABI are
unchanged; this uses a separate internal result type and export.
Do not ship the internal debug header in customer packages.

### Unsafe local zero-copy experiment (default OFF)

Set `GVFG_EXPERIMENTAL_ZERO_COPY_OSD_INPLACE=ON` in the standalone SDK's
CMake configuration, rebuild `gvfg.dll` and its import library, then rebuild
the srcroot viewer against the matching SDK. Enable zero-copy and apply OSD.
This is an SDK build option, not the viewer console option.

The experimental backend supplies the original mutable vendor pointer, without
casting away const on the public API. Before processing, `VirtualQuery` checks
the entire destination ROI span for committed `PAGE_READWRITE` pages; readonly,
guard, copy-on-write, and failed queries fall back to independent output and a
full-frame copy. Permissions are checked again immediately before ROI writes;
failure at that point returns `GVFG_EIO` without writing. No `VirtualProtect`
or vendor release contract change is used. Successful in-place processing
retains the backend hold until normal application frame release.

WARNING: these checks do not prevent concurrent DMA, buffer reuse, or a mapping
change after the check. Corruption and access violations remain possible; C++
exception handling is not an access-violation safety guarantee. Do not ship this
configuration. A successful capture does not establish driver ownership safety.

The viewer warns at startup and labels each 300-frame diagnostic batch as actual
driver-buffer writes, fallback copies, or no in-place processing. Flags use the
separate `gvfg_debug_get_channel_osd_experiment_flags` export; existing timing
struct layouts stay unchanged. Revert by setting the option OFF and rebuilding
the SDK. The client-facing frame pointer remains read-only in all modes.

Fallback batches also report the final frame's first rejected region: a readable
reason, native Protect/State/Type, VirtualQuery return byte count, and Win32 error
(captured immediately only when VirtualQuery returns zero). Other frames may
have different reasons. This uses the separate internal
`gvfg_debug_get_channel_osd_page_check` export, queried while the frame is held;
rebuild both SDK and viewer together. No per-frame logging or permission changes
are introduced. Zero Win32 error does not imply a writable page.
