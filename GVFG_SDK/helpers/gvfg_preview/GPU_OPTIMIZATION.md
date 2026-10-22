# Preview GPU conversion paths

The static `gvfg_preview` backend is linked into `gvfg_playback.dll` and remains
independent of `gvfg.dll`. Both playback and capture compile the shared private
conversion source, `d3d_conversion_pipeline.cpp`, into their own DLLs. Preview
functions are internal; applications use `gvfg_playback_*`.

CPU-buffer conversion uses dedicated outputs. YUY2/Y210 to NV12 preserves
native luma and averages chroma vertically; Y210 codes are reduced to 8 bits.
BGRA8 and RGB10A2 use one native RGB draw. No export path requires FP16 or a
scene copy. Output targets and staging textures are allocated on demand and
reused. Both NV12 plane copies are submitted before either CPU Map.

Preview uploads remain in the existing four slots, with three FIFO pending
frames and a 50 ms queue-age limit. Native-size and reduced-size preview draw
directly into the swapchain. Reduction interpolates decoded RGB. Enlargement
lazily allocates one FP16 target and uses hardware bilinear sampling, avoiding
repeated native decoding for each enlarged output sample. All five existing
formats retain their color/bit-depth rules. Preview has no CPU readback.

## Verification (2026-10-12)

Rebuilt Release DLLs passed public YUY2/Y210 to BGRA8/RGB10A2/NV12 checks
against independent CPU reference values, padded strides, chroma averaging,
size/format switching, and odd-NV12 rejection. WARP and hardware tests of the
private conversion source passed all five formats, 8/10-bit output, native,
reduced and enlarged sizes, and cached/uncached source views. Actual independent
preview DLL hidden-HWND tests passed format and precision switches, queue
drain, clear and destruction. DLL imports and exports are checked separately.

These component tests do not establish long-run capture/recording stability,
visible fullscreen behavior, physical monitor scanout, or Task Manager GPU
percentages. Previous performance measurements motivated the adaptive scaling
choice; this verification does not claim a new utilization benchmark.
