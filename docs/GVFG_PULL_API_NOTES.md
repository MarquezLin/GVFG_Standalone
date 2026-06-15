# GVFG Pull API Notes

## Direction

The customer-facing capture path follows an FFmpeg-style pull model:

```text
gvfg_enumerate_devices
-> gvfg_create
-> gvfg_open
-> gvfg_start
-> loop:
   gvfg_read_frame
   use frame data
   gvfg_release_frame
   gvfg_poll_event optional
-> gvfg_stop
-> gvfg_destroy
```

## Frame Ownership

- `gvfg_read_frame()` returns one SDK-owned frame buffer.
- `frame.data` remains valid until `gvfg_release_frame()` is called.
- A handle may hold only one frame at a time.
- If the application needs the frame after release, it must copy the data.

## Threading Model

The SDK may keep internal driver I/O workers, but it no longer starts a public
frame callback loop for the application.

Applications own their threading model:

```text
UI thread
-> start capture
-> create app-owned worker thread

worker thread
-> gvfg_read_frame
-> process or signal UI
-> gvfg_release_frame
```

This keeps the customer API simple while avoiding hidden callback-thread rules.

## Preview Boundary

Preview is not part of the customer SDK API. Internal apps or debug tools may
render frames after `gvfg_read_frame()` with app-owned code:

```text
gvfg_read_frame
-> app/private preview renderer
-> optional snapshot or recording
-> gvfg_release_frame
```

This keeps `gvfg.dll` focused on capture and avoids exposing D3D/render helper
APIs to customers.

## Legacy APIs

`gvfg_set_callbacks()` and `gvfg_set_event_callback()` remain in the header for
source compatibility during the transition. New customer code should prefer:

```text
gvfg_read_frame
gvfg_release_frame
gvfg_poll_event
```
