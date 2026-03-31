#pragma once
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char gateway_url[128];   // e.g., "http://8.133.3.7:8000"
    char llm_key[256];
    char llm_model[64];      // e.g., "MiniMax-M2.5-highspeed"
} kidpal_config_t;

esp_err_t config_load(kidpal_config_t *out);
esp_err_t config_save(const kidpal_config_t *cfg);
esp_err_t config_erase(void);
