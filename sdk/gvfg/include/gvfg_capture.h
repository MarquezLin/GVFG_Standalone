#pragma once

/*
 * Customer-facing GVFG capture API.
 *
 * Include only this header in customer applications. Driver/backend headers
 * are implementation details behind gvfg.dll.
 *
 * Minimal capture flow:
 *
 *   gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {};
 *   int count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);
 *
 *   gvfg_handle h = NULL;
 *   gvfg_create(&h);
 *   gvfg_open(h, devices[0].index);
 *   gvfg_start(h);
 *
 *   while (running) {
 *       gvfg_frame_t frame = {};
 *       if (gvfg_read_frame(h, &frame, 1000) == GVFG_OK) {
 *           // Use frame.data before releasing the frame.
 *           gvfg_release_frame(h, &frame);
 *       }
 *   }
 *
 *   gvfg_stop(h);
 *   gvfg_destroy(h);
 *
 * Threading notes:
 * - The main frame API is pull-based: applications call gvfg_read_frame() from
 *   the thread they choose.
 * - The frame data pointer remains valid until gvfg_release_frame() is called.
 *   Copy the data if it must outlive that call.
 * - At most one frame may be held by a handle at a time.
 */

#ifdef _WIN32
#include <sal.h>
#ifdef GVFG_BUILD
#define GVFG_API __declspec(dllexport)
#else
#define GVFG_API __declspec(dllimport)
#endif
#else
#define GVFG_API
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Out_writes_to_opt_
#define _Out_writes_to_opt_(size, count)
#endif
#ifndef _Out_writes_z_
#define _Out_writes_z_(size)
#endif
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    GVFG_MAX_DEVICES = 16,
    GVFG_MAX_PLANES = 4
};

typedef enum
{
    GVFG_OK = 0,
    GVFG_EINVAL = -1,  /* Invalid argument, such as NULL handle/output pointer. */
    GVFG_ENODEV = -2,  /* No GVFG device, no signal, or device open failed. */
    GVFG_ESTATE = -3,  /* API was called in the wrong state. */
    GVFG_EIO = -4,     /* Driver/backend I/O failure. */
    GVFG_ENOTSUP = -5, /* Requested feature or format is not supported. */
    GVFG_ETIMEOUT = -6 /* Timed out waiting for driver/backend work. */
} gvfg_status_t;

typedef enum
{
    GVFG_PIXFMT_UNKNOWN = 0,
    GVFG_PIXFMT_YUY2 = 1,
    GVFG_PIXFMT_UYVY = 2,
    GVFG_PIXFMT_RGB24 = 3,
    GVFG_PIXFMT_BGRX32 = 4,
    GVFG_PIXFMT_NV12 = 5,
    GVFG_PIXFMT_P010 = 6,
    GVFG_PIXFMT_Y210 = 7,
    GVFG_PIXFMT_YUV444 = 8,
    GVFG_PIXFMT_BGRA8 = 100
} gvfg_pixel_format_t;

typedef enum
{
    GVFG_FRAME_LAYOUT_CONTIGUOUS = 1u << 0,  /* Planes are contained inside one contiguous data buffer. */
    GVFG_FRAME_LAYOUT_SDK_DERIVED = 1u << 1  /* Layout was derived by the SDK from the native format. */
} gvfg_frame_layout_flags_t;

typedef struct
{
    int index;       /* Device index to pass to gvfg_open(). */
    char name[128];  /* Display name for UI/logging. UTF-8, null-terminated. */
} gvfg_device_info_t;

typedef struct
{
    int width;                       /* Signal width in pixels when available. */
    int height;                      /* Signal height in pixels when available. */
    char video_format[16];           /* Decoded signal format name. */
    char frame_rate_name[16];        /* None, 23.98, 24, 47.95, ..., or "--" for unsupported codes. */
    int bit_depth;                   /* Signal bit depth: 8 or 10 when valid. */
    int sdi_locked;                  /* Non-zero when SDI reports locked. */
    int hdmi_locked;                 /* Non-zero when HDMI reports locked. */
} gvfg_signal_status_t;

