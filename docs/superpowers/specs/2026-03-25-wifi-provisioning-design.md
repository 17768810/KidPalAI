# WiFi Provisioning via SoftAP + HTTP — Design Spec

**Date:** 2026-03-25
**Project:** KidPalAI firmware (ESP32-S3)
**Status:** Approved

---

## Problem

WiFi SSID, WiFi password, and Gateway URL are currently compile-time constants set via `idf.py menuconfig`. Every change requires a re-flash.

## Goal

Allow runtime configuration of the three device parameters without re-flashing, via a web-based setup page hosted on the ESP32 itself.

---

## Approach: SoftAP + HTTP Config Portal

On boot, the device checks NVS for saved config. If none exists (or WiFi connection fails), it switches to AP mode and runs a lightweight HTTP server. The user connects their phone to the ESP32's hotspot and fills in a form at `http://192.168.4.1`. On submit, config is saved to NVS and the device restarts.

---

## Boot Flow

```
Power on
  → NVS init (already in main.c — erase+reinit on corruption)
  → esp_netif_init()                   ← called ONCE in main.c
  → esp_event_loop_create_default()    ← called ONCE in main.c
  → config_load(&cfg)
      ├─ ESP_OK → wifi_init_sta(cfg.ssid, cfg.pass, &sta_netif)
      └─ ESP_ERR_NVS_NOT_FOUND → copy Kconfig defaults into cfg
                                  → wifi_init_sta(cfg.ssid, cfg.pass, &sta_netif)

  wifi_init_sta returns:
      ├─ ESP_OK (connected) → main audio loop, pass cfg.gateway_url to voice_upload()
      └─ ESP_FAIL (timeout) → teardown sequence → CONFIG mode

Teardown sequence (in main.c, in this exact order):
  1. wifi_sta_teardown(sta_netif, event_group)   // see wifi.h — encapsulates steps below
     internally: unregister both event handler instance handles (stored as file-static
                 in wifi.c during wifi_init_sta), then esp_wifi_disconnect,
                 esp_wifi_stop, esp_wifi_deinit, esp_netif_destroy(sta_netif),
                 vEventGroupDelete(event_group)

CONFIG mode (LED: purple slow-blink):
  → config_server_start()   // noreturn; re-inits WiFi driver internally as AP
```

### Forced CONFIG Mode (GPIO0 Long Press)

If GPIO0 (BOOT button) is held LOW for ≥ 3 seconds at boot (checked once after NVS init, before wifi_init_sta), the device calls `config_erase()` and enters CONFIG mode directly, regardless of NVS contents. This lets users recover from a wrong password or renamed network without re-flashing.

---

## New Files

| File | Responsibility |
|------|---------------|
| `firmware/main/config_store.c` | NVS read/write/erase |
| `firmware/main/config_store.h` | config_load, config_save, config_erase |
| `firmware/main/config_server.c` | SoftAP + HTTP server + HTML form |
| `firmware/main/config_server.h` | config_server_start (noreturn) |

---

## config_store

### NVS Namespace and Keys

| Constant | Value |
|----------|-------|
| Namespace | `"kidpal"` |
| SSID key | `"wifi_ssid"` |
| Password key | `"wifi_pass"` |
| URL key | `"gateway_url"` |

### API

```c
typedef struct {
    char wifi_ssid[33];     // max 32 chars + null
    char wifi_pass[65];     // max 64 chars + null (empty = open network)
    char gateway_url[129];  // max 128 chars + null
} kidpal_config_t;

// Load from NVS. Returns ESP_ERR_NVS_NOT_FOUND if no config stored.
esp_err_t config_load(kidpal_config_t *out);

// Save to NVS.
esp_err_t config_save(const kidpal_config_t *cfg);

// Erase all kidpal NVS entries. Used by forced CONFIG mode (GPIO0 long press).
esp_err_t config_erase(void);
```

### Compile-time Default Fallback in main.c

```c
kidpal_config_t cfg = {0};
if (config_load(&cfg) != ESP_OK) {
    strlcpy(cfg.wifi_ssid,   CONFIG_KIDPAL_WIFI_SSID,   sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass,   CONFIG_KIDPAL_WIFI_PASS,   sizeof(cfg.wifi_pass));
    strlcpy(cfg.gateway_url, CONFIG_KIDPAL_GATEWAY_URL, sizeof(cfg.gateway_url));
    // If Kconfig values are placeholders, wifi_init_sta will timeout → CONFIG mode.
}
```

