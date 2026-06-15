# GVFG Customer API

This document describes the customer-facing GVFG capture API in
`sdk/gvfg/include/gvfg_capture.h`.

The customer SDK is capture-only. It does not expose SDK-managed preview,
driver registers, DMA internals, or FPGA debug controls. Preview rendering is
handled by internal applications or tools on top of frames returned by the SDK.

## Basic Flow

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_open
-> gvfg_start
-> loop:
   gvfg_read_frame
   use or copy frame data
   gvfg_release_frame
   gvfg_poll_event optional
-> gvfg_stop
-> gvfg_destroy
```

## Threading

The frame API is pull-based. The SDK does not start a customer frame thread or
preview thread from the public API layer.

Applications decide where to call `gvfg_read_frame()`:

```text
UI app:
  create an app-owned worker thread
  call gvfg_read_frame() in that worker
  marshal UI updates back to the UI thread

console/service app:
  call gvfg_read_frame() directly from its processing loop
```

The backend may still use internal driver I/O workers while capture is running.
Those are implementation details behind `gvfg.dll`.

## Frame Ownership

`gvfg_read_frame()` returns one SDK-owned frame buffer.

Rules:

- `frame.data` is valid until `gvfg_release_frame()` is called.
- A handle may hold only one frame at a time.
- If the application needs the data after release, it must copy the frame.
- `gvfg_stop()` invalidates any unreleased frame.

## Public Types

### `gvfg_handle`

Opaque session handle.

```c
typedef struct gvfg_handle_t *gvfg_handle;
```

### `gvfg_status_t`

Common return status.

```c
typedef enum
{
    GVFG_OK = 0,
    GVFG_EINVAL = -1,
    GVFG_ENODEV = -2,
    GVFG_ESTATE = -3,
    GVFG_EIO = -4,
    GVFG_ENOTSUP = -5,
    GVFG_ETIMEOUT = -6
} gvfg_status_t;
```

### `gvfg_device_info_t`

Device entry returned by `gvfg_enumerate_devices()`.

### `gvfg_signal_status_t`

Customer-readable input signal status:

- width / height
- video format text
- frame-rate text
- bit depth
- SDI / HDMI lock status

Raw FPGA values, register-like values, and validity masks are internal debug
data and are available only through `gvfg_debug.h` in the internal debug
package.

### `gvfg_frame_t`

Frame returned by `gvfg_read_frame()`.

```c
typedef struct
{
    const void *data;
    uint64_t data_size;
    int width;
    int height;
    int pixel_format;
    int bit_depth;
    uint64_t frame_id;
} gvfg_frame_t;
```

### `gvfg_event_t`

Capture event returned by `gvfg_poll_event()`.

Customer events are driver-neutral:

- `GVFG_EVENT_PLUG_IN`
- `GVFG_EVENT_PLUG_OUT`
- `GVFG_EVENT_CAPTURE_PAUSED`
- `GVFG_EVENT_CAPTURE_RESUMED`

Frame interrupts, IRQ bit numbers, and IRQ masks are internal details and are
not exposed through `gvfg_capture.h`.

## Main APIs

### `gvfg_enumerate_devices`

```c
int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);
```

Enumerates GVFG capture devices. Pass `NULL, 0` to query the device count.

### `gvfg_create` / `gvfg_destroy`

```c
gvfg_status_t gvfg_create(gvfg_handle *out_handle);
gvfg_status_t gvfg_destroy(gvfg_handle handle);
```

Creates or destroys a session handle. Destroying a running handle stops capture
first.

### `gvfg_open`

```c
gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);
```

Opens a device by index from `gvfg_enumerate_devices()`.

### `gvfg_start` / `gvfg_stop`

```c
gvfg_status_t gvfg_start(gvfg_handle handle);
gvfg_status_t gvfg_stop(gvfg_handle handle);
```

Starts or stops capture. After `gvfg_start()`, call `gvfg_read_frame()` to
receive frames.

### `gvfg_read_frame`

```c
gvfg_status_t gvfg_read_frame(gvfg_handle handle,
                              gvfg_frame_t *out_frame,
                              uint32_t timeout_ms);
```

Reads one frame. Use `timeout_ms == 0` to wait indefinitely.

Returns:

- `GVFG_OK` when a frame is available.
- `GVFG_ETIMEOUT` when no frame arrives before the timeout.
- `GVFG_ESTATE` when capture is not running or a previous frame is still held.

### `gvfg_release_frame`

```c
gvfg_status_t gvfg_release_frame(gvfg_handle handle,
                                 const gvfg_frame_t *frame);
```

Releases the frame returned by `gvfg_read_frame()`.

### `gvfg_poll_event`

```c
gvfg_status_t gvfg_poll_event(gvfg_handle handle,
                              gvfg_event_t *out_event,
                              uint32_t timeout_ms);
```

Polls one event. Use `timeout_ms == 0` for a non-blocking poll.

### `gvfg_get_signal_status`

```c
gvfg_status_t gvfg_get_signal_status(gvfg_handle handle,
                                     gvfg_signal_status_t *out_status);
```

Queries current input signal metadata.

### `gvfg_get_runtime_info`

```c
gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle,
                                    gvfg_runtime_info_t *out_info);
```

Queries current signal status, last read frame format, FPS, and delivered frame
count.

## Preview Boundary

Preview is not part of the core capture SDK API.

Applications have two choices after `gvfg_read_frame()` returns a frame:

```text
gvfg_read_frame
-> customer-owned display / processing / recording / snapshot
-> optional gvfg_preview_render_frame from gvfg_preview.dll
-> gvfg_release_frame
```

Customer demo source may include `gvfg_preview.h` and link `gvfg_preview.dll`
when it needs a simple display path. The preview helper source remains private;
customer code only sees the helper API and binary.
