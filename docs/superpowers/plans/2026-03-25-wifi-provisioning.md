# WiFi Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace compile-time WiFi/gateway config with runtime NVS-backed config, settable via a SoftAP HTTP portal without re-flashing.

**Architecture:** On boot, load config from NVS (fall back to Kconfig defaults). If WiFi STA connection fails, tear down the driver and enter CONFIG mode: broadcast `KidPalAI-Setup` AP, serve an HTML form at `http://192.168.4.1`, save submitted values to NVS, and restart. All config handling is in two new files (`config_store`, `config_server`); existing files get focused, backward-compatible changes only.

**Tech Stack:** ESP-IDF v5.x, C99, FreeRTOS, `nvs_flash`, `esp_wifi`, `esp_http_server`, `esp_timer`, GPIO driver. Build: `idf.py build`. Flash+monitor: `idf.py -p <PORT> flash monitor`.

---

## File Map

| File | Action | What changes |
|------|--------|-------------|
| `firmware/main/config_store.h` | **Create** | `kidpal_config_t` struct + `config_load/save/erase` declarations |
| `firmware/main/config_store.c` | **Create** | NVS read/write/erase implementation |
| `firmware/main/config_server.h` | **Create** | `config_server_start()` noreturn declaration |
| `firmware/main/config_server.c` | **Create** | SoftAP + httpd + HTML form + POST /save handler |
| `firmware/main/led.h` | **Modify** | Add `LED_STATE_CONFIG` to enum |
| `firmware/main/led.c` | **Modify** | Add `esp_timer` periodic blink for CONFIG state |
| `firmware/main/wifi.h` | **Modify** | Update `wifi_init_sta` sig; add `wifi_sta_teardown`, `wifi_init_ap` |
| `firmware/main/wifi.c` | **Modify** | Remove global inits; bounded retry; teardown fn; AP fn |
| `firmware/main/voice_upload.h` | **Modify** | Add `const char *url` first param |
| `firmware/main/voice_upload.c` | **Modify** | Use passed-in URL instead of `CONFIG_KIDPAL_GATEWAY_URL` macro |
| `firmware/main/main.c` | **Modify** | Global inits once; GPIO0 check; config load; updated call sites; teardown+CONFIG path |

---

## Task 1: Add `LED_STATE_CONFIG` with timer blink

**Files:**
- Modify: `firmware/main/led.h`
- Modify: `firmware/main/led.c`

> **Note:** ESP-IDF has no unit test framework for GPIO/timer in host builds — correctness is verified by build success + manual hardware observation. "Test" steps below mean: build compiles cleanly and the LED visually blinks purple on hardware.

- [ ] **Step 1: Update `led.h` — add CONFIG state to enum**

Replace the existing enum in `firmware/main/led.h`:

```c
#pragma once

typedef enum {
    LED_STATE_IDLE,       // green steady  — ready, waiting for wake word
    LED_STATE_LISTENING,  // blue blink    — recording voice
    LED_STATE_WAITING,    // yellow steady — uploading / waiting for response
    LED_STATE_PLAYING,    // green blink   — playing back response
    LED_STATE_ERROR,      // red blink     — wifi lost / error
    LED_STATE_CONFIG,     // purple blink  — waiting for provisioning
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);
```

- [ ] **Step 2: Rewrite `led.c` with timer infrastructure**

Replace `firmware/main/led.c` entirely:

