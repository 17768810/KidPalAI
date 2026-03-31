#include "audio_input.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>

static const char *TAG = "audio_input";

// INMP441 wiring (adjust GPIO to match your board)
#define I2S_MIC_SCK   GPIO_NUM_12
#define I2S_MIC_WS    GPIO_NUM_11
#define I2S_MIC_SD    GPIO_NUM_10

static i2s_chan_handle_t rx_handle = NULL;

esp_err_t audio_input_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        // INMP441 outputs 32-bit words; we shift down to 16-bit in read()
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK,
            .ws   = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S mic initialized at %d Hz", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

int audio_input_read(int16_t *buf, size_t max_samples)
{
    // INMP441 outputs 32-bit words; audio data is in the upper 18 bits
    assert(max_samples <= AUDIO_FRAME_SAMPLES);
    int32_t raw[AUDIO_FRAME_SAMPLES];
    size_t bytes_read = 0;
    esp_err_t read_err = i2s_channel_read(rx_handle, raw, AUDIO_FRAME_SAMPLES * sizeof(int32_t),
                                           &bytes_read, pdMS_TO_TICKS(100));
    if (read_err != ESP_OK || bytes_read == 0) return -1;
    int samples = (int)(bytes_read / sizeof(int32_t));
    for (int i = 0; i < samples; i++) {
        buf[i] = (int16_t)(raw[i] >> 14);
    }
    return samples;
}
