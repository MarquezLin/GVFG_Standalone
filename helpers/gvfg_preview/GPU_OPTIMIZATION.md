# Preview GPU path

The preview worker now runs:

1. Execute the upload slot's command list (the SDK frame was already released by the caller).
2. Convert packed YUY2/Y210 into a full-resolution FP16 target.
3. Sample that FP16 target directly into the 8-bit or 10-bit swapchain, preserving aspect ratio.
4. Present. Startup retries repeat only Present, not the draw.

Compared with the previous preview, this removes one full-frame FP16 CopyResource per image. The 8-bit path also removes an intermediate BGRA8 draw and quantizes only at the swapchain. FP16 precision and the requested swapchain bit depth remain; no capture-frame decimation was added. Scaled 8-bit output may differ slightly because rounding now occurs after scaling instead of before it.

Each of the three upload slots owns its SRV; the worker no longer creates an SRV for every preview frame. YUY2/Y210 shaders use one packed texture load per output pixel. The old ProcAmp settings were fixed at neutral, so unused neighbor sharpness reads and neutral color adjustments were removed; BT.709 coefficients, packing, Y210 word shifts and odd-tail chroma behavior remain. Constant-buffer data updates only when source dimensions change, but its binding is restored each draw.

Preview-only resource initialization allocates only the FP16 conversion target, omitting scene, BGRA8 export, RGB10 export and NV12 planes. Export conversion still uses the default full resource path; no public capture or buffer API signature changed. Export shaders are not compiled for preview-only initialization.

Swapchain resize/release unbinds the render target before releasing local references. Preview configuration/resource changes and drawing share the D3D mutex. No GPU mutex is held while subsequently acquiring the renderer mutex in the worker completion path.

## Validation boundary

Source checks only in this change: no application/shader build, GPU timing or hardware playback test. These changes remove identifiable work but do not establish a GPU utilization reduction or identify the reported 10-bit startup failure.

User validation reported after the source change: GPU utilization decreased by approximately 15%. This is a user-reported observation, not an agent-run benchmark; baseline/final readings and whether the difference is relative or in percentage points were not specified. Resolution of the 10-bit startup failure was not reported.

Build the preview DLL with the changed shared pipeline source. If distributing the SDK GPU buffer converter, rebuild that DLL too because it shares the shader source. Keep the APP and preview DLL from the same delivery-statistics API revision.

On the target Ryzen 7 7700 system, compare identical resolution/FPS/window size and input formats before/after. Check 8-bit and 10-bit colors (including gradients), resize/fullscreen, repeated Start/Stop, Present skip counts and GPU engine utilization. Also verify public BGRA8/RGB10A2/NV12 buffer conversion if shipping its rebuilt DLL. Capture HRESULT details if 10-bit still fails; do not infer insufficient GPU capacity solely from utilization.
