#pragma once

#include "esp_err.h"

/**
 * Initialize the QQ bridge client by loading config from NVS/build defaults.
 */
esp_err_t qqbot_bridge_init(void);

/**
 * Start the QQ bridge WebSocket task if QQ bridge config is present.
 */
esp_err_t qqbot_bridge_start(void);

/**
 * Send a QQ reply for the given routed chat_id over the bridge.
 */
esp_err_t qqbot_bridge_send_message(const char *chat_id, const char *text);