typedef struct
{
    int width;              /* Width of the most recent frame returned by gvfg_read_frame(). */
    int height;             /* Height of the most recent frame returned by gvfg_read_frame(). */
    int bit_depth;          /* Bits per color channel of the frame buffer. */
    char pixel_format[32];  /* Native frame buffer format, for example YUY2, Y210, NV12, or P010. */
    int valid;              /* Non-zero while capture is running after at least one frame read. */
} gvfg_last_frame_info_t;

typedef struct
{
    gvfg_signal_status_t input_signal; /* Decoded input signal metadata. */
    gvfg_last_frame_info_t last_frame;           /* Last frame returned by gvfg_read_frame(). */
    double capture_fps;                /* Runtime FPS measured from frames returned by gvfg_read_frame(). */
    uint64_t delivered_frames;         /* Number of frames returned by gvfg_read_frame(). */
} gvfg_runtime_info_t;

typedef struct
{
    const void *data;       /* Native frame buffer. Valid until gvfg_release_frame() is called. */
    uint64_t data_size;     /* Total bytes available from data. */
    int width;              /* Frame width in pixels. */
    int height;             /* Frame height in pixels. */
    int pixel_format;       /* gvfg_pixel_format_t value. */
    int bit_depth;          /* Bits per color channel of the native frame. */
    uint64_t frame_id;      /* Monotonic frame identifier from the backend. */
} gvfg_frame_t;

typedef struct
{
    uint32_t struct_size;                    /* Set to sizeof(gvfg_frame_layout_t) before calling. */
    uint32_t layout_flags;                   /* Bitmask of gvfg_frame_layout_flags_t values. */
    int row_bytes;                           /* Bytes per row for plane 0. Prefer plane_stride[] for new code. */
    int plane_count;                         /* Number of valid entries in plane_data/plane_stride/plane_size. */
    const void *plane_data[GVFG_MAX_PLANES]; /* Plane pointers inside frame.data, valid until gvfg_release_frame(). */
    int plane_stride[GVFG_MAX_PLANES];       /* Bytes from one row to the next for each plane. */
    uint64_t plane_size[GVFG_MAX_PLANES];    /* Bytes available in each plane. */
    uint64_t plane_offset[GVFG_MAX_PLANES];  /* Byte offset from frame.data to each plane. */
    uint64_t reserved[8];                    /* Reserved for future SDK/driver layout metadata. Must be ignored. */
} gvfg_frame_layout_t;

typedef enum
{
    GVFG_EVENT_UNKNOWN = 0,
    GVFG_EVENT_PLUG_IN = 1,
    GVFG_EVENT_PLUG_OUT = 2,
    GVFG_EVENT_CAPTURE_PAUSED = 3,
    GVFG_EVENT_CAPTURE_RESUMED = 4
} gvfg_event_type_t;

typedef struct
{
    gvfg_event_type_t type;
    uint64_t timestamp_ns;
} gvfg_event_t;

/* Opaque session handle created by gvfg_create() and released by gvfg_destroy(). */
typedef struct gvfg_handle_t *gvfg_handle;

/*
 * Optional callback-mode frame delivery.
 *
 * The frame pointer is valid only for the duration of the callback. The SDK
 * releases the frame automatically after the callback returns. Copy the data if
 * it must outlive the callback.
 */
typedef void (*gvfg_frame_callback_t)(
    _In_ gvfg_handle handle,
    _In_ const gvfg_frame_t *frame,
    _In_opt_ void *user_data);

/*
 * Optional callback-mode event delivery.
 *
 * Event callbacks may be invoked from a different SDK-owned thread than frame
 * callbacks. Do not assume ordering between frame and event callbacks.
 */
typedef void (*gvfg_event_callback_t)(
    _In_ gvfg_handle handle,
    _In_ const gvfg_event_t *event,
    _In_opt_ void *user_data);

