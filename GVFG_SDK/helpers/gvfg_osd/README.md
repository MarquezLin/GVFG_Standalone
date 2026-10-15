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
- BT.709 limited-range RGB-to-YUV conversion
- an 8-bit coverage mask (`0` transparent, `255` fully covered)

The caller owns all output memory. Width must be even. YUY2 requires at least
`width * 2` bytes per output row; Y210 requires `width * 4`. The mask requires
at least `width` bytes per row.

For live SDK OSD, `GpuOsdProcessor` caches the non-zero mask bounds, aligned
to complete YUV422 pixel pairs. Only this rectangle is uploaded, blended,
and read back each frame. The SDK preserves the complete native frame with
memory copies and replaces the processed rectangle; CPU code does not blend
pixels. Text with no visible coverage bypasses processing. The public frame
format and read/release contract remain unchanged.
