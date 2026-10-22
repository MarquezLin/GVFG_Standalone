#pragma once

/* Public PCM format types used by gvfg_playback.h. */
#include <stdint.h>

typedef struct
{
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample; /* Signed interleaved PCM; must be 16. */
} gvfg_audio_playback_format_t;
