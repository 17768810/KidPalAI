#include "audio_output.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "audio_output";

// MAX98357A wiring (adjust GPIO to match your board)
#define I2S_SPK_BCLK  GPIO_NUM_6
#define I2S_SPK_LRC   GPIO_NUM_5
#define I2S_SPK_DIN   GPIO_NUM_4

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t audio_output_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        // 16kHz matches the PCM contract from the gateway TTS
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        // MONO: gateway sends 16-bit mono PCM; STEREO would halve the pitch
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCLK,
            .ws   = I2S_SPK_LRC,
            .dout = I2S_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S speaker initialized (16kHz MONO)");
    return ESP_OK;
}

esp_err_t audio_output_play_mp3(const uint8_t *mp3_data, size_t mp3_len)
{
    // Not used in streaming mode. Kept as stub.
    ESP_LOGW(TAG, "play_mp3 stub called (%d bytes) — use audio_output_write_pcm instead", (int)mp3_len);
    return ESP_OK;
}

esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    size_t written = 0;
    esp_err_t err = i2s_channel_write(tx_handle, data, len, &written, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write error: %d", err);
    }
    return err;
}