```c
#include "led.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define LED_R_GPIO  GPIO_NUM_1
#define LED_G_GPIO  GPIO_NUM_2
#define LED_B_GPIO  GPIO_NUM_3

static esp_timer_handle_t s_blink_timer;
static bool s_blink_r = false;
static bool s_blink_b = false;

static void blink_cb(void *arg)
{
    static bool on = false;
    on = !on;
    gpio_set_level(LED_R_GPIO, on && s_blink_r ? 1 : 0);
    gpio_set_level(LED_B_GPIO, on && s_blink_b ? 1 : 0);
}

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_R_GPIO) |
                        (1ULL << LED_G_GPIO) |
                        (1ULL << LED_B_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    esp_timer_create_args_t args = {
        .callback = blink_cb,
        .name     = "led_blink",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_blink_timer));

    led_set_state(LED_STATE_IDLE);
}

void led_set_state(led_state_t state)
{
    // Stop any active blink before changing state
    if (esp_timer_is_active(s_blink_timer)) {
        esp_timer_stop(s_blink_timer);
    }
    s_blink_r = false;
    s_blink_b = false;

    // Turn all LEDs off
    gpio_set_level(LED_R_GPIO, 0);
    gpio_set_level(LED_G_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    switch (state) {
        case LED_STATE_IDLE:
            gpio_set_level(LED_G_GPIO, 1);  // green steady
            break;
        case LED_STATE_LISTENING:
            gpio_set_level(LED_B_GPIO, 1);  // blue steady
            break;
        case LED_STATE_WAITING:
            gpio_set_level(LED_R_GPIO, 1);  // yellow = R+G
            gpio_set_level(LED_G_GPIO, 1);
            break;
        case LED_STATE_PLAYING:
            gpio_set_level(LED_G_GPIO, 1);  // green steady
            break;
        case LED_STATE_ERROR:
            gpio_set_level(LED_R_GPIO, 1);  // red steady
            break;
        case LED_STATE_CONFIG:
            // purple = R+B, 1 Hz blink (500 ms half-period)
            s_blink_r = true;
            s_blink_b = true;
            esp_timer_start_periodic(s_blink_timer, 500000);  // 500 ms in µs
            break;
    }
}
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd firmware
idf.py build
```

Expected: `Build successful` — no errors or warnings about `led.c`.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/led.h firmware/main/led.c
git commit -m "feat: add LED_STATE_CONFIG with purple 1Hz blink via esp_timer"
```

---

## Task 2: Create `config_store` — NVS config persistence

**Files:**
- Create: `firmware/main/config_store.h`
- Create: `firmware/main/config_store.c`

- [ ] **Step 1: Create `config_store.h`**

```c
#pragma once
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];     // max 32 chars + null
    char wifi_pass[65];     // max 64 chars + null (empty string = open network)
    char gateway_url[129];  // max 128 chars + null
} kidpal_config_t;

// Load config from NVS namespace "kidpal".
// Returns ESP_ERR_NVS_NOT_FOUND if no config has been saved yet.
esp_err_t config_load(kidpal_config_t *out);

// Save config to NVS namespace "kidpal".
esp_err_t config_save(const kidpal_config_t *cfg);

// Erase all config from NVS namespace "kidpal". Used for factory reset.
esp_err_t config_erase(void);
```

- [ ] **Step 2: Create `config_store.c`**

```c
#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define NVS_NAMESPACE  "kidpal"
#define KEY_SSID       "wifi_ssid"
#define KEY_PASS       "wifi_pass"
#define KEY_URL        "gateway_url"

static const char *TAG = "config_store";

esp_err_t config_load(kidpal_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len;
    bool ok = true;

    len = sizeof(out->wifi_ssid);
    ok &= (nvs_get_str(h, KEY_SSID, out->wifi_ssid, &len) == ESP_OK);

    len = sizeof(out->wifi_pass);
    ok &= (nvs_get_str(h, KEY_PASS, out->wifi_pass, &len) == ESP_OK);

    len = sizeof(out->gateway_url);
    ok &= (nvs_get_str(h, KEY_URL,  out->gateway_url, &len) == ESP_OK);

    nvs_close(h);

    if (!ok) {
        ESP_LOGW(TAG, "one or more keys missing — treating as not configured");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ESP_LOGI(TAG, "loaded: ssid=%s url=%s", out->wifi_ssid, out->gateway_url);
    return ESP_OK;
}

esp_err_t config_save(const kidpal_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_SSID, cfg->wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_PASS, cfg->wifi_pass));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_URL,  cfg->gateway_url));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "saved: ssid=%s url=%s", cfg->wifi_ssid, cfg->gateway_url);
    return ESP_OK;
}

esp_err_t config_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config erased");
    return err;
}
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd firmware
idf.py build
```

Expected: `Build successful`.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/config_store.h firmware/main/config_store.c
git commit -m "feat: add config_store for NVS-backed runtime config (wifi + gateway url)"
```

