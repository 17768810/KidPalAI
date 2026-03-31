#pragma once
#include <stdint.h>
#include <stdbool.h>

// VAD configuration
#define VAD_ENERGY_THRESHOLD  200   // silence threshold (tune per environment)
#define VAD_SILENCE_FRAMES    75    // 1.5s silence → stop recording (75 × 20ms)
#define VAD_WAKE_FRAMES       25    // 0.5s loud audio → wake trigger (25 × 20ms)
#define VAD_WAKE_MULTIPLIER   5     // wake threshold = VAD_ENERGY_THRESHOLD × 5
#define MAX_RECORD_FRAMES     150   // max recording: 3s (150 × 20ms × 320 samples = 96KB)

// Returns mean energy of a 16-bit PCM frame
int32_t vad_frame_energy(const int16_t *frame, int samples);

// Update wake detector state. Returns true when wake is triggered.
// Call once per 20ms frame. Resets after returning true.
bool vad_update_wake(int32_t energy);

// Update silence detector state. Returns true when silence threshold exceeded.
// Reset silence_frames to 0 after a non-silent frame.
bool vad_update_silence(int32_t energy, int *silence_frames);
