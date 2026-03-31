#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_FRAME_SAMPLES 320   // 20ms at 16kHz

esp_err_t audio_input_init(void);
// Returns number of int16 samples read (always AUDIO_FRAME_SAMPLES on success, ≤0 on error)
int audio_input_read(int16_t *buf, size_t max_samples);