---

## Task 3: Update `wifi.c/h` — remove global inits, bounded retry, teardown, AP mode

**Files:**
- Modify: `firmware/main/wifi.h`
- Modify: `firmware/main/wifi.c`

- [ ] **Step 1: Replace `wifi.h`**

```c
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Connect to a WiFi network in STA mode.
// Preconditions: esp_netif_init() and esp_event_loop_create_default() already called.
// On return (ESP_OK or ESP_FAIL): *netif_out and *event_group_out are valid.
// Caller must pass them to wifi_sta_teardown() before calling wifi_init_ap().
esp_err_t wifi_init_sta(const char *ssid, const char *password,
                        esp_netif_t **netif_out,
                        EventGroupHandle_t *event_group_out);

// Tear down STA mode completely. Safe to call after both success and failure.
// Unregisters event handlers, disconnects, stops, deinits WiFi driver,
// destroys netif, deletes event group.
void wifi_sta_teardown(esp_netif_t *netif, EventGroupHandle_t event_group);

// Start SoftAP "KidPalAI-Setup" (open, ch1, max 1 client).
// Preconditions: esp_netif_init() and esp_event_loop_create_default() already called.
//                WiFi driver is NOT started (call after wifi_sta_teardown or on first boot in AP path).
// Calls esp_wifi_init() and esp_netif_create_default_wifi_ap() internally.
// IP: 192.168.4.1 (ESP-IDF default).
esp_err_t wifi_init_ap(void);

bool wifi_is_connected(void);
```

- [ ] **Step 2: Replace `wifi.c`**

```c
#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           3

static bool                         s_connected = false;
static int                          s_retry_count = 0;
static EventGroupHandle_t           s_wifi_event_group;
static esp_event_handler_instance_t s_inst_wifi;
static esp_event_handler_instance_t s_inst_ip;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "retrying wifi (%d/%d)...", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "max retries reached, giving up");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP, wifi connected");
    }
}

esp_err_t wifi_init_sta(const char *ssid, const char *password,
                        esp_netif_t **netif_out,
                        EventGroupHandle_t *event_group_out)
{
    s_retry_count = 0;
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler, NULL, &s_inst_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler, NULL, &s_inst_ip);

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid,     ssid,     sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable  = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));

    *netif_out        = netif;
    *event_group_out  = s_wifi_event_group;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to SSID: %s", ssid);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to connect to SSID: %s", ssid);
    return ESP_FAIL;
}

void wifi_sta_teardown(esp_netif_t *netif, EventGroupHandle_t event_group)
{
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_inst_wifi);
    esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, s_inst_ip);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy(netif);
    vEventGroupDelete(event_group);
    s_connected = false;
    ESP_LOGI(TAG, "STA teardown complete");
}

esp_err_t wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();  // handle intentionally not stored (noreturn caller)

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, "KidPalAI-Setup", sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len       = strlen("KidPalAI-Setup");
    ap_config.ap.channel        = 1;
    ap_config.ap.authmode       = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP started: KidPalAI-Setup  ip=192.168.4.1");
    return ESP_OK;
}

bool wifi_is_connected(void) { return s_connected; }
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd firmware
idf.py build
```

Expected: `wifi.c` compiles cleanly. **`main.c` will fail** (old `wifi_init_sta` call signature) — this is expected and resolved in Task 6.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/wifi.h firmware/main/wifi.c
git commit -m "feat: update wifi module — bounded retry, wifi_sta_teardown, wifi_init_ap, no global inits"
```

---

## Task 4: Update `voice_upload` — accept URL as parameter

**Files:**
- Modify: `firmware/main/voice_upload.h`
- Modify: `firmware/main/voice_upload.c`

- [ ] **Step 1: Update `voice_upload.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Upload PCM audio to the given gateway URL.
// On success, *mp3_out is heap-allocated and must be freed by caller.
esp_err_t voice_upload(const char *url,
                       const uint8_t *pcm_data, size_t pcm_len,
                       uint8_t **mp3_out, size_t *mp3_len);
