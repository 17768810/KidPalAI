#pragma once
#include "esp_err.h"

// POST text to gateway/tts, receive PCM, play via audio_output_write_pcm().
// Blocks until all PCM has been played.
// gateway_tts_url example: "http://8.133.3.7:8000/tts"
esp_err_t voice_tts(const char *gateway_tts_url, const char *text);
