#pragma once

typedef enum {
    LED_STATE_IDLE,       // green steady  — ready, waiting for wake word
    LED_STATE_LISTENING,  // blue blink    — recording voice
    LED_STATE_WAITING,    // yellow steady — uploading / waiting for response
    LED_STATE_PLAYING,    // green blink   — playing back response
    LED_STATE_ERROR,      // red blink     — wifi lost / error
    LED_STATE_CONFIG,     // purple blink  — waiting for provisioning
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);
