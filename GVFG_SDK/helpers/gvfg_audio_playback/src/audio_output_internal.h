#pragma once
#include "audio_backend_internal.h"
#include <cstdint>

// Private bridge inside the same DLL; never exported or exposed to applications.
struct AudioOutputPosition {
    uint64_t played_frames = 0;
    uint64_t queued_frames = 0;
    bool running = false;
    bool paused = true;
    bool resetting = false;
};
gvfg_audio_player audio_output_create_managed();
AudioOutputPosition audio_output_position(gvfg_audio_player p);
void audio_output_pause(gvfg_audio_player p, bool paused);
void audio_output_rate(gvfg_audio_player p, double ratio);
