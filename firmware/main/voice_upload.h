#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Callback invoked for each PCM chunk received from the gateway.
// data: pointer to chunk bytes (valid only during callback)
// len:  number of bytes in this chunk
typedef void (*voice_pcm_callback_t)(const uint8_t *data, int len);

// Original blocking upload — returns heap-allocated MP3. Caller must free mp3_out.
esp_err_t voice_upload(const char *url,
                       const uint8_t *pcm_data, size_t pcm_len,
                       uint8_t **mp3_out, size_t *mp3_len);

// Streaming upload to /voice/stream endpoint.
// Sends PCM, receives chunked PCM response, calls on_chunk for each received chunk.
// Blocks until the full response is received or an error occurs.
// watchdog_ms: abort if no data received for this many ms (0 = disable watchdog)
esp_err_t voice_upload_stream(const char *base_url,
                               const uint8_t *pcm_data, size_t pcm_len,
                               voice_pcm_callback_t on_chunk,
                               int watchdog_ms);