/*
 * Enumerate GVFG capture devices.
 *
 * Parameters:
 * - out_devices: Output array that receives device entries. Pass NULL to query
 *   the device count only.
 * - max_devices: Number of entries available in out_devices. Values larger
 *   than GVFG_MAX_DEVICES are clamped. Pass 0 when out_devices is NULL.
 *
 * Returns:
 * - When out_devices is non-NULL, returns the number of entries written.
 * - When out_devices is NULL or max_devices is 0, returns the number of
 *   available devices.
 * - Returns <= 0 when no device is available.
 */
GVFG_API int gvfg_enumerate_devices(
    _Out_writes_to_opt_(max_devices, return) gvfg_device_info_t *out_devices,
    _In_ int max_devices);

/*
 * Create a GVFG capture session.
 *
 * Parameters:
 * - out_handle: Receives the new session handle. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if out_handle is NULL.
 *
 * The returned handle starts in the closed state. Release it with
 * gvfg_destroy().
 */
GVFG_API gvfg_status_t gvfg_create(
    _Outptr_ gvfg_handle *out_handle);

/*
 * Destroy a GVFG capture session.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 *
 * Returns:
 * - GVFG_OK.
 *
 * If capture is still running, it is stopped first. After this call, the handle
 * must not be used again.
 */
GVFG_API gvfg_status_t gvfg_destroy(
    _In_opt_ gvfg_handle handle);

/*
 * Open a device by index from gvfg_enumerate_devices().
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - device_index: Device index from gvfg_device_info_t::index.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 * - GVFG_ENODEV if the device cannot be opened.
 * - GVFG_EIO for driver/backend failures.
 *
 * The current implementation selects the SDI input internally.
 */
GVFG_API gvfg_status_t gvfg_open(
    _In_ gvfg_handle handle,
    _In_ int device_index);

/*
 * Configure and start capture on an opened device.
 *
 * Parameters:
 * - handle: Opened session handle.
 *
 * Returns:
 * - GVFG_OK on success, including when capture is already running.
 * - GVFG_EINVAL if handle is NULL.
 * - GVFG_ESTATE if no device is open.
 * - GVFG_EIO or another status code if stream configuration/start fails.
 *
 * After success, call gvfg_read_frame() to receive frames and gvfg_poll_event()
 * to receive capture events.
 */
GVFG_API gvfg_status_t gvfg_start(
    _In_ gvfg_handle handle);

/*
 * Set the optional frame callback used by gvfg_start_callback_mode().
 *
 * Callback mode and pull mode are mutually exclusive for the same handle.
 */
GVFG_API gvfg_status_t gvfg_set_frame_callback(
    _In_ gvfg_handle handle,
    _In_opt_ gvfg_frame_callback_t callback,
    _In_opt_ void *user_data);

/*
 * Set the optional event callback used while callback mode is active.
 */
GVFG_API gvfg_status_t gvfg_set_event_callback(
    _In_ gvfg_handle handle,
    _In_opt_ gvfg_event_callback_t callback,
    _In_opt_ void *user_data);

/*
 * Start capture in callback mode.
 *
 * The SDK creates one frame worker thread for this handle. The frame callback
 * is not invoked concurrently for the same handle.
 */
GVFG_API gvfg_status_t gvfg_start_callback_mode(
    _In_ gvfg_handle handle);

/*
 * Stop callback mode and wait for the frame worker thread to exit.
 *
 * Do not call this from inside the frame callback.
 */
GVFG_API gvfg_status_t gvfg_stop_callback_mode(
    _In_ gvfg_handle handle);

/*
 * Read one captured frame.
 *
 * Parameters:
 * - handle: Running capture session.
 * - out_frame: Receives a frame descriptor. Must not be NULL.
 * - timeout_ms: Maximum time to wait. Use 0 to wait indefinitely.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_frame is NULL.
 * - GVFG_ESTATE if capture is not running or a previous frame has not been
 *   released.
 * - GVFG_ETIMEOUT if no frame is ready before timeout_ms expires.
 * - GVFG_EIO for driver/backend failures.
 *
 * The returned data pointer is owned by the SDK and remains valid until
 * gvfg_release_frame() is called. A handle may hold only one frame at a time.
 */