`cfg` is a local variable in `app_main()`. Since `app_main()` never returns, its stack is permanent.

---

## WiFi Module Changes (wifi.c / wifi.h)

### Removed from wifi_init_sta()

- `esp_netif_init()` — moved to main.c, called once
- `esp_event_loop_create_default()` — moved to main.c, called once

Neither `wifi_init_sta()` nor `wifi_init_ap()` calls these. Callers must initialize them first.

### WIFI_FAIL_BIT and Retry Loop

The existing disconnect handler calls `esp_wifi_connect()` unconditionally on every disconnect. This retry loop must be **bounded**: after 3 retries, the handler sets `WIFI_FAIL_BIT` and stops reconnecting. This ensures `wifi_init_sta()` returns `ESP_FAIL` cleanly within the 10-second timeout rather than looping indefinitely.

```c
static int s_retry_count = 0;
#define MAX_RETRY 3

// In event_handler, WIFI_EVENT_STA_DISCONNECTED:
if (s_retry_count < MAX_RETRY) {
    esp_wifi_connect();
    s_retry_count++;
} else {
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
}
```

### Updated wifi.h Declarations

```c
// Preconditions: esp_netif_init() and esp_event_loop_create_default() already called.
// Event handler instance handles are stored as file-statics inside wifi.c.
// On return (success or fail): *netif_out and *event_group_out are valid.
// Caller must pass both to wifi_sta_teardown() before calling wifi_init_ap().
esp_err_t wifi_init_sta(const char *ssid, const char *password,
                        esp_netif_t **netif_out,
                        EventGroupHandle_t *event_group_out);

// Teardown helper: unregisters stored event handler instances, disconnects,
// stops, deinits WiFi driver, destroys netif, deletes event group.
// Must be called before wifi_init_ap() when falling through to CONFIG mode.
void wifi_sta_teardown(esp_netif_t *netif, EventGroupHandle_t event_group);

// Preconditions: esp_netif_init() and esp_event_loop_create_default() already called.
//                wifi_sta_teardown() already called (or this is first boot).
// Creates AP netif with esp_netif_create_default_wifi_ap() (handle not exposed;
//   not destroyed since config_server_start is noreturn).
// Calls esp_wifi_init() internally (driver was deinited by teardown).
// Starts SoftAP: SSID = "KidPalAI-Setup", open auth, channel 1, max_connection = 1.
// IP: 192.168.4.1 (ESP-IDF default for AP mode).
esp_err_t wifi_init_ap(void);
```

---

## Config Server (config_server.c)

### API

```c
// Starts SoftAP (via wifi_init_ap) and HTTP config server.
// Preconditions: esp_netif_init() and esp_event_loop_create_default() already called.
//                WiFi driver torn down (esp_wifi_deinit called).
// This function never returns. Loops with vTaskDelay until POST /save triggers esp_restart().
__attribute__((noreturn)) void config_server_start(void);
```

### Internal Flow

```c
void config_server_start(void) {
    wifi_init_ap();                  // calls esp_wifi_init() + start AP
    httpd_handle_t server = start_webserver();  // registers GET / and POST /save
    led_set_state(LED_STATE_CONFIG);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        // POST /save handler calls esp_restart() — this loop never exits.
    }
}
```

`wifi_init_ap()` calls `esp_wifi_init()` internally (the driver was torn down before `config_server_start` was called). No cleanup before `esp_restart()` is required — `esp_restart()` performs a hard chip reset.

### HTTP Server

- Listens on port **80**
- AP interface IP: **192.168.4.1**
- No captive portal DNS redirect in this implementation. User must manually navigate to `http://192.168.4.1` after connecting to the `KidPalAI-Setup` hotspot.

### HTTP Routes

| Route | Method | Description |
|-------|--------|-------------|
| `/` | GET | Returns HTML config form (200) |
| `/save` | POST | Parse, percent-decode, validate, save, return success HTML, then `esp_restart()` after 1s |

### Form Parsing and Percent-Decoding

POST body is `application/x-www-form-urlencoded`. Values must be **percent-decoded** before saving (browsers encode special chars like `&`, `=`, `#`, `+`, space).

Per-field parse buffers (to hold worst-case percent-encoded data before decoding):