```

- [ ] **Step 2: Update `voice_upload.c` — remove macro, accept url param**

In `firmware/main/voice_upload.c`:

1. Remove the `#ifndef CONFIG_KIDPAL_GATEWAY_URL` block (lines 11-13).
2. Change the function signature to match the header above.
3. Replace `.url = CONFIG_KIDPAL_GATEWAY_URL` in the `esp_http_client_config_t` with `.url = url`.

Full updated file:

```c
#include "voice_upload.h"
#include "esp_http_client.h"
#include "esp_log.h"
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
    ESP_LOGI(TAG, "received %d bytes of MP3", rb.len);
    return ESP_OK;
}
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd firmware
idf.py build
```

Expected: `Build successful` (main.c call site will need updating in Task 6, but the module itself compiles).

- [ ] **Step 4: Commit**

```bash
git add firmware/main/voice_upload.h firmware/main/voice_upload.c
git commit -m "feat: voice_upload accepts runtime url param, removes CONFIG_KIDPAL_GATEWAY_URL macro"
```

---

## Task 5: Create `config_server` — SoftAP HTTP portal

**Files:**
- Create: `firmware/main/config_server.h`
- Create: `firmware/main/config_server.c`

- [ ] **Step 1: Create `config_server.h`**

```c
#pragma once

// Start the WiFi provisioning AP and HTTP config server.
// Preconditions:
//   - esp_netif_init() and esp_event_loop_create_default() already called in main.c.
//   - WiFi driver is stopped (wifi_sta_teardown called, or first boot in AP path).
// This function never returns. It starts SoftAP "KidPalAI-Setup", serves a config
// form at http://192.168.4.1, and calls esp_restart() after a successful POST /save.
__attribute__((noreturn)) void config_server_start(void);
```

- [ ] **Step 2: Create `config_server.c`**

```c
#include "config_server.h"
#include "config_store.h"
#include "wifi.h"
#include "led.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "config_server";

// ── HTML ────────────────────────────────────────────────────────────────────

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
    "<input type='text' name='url' value='" CONFIG_KIDPAL_GATEWAY_URL "' required>"
    "<button type='submit'>\xe4\xbf\x9d\xe5\xad\x98\xe5\xb9\xb6\xe9\x87\x8d\xe5\x90\xaf</button>"
    "</form></body></html>";

static const char *HTML_OK =
    "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
    "<h2>\xe9\x85\x8d\xe7\xbd\xae\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xef\xbc\x8c"
    "\xe8\xae\xbe\xe5\xa4\x87\xe5\x8d\xb3\xe5\xb0\x86\xe9\x87\x8d\xe5\x90\xaf\xe2\x80\xa6</h2>"
    "</body></html>";

static const char *HTML_ERR =
    "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
    "<h2>\xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x9aSSID \xe5\x92\x8c URL \xe4\xb8\x8d\xe8\x83\xbd\xe4\xb8\xba\xe7\xa9\xba</h2>"
    "<a href='/'>\xe8\xbf\x94\xe5\x9b\x9e</a></body></html>";

// ── Percent-decode ───────────────────────────────────────────────────────────

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode src (url-encoded) into dst (size dst_size, including null).
// Returns number of decoded bytes written (not counting null).
static size_t url_decode(const char *src, char *dst, size_t dst_size)
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
    return out;
}

// ── POST body parser ─────────────────────────────────────────────────────────

// Extract value for `key=` from a url-encoded body into `out` (size out_size).
static bool get_field(const char *body, size_t body_len,
                      const char *key, char *out, size_t out_size)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    size_t klen = strlen(search);

    const char *p = body;
    while (p < body + body_len) {
        if (strncmp(p, search, klen) == 0) {
            p += klen;
            // find end of value (& or end of body)
            const char *end = memchr(p, '&', (body + body_len) - p);
            size_t vlen = end ? (size_t)(end - p) : (size_t)((body + body_len) - p);
            // copy to temp encoded buffer (max field parse buffer = out_size * 3 + 1)
            size_t enc_max = out_size * 3 + 1;
            char *enc = malloc(enc_max);
            if (!enc) return false;
            size_t copy = vlen < enc_max - 1 ? vlen : enc_max - 1;
            memcpy(enc, p, copy);
            enc[copy] = '\0';
            url_decode(enc, out, out_size);
            free(enc);
            return true;
        }
        // skip to next field
        const char *next = memchr(p, '&', (body + body_len) - p);
        if (!next) break;
        p = next + 1;
    }
    out[0] = '\0';
    return false;
}

// ── HTTP handlers ────────────────────────────────────────────────────────────

static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_FORM, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    // Read POST body
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
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

    // Parse and decode fields
    kidpal_config_t cfg = {0};
    get_field(body, received, "ssid", cfg.wifi_ssid,   sizeof(cfg.wifi_ssid));
    get_field(body, received, "pass", cfg.wifi_pass,   sizeof(cfg.wifi_pass));
    get_field(body, received, "url",  cfg.gateway_url, sizeof(cfg.gateway_url));
    free(body);

    // Validate
    if (cfg.wifi_ssid[0] == '\0' || cfg.gateway_url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, HTML_ERR, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    // Truncate URL silently if somehow over limit (strlcpy already did this)

    // Save and restart
    config_save(&cfg);
    ESP_LOGI(TAG, "config saved, restarting...");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_OK, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;  // unreachable — suppresses -Wreturn-type
}

// ── Public API ────────────────────────────────────────────────────────────────

__attribute__((noreturn)) void config_server_start(void)
{
    ESP_ERROR_CHECK(wifi_init_ap());

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

    led_set_state(LED_STATE_CONFIG);
    ESP_LOGI(TAG, "config portal ready at http://192.168.4.1");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        // POST /save calls esp_restart() — this loop never exits.
    }
}
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd firmware
idf.py build
```

