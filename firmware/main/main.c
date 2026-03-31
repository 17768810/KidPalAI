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

// PCM chunk callback: write each received chunk directly to I2S speaker
static void pcm_chunk_cb(const uint8_t *data, int len)
{
    audio_output_write_pcm(data, (size_t)len);
}

// ESP-SR headers — uncomment after installing esp-sr component:
// #include "esp_wn_iface.h"
// #include "esp_wn_models.h"
// #include "esp_afe_sr_iface.h"
// #include "esp_afe_sr_models.h"

static const char *TAG = "main";

// VAD: stop recording after 1.5s of silence (75 frames × 20ms)
#define VAD_SILENCE_FRAMES  75
// Max recording: 3 seconds (150 frames × 20ms) — fits in internal DRAM without PSRAM
#define MAX_RECORD_FRAMES   150
// 20ms of audio at 16kHz mono
#define FRAME_SAMPLES       320
// Silence energy threshold (tune based on environment)
#define VAD_ENERGY_THRESHOLD 200

// Hold GPIO0 LOW for this many ms at boot to force CONFIG mode (factory reset)
#define FACTORY_RESET_HOLD_MS 3000

// Check if GPIO0 (BOOT button) is held LOW for >= FACTORY_RESET_HOLD_MS ms.
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

    // ── Global network init — called ONCE here ───────────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ── Forced CONFIG mode: hold BOOT (GPIO0) for 3s at power-on ────────────
    if (gpio0_held_long()) {
        ESP_LOGW(TAG, "GPIO0 held — erasing config and entering CONFIG mode");
        config_erase();
        config_server_start();  // noreturn
    }

    // ── Load config (fall back to Kconfig compile-time defaults) ────────────
    kidpal_config_t cfg = {0};
    if (config_load(&cfg) != ESP_OK) {
        ESP_LOGI(TAG, "no NVS config — using Kconfig defaults");
        strlcpy(cfg.wifi_ssid,   CONFIG_KIDPAL_WIFI_SSID,   sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass,   CONFIG_KIDPAL_WIFI_PASS,   sizeof(cfg.wifi_pass));
        strlcpy(cfg.gateway_url, CONFIG_KIDPAL_GATEWAY_URL, sizeof(cfg.gateway_url));
        // If Kconfig values are placeholders, wifi_init_sta will timeout → CONFIG mode.
    }

    // ── Connect WiFi ─────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Connecting to WiFi: %s", cfg.wifi_ssid);
    esp_netif_t       *sta_netif   = NULL;
    EventGroupHandle_t event_group = NULL;

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

    // ── Init ESP-SR (wake word + VAD) ────────────────────────────────────────
    // TODO: Uncomment after installing esp-sr component
    //
    // esp_afe_sr_iface_t *afe_handle = &ESP_AFE_SR_HANDLE;
    // afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    // esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);
    // esp_wn_iface_t *wakenet = &WAKENET_MODEL;
    // model_iface_data_t *wn_model = wakenet->create(NULL, DET_MODE_90);

    ESP_LOGI(TAG, "Ready. Listening for wake word (\"嘿书童\" / \"小书童\")...");

    // PCM recording buffer: 10 seconds max
    // 150 × 320 × 2 = 96 KB — fits in internal DRAM (no PSRAM required)
    int16_t *record_buf = malloc(MAX_RECORD_FRAMES * FRAME_SAMPLES * sizeof(int16_t));
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
        // Until ESP-SR is integrated, a button press on GPIO0 (BOOT) can
        // trigger recording for development testing:
        //
        //   if (gpio_get_level(GPIO_NUM_0) == 0) { /* wake triggered */ }
        //
        // For now, skip to recording immediately (dev mode only):
        bool wake_triggered = false;

        // ── PLACEHOLDER: remove this block once ESP-SR is wired up ──────────
        // In dev mode, any non-silent audio longer than 0.5s triggers recording
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
            if (loud_frames >= 25) {  // ~0.5s of loud audio
                wake_triggered = true;
                loud_frames = 0;
            }
        }
        // ────────────────────────────────────────────────────────────────────

        if (!wake_triggered) continue;

        // ── RECORDING ───────────────────────────────────────────────────────
        ESP_LOGI(TAG, "Wake detected! Recording...");
        led_set_state(LED_STATE_LISTENING);

        int total_samples  = 0;
        int silence_frames = 0;

        while (total_samples < MAX_RECORD_FRAMES * FRAME_SAMPLES) {
            int n = audio_input_read(
                record_buf + total_samples,
                FRAME_SAMPLES);
            if (n <= 0) continue;
            total_samples += n;

            // Simple energy-based VAD
            int32_t energy = 0;
            for (int i = total_samples - n; i < total_samples; i++) {
                energy += abs(record_buf[i]);
            }
            energy /= n;

            if (energy < VAD_ENERGY_THRESHOLD) {
                silence_frames++;
            } else {
                silence_frames = 0;
            }

            if (silence_frames >= VAD_SILENCE_FRAMES) {
                ESP_LOGI(TAG, "Silence detected, stopping recording");
                break;
            }
        }

        ESP_LOGI(TAG, "Recorded %d samples (%.1fs), uploading...",
                 total_samples, (float)total_samples / AUDIO_SAMPLE_RATE);

        // ── UPLOAD + STREAM PLAY ─────────────────────────────────────────────
        led_set_state(LED_STATE_WAITING);
        ESP_LOGI(TAG, "Streaming to gateway...");

        // Append "/stream" to the configured /voice URL → /voice/stream
        char stream_url[192];
        snprintf(stream_url, sizeof(stream_url), "%s/stream", cfg.gateway_url);

        esp_err_t err = voice_upload_stream(
            stream_url,
            (uint8_t *)record_buf,
            (size_t)(total_samples * sizeof(int16_t)),
            pcm_chunk_cb,
            30000);  // 30s watchdog: abort if no data for 30s

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Stream complete");
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Stream watchdog triggered");
            led_set_state(LED_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGE(TAG, "Stream failed: %d", err);
            led_set_state(LED_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        led_set_state(LED_STATE_IDLE);
        ESP_LOGI(TAG, "Ready. Listening again...");
    }

    free(record_buf);
}
