#pragma once

/*
 * Customer-facing GVFG capture API.
 *
 * Include only this header in customer applications. Internal XDMA backend
 * headers are implementation details behind gvfg.dll.
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

#include <stdint.h>

#ifdef _WIN32
#ifdef GVFG_BUILD
#define GVFG_API __declspec(dllexport)
#else
#define GVFG_API __declspec(dllimport)
#endif
#else
#define GVFG_API
#endif

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

typedef struct
{
    int index;       /* Device index to pass to gvfg_open(). */
    char name[128];  /* Display name for UI/logging. UTF-8, null-terminated. */
} gvfg_device_info_t;

typedef struct
{
    uint32_t width;         /* Raw FPGA width value. */
    uint32_t height;        /* Raw FPGA height value. */
    uint32_t video_format;  /* Raw FPGA video-format value. */
    uint32_t frame_rate;    /* Raw FPGA frame-rate value. */
    uint32_t bit_depth;     /* Raw FPGA bit-depth value. */
    uint32_t status;        /* Raw FPGA lock/status value. */
} gvfg_fpga_signal_raw_t;

typedef struct
{
    int width;                       /* Signal width in pixels, from FPGA when available. */
    int height;                      /* Signal height in pixels, from FPGA when available. */
    int video_format_code;           /* 0=yuv422, 1=rgb, 2=yuv444, 3=yuv420. */
    char video_format[16];           /* Decoded FPGA signal format name. */
    int frame_rate_code;             /* FPGA frame-rate code. */
    char frame_rate_bits[5];         /* 4-bit binary text, for example "0110". */
    char frame_rate_name[16];        /* None, 23.98, 24, 47.95, ..., or "--" for unsupported codes. */
    int bit_depth;                   /* FPGA signal bit depth: 8 or 10 when valid. */
    int sdi_locked;                  /* Non-zero when SDI reports locked. */
    int sdi_ddr_ok;                  /* Non-zero when SDI DDR status is OK. */
    int hdmi_locked;                 /* Non-zero when HDMI reports locked. */
    int hdmi_ddr_ok;                 /* Non-zero when HDMI DDR status is OK. */
    gvfg_fpga_signal_raw_t raw;       /* Raw FPGA values for diagnostics; validity is handled by the SDK. */
} gvfg_signal_status_t;

typedef struct
{
    int width;              /* Width of the most recent frame returned by gvfg_read_frame(). */
    int height;             /* Height of the most recent frame returned by gvfg_read_frame(). */
    int bit_depth;          /* Bits per color channel of the frame buffer. */
    char pixel_format[32];  /* Native frame buffer format, for example YUY2, Y210, NV12, or P010. */
    int valid;              /* Non-zero while capture is running after at least one frame read. */
} gvfg_callback_frame_info_t;

typedef struct
{
    gvfg_signal_status_t input_signal; /* FPGA-reported signal metadata. */
    gvfg_callback_frame_info_t callback_frame;   /* Last frame returned by gvfg_read_frame(); name kept for ABI compatibility. */
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

typedef enum
{
    GVFG_EVENT_VIDEO_IRQ = 1,
    GVFG_EVENT_PLUG_IN = 2,
    GVFG_EVENT_PLUG_OUT = 3,
    GVFG_EVENT_CAPTURE_PAUSED = 4,
    GVFG_EVENT_CAPTURE_RESUMED = 5
} gvfg_event_type_t;

enum
{
    GVFG_EVENT_MASK_VIDEO_IRQ = 1u << 0,
    GVFG_EVENT_MASK_PLUG_IN = 1u << 1,
    GVFG_EVENT_MASK_PLUG_OUT = 1u << 2,
    GVFG_EVENT_MASK_CAPTURE_PAUSED = 1u << 3,
    GVFG_EVENT_MASK_CAPTURE_RESUMED = 1u << 4,
    GVFG_EVENT_MASK_HOTPLUG = GVFG_EVENT_MASK_PLUG_IN |
                              GVFG_EVENT_MASK_PLUG_OUT |
                              GVFG_EVENT_MASK_CAPTURE_PAUSED |
                              GVFG_EVENT_MASK_CAPTURE_RESUMED,
    GVFG_EVENT_MASK_DEFAULT = GVFG_EVENT_MASK_HOTPLUG,
    GVFG_EVENT_MASK_ALL = GVFG_EVENT_MASK_VIDEO_IRQ | GVFG_EVENT_MASK_HOTPLUG
};

typedef struct
{
    gvfg_event_type_t type;
    uint32_t irq_bit;
    uint32_t irq_mask;
    uint64_t timestamp_ns;
} gvfg_event_t;

/* Opaque session handle created by gvfg_create() and released by gvfg_destroy(). */
typedef struct gvfg_handle_t *gvfg_handle;

/* Legacy callback type kept for source compatibility. */
typedef void (*gvfg_on_frame_cb)(const gvfg_frame_t *frame, void *user);

/* Called for capture events such as PLUG_IN / PLUG_OUT. */
typedef void (*gvfg_on_event_cb)(const gvfg_event_t *event, void *user);

/* Called for asynchronous SDK messages or errors. */
typedef void (*gvfg_on_error_cb)(gvfg_status_t status, const char *message, void *user);

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
GVFG_API int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);

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
GVFG_API gvfg_status_t gvfg_create(gvfg_handle *out_handle);

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
GVFG_API gvfg_status_t gvfg_destroy(gvfg_handle handle);

