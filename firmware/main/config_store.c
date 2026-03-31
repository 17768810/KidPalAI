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
