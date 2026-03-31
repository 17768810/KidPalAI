#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t audio_output_init(void);

// Legacy stub — kept for reference, not used in streaming mode
esp_err_t audio_output_play_mp3(const uint8_t *mp3_data, size_t mp3_len);

// Write raw 16kHz 16-bit mono PCM samples directly to I2S output.
// Called repeatedly as PCM chunks arrive from the gateway.
esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len);
