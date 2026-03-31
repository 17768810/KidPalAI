#include "led.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define LED_R_GPIO  GPIO_NUM_1
#define LED_G_GPIO  GPIO_NUM_2
#define LED_B_GPIO  GPIO_NUM_3

static esp_timer_handle_t s_blink_timer;
static bool s_blink_r = false;
static bool s_blink_b = false;

static void blink_cb(void *arg)
{
    static bool on = false;
    on = !on;
    gpio_set_level(LED_R_GPIO, on && s_blink_r ? 1 : 0);
    gpio_set_level(LED_B_GPIO, on && s_blink_b ? 1 : 0);
}

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_R_GPIO) |
                        (1ULL << LED_G_GPIO) |
                        (1ULL << LED_B_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    esp_timer_create_args_t args = {
        .callback = blink_cb,
        .name     = "led_blink",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_blink_timer));

    led_set_state(LED_STATE_IDLE);
}

void led_set_state(led_state_t state)
{
    // Stop any active blink before changing state
    if (esp_timer_is_active(s_blink_timer)) {
        esp_timer_stop(s_blink_timer);
    }
    s_blink_r = false;
    s_blink_b = false;

    // Turn all LEDs off
    gpio_set_level(LED_R_GPIO, 0);
    gpio_set_level(LED_G_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    switch (state) {
        case LED_STATE_IDLE:
            gpio_set_level(LED_G_GPIO, 1);  // green steady
            break;
        case LED_STATE_LISTENING:
            gpio_set_level(LED_B_GPIO, 1);  // blue steady
            break;
        case LED_STATE_WAITING:
            gpio_set_level(LED_R_GPIO, 1);  // yellow = R+G
            gpio_set_level(LED_G_GPIO, 1);
            break;
        case LED_STATE_PLAYING:
            gpio_set_level(LED_G_GPIO, 1);  // green steady
            break;
        case LED_STATE_ERROR:
            gpio_set_level(LED_R_GPIO, 1);  // red steady
            break;
        case LED_STATE_CONFIG:
            // purple = R+B, 1 Hz blink (500 ms half-period)
            s_blink_r = true;
            s_blink_b = true;
            esp_timer_start_periodic(s_blink_timer, 500000);  // 500 ms in µs
            break;
    }
}