| Field | Stored max | Parse buffer size | Worst-case encoded |
|-------|-----------|------------------|-------------------|
| `ssid` | 32 chars | 97 bytes | 32 × 3 + 1 |
| `pass` | 64 chars | 193 bytes | 64 × 3 + 1 |
| `url` | 128 chars | 385 bytes | 128 × 3 + 1 |

After decoding, truncate to stored field max length before writing to `kidpal_config_t`.

### Validation in POST /save

| Condition | Action |
|-----------|--------|
| `ssid` empty after decode | Return HTTP 400 with error message; do not save |
| `pass` empty after decode | Allowed (open WiFi network) |
| `url` empty after decode | Return HTTP 400 with error message; do not save |
| `url` > 128 chars after decode | Truncate to 128 chars, log warning, proceed |
| All valid | `config_save()`, return success HTML (200), call `esp_restart()` after 1 second |

### HTML Content

**GET / response:** Inline HTML string (no external assets), UTF-8, Content-Type `text/html`:
- Page title: `KidPalAI 设备配置`
- Form action `/save`, method POST, enctype `application/x-www-form-urlencoded`
- Field `ssid` (text, label: WiFi 名称)
- Field `pass` (password, label: WiFi 密码)
- Field `url` (text, label: 网关地址, value pre-filled with `CONFIG_KIDPAL_GATEWAY_URL`)
- Submit button: `保存并重启`

**POST /save success response:** HTTP 200, inline HTML:
- Body text: `配置已保存，设备即将重启…`

---

## LED Changes (led.c / led.h)

### Updated Enum

```c
typedef enum {
    LED_STATE_IDLE,
    LED_STATE_LISTENING,
    LED_STATE_WAITING,
    LED_STATE_PLAYING,
    LED_STATE_ERROR,
    LED_STATE_CONFIG,   // NEW: purple (R+B), 1 Hz blink
} led_state_t;
```

### Blinking Mechanism

A periodic `esp_timer` is added to `led.c`:

- `esp_timer_handle_t s_blink_timer` — file-static, created once in `led_init()` with `esp_timer_create()`.
- `led_set_state(LED_STATE_CONFIG)`: calls `esp_timer_start_periodic(s_blink_timer, 500000)` (500 ms). The callback toggles R+B GPIO pins each fire → 1 Hz visual blink.
- Any other `led_set_state()` call: calls `esp_timer_stop(s_blink_timer)` **only if the timer is currently running** (guard with `esp_timer_is_active(s_blink_timer)`), then sets static GPIO levels.

The `esp_timer_is_active()` guard prevents errors when `led_set_state()` is called with a non-CONFIG state before CONFIG mode was ever entered (timer not yet started).

No changes to `led_set_state()` callers outside `led.c`.

---

## Changes to Existing Files

| File | Change |
|------|--------|
| `main/main.c` | (1) Move `esp_netif_init()` + `esp_event_loop_create_default()` here, called once. (2) Check GPIO0 for forced CONFIG mode. (3) Call `config_load()` with Kconfig fallback. (4) Update `wifi_init_sta()` call to match new signature. (5) Add teardown sequence on STA fail. (6) Call `config_server_start()` on CONFIG mode. (7) Pass `cfg.gateway_url` to `voice_upload()`. |
| `main/wifi.c` / `wifi.h` | Remove global init calls from `wifi_init_sta()`. Update signature (add `netif_out`, `event_group_out`). Store event handler instance handles as file-statics. Add bounded retry (max 3). Add `WIFI_FAIL_BIT` set. Add `wifi_sta_teardown()`. Add `wifi_init_ap()` (creates AP netif internally, calls `esp_wifi_init()`). |
| `main/voice_upload.c` / `voice_upload.h` | New signature: `esp_err_t voice_upload(const char *url, const uint8_t *pcm_data, size_t pcm_len, uint8_t **mp3_out, size_t *mp3_len)`. Remove `CONFIG_KIDPAL_GATEWAY_URL` macro reference. |
| `main/led.c` / `led.h` | Add `LED_STATE_CONFIG` to enum. Add `esp_timer` blink infrastructure. |
| `main/Kconfig.projbuild` | No structural change; values are optional compile-time fallback. |

---

## Out of Scope

- HTTPS for the config portal (local AP, low risk)
- Scanning available WiFi networks (user types SSID manually)
- OTA firmware update via config portal
- Captive portal DNS redirect (auto-open on phone connect)
