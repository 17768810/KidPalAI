#include "voice_upload.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "voice_upload";

#define RESPONSE_BUF_MAX (256 * 1024)  // 256 KB max MP3 response

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   capacity;
} response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buf_t *rb = (response_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (rb->len + evt->data_len < rb->capacity) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "response buffer full, truncating");
        }
    }
    return ESP_OK;
}

esp_err_t voice_upload(const char *url,
                       const uint8_t *pcm_data, size_t pcm_len,
                       uint8_t **mp3_out, size_t *mp3_len)
{
    response_buf_t rb = {
        .buf      = malloc(RESPONSE_BUF_MAX),
        .len      = 0,
        .capacity = RESPONSE_BUF_MAX,
    };
    if (!rb.buf) return ESP_ERR_NO_MEM;

    // Build multipart/form-data body
    const char *boundary = "kidpalai_boundary_esp32";
    char header[256];
    snprintf(header, sizeof(header),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n",
             boundary);
    char footer[64];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t hlen     = strlen(header);
    size_t flen     = strlen(footer);
    size_t body_len = hlen + pcm_len + flen;

    uint8_t *body = malloc(body_len);
    if (!body) { free(rb.buf); return ESP_ERR_NO_MEM; }

    memcpy(body,                  header,   hlen);
    memcpy(body + hlen,           pcm_data, pcm_len);
    memcpy(body + hlen + pcm_len, footer,   flen);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t config = {
        .url            = url,
        .event_handler  = http_event_handler,
        .user_data      = &rb,
        .timeout_ms     = 20000,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP error: err=%d status=%d", err, status);
        free(rb.buf);
        return ESP_FAIL;
    }

    *mp3_out = rb.buf;
    *mp3_len = rb.len;
    ESP_LOGI(TAG, "received %d bytes of MP3", (int)rb.len);
    return ESP_OK;
}

esp_err_t voice_upload_stream(const char *base_url,
                               const uint8_t *pcm_data, size_t pcm_len,
                               voice_pcm_callback_t on_chunk,
                               int watchdog_ms)
{
    // Use base_url directly as the full endpoint URL
    const char *url = base_url;

    // Build multipart/form-data body (same layout as voice_upload)
    const char *boundary = "kidpalai_boundary_esp32";
    char header[256];
    snprintf(header, sizeof(header),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n",
             boundary);
    char footer[64];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t hlen     = strlen(header);
    size_t flen     = strlen(footer);
    size_t body_len = hlen + pcm_len + flen;

    uint8_t *body = malloc(body_len);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body,                  header,   hlen);
    memcpy(body + hlen,           pcm_data, pcm_len);
    memcpy(body + hlen + pcm_len, footer,   flen);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    // Open connection (timeout_ms=0: disable read timeout for streaming)
    esp_http_client_config_t config = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 0,
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_open failed: %d", err);
        free(body);
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, (const char *)body, (int)body_len);
    free(body);
    if (written < 0) {
        ESP_LOGE(TAG, "http_write failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Read response status
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "stream endpoint returned HTTP %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Stream response: read PCM chunks and pass to callback
#define STREAM_CHUNK 640  // 20ms @ 16kHz 16-bit mono
    uint8_t chunk[STREAM_CHUNK];
    int64_t last_data_us = esp_timer_get_time();
    int total_bytes = 0;
    int bytes_read;
    err = ESP_OK;

    while (1) {
        bytes_read = esp_http_client_read(client, (char *)chunk, STREAM_CHUNK);

        if (bytes_read > 0) {
            last_data_us = esp_timer_get_time();
            total_bytes += bytes_read;
            on_chunk(chunk, bytes_read);
        } else if (bytes_read == 0) {
            // Connection closed cleanly — stream ended
            break;
        } else {
            // bytes_read < 0 means error
            ESP_LOGE(TAG, "read error: %d", bytes_read);
            err = ESP_FAIL;
            break;
        }

        // Watchdog: abort if no new data for watchdog_ms
        if (watchdog_ms > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - last_data_us) / 1000;
            if (elapsed_ms > watchdog_ms) {
                ESP_LOGW(TAG, "stream watchdog triggered after %d ms", watchdog_ms);
                err = ESP_ERR_TIMEOUT;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "stream complete: %d PCM bytes received", total_bytes);
    esp_http_client_cleanup(client);
    return err;
}
