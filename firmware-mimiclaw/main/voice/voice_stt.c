#include "voice_stt.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "voice_stt";

// HTTP event handler collects response body
typedef struct { char *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb->len + evt->data_len < rb->cap) {
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
    }
    return ESP_OK;
}

esp_err_t voice_stt(const char *gateway_stt_url,
                    const uint8_t *pcm, size_t pcm_len,
                    char *text_out, size_t text_max)
{
    text_out[0] = '\0';

    // Build multipart/form-data body
    const char *boundary = "kidpalai_stt";
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "--%s\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", boundary);
    char ftr[64];
    snprintf(ftr, sizeof(ftr), "\r\n--%s--\r\n", boundary);

    size_t hlen = strlen(hdr), flen = strlen(ftr);
    size_t body_len = hlen + pcm_len + flen;
    uint8_t *body = malloc(body_len);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body,              hdr, hlen);
    memcpy(body + hlen,       pcm, pcm_len);
    memcpy(body + hlen + pcm_len, ftr, flen);

    char ct[80];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);

    // Allocate response buffer: VOICE_STT_MAX_TEXT + JSON wrapper overhead (~128 bytes)
    resp_buf_t rb = { .buf = malloc(VOICE_STT_MAX_TEXT + 128), .len = 0, .cap = VOICE_STT_MAX_TEXT + 128 };
    if (!rb.buf) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url           = gateway_stt_url,
        .event_handler = http_event_handler,
        .user_data     = &rb,
        .timeout_ms    = 15000,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        free(body);
        free(rb.buf);
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", ct);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "STT HTTP error: err=%d status=%d", err, status);
        free(rb.buf);
        return ESP_FAIL;
    }

    rb.buf[rb.len] = '\0';
    cJSON *json = cJSON_Parse(rb.buf);
    free(rb.buf);
    if (!json) { ESP_LOGE(TAG, "STT JSON parse error"); return ESP_FAIL; }

    cJSON *text_item = cJSON_GetObjectItemCaseSensitive(json, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring) {
        strlcpy(text_out, text_item->valuestring, text_max);
        ESP_LOGI(TAG, "STT result: %s", text_out);
    }
    cJSON_Delete(json);
    return ESP_OK;
}
