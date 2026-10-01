#pragma once

/*
 * Optional GVFG audio playback helper.
 *
 * The helper copies application-owned interleaved PCM into a bounded queue
 * and plays it through the Windows default audio output using WASAPI. It has
 * no dependency on gvfg.dll or Qt.
 */

#include <stdint.h>

#ifdef _WIN32
#ifdef GVFG_AUDIO_PLAYBACK_BUILD
#define GVFG_AUDIO_PLAYBACK_API __declspec(dllexport)
#else
#define GVFG_AUDIO_PLAYBACK_API __declspec(dllimport)
#endif
#else
#define GVFG_AUDIO_PLAYBACK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GVFG_AUDIO_PLAYBACK_OK = 0,
    GVFG_AUDIO_PLAYBACK_EINVAL = -1,
    GVFG_AUDIO_PLAYBACK_ESTATE = -2,
    GVFG_AUDIO_PLAYBACK_ENODEV = -3,
    GVFG_AUDIO_PLAYBACK_EFORMAT = -4,
    GVFG_AUDIO_PLAYBACK_EQUEUE_FULL = -5,
    GVFG_AUDIO_PLAYBACK_EIO = -6
} gvfg_audio_playback_status_t;

typedef struct gvfg_audio_player_t *gvfg_audio_player;

typedef struct
{
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample; /* Signed interleaved PCM; must be 16. */
} gvfg_audio_playback_format_t;

GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_create(
    gvfg_audio_player *out_player);

/* Start PCM16 playback on the Windows default output device. */
GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_start(
    gvfg_audio_player player,
    const gvfg_audio_playback_format_t *format);

/*
 * Copy one or more complete interleaved PCM frames into the playback queue.
 * The caller may release or reuse data immediately after this function
 * returns. EQUEUE_FULL means the packet was not accepted.
 */
GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_write(
    gvfg_audio_player player,
    const void *data,
    uint32_t data_size);

/* Queue PCM with its SDK delivery timestamp for A/V synchronization. */
GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_write_timed(
    gvfg_audio_player player,
    const void *data,
    uint32_t data_size,
    uint64_t audio_timestamp_ns);

/* Publish the newest video timestamp from the same SDK clock domain. */
GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_update_video_timestamp(
    gvfg_audio_player player,
    uint64_t video_timestamp_ns);

/* Clear queued PCM and establish a new A/V synchronization timeline. */
GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_reset_timeline(
    gvfg_audio_player player);

/* Set software PCM16 playback gain. The accepted range is 0.0 to 2.0. */
GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_set_volume(
    gvfg_audio_player player,
    float volume);

GVFG_AUDIO_PLAYBACK_API gvfg_audio_playback_status_t gvfg_audio_player_stop(
    gvfg_audio_player player);

GVFG_AUDIO_PLAYBACK_API void gvfg_audio_player_destroy(
    gvfg_audio_player player);

GVFG_AUDIO_PLAYBACK_API const char *gvfg_audio_player_strerror(
    gvfg_audio_playback_status_t status);

#ifdef __cplusplus
}
#endif