Expected: `Build successful`.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/config_server.h firmware/main/config_server.c
git commit -m "feat: add config_server — SoftAP HTTP portal for WiFi/gateway provisioning"
```

---

## Task 6: Update `main.c` — wire everything together

**Files:**
- Modify: `firmware/main/main.c`

- [ ] **Step 1: Replace `main.c`**

Replace the full contents of `firmware/main/main.c`:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"

#include "wifi.h"
#include "led.h"
#include "audio_input.h"
#include "audio_output.h"
#include "voice_upload.h"
#include "config_store.h"
#include "config_server.h"

// ESP-SR headers — uncomment after installing esp-sr component:
// #include "esp_wn_iface.h"
// #include "esp_wn_models.h"
// #include "esp_afe_sr_iface.h"
// #include "esp_afe_sr_models.h"

static const char *TAG = "main";

// VAD: stop recording after 1.5s of silence (75 frames × 20ms)
#define VAD_SILENCE_FRAMES  75
// Max recording: 10 seconds (500 frames × 20ms)
#define MAX_RECORD_FRAMES   500
// 20ms of audio at 16kHz mono
#define FRAME_SAMPLES       320
// Silence energy threshold (tune based on environment)
#define VAD_ENERGY_THRESHOLD 200

// Hold GPIO0 LOW for this many ms at boot to force CONFIG mode
#define FACTORY_RESET_HOLD_MS 3000

static bool gpio0_held_long(void)
{
    gpio_config_t io = {
        .pin_bit_mask = BIT64(GPIO_NUM_0),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (gpio_get_level(GPIO_NUM_0) != 0) return false;

    int held_ms = 0;
    while (gpio_get_level(GPIO_NUM_0) == 0 && held_ms < FACTORY_RESET_HOLD_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held_ms += 100;
    }
    return held_ms >= FACTORY_RESET_HOLD_MS;
}

void app_main(void)
{
    // ── Init NVS ────────────────────────────────────────────────────────────
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // ── Init LED (red = error until wifi connects) ───────────────────────────
    led_init();
    led_set_state(LED_STATE_ERROR);

    // ── Global network init (done ONCE here) ─────────────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ── Forced CONFIG mode: hold BOOT (GPIO0) for 3s at power-on ────────────
    if (gpio0_held_long()) {
        ESP_LOGW(TAG, "GPIO0 held — erasing config and entering CONFIG mode");
        config_erase();
        config_server_start();  // noreturn
    }

    // ── Load config (fall back to Kconfig defaults) ──────────────────────────
    kidpal_config_t cfg = {0};
    if (config_load(&cfg) != ESP_OK) {
        ESP_LOGI(TAG, "no NVS config — using Kconfig defaults");
        strlcpy(cfg.wifi_ssid,   CONFIG_KIDPAL_WIFI_SSID,   sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass,   CONFIG_KIDPAL_WIFI_PASS,   sizeof(cfg.wifi_pass));
        strlcpy(cfg.gateway_url, CONFIG_KIDPAL_GATEWAY_URL, sizeof(cfg.gateway_url));
    }

    // ── Connect WiFi ─────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Connecting to WiFi: %s", cfg.wifi_ssid);
    esp_netif_t       *sta_netif    = NULL;
    EventGroupHandle_t event_group  = NULL;

    if (wifi_init_sta(cfg.wifi_ssid, cfg.wifi_pass,
                      &sta_netif, &event_group) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — entering provisioning mode");
        wifi_sta_teardown(sta_netif, event_group);
        config_server_start();  // noreturn
    }

    led_set_state(LED_STATE_IDLE);
    ESP_LOGI(TAG, "WiFi connected. Gateway: %s", cfg.gateway_url);

    // ── Init audio I/O ───────────────────────────────────────────────────────
    audio_input_init();
    audio_output_init();

    // ── Init ESP-SR (wake word + VAD) ─────────────────────────────────────────
    // TODO: Uncomment after installing esp-sr component
    // esp_afe_sr_iface_t *afe_handle = &ESP_AFE_SR_HANDLE;
    // afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    // esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);
    // esp_wn_iface_t *wakenet = &WAKENET_MODEL;
    // model_iface_data_t *wn_model = wakenet->create(NULL, DET_MODE_90);

    ESP_LOGI(TAG, "Ready. Listening for wake word...");

    // PCM recording buffer: 10 seconds max
    int16_t *record_buf = heap_caps_malloc(MAX_RECORD_FRAMES * FRAME_SAMPLES * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!record_buf) {
        ESP_LOGE(TAG, "Failed to allocate record buffer");
        return;
    }
    int16_t frame_buf[FRAME_SAMPLES];

    while (1) {
        // ── IDLE: poll for wake word ─────────────────────────────────────────
        int samples = audio_input_read(frame_buf, FRAME_SAMPLES);
        if (samples <= 0) continue;

        // TODO: Feed frame_buf to ESP-SR AFE and check WakeNet result.
        bool wake_triggered = false;

        // ── PLACEHOLDER: remove once ESP-SR is wired up ──────────────────────
        {
            int32_t energy = 0;
            for (int i = 0; i < samples; i++) energy += abs(frame_buf[i]);
            energy /= samples;
            static int loud_frames = 0;
            if (energy > VAD_ENERGY_THRESHOLD * 5) {
                loud_frames++;
            } else {
                loud_frames = 0;
            }
            if (loud_frames >= 25) {
                wake_triggered = true;
                loud_frames = 0;
            }
        }

        if (!wake_triggered) continue;

        // ── RECORDING ────────────────────────────────────────────────────────
        ESP_LOGI(TAG, "Wake detected! Recording...");
        led_set_state(LED_STATE_LISTENING);

        int total_samples  = 0;
        int silence_frames = 0;

        while (total_samples < MAX_RECORD_FRAMES * FRAME_SAMPLES) {
            int n = audio_input_read(record_buf + total_samples, FRAME_SAMPLES);
            if (n <= 0) continue;
            total_samples += n;

            int32_t energy = 0;
            for (int i = total_samples - n; i < total_samples; i++)
                energy += abs(record_buf[i]);
            energy /= n;

            if (energy < VAD_ENERGY_THRESHOLD) silence_frames++;
            else silence_frames = 0;

            if (silence_frames >= VAD_SILENCE_FRAMES) {
                ESP_LOGI(TAG, "Silence detected, stopping recording");
                break;
            }
        }

        ESP_LOGI(TAG, "Recorded %d samples (%.1fs), uploading...",
                 total_samples, (float)total_samples / AUDIO_SAMPLE_RATE);

        // ── UPLOAD ───────────────────────────────────────────────────────────
        led_set_state(LED_STATE_WAITING);

        uint8_t *mp3_data = NULL;
        size_t   mp3_len  = 0;
        esp_err_t err = voice_upload(
            cfg.gateway_url,
            (uint8_t *)record_buf,
            (size_t)(total_samples * sizeof(int16_t)),
            &mp3_data, &mp3_len);

        if (err == ESP_OK && mp3_len > 0) {
            ESP_LOGI(TAG, "Playing response (%d bytes)...", mp3_len);
            led_set_state(LED_STATE_PLAYING);
            audio_output_play_mp3(mp3_data, mp3_len);
            free(mp3_data);
        } else {
            ESP_LOGE(TAG, "Upload failed or empty response");
            led_set_state(LED_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        led_set_state(LED_STATE_IDLE);
        ESP_LOGI(TAG, "Ready. Listening again...");
    }

    free(record_buf);
}
```

