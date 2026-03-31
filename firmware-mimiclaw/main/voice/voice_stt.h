#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// Maximum transcription text length
#define VOICE_STT_MAX_TEXT 512

// POST PCM to gateway/stt, write transcribed text to text_out.
// Returns ESP_OK on success. text_out[0]=='\0' means empty result (silence).
// gateway_stt_url example: "http://8.133.3.7:8000/stt"
esp_err_t voice_stt(const char *gateway_stt_url,
                    const uint8_t *pcm, size_t pcm_len,
                    char *text_out, size_t text_max);
