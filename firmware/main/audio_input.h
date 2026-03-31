#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      1

esp_err_t audio_input_init(void);
// Read PCM samples into buf. Returns number of samples read.
int audio_input_read(int16_t *buf, size_t max_samples);