/*
 * Register legacy frame and error callbacks.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - on_frame: Reserved for the legacy callback path. The pull-based
 *   gvfg_read_frame() API is the primary frame API.
 * - on_error: Function called for asynchronous SDK messages/errors. Pass NULL
 *   if error callbacks are not needed.
 * - user: Application-defined pointer passed back to both callbacks.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 *
 * New applications should use gvfg_read_frame() and gvfg_release_frame()
 * instead of frame callbacks.
 */
GVFG_API gvfg_status_t gvfg_set_callbacks(gvfg_handle handle,
                                             gvfg_on_frame_cb on_frame,
                                             gvfg_on_error_cb on_error,
                                             void *user);

/*
 * Configure frame callback rate.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - frame_interval: 0 or 1 calls on_frame for every delivered frame. N > 1
 *   calls on_frame once for every N delivered backend frames.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 *
 * This setting is kept for source compatibility with the legacy callback API.
 */
GVFG_API gvfg_status_t gvfg_set_frame_callback_interval(gvfg_handle handle,
                                                        uint32_t frame_interval);

/*
 * Register legacy capture event callback.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - on_event: Function called when an enabled capture event occurs. Pass NULL
 *   to disable event callbacks. New applications should use gvfg_poll_event().
 * - user: Application-defined pointer passed back to the callback.
 * - event_mask: GVFG_EVENT_MASK_* bits. Pass 0 to use GVFG_EVENT_MASK_DEFAULT.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 *
 * VIDEO_IRQ is not included in the default mask because it can occur once per
 * video frame. Enable GVFG_EVENT_MASK_VIDEO_IRQ only for debug or when the
 * application explicitly needs it.
 */
GVFG_API gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                               gvfg_on_event_cb on_event,
                                               void *user,
                                               uint32_t event_mask);

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
GVFG_API gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);

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
GVFG_API gvfg_status_t gvfg_start(gvfg_handle handle);

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
GVFG_API gvfg_status_t gvfg_read_frame(gvfg_handle handle,
                                       gvfg_frame_t *out_frame,
                                       uint32_t timeout_ms);

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
GVFG_API gvfg_status_t gvfg_release_frame(gvfg_handle handle,
                                          const gvfg_frame_t *frame);

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
GVFG_API gvfg_status_t gvfg_poll_event(gvfg_handle handle,
                                       gvfg_event_t *out_event,
                                       uint32_t timeout_ms);

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
GVFG_API gvfg_status_t gvfg_stop(gvfg_handle handle);

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
 * The FPGA metadata describes the hardware signal. Use
 * gvfg_runtime_info_t::callback_frame for the callback buffer format.
 */
GVFG_API gvfg_status_t gvfg_get_signal_status(gvfg_handle handle, gvfg_signal_status_t *out_status);

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
GVFG_API gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle, gvfg_runtime_info_t *out_info);

/*
 * Convert a GVFG status code to a static English error string.
 *
 * Parameters:
 * - status: Status code returned by a GVFG API.
 *
 * Returns:
 * - Static null-terminated English string. The caller must not free it.
 */
GVFG_API const char *gvfg_strerror(gvfg_status_t status);

#ifdef __cplusplus
}
#endif
