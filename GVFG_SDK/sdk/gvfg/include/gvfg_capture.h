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
 *   const int count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);
 *   if (count <= 0)
 *       return;
 *
 *   gvfg_handle h = NULL;
 *   if (gvfg_create(&h) != GVFG_OK)
 *       return;
 *   if (gvfg_open_channel(h, 0, GVFG_CHANNEL_0) != GVFG_OK ||
 *       gvfg_start_channel(h, GVFG_CHANNEL_0) != GVFG_OK) {
 *       gvfg_destroy(h);
 *       return;
 *   }
 *
 *   while (running) {
 *       gvfg_frame_t frame = {};
 *       if (gvfg_read_channel_frame(h, GVFG_CHANNEL_0, &frame, 1000) == GVFG_OK) {
 *           // Use frame.data before releasing the frame.
 *           gvfg_release_channel_frame(h, GVFG_CHANNEL_0, &frame);
 *       }
 *   }
 *
 *   gvfg_stop(h);
 *   gvfg_destroy(h);
 *
 * Threading notes:
 * - The frame API is pull-based: applications call gvfg_read_channel_frame() from
 *   the thread they choose.
 * - The frame data pointer remains valid until gvfg_release_channel_frame() is called.
 *   Copy the data if it must outlive that call.
 * - At most one frame may be held by each channel at a time.
 * - Audio uses the same read/release ownership model. Applications should use
 *   a separate worker thread when reading video and audio concurrently.
 */

#ifdef _WIN32
#ifdef GVFG_BUILD
#define GVFG_API __declspec(dllexport)
#else
#define GVFG_API __declspec(dllimport)
#endif
#else
#define GVFG_API
#endif

#ifndef GVFG_PARAM_IN
#define GVFG_PARAM_IN
#endif
#ifndef GVFG_PARAM_OUT
#define GVFG_PARAM_OUT
#endif
#ifndef GVFG_PARAM_INOUT
#define GVFG_PARAM_INOUT
#endif

#include <stdint.h>

