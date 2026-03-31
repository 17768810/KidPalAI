#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// URLs for gateway STT and TTS
#define AUDIO_GATEWAY_STT_URL  "http://8.133.3.7:8000/stt"
#define AUDIO_GATEWAY_TTS_URL  "http://8.133.3.7:8000/tts"

// Sentence queue: agent_loop writes char* sentences, audio_task consumes them.
// NULL pointer = sentinel (end of response).
extern QueueHandle_t g_sentence_queue;

// Start audio_task on Core 0 at priority 10.
// Call once from app_main after WiFi is connected.
void audio_task_start(void);
