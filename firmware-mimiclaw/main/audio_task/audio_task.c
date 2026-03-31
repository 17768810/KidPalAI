#include "audio_task.h"
#include "audio/audio_input.h"
#include "audio/audio_output.h"
#include "audio/vad.h"
#include "voice/voice_stt.h"
#include "voice/voice_tts.h"
#include "led/led.h"
#include "bus/message_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio_task";

QueueHandle_t g_sentence_queue = NULL;

// PCM recording buffer: 150 frames × 320 samples × 2 bytes = 96KB
// Allocate from PSRAM (8MB available on N16R8).
#define PCM_BUF_SIZE (MAX_RECORD_FRAMES * AUDIO_FRAME_SAMPLES * sizeof(int16_t))

static void audio_task_fn(void *arg)
{
    // Initialize I2S and LED
    ESP_ERROR_CHECK(audio_input_init());
    ESP_ERROR_CHECK(audio_output_init());
    led_init();
    led_set(LED_STATE_IDLE);

    int16_t *record_buf = heap_caps_malloc(PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    int16_t frame[AUDIO_FRAME_SAMPLES];

    if (!record_buf) {
        ESP_LOGE(TAG, "FATAL: cannot allocate PCM buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio_task ready — listening for wake");

    while (1) {
        // ── IDLE: poll for wake ──────────────────────────────────────────────
        int n = audio_input_read(frame, AUDIO_FRAME_SAMPLES);
        if (n <= 0) continue;

        int32_t energy = vad_frame_energy(frame, n);
        if (!vad_update_wake(energy)) continue;

        // ── RECORDING ────────────────────────────────────────────────────────
        ESP_LOGI(TAG, "Wake detected — recording");
        led_set(LED_STATE_LISTENING);
        int total  = 0;
        int silence = 0;

        while (total < MAX_RECORD_FRAMES * AUDIO_FRAME_SAMPLES) {
            n = audio_input_read(record_buf + total, AUDIO_FRAME_SAMPLES);
            if (n <= 0) continue;
            total += n;
            energy = vad_frame_energy(record_buf + total - n, n);
            if (vad_update_silence(energy, &silence)) {
                ESP_LOGI(TAG, "Silence — stop recording (%d samples)", total);
                break;
            }
        }

        // ── STT ──────────────────────────────────────────────────────────────
        led_set(LED_STATE_WAITING);
        char text[VOICE_STT_MAX_TEXT] = {0};
        esp_err_t err = voice_stt(AUDIO_GATEWAY_STT_URL,
                                  (uint8_t *)record_buf,
                                  (size_t)(total * sizeof(int16_t)),
                                  text, sizeof(text));
        if (err != ESP_OK || text[0] == '\0') {
            ESP_LOGW(TAG, "STT failed or empty — back to idle");
            led_set(LED_STATE_IDLE);
            continue;
        }
        ESP_LOGI(TAG, "STT: %s", text);

        // ── AGENT DISPATCH: push to inbound queue, consume sentence queue ────
        mimi_msg_t inbound = {0};
        strlcpy(inbound.channel, MIMI_CHAN_VOICE, sizeof(inbound.channel));
        strlcpy(inbound.chat_id, "voice_main", sizeof(inbound.chat_id));
        inbound.content = strdup(text);   /* heap copy — agent_task frees after processing */
        if (inbound.content) {
            if (message_bus_push_inbound(&inbound) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound queue full, drop voice message");
                free(inbound.content);
            } else {
                /* Note: message_bus owns inbound.content after push; do not free here */

                /* Consume sentences from g_sentence_queue until sentinel */
                char *sentence;
                while (xQueueReceive(g_sentence_queue, &sentence,
                                     pdMS_TO_TICKS(30000)) == pdTRUE) {
                    if (sentence == NULL) break;   /* sentinel = end of response */
                    voice_tts(AUDIO_GATEWAY_TTS_URL, sentence);
                    free(sentence);
                }
                /* Drain any stale items left in queue (e.g., if sentinel send failed) */
                char *stale;
                while (xQueueReceive(g_sentence_queue, &stale, 0) == pdTRUE) {
                    if (stale) free(stale);
                }
            }
        }

        led_set(LED_STATE_IDLE);
        ESP_LOGI(TAG, "Done — back to idle");
    }
}

void audio_task_start(void)
{
    // Create sentence queue before launching the task so the handle is
    // valid before the agent task can try to push to it.
    g_sentence_queue = xQueueCreate(16, sizeof(char *));

    xTaskCreatePinnedToCore(
        audio_task_fn,
        "audio_task",
        16384,         // 16KB stack — voice_stt/tts call esp_http_client_perform (~6KB stack)
        NULL,
        10,            // priority 10 (higher than telegram/ws tasks at 5)
        NULL,
        0              // Core 0
    );
}
