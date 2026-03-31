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
