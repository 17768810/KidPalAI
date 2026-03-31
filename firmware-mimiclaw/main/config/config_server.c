#include "config_server.h"
#include "config_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "config_server";

// ── HTML ─────────────────────────────────────────────────────────────────────
// Chinese text is embedded as UTF-8 escape sequences for portability.

static const char *HTML_FORM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>KidPalAI \xe8\xae\xbe\xe5\xa4\x87\xe9\x85\x8d\xe7\xbd\xae</title>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px}"
    "label{display:block;margin-top:16px;font-weight:bold}"
    "input{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;font-size:16px}"
    "button{margin-top:24px;width:100%;padding:12px;font-size:18px;"
    "background:#4a90e2;color:#fff;border:none;border-radius:4px;cursor:pointer}"
    "</style></head><body>"
    "<h2>KidPalAI \xe8\xae\xbe\xe5\xa4\x87\xe9\x85\x8d\xe7\xbd\xae</h2>"
    "<form action='/save' method='post'>"
    "<label>WiFi \xe5\x90\x8d\xe7\xa7\xb0</label>"
    "<input type='text' name='ssid' required>"
    "<label>WiFi \xe5\xaf\x86\xe7\xa0\x81</label>"
    "<input type='password' name='pass'>"
    "<label>\xe7\xbd\x91\xe5\x85\xb3\xe5\x9c\xb0\xe5\x9d\x80</label>"
    "<input type='text' name='url' placeholder='http://host:8000' required>"
    "<label>MiniMax API Key (\xe5\x8f\xaf\xe7\x95\x99\xe7\xa9\xba)</label>"
    "<input type='password' name='llm_key' placeholder='sk-...'>"
    "<button type='submit'>\xe4\xbf\x9d\xe5\xad\x98\xe5\xb9\xb6\xe9\x87\x8d\xe5\x90\xaf</button>"
    "</form></body></html>";

static const char *HTML_OK =
    "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
    "<h2>\xe9\x85\x8d\xe7\xbd\xae\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xef\xbc\x8c"
    "\xe8\xae\xbe\xe5\xa4\x87\xe5\x8d\xb3\xe5\xb0\x86\xe9\x87\x8d\xe5\x90\xaf\xe2\x80\xa6</h2>"
    "</body></html>";

static const char *HTML_ERR =
    "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
    "<h2>\xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x9aSSID \xe5\x92\x8c URL "
    "\xe4\xb8\x8d\xe8\x83\xbd\xe4\xb8\xba\xe7\xa9\xba</h2>"
    "<a href='/'>\xe8\xbf\x94\xe5\x9b\x9e</a></body></html>";

// ── Percent-decode ────────────────────────────────────────────────────────────

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode url-encoded src into dst (dst_size includes null terminator).
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    while (*src && out < dst_size - 1) {
        if (*src == '%' && hex_val(src[1]) >= 0 && hex_val(src[2]) >= 0) {
            dst[out++] = (char)((hex_val(src[1]) << 4) | hex_val(src[2]));
            src += 3;
        } else if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

// ── POST body field extractor ─────────────────────────────────────────────────

// Find key= in url-encoded body; percent-decode value into out (out_size incl. null).
static void get_field(const char *body, size_t body_len,
                      const char *key, char *out, size_t out_size)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    size_t klen = strlen(search);

    const char *p = body;
    while (p < body + body_len) {
        if (strncmp(p, search, klen) == 0) {
            p += klen;
            const char *end = memchr(p, '&', (body + body_len) - p);
            size_t vlen = end ? (size_t)(end - p) : (size_t)((body + body_len) - p);
            // allocate worst-case encoded buffer (3× decoded size + null)
            size_t enc_max = out_size * 3 + 1;
            char *enc = malloc(enc_max);
            if (!enc) { out[0] = '\0'; return; }
            size_t copy = vlen < enc_max - 1 ? vlen : enc_max - 1;
            memcpy(enc, p, copy);
            enc[copy] = '\0';
            url_decode(enc, out, out_size);
            free(enc);
            return;
        }
        const char *next = memchr(p, '&', (body + body_len) - p);
        if (!next) break;
        p = next + 1;
    }
    out[0] = '\0';
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────

static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_FORM, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1536) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) { free(body); return ESP_FAIL; }
    body[received] = '\0';

    kidpal_config_t cfg = {0};
    get_field(body, received, "ssid",    cfg.wifi_ssid,   sizeof(cfg.wifi_ssid));
    get_field(body, received, "pass",    cfg.wifi_pass,   sizeof(cfg.wifi_pass));
    get_field(body, received, "url",     cfg.gateway_url, sizeof(cfg.gateway_url));
    get_field(body, received, "llm_key", cfg.llm_key,     sizeof(cfg.llm_key));
    free(body);

    if (cfg.wifi_ssid[0] == '\0' || cfg.gateway_url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, HTML_ERR, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Set LLM defaults if not provided
    if (cfg.llm_model[0] == '\0')
        strlcpy(cfg.llm_model, "MiniMax-M2.5-highspeed", sizeof(cfg.llm_model));

    if (config_save(&cfg) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "<h1>Save failed</h1>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "config saved, restarting...");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_OK, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;  // unreachable — suppresses -Wreturn-type
}

// ── SoftAP setup ──────────────────────────────────────────────────────────────

static esp_err_t start_softap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "KidPalAI-%02X%02X", mac[4], mac[5]);

    static esp_netif_t *ap_netif = NULL;
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
            .channel        = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Soft AP started: %s (open)", ssid);
    return ESP_OK;
}

// ── Public API ────────────────────────────────────────────────────────────────

__attribute__((noreturn)) void config_server_start(void)
{
    ESP_ERROR_CHECK(start_softap());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t get_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_root_handler,
    };
    httpd_uri_t post_save = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = post_save_handler,
    };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);

    ESP_LOGI(TAG, "config portal ready at http://192.168.4.1");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        // POST /save calls esp_restart() — this loop never exits.
    }
}
