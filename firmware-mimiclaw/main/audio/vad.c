#include "vad.h"
#include <stdlib.h>

static int s_loud_frames = 0;

int32_t vad_frame_energy(const int16_t *frame, int samples)
{
    int32_t energy = 0;
    for (int i = 0; i < samples; i++) energy += abs(frame[i]);
    return energy / samples;
}

bool vad_update_wake(int32_t energy)
{
    if (energy > VAD_ENERGY_THRESHOLD * VAD_WAKE_MULTIPLIER) {
        s_loud_frames++;
    } else {
        s_loud_frames = 0;
    }
    if (s_loud_frames >= VAD_WAKE_FRAMES) {
        s_loud_frames = 0;
        return true;
    }
    return false;
}

bool vad_update_silence(int32_t energy, int *silence_frames)
{
    if (energy < VAD_ENERGY_THRESHOLD) {
        (*silence_frames)++;
    } else {
        *silence_frames = 0;
    }
    return (*silence_frames >= VAD_SILENCE_FRAMES);
}
