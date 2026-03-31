#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t audio_output_init(void);
// Write raw PCM (16kHz/16bit/mono) to I2S speaker. Blocks until written.
esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len);
