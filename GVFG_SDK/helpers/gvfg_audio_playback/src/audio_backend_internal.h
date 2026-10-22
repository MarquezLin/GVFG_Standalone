#pragma once

/* Private static backend; not part of the customer API. */
#include "gvfg_audio_playback.h"

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

/* Start PCM16 playback on the Windows default output device. */
gvfg_audio_playback_status_t gvfg_audio_player_start(
    gvfg_audio_player player,
    const gvfg_audio_playback_format_t *format);

/*
 * Copy one or more complete interleaved PCM frames into the playback queue.
 * The caller may release or reuse data immediately after this function
 * returns. EQUEUE_FULL means the packet was not accepted.
 */
gvfg_audio_playback_status_t gvfg_audio_player_write(
    gvfg_audio_player player,
    const void *data,
    uint32_t data_size);

/* Clear queued PCM, reset the audio stream and its position bookkeeping. */
gvfg_audio_playback_status_t gvfg_audio_player_reset_timeline(
    gvfg_audio_player player);

/* Set software PCM16 playback gain. The accepted range is 0.0 to 2.0. */
gvfg_audio_playback_status_t gvfg_audio_player_set_volume(
    gvfg_audio_player player,
    float volume);

gvfg_audio_playback_status_t gvfg_audio_player_stop(
    gvfg_audio_player player);

void gvfg_audio_player_destroy(
    gvfg_audio_player player);

#ifdef __cplusplus
}
#endif