GVFG_API gvfg_status_t gvfg_read_frame(
    _In_ gvfg_handle handle,
    _Out_ gvfg_frame_t *out_frame,
    _In_ uint32_t timeout_ms);

/*
 * Query per-plane layout for a frame returned by gvfg_read_frame().
 *
 * Parameters:
 * - frame: Frame descriptor returned by gvfg_read_frame(). Must not be NULL.
 * - out_layout: Receives plane pointers, strides, sizes, and offsets. Must not
 *   be NULL. Set out_layout->struct_size to sizeof(gvfg_frame_layout_t) before
 *   calling so future SDKs can safely extend this struct.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if frame/out_layout is NULL or struct_size is too small.
 * - GVFG_ENOTSUP if the SDK cannot describe the frame layout.
 *
 * The returned plane_data pointers are owned by the SDK and remain valid only
 * until gvfg_release_frame() is called for the source frame.
 */
GVFG_API gvfg_status_t gvfg_get_frame_layout(
    _In_ const gvfg_frame_t *frame,
    _Inout_ gvfg_frame_layout_t *out_layout);

/*
 * Release a frame returned by gvfg_read_frame().
 *
 * Parameters:
 * - handle: Running capture session.
 * - frame: Frame previously returned by gvfg_read_frame(). The SDK currently
 *   uses this as a lifetime token; pass the same descriptor back.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or frame is NULL.
 * - GVFG_ESTATE if no frame is currently held.
 */
GVFG_API gvfg_status_t gvfg_release_frame(
    _In_ gvfg_handle handle,
    _In_ const gvfg_frame_t *frame);

/*
 * Poll one capture event.
 *
 * Parameters:
 * - handle: Opened session handle.
 * - out_event: Receives the event. Must not be NULL.
 * - timeout_ms: Maximum time to wait. Use 0 to return immediately.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_event is NULL.
 * - GVFG_ETIMEOUT if no event is available before timeout_ms expires.
 */
GVFG_API gvfg_status_t gvfg_poll_event(
    _In_ gvfg_handle handle,
    _Out_ gvfg_event_t *out_event,
    _In_ uint32_t timeout_ms);

/*
 * Stop capture.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 *
 * Returns:
 * - GVFG_OK on success, including when capture is already stopped.
 * - GVFG_EINVAL if handle is NULL.
 *
 * This stops backend capture and invalidates any unreleased frame.
 */
GVFG_API gvfg_status_t gvfg_stop(
    _In_ gvfg_handle handle);

/*
 * Query current signal information and delivered buffer format.
 *
 * Parameters:
 * - handle: Opened session handle.
 * - out_status: Receives signal status. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_status is NULL.
 * - GVFG_ENODEV if no valid signal information is available.
 *
 * Use gvfg_runtime_info_t::last_frame for the frame buffer format.
 */
GVFG_API gvfg_status_t gvfg_get_signal_status(
    _In_ gvfg_handle handle,
    _Out_ gvfg_signal_status_t *out_status);

/*
 * Query runtime capture diagnostics.
 *
 * Parameters:
 * - handle: Opened or running session handle.
 * - out_info: Receives runtime information. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_info is NULL.
 *
 * The result includes current signal status, SDK-measured capture FPS, and the
 * number of frames delivered by the SDK.
 */
GVFG_API gvfg_status_t gvfg_get_runtime_info(
    _In_ gvfg_handle handle,
    _Out_ gvfg_runtime_info_t *out_info);

/*
 * Convert a GVFG status code to a static English error string.
 *
 * Parameters:
 * - status: Status code returned by a GVFG API.
 *
 * Returns:
 * - Static null-terminated English string. The caller must not free it.
 */
GVFG_API const char *gvfg_strerror(
    _In_ gvfg_status_t status);

#ifdef __cplusplus
}
#endif
