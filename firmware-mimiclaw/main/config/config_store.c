#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define NVS_NAMESPACE "kidpal"
#define KEY_SSID      "wifi_ssid"
#define KEY_PASS      "wifi_pass"
#define KEY_URL       "gateway_url"
#define KEY_LLM_KEY   "llm_key"
#define KEY_LLM_MODEL "llm_model"

static const char *TAG = "config_store";

esp_err_t config_load(kidpal_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) return err;

    size_t len;
    bool ok = true;

    len = sizeof(out->wifi_ssid);   ok &= (nvs_get_str(h, KEY_SSID,      out->wifi_ssid,   &len) == ESP_OK);
    len = sizeof(out->wifi_pass);   ok &= (nvs_get_str(h, KEY_PASS,      out->wifi_pass,   &len) == ESP_OK);
    len = sizeof(out->gateway_url); ok &= (nvs_get_str(h, KEY_URL,       out->gateway_url, &len) == ESP_OK);
    len = sizeof(out->llm_key);     nvs_get_str(h, KEY_LLM_KEY,   out->llm_key,   &len);  // optional
    len = sizeof(out->llm_model);   nvs_get_str(h, KEY_LLM_MODEL, out->llm_model, &len);  // optional

    nvs_close(h);
    if (!ok) return ESP_ERR_NVS_NOT_FOUND;
    ESP_LOGI(TAG, "loaded: ssid=%s url=%s", out->wifi_ssid, out->gateway_url);
    return ESP_OK;
}

esp_err_t config_save(const kidpal_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, KEY_SSID, cfg->wifi_ssid);
    if (err != ESP_OK) { nvs_close(h); return err; }
    err = nvs_set_str(h, KEY_PASS, cfg->wifi_pass);
    if (err != ESP_OK) { nvs_close(h); return err; }
    err = nvs_set_str(h, KEY_URL, cfg->gateway_url);
    if (err != ESP_OK) { nvs_close(h); return err; }

    // Optional LLM fields — log on failure but don't abort
    if (cfg->llm_key[0]) {
        esp_err_t e = nvs_set_str(h, KEY_LLM_KEY, cfg->llm_key);
        if (e != ESP_OK) ESP_LOGW(TAG, "llm_key write failed: %d", e);
    }
    if (cfg->llm_model[0]) {
        esp_err_t e = nvs_set_str(h, KEY_LLM_MODEL, cfg->llm_model);
        if (e != ESP_OK) ESP_LOGW(TAG, "llm_model write failed: %d", e);
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %d", err);
        return err;
    }
    ESP_LOGI(TAG, "config saved");
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
    return err;
}