- [ ] **Step 2: Build — expect clean compile**

```bash
cd firmware
idf.py build
```

Expected: `Build successful` with no errors.

- [ ] **Step 3: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat: wire provisioning into main — config load, GPIO0 factory reset, STA teardown → config_server"
```

---

## Task 7: Flash and verify on hardware

> This task requires a connected ESP32-S3 board on a USB serial port (e.g. `/dev/ttyUSB0` or `COM3`).

- [ ] **Step 1: Flash and open serial monitor**

```bash
cd firmware
idf.py -p /dev/ttyUSB0 flash monitor
```

- [ ] **Step 2: Verify first-boot CONFIG mode (no NVS config)**

If Kconfig values are placeholders (`myssid` / `mypassword`), the device should:
1. Print `no NVS config — using Kconfig defaults`
2. Print `Connecting to WiFi: myssid`
3. After ~10s, print `WiFi failed — entering provisioning mode`
4. LED turns purple (blinking)
5. Print `AP started: KidPalAI-Setup`
6. Print `config portal ready at http://192.168.4.1`

- [ ] **Step 3: Connect phone to `KidPalAI-Setup` hotspot and configure**

1. On a phone, connect to WiFi network `KidPalAI-Setup` (no password)
2. Open browser and navigate to `http://192.168.4.1`
3. Fill in real WiFi SSID, password, and gateway URL
4. Tap "保存并重启"
5. Browser shows "配置已保存，设备即将重启…"
6. Device restarts (serial monitor reconnects)