/* Consistent timeout value for an indefinite wait. A timeout of 0 never waits. */
#define GVFG_TIMEOUT_INFINITE UINT32_MAX

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        GVFG_MAX_DEVICES = 16,
    };

    typedef enum
    {
        GVFG_OK = 0,
        GVFG_EINVAL = -1,  /* Invalid argument, such as NULL handle/output pointer. */
        GVFG_ENODEV = -2,  /* No GVFG capture device, or the device cannot be opened. */
        GVFG_ESTATE = -3,  /* API was called in the wrong state. */
        GVFG_EIO = -4,     /* Driver/backend I/O failure. */
        GVFG_ENOTSUP = -5, /* Requested feature or format is not supported. */
        GVFG_ETIMEOUT = -6 /* Timed out waiting for driver/backend work. */
    } gvfg_status_t;

    typedef enum
    {
        GVFG_PIXFMT_UNKNOWN = 0,
        GVFG_PIXFMT_Y210 = 2,
        GVFG_PIXFMT_YUY2 = 3
    } gvfg_pixel_format_t;

    typedef enum
    {
        GVFG_CHANNEL_0 = 0,
        GVFG_CHANNEL_1 = 1
    } gvfg_channel_t;

    typedef enum
    {
        GVFG_INPUT_INTERFACE_SDI = 0,
        GVFG_INPUT_INTERFACE_HDMI = 1
    } gvfg_video_interface_t;

    typedef struct
    {
        uint32_t sample_rate;
        uint32_t channels;
        uint32_t bits_per_sample;
    } gvfg_audio_format_t;

    typedef struct
    {
        char name[128]; /* Display name for UI/logging. UTF-8, null-terminated. */
    } gvfg_device_info_t;

    typedef struct
    {
        uint32_t video_channel_count;
        int has_audio;
    } gvfg_device_capabilities_t;

    typedef struct
    {
        int connected;
        uint32_t mode;
        uint32_t resolution;
        uint32_t fps;
        int progressive;
        int level_b;
        uint32_t st352_payload;
        uint32_t error_count;
        char mode_name[16];
        char resolution_name[64];
        char fps_name[24];
        char scan_name[16];
        char st352_format_name[64];
        char st352_fps_name[24];
        char st352_chroma_name[36];
        char st352_bit_depth_name[24];
    } gvfg_sdi_info_t;

    typedef struct
    {
        int connected;    /* Non-zero while this channel has a valid input signal. */
        int channel;      /* gvfg_channel_t selected when the device was opened. */
        int width;        /* Signal width in pixels when connected. */
        int height;       /* Signal height in pixels when connected. */
        int pixel_format; /* gvfg_pixel_format_t value for the actual DMA payload. */
        int bit_depth;    /* Signal bit depth derived from the payload format. */
        int video_interface; /* gvfg_video_interface_t reported by GigabyteLib. */
    } gvfg_signal_status_t;

    typedef struct
    {
        double capture_fps;        /* Runtime FPS measured from frames returned by gvfg_read_channel_frame(). */
        uint64_t delivered_frames; /* Number of frames returned by gvfg_read_channel_frame(). */
    } gvfg_runtime_info_t;

    typedef struct
    {
        const void *data;     /* Native frame buffer. Valid until gvfg_release_channel_frame() is called. */
        uint64_t data_size;   /* Total bytes available from data. */
        int width;            /* Frame width in pixels. */
        int height;           /* Frame height in pixels. */
        int row_stride_bytes; /* Byte distance between the starts of adjacent rows. */
        int pixel_format;     /* gvfg_pixel_format_t value. */
        int bit_depth;        /* Bits per color channel of the native frame. */
        uint64_t frame_id;    /* GigabyteLib GVFG_VIDEO_INFO.FrameCount. */
        uint64_t timestamp_ns; /* Monotonic SDK delivery time; same clock domain as audio. */
    } gvfg_frame_t;

    typedef struct
    {
        const void *data;          /* PCM data. Valid until gvfg_release_channel_audio_frame(). */
        uint64_t data_size;        /* Valid PCM bytes available from data. */
        uint32_t sample_rate;      /* Samples per second. */
        uint32_t channels;         /* Interleaved PCM channel count. */
        uint32_t bits_per_sample;  /* Bits in each native PCM sample. */
        uint32_t reserved;
        uint64_t frame_id;         /* GigabyteLib GVFG_AUDIO_INFO.FrameCount. */
        uint64_t timestamp_ns;     /* Monotonic SDK delivery time; same clock domain as video. */
    } gvfg_audio_frame_t;

    /* Formats produced by gvfg_gpu_convert_to_buffer(). */
    typedef enum
    {
        /* DXGI-compatible B8G8R8A8_UNORM byte layout, alpha is 255. */
        GVFG_GPU_OUTPUT_BGRA8 = 1,
        /* DXGI-compatible R10G10B10A2_UNORM packed uint32 layout, alpha is 3. */
        GVFG_GPU_OUTPUT_RGB10A2 = 2,
        /* 8-bit BT.709 limited-range 4:2:0: Y plane followed by interleaved UV. */
        GVFG_GPU_OUTPUT_NV12 = 3
    } gvfg_gpu_output_format_t;

    typedef struct
    {
        void *data;         /* Caller-owned destination memory. */
        uint64_t data_size; /* Bytes available from data. */
        int row_bytes;      /* Destination stride. For NV12, shared by Y and UV planes. */
        int pixel_format;   /* gvfg_gpu_output_format_t. */
    } gvfg_gpu_output_buffer_t;

    typedef enum
    {
        GVFG_EVENT_UNKNOWN = 0,
        GVFG_EVENT_SIGNAL_CONNECTED = 1,
        GVFG_EVENT_SIGNAL_DISCONNECTED = 2,
        /* First complete frame after stream start, plug-in, or format recovery is ready. */
        GVFG_EVENT_STREAM_READY = 3,
        /* Input format is changing; pause use of resources created for the old format. */
        GVFG_EVENT_FORMAT_CHANGE_BEGIN = 4
    } gvfg_event_type_t;

    typedef struct
    {
        /* Set to sizeof(gvfg_event_t) before calling gvfg_poll_channel_event(). */
        uint32_t struct_size;
        int32_t type; /* gvfg_event_type_t value. */
        uint64_t reserved[4];
    } gvfg_event_t;

    /* Opaque session handle created by gvfg_create() and released by gvfg_destroy(). */
    typedef struct gvfg_handle_t *gvfg_handle;

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
        GVFG_PARAM_OUT gvfg_device_info_t *out_devices,
        GVFG_PARAM_IN int max_devices);

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
        GVFG_PARAM_OUT gvfg_handle *out_handle);

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
        GVFG_PARAM_IN gvfg_handle handle);

    /*
     * Open a device and select its capture channel.
     *
     * Parameters:
     * - handle: Session handle returned by gvfg_create().
     * - device_index: Zero-based position returned by gvfg_enumerate_devices().
     * - channel_index: GVFG_CHANNEL_0 or GVFG_CHANNEL_1.
     *
     * Returns:
     * - GVFG_OK on success.
     * - GVFG_EINVAL if handle is NULL or channel_index is invalid.
     * - GVFG_ENODEV if the device cannot be opened.
     * - GVFG_EIO for driver/backend failures.
     */
    GVFG_API gvfg_status_t gvfg_open_channel(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int device_index,
        GVFG_PARAM_IN int channel_index);

    /*
     * A handle may open both channels of the same device. The first call opens
     * the Windows device; the second channel shares that device connection and
     * owns independent capture, event, frame, and zero-copy state.
     */

    /*
     * Select driver zero-copy frame delivery.
     *
     * Call after gvfg_create() and before opening the selected channel. The
     * default is disabled independently for each channel. Once that channel is
     * open its mode cannot be changed. In zero-copy
     * mode, gvfg_read_channel_frame() returns driver-owned memory and every
     * successful read must be paired with gvfg_release_channel_frame().
     */
    GVFG_API gvfg_status_t gvfg_set_channel_zero_copy_enabled(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_IN int enabled);

    GVFG_API gvfg_status_t gvfg_get_channel_zero_copy_enabled(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT int *out_enabled);

    /*
     * Select the hardware output format before capture starts. This capability
     * currently uses an isolated driver extension because GvfgSdk.lib 1.0.0
     * does not expose an equivalent API.
     */
    GVFG_API gvfg_status_t gvfg_set_channel_video_format(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_IN gvfg_pixel_format_t format);

    /*
     * Enable or disable audio for the next channel start. Video remains
     * enabled; applications do not need to construct stream flag masks.
     */
    GVFG_API gvfg_status_t gvfg_set_channel_audio_enabled(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_IN int enabled);

    GVFG_API gvfg_status_t gvfg_get_channel_audio_format(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT gvfg_audio_format_t *out_format);

    /*
     * Configure and start capture on an opened device.
     *
     * Parameters:
     * - handle: Running session handle.
     *
     * Returns:
     * - GVFG_OK on success, including when capture is already running or the SDK
     *   has entered signal-monitoring mode while no input is connected.
     * - GVFG_EINVAL if handle is NULL.
     * - GVFG_ESTATE if no device is open.
     * - GVFG_EIO or another status code if stream configuration/start fails.
     *
     * After success, call gvfg_read_channel_frame() to receive frames and
     * gvfg_poll_channel_event()
     * to receive capture events. With no input signal, frame reads time out; capture
     * starts automatically after a signal-connected event.
     */
    GVFG_API gvfg_status_t gvfg_start_channel(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index);

    /*
     * Read one captured frame.
     *
     * Parameters:
     * - handle: Running capture session.
     * - out_frame: Receives a frame descriptor. Must not be NULL.
     * - timeout_ms: Maximum time to wait. Use 0 for a non-blocking read or
     *   GVFG_TIMEOUT_INFINITE to wait indefinitely.
     *
     * Returns:
     * - GVFG_OK on success.
     * - GVFG_EINVAL if handle or out_frame is NULL.
     * - GVFG_ESTATE if capture is not running or a previous frame has not been
     *   released.
     * - GVFG_ETIMEOUT if no frame is ready before timeout_ms expires.
     * - GVFG_EIO for driver/backend failures.
     *
     * In copy mode the returned data pointer is owned by the SDK. In zero-copy
     * mode it points to driver-owned memory. In both modes it remains valid
     * until gvfg_release_channel_frame() is called, and each channel may hold
     * only one frame.
     */
    GVFG_API gvfg_status_t gvfg_read_channel_frame(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT gvfg_frame_t *out_frame,
        GVFG_PARAM_IN uint32_t timeout_ms);

    /*
     * Release a frame returned by gvfg_read_channel_frame().
     *
     * Parameters:
     * - handle: Running capture session.
     * - frame: Frame previously returned by gvfg_read_channel_frame(). The SDK currently
     *   uses this as a lifetime token; pass the same descriptor back.
     *
     * Returns:
     * - GVFG_OK on success.
     * - GVFG_EINVAL if handle or frame is NULL.
     * - GVFG_ESTATE if no frame is currently held.
     */
    GVFG_API gvfg_status_t gvfg_release_channel_frame(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_IN const gvfg_frame_t *frame);

    /*
     * Acquire one PCM frame. The channel must have audio enabled before start.
     * The returned SDK-owned data remains valid until the matching release.
     * Each channel may hold only one audio frame at a time.
     */
    GVFG_API gvfg_status_t gvfg_read_channel_audio_frame(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT gvfg_audio_frame_t *out_frame,
        GVFG_PARAM_IN uint32_t timeout_ms);

    /* Release the unchanged token returned by gvfg_read_channel_audio_frame(). */
    GVFG_API gvfg_status_t gvfg_release_channel_audio_frame(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_IN const gvfg_audio_frame_t *frame);

    /*
     * Convert a captured YUY2 or Y210 frame with the GPU and copy the result
     * into caller-owned memory.
     *
     * This call is synchronous. The source frame and destination buffer must
     * remain valid until it returns; the SDK retains neither pointer. A frame
     * returned by gvfg_read_channel_frame() must therefore be converted before
     * gvfg_release_channel_frame().
     *
     * BGRA8 and RGB10A2 require at least width * 4 bytes per row and
     * output->data_size >= output->row_bytes * height.
     * NV12 requires even width and height, row_bytes >= width, and
     * output->data_size >= output->row_bytes * (height + height / 2). Its
     * layout is height rows of Y followed by height / 2 rows of interleaved UV.
     */
    GVFG_API gvfg_status_t gvfg_gpu_convert_to_buffer(
        GVFG_PARAM_IN const gvfg_frame_t *source,
        GVFG_PARAM_IN const gvfg_gpu_output_buffer_t *output);

    /*
     * Convenience wrappers for the corresponding gvfg_gpu_output_format_t.
     * These calls have the same synchronous lifetime rules as
     * gvfg_gpu_convert_to_buffer().
     *
     * BGRA8 and RGB10A2 require row_bytes >= source->width * 4 and
     * destination_size >= row_bytes * source->height.
     */
    GVFG_API gvfg_status_t gvfg_gpu_convert_to_bgra8(
        GVFG_PARAM_IN const gvfg_frame_t *source,
        GVFG_PARAM_OUT void *destination,
        GVFG_PARAM_IN uint64_t destination_size,
        GVFG_PARAM_IN int row_bytes);

    GVFG_API gvfg_status_t gvfg_gpu_convert_to_rgb10a2(
        GVFG_PARAM_IN const gvfg_frame_t *source,
        GVFG_PARAM_OUT void *destination,
        GVFG_PARAM_IN uint64_t destination_size,
        GVFG_PARAM_IN int row_bytes);

    /*
     * Convert to BT.709 limited-range NV12. Width and height must be even.
     * row_bytes must be at least source->width and is shared by both planes.
     * destination_size must be at least row_bytes * (height + height / 2).
     */
    GVFG_API gvfg_status_t gvfg_gpu_convert_to_nv12(
        GVFG_PARAM_IN const gvfg_frame_t *source,
        GVFG_PARAM_OUT void *destination,
        GVFG_PARAM_IN uint64_t destination_size,
        GVFG_PARAM_IN int row_bytes);

    /*
     * Poll one capture event.
     *
     * Parameters:
     * - handle: Opened session handle.
     * - out_event: Receives the event. Initialize it to zero and set
     *   struct_size to sizeof(gvfg_event_t).
     * - timeout_ms: Maximum time to wait. Use 0 to return immediately or
     *   GVFG_TIMEOUT_INFINITE to wait indefinitely.
     *
     * Returns:
     * - GVFG_OK on success.
     * - GVFG_EINVAL if handle/out_event is NULL or struct_size is invalid.
     * - GVFG_ESTATE if no capture device is open or capture has been stopped.
     * - GVFG_ETIMEOUT if no event is available before timeout_ms expires.
     */
    GVFG_API gvfg_status_t gvfg_poll_channel_event(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_INOUT gvfg_event_t *out_event,
        GVFG_PARAM_IN uint32_t timeout_ms);

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
     * This stops DMA capture and signal/format event monitoring, invalidates
     * any unreleased frames, and wakes blocking gvfg_poll_channel_event() calls.
     */
    GVFG_API gvfg_status_t gvfg_stop(
        GVFG_PARAM_IN gvfg_handle handle);

    /* gvfg_stop() stops every opened channel; this function stops only one. */
    GVFG_API gvfg_status_t gvfg_stop_channel(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index);

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
     * - GVFG_ESTATE if no capture device is open.
     *
     * No input signal is a normal state: the function returns GVFG_OK with
     * out_status->connected set to 0.
     */
    GVFG_API gvfg_status_t gvfg_get_channel_signal_status(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT gvfg_signal_status_t *out_status);

    /* Optional vendor-backed device and SDI detail queries. */
    GVFG_API gvfg_status_t gvfg_get_device_capabilities(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_OUT gvfg_device_capabilities_t *out_capabilities);

    GVFG_API gvfg_status_t gvfg_get_channel_sdi_info(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT gvfg_sdi_info_t *out_info);

    /*
     * Query runtime frame-delivery diagnostics.
     *
     * Parameters:
     * - handle: Opened or running session handle.
     * - out_info: Receives runtime information. Must not be NULL.
     *
     * Returns:
     * - GVFG_OK on success.
     * - GVFG_EINVAL if handle or out_info is NULL.
     *
     * The result includes SDK-measured capture FPS and the number of frames
     * delivered by the SDK. Query current input signal metadata separately with
     * gvfg_get_channel_signal_status().
     */
    GVFG_API gvfg_status_t gvfg_get_channel_runtime_info(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT gvfg_runtime_info_t *out_info);

    /* Return the loaded GVFG runtime DLL version, for example "0.2.1". */
    GVFG_API const char *gvfg_get_version(void);

    /* Convert a gvfg_pixel_format_t value to a static English format name. */
    GVFG_API const char *gvfg_pixel_format_name(
        GVFG_PARAM_IN int pixel_format);

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
        GVFG_PARAM_IN gvfg_status_t status);

    /*
     * Copy the most recent detailed fault or rejected API call for one
     * channel. Event-poll timeouts are not stored; frame-read timeouts currently
     * retain the backend timeout detail. A successful gvfg_start_channel()
     * begins a new diagnostic lifetime.
     */
    GVFG_API gvfg_status_t gvfg_get_channel_last_error_detail(
        GVFG_PARAM_IN gvfg_handle handle,
        GVFG_PARAM_IN int channel_index,
        GVFG_PARAM_OUT char *out_message,
        GVFG_PARAM_IN uint32_t out_message_size);

#ifdef __cplusplus
}
#endif
