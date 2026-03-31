#include "voice_tts.h"
#include "audio/audio_output.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "voice_tts";

// PCM response can be up to ~500KB for a long sentence.
// Use PSRAM for the receive buffer (8MB available on N16R8).
#define TTS_PCM_BUF_MAX (512 * 1024)

typedef struct { uint8_t *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (rb->len + evt->data_len <= rb->cap) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "PCM buffer full — truncating");
        }
    }
    return ESP_OK;
}

esp_err_t voice_tts(const char *gateway_tts_url, const char *text)
{
    if (!text || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    // Build JSON body using cJSON to safely escape quotes/backslashes in text
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "text", text);
    char *body_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body_str) return ESP_ERR_NO_MEM;
    int body_len = strlen(body_str);

    // Allocate PCM receive buffer from PSRAM
    resp_buf_t rb = {
        .buf = heap_caps_malloc(TTS_PCM_BUF_MAX, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = TTS_PCM_BUF_MAX,
    };
    if (!rb.buf) { free(body_str); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url           = gateway_tts_url,
        .event_handler = http_evt,
        .user_data     = &rb,
        .timeout_ms    = 20000,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        free(body_str);
        heap_caps_free(rb.buf);
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body_str);  // cJSON_PrintUnformatted allocates with malloc

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "TTS HTTP error: err=%d status=%d", err, status);
        heap_caps_free(rb.buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS: received %d PCM bytes, playing...", (int)rb.len);
    err = audio_output_write_pcm(rb.buf, rb.len);
    heap_caps_free(rb.buf);
    return err;
}
