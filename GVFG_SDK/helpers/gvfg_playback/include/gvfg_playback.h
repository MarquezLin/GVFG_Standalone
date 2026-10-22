#pragma once

/* Unified playback API, exported by gvfg_playback.dll.
 * Audio and preview backends are statically linked into this DLL.
 * Capture remains application owned. Submissions COPY data before returning;
 * the caller may immediately release its SDK frame. Handles own both outputs.
 * Control calls (start/stop/reset/attach/destroy) must be serialized by the
 * caller; one video producer and one audio producer may submit concurrently.
 */
#include "gvfg_audio_playback.h"
#include "gvfg_preview.h"

#ifdef _WIN32
#ifdef GVFG_PLAYBACK_BUILD
#define GVFG_PLAYBACK_API __declspec(dllexport)
#else
#define GVFG_PLAYBACK_API __declspec(dllimport)
#endif
#else
#define GVFG_PLAYBACK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gvfg_playback_t *gvfg_playback;
typedef enum {
    GVFG_PLAYBACK_OK = 0,
    GVFG_PLAYBACK_EINVAL = -1,
    GVFG_PLAYBACK_ESTATE = -2,
    GVFG_PLAYBACK_EOUTPUT = -3,
    GVFG_PLAYBACK_EQUEUE_FULL = -4,
    GVFG_PLAYBACK_EDISCONTINUITY = -5,
    GVFG_PLAYBACK_EAUDIO = -6 /* A/V audio output disabled; video remains running. */
} gvfg_playback_status_t;
typedef enum {
    GVFG_PLAYBACK_VIDEO_ONLY = 1,
    GVFG_PLAYBACK_AUDIO_ONLY = 2,
    GVFG_PLAYBACK_AUDIO_VIDEO = 3
} gvfg_playback_mode_t;
typedef enum {
    GVFG_PLAYBACK_MASTER_EXTERNAL = 0,
    GVFG_PLAYBACK_MASTER_AUDIO = 1,
    GVFG_PLAYBACK_MASTER_VIDEO = 2
} gvfg_playback_master_t;

typedef struct {
    uint32_t struct_size;
    gvfg_playback_mode_t mode;
    gvfg_playback_master_t master;
    gvfg_audio_playback_format_t audio_format;
    uint32_t preroll_ms;       /* Default 100; required coverage of both streams. */
    uint32_t max_video_frames; /* Default 32; late preview frames are retired and counted. */
    uint32_t max_audio_ms;     /* Default 1000. */
    uint64_t max_video_bytes;  /* Default 256 MiB, including in-flight copy. */
} gvfg_playback_config_t;
typedef struct {
    gvfg_preview_frame_t image;
    uint64_t timestamp_ns; /* Common monotonic delivery clock, NOT UTC. */
} gvfg_playback_video_frame_t;
typedef struct {
    uint32_t struct_size;
    int running;
    int buffering;
    gvfg_playback_status_t last_error;
    uint64_t video_submitted;
    uint64_t video_processed; /* Includes hidden frames and counted late/skipped previews. */
    uint64_t video_presented;
    uint64_t audio_submitted;
    uint64_t queue_rejections;
    uint64_t rebuffer_count;
    uint64_t video_queue_bytes;
    uint32_t video_queue_frames;
    double audio_queue_ms;
    uint64_t video_timestamp_ns;
    uint64_t audio_timestamp_ns; /* Estimated output position, NOT acoustic time. */
    int64_t av_difference_ns;   /* audio - video; valid only with both clocks. */
    double audio_rate_ratio;
    uint64_t video_dropped; /* Late or GPU-busy preview frames; never audio/recording loss. */
    int sync_degraded;      /* Independent best-effort playback; capture may continue. */
    uint64_t sync_fallbacks;
    uint64_t audio_dropped_frames; /* PCM frames discarded from the core queue, not device padding. */
} gvfg_playback_stats_t;

GVFG_PLAYBACK_API void gvfg_playback_default_config(gvfg_playback_config_t *config);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_create(gvfg_playback *out);
/* A null HWND hides video while preserving its timeline. Attach on HWND thread. */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_attach_window(gvfg_playback p, void *hwnd);
/* Forward Capture's signal.connected (0/1). The helper owns the NO SIGNAL
 * overlay; it does not query Capture or infer signal from missing frames.
 * Call on the attached HWND thread, serialized with attach/destroy.
 * Default connected=1. State persists across stop/start/reset and reattach. */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_set_signal_connected(gvfg_playback p, int connected);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_start(gvfg_playback p, const gvfg_playback_config_t *config);
/* Monotonic timestamps required per stream. Zero/reversed timestamps rejected.
 * Queue pressure triggers bounded best-effort playback; obsolete queued data
 * may be discarded even on OK. Never block capture holding an SDK frame.
 * A single packet/frame exceeding its configured capacity returns EINVAL.
 * Each PCM submission must contain at most 100 ms of audio.
 * Audio timestamp is SDK DELIVERY time. The first PCM sample is estimated by
 * subtracting packet duration. Following PCM remains continuous while its
 * source-time mapping smoothly tracks long-term delivery-clock differences.
 * Forward audio delivery gaps recover automatically in best-effort playback.
 * Delivery timestamps cannot recover unknown hardware latency or lost samples.
 */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_submit_video(gvfg_playback p, const gvfg_playback_video_frame_t *frame);
/* Synchronously present one still image without starting a playback timeline.
 * Requires an attached window and a stopped handle. Caller data stays valid
 * through return. Used for RAW/DICOM image display; no audio is initialized. */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_present_image(gvfg_playback p, const gvfg_preview_frame_t *image);
/* In A/V mode an audio-output failure disables monitoring for this handle,
 * preserving video playback. Subsequent audio submissions return EAUDIO.
 * Keep capture/recording audio independent; do not retry output on this handle.
 * Recreate the handle to retry monitoring. Stop/reset does not clear EAUDIO.
 * A/V start may succeed with monitoring disabled; audio-only start still fails
 * if its output is unavailable. Other errors retain their normal meaning. */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_submit_audio(gvfg_playback p, const void *pcm, uint32_t bytes, uint64_t timestamp_ns);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_set_volume(gvfg_playback p, float gain);
/* Default enabled=1. Set 0 for independent best-effort playback with no
 * automatic return to synchronization. May be called before start or while
 * active. Re-enabling waits for stable input/output before synchronizing.
 * This choice persists across stop/start and reset_timeline. */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_set_sync_enabled(gvfg_playback p, int enabled);
/* Explicit discontinuity: discards pending output and establishes a new epoch.
 * Stop/reset discard queued data; they are NOT lossless end-of-stream drains. */
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_reset_timeline(gvfg_playback p);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_stop(gvfg_playback p);
GVFG_PLAYBACK_API void gvfg_playback_destroy(gvfg_playback p);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_get_stats(gvfg_playback p, gvfg_playback_stats_t *stats);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_get_preview_info(gvfg_playback p, gvfg_preview_info_t *info);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_get_preview_stats(gvfg_playback p, gvfg_preview_stats_t *stats);
GVFG_PLAYBACK_API gvfg_playback_status_t gvfg_playback_get_delivery_stats(gvfg_playback p, gvfg_preview_delivery_stats_t *stats);
GVFG_PLAYBACK_API const char *gvfg_playback_strerror(gvfg_playback_status_t status);

#ifdef __cplusplus
}
#endif
