#pragma once

typedef enum {
    LED_STATE_IDLE     = 0,
    LED_STATE_LISTENING,
    LED_STATE_WAITING,
    LED_STATE_ERROR,
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
