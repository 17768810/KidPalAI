#pragma once

// Start the WiFi provisioning AP and HTTP config server.
// Preconditions:
//   - esp_netif_init() and esp_event_loop_create_default() already called in main.c.
//   - WiFi driver is stopped (wifi_sta_teardown called, or first boot in AP path).
// This function never returns. It starts SoftAP "KidPalAI-Setup", serves a config
// form at http://192.168.4.1, and calls esp_restart() after a successful POST /save.
__attribute__((noreturn)) void config_server_start(void);