- [ ] **Step 4: Verify device connects after restart**

After restart, serial monitor should show:
1. `loaded: ssid=<your-ssid> url=<your-url>`
2. `Connecting to WiFi: <your-ssid>`
3. `connected to SSID: <your-ssid>`
4. LED turns green (IDLE)
5. `WiFi connected. Gateway: <your-url>`

- [ ] **Step 5: Verify GPIO0 factory reset**

1. Hold the BOOT button on the board, then power on (or press RESET while holding BOOT)
2. Keep holding for 3 seconds
3. Serial monitor: `GPIO0 held — erasing config and entering CONFIG mode`
4. Device enters CONFIG mode again

- [ ] **Step 6: Final commit (if any last-minute tweaks made)**

```bash
cd firmware
git add -p   # stage only intentional changes
git commit -m "fix: hardware-verified tweaks from provisioning test"
```

---

## Summary of all commits

After completing all tasks, the git log should contain:

```
feat: wire provisioning into main — config load, GPIO0 factory reset, STA teardown → config_server
feat: add config_server — SoftAP HTTP portal for WiFi/gateway provisioning
feat: voice_upload accepts runtime url param, removes CONFIG_KIDPAL_GATEWAY_URL macro
feat: update wifi module — bounded retry, wifi_sta_teardown, wifi_init_ap, no global inits
feat: add config_store for NVS-backed runtime config (wifi + gateway url)
feat: add LED_STATE_CONFIG with purple 1Hz blink via esp_timer
```
