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
