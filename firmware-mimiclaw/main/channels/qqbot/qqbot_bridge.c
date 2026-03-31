#include "channels/qqbot/qqbot_bridge.h"

#include "mimi_config.h"
#include "bus/message_bus.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "qqbot_bridge";

#define QQBOT_APP_ID_LEN            64
#define QQBOT_APP_SECRET_LEN        128
#define QQBOT_WS_URL_LEN            384
#define QQBOT_DEVICE_ID_LEN         32
#define QQBOT_ROUTE_CACHE_SIZE      16
#define QQBOT_WS_STACK              (10 * 1024)
#define QQBOT_WS_PRIO               5
#define QQBOT_WS_CORE               0
#define QQBOT_WS_SEND_TIMEOUT_MS    5000
#define QQBOT_WS_RECONNECT_MS       3000

typedef struct {
    bool active;
    char chat_id[sizeof(((mimi_msg_t *)0)->chat_id)];
    char chat_type[16];
    char user_openid[96];
    char group_openid[96];
    char msg_id[96];
} qq_reply_route_t;

static char s_qq_app_id[QQBOT_APP_ID_LEN] = MIMI_SECRET_QQ_APP_ID;
static char s_qq_app_secret[QQBOT_APP_SECRET_LEN] = MIMI_SECRET_QQ_APP_SECRET;
static char s_ws_url[QQBOT_WS_URL_LEN] = MIMI_SECRET_QQ_GATEWAY_WS_URL;
static char s_device_id[QQBOT_DEVICE_ID_LEN] = {0};

static esp_websocket_client_handle_t s_ws_client = NULL;
static TaskHandle_t s_bridge_task = NULL;
static SemaphoreHandle_t s_send_lock = NULL;
static SemaphoreHandle_t s_route_lock = NULL;
static bool s_ws_connected = false;
static bool s_registered = false;

static qq_reply_route_t s_reply_routes[QQBOT_ROUTE_CACHE_SIZE];
static size_t s_reply_route_idx = 0;

static void qqbot_copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void qqbot_build_device_id(void)
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(
            s_device_id,
            sizeof(s_device_id),
            "mimi-%02X%02X%02X",
            mac[3], mac[4], mac[5]);
        return;
    }
    qqbot_copy_string(s_device_id, sizeof(s_device_id), "mimi-qq");
}

static void qqbot_load_nvs_string(
    nvs_handle_t nvs,
    const char *key,
    char *dst,
    size_t dst_size,
    const char *label)
{
    char tmp[QQBOT_WS_URL_LEN] = {0};
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_str(nvs, key, tmp, &len);
    if (err == ESP_OK && tmp[0]) {
        qqbot_copy_string(dst, dst_size, tmp);
        ESP_LOGI(TAG, "Loaded %s from NVS", label);
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Ignoring %s from NVS: value too long", label);
    }
}

static bool qqbot_bridge_configured(void)
{
    return s_qq_app_id[0] != '\0' && s_ws_url[0] != '\0';
}

static void qqbot_reset_routes(void)
{
    if (!s_route_lock) {
        memset(s_reply_routes, 0, sizeof(s_reply_routes));
        s_reply_route_idx = 0;
        return;
    }

    if (xSemaphoreTake(s_route_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    memset(s_reply_routes, 0, sizeof(s_reply_routes));
    s_reply_route_idx = 0;
    xSemaphoreGive(s_route_lock);
}

static void qqbot_cache_reply_route(const char *chat_id, cJSON *reply_to)
{
    if (!chat_id || !chat_id[0] || !cJSON_IsObject(reply_to) || !s_route_lock) {
        return;
    }

    cJSON *chat_type = cJSON_GetObjectItem(reply_to, "chat_type");
    cJSON *user_openid = cJSON_GetObjectItem(reply_to, "user_openid");
    cJSON *group_openid = cJSON_GetObjectItem(reply_to, "group_openid");
    cJSON *msg_id = cJSON_GetObjectItem(reply_to, "msg_id");

    if (!cJSON_IsString(chat_type) || !cJSON_IsString(msg_id)) {
        return;
    }

    qq_reply_route_t route = {0};
    route.active = true;
    qqbot_copy_string(route.chat_id, sizeof(route.chat_id), chat_id);
    qqbot_copy_string(route.chat_type, sizeof(route.chat_type), chat_type->valuestring);
    qqbot_copy_string(route.msg_id, sizeof(route.msg_id), msg_id->valuestring);

    if (strcmp(route.chat_type, "private") == 0) {
        if (!cJSON_IsString(user_openid) || !user_openid->valuestring[0]) {
            return;
        }
        qqbot_copy_string(route.user_openid, sizeof(route.user_openid), user_openid->valuestring);
    } else if (strcmp(route.chat_type, "group") == 0) {
        if (!cJSON_IsString(group_openid) || !group_openid->valuestring[0]) {
            return;
        }
        qqbot_copy_string(route.group_openid, sizeof(route.group_openid), group_openid->valuestring);
    } else {
        return;
    }

    if (xSemaphoreTake(s_route_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    for (size_t i = 0; i < QQBOT_ROUTE_CACHE_SIZE; i++) {
        if (s_reply_routes[i].active && strcmp(s_reply_routes[i].chat_id, chat_id) == 0) {
            s_reply_routes[i] = route;
            xSemaphoreGive(s_route_lock);
            return;
        }
    }

    s_reply_routes[s_reply_route_idx] = route;
    s_reply_route_idx = (s_reply_route_idx + 1) % QQBOT_ROUTE_CACHE_SIZE;
    xSemaphoreGive(s_route_lock);
}

static bool qqbot_lookup_reply_route(const char *chat_id, qq_reply_route_t *out_route)
{
    if (!chat_id || !chat_id[0] || !out_route || !s_route_lock) {
        return false;
    }

    if (xSemaphoreTake(s_route_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    for (size_t i = 0; i < QQBOT_ROUTE_CACHE_SIZE; i++) {
        if (s_reply_routes[i].active && strcmp(s_reply_routes[i].chat_id, chat_id) == 0) {
            *out_route = s_reply_routes[i];
            xSemaphoreGive(s_route_lock);
            return true;
        }
    }

    xSemaphoreGive(s_route_lock);
    return false;
}

static esp_err_t qqbot_send_json(cJSON *root)
{
    esp_err_t ret = ESP_FAIL;
    char *json_str = NULL;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_send_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(QQBOT_WS_SEND_TIMEOUT_MS)) != pdTRUE) {
        free(json_str);
        return ESP_ERR_TIMEOUT;
    }

    if (!s_ws_client || !s_ws_connected) {
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    if (esp_websocket_client_send_text(
            s_ws_client,
            json_str,
            strlen(json_str),
            pdMS_TO_TICKS(QQBOT_WS_SEND_TIMEOUT_MS)) < 0) {
        ESP_LOGW(TAG, "WebSocket send failed");
        s_registered = false;
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = ESP_OK;

cleanup:
    xSemaphoreGive(s_send_lock);
    free(json_str);
    return ret;
}

static esp_err_t qqbot_send_register(void)
{
    cJSON *root = cJSON_CreateObject();
    esp_err_t ret;

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "register");
    cJSON_AddStringToObject(root, "qq_app_id", s_qq_app_id);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    if (s_qq_app_secret[0]) {
        cJSON_AddStringToObject(root, "qq_app_secret", s_qq_app_secret);
    }

    ret = qqbot_send_json(root);
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "QQ bridge registration sent for app_id=%s", s_qq_app_id);
    }

    return ret;
}

static void qqbot_handle_inbound_payload(cJSON *payload)
{
    cJSON *channel = cJSON_GetObjectItem(payload, "channel");
    cJSON *chat_id = cJSON_GetObjectItem(payload, "chat_id");
    cJSON *content = cJSON_GetObjectItem(payload, "content");
    cJSON *reply_to = cJSON_GetObjectItem(payload, "reply_to");

    if (!cJSON_IsString(channel) || strcmp(channel->valuestring, MIMI_CHAN_QQBOT) != 0) {
        return;
    }
    if (!cJSON_IsString(chat_id) || !chat_id->valuestring[0]) {
        return;
    }
    if (!cJSON_IsString(content) || !content->valuestring[0]) {
        return;
    }

    qqbot_cache_reply_route(chat_id->valuestring, reply_to);

    mimi_msg_t msg = {0};
    qqbot_copy_string(msg.channel, sizeof(msg.channel), MIMI_CHAN_QQBOT);
    qqbot_copy_string(msg.chat_id, sizeof(msg.chat_id), chat_id->valuestring);
    msg.content = strdup(content->valuestring);
    if (!msg.content) {
        ESP_LOGW(TAG, "Failed to allocate inbound QQ message");
        return;
    }

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Inbound queue full, drop QQ message");
        free(msg.content);
        return;
    }

    ESP_LOGI(TAG, "Queued QQ inbound for %s", msg.chat_id);
}

static void qqbot_handle_bridge_message(const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    cJSON *type;
    if (!root) {
        ESP_LOGW(TAG, "Invalid QQ bridge payload");
        return;
    }

    type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "registered") == 0) {
        s_registered = true;
        ESP_LOGI(TAG, "QQ bridge registered");
    } else if (strcmp(type->valuestring, "inbound_message") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            qqbot_handle_inbound_payload(payload);
        }
    } else if (strcmp(type->valuestring, "outbound_sent") == 0) {
        ESP_LOGI(TAG, "QQ outbound delivery acknowledged");
    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *detail = cJSON_GetObjectItem(root, "detail");
        ESP_LOGW(TAG, "QQ bridge error: %s",
                 cJSON_IsString(detail) ? detail->valuestring : "unknown");
    }

    cJSON_Delete(root);
}

static void qqbot_ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    static char *rx_buf = NULL;
    static size_t rx_cap = 0;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_ws_connected = true;
        s_registered = false;
        ESP_LOGI(TAG, "QQ bridge connected");
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_ws_connected = false;
        s_registered = false;
        ESP_LOGW(TAG, "QQ bridge disconnected");
        if (rx_buf) {
            free(rx_buf);
            rx_buf = NULL;
            rx_cap = 0;
        }
        return;
    }

    if (event_id != WEBSOCKET_EVENT_DATA) {
        return;
    }

    if (e->op_code != WS_TRANSPORT_OPCODES_TEXT) {
        return;
    }

    size_t needed = e->payload_offset + e->data_len + 1;
    if (e->payload_offset == 0) {
        if (rx_buf) {
            free(rx_buf);
        }
        rx_cap = (e->payload_len + 1 > needed) ? (e->payload_len + 1) : needed;
        rx_buf = calloc(1, rx_cap);
        if (!rx_buf) {
            rx_cap = 0;
            return;
        }
    } else if (!rx_buf || needed > rx_cap) {
        return;
    }

    memcpy(rx_buf + e->payload_offset, e->data_ptr, e->data_len);

    if ((e->payload_offset + e->data_len) >= e->payload_len) {
        rx_buf[e->payload_len] = '\0';
        qqbot_handle_bridge_message(rx_buf, e->payload_len);
        free(rx_buf);
        rx_buf = NULL;
        rx_cap = 0;
    }
}

static void qqbot_bridge_task(void *arg)
{
    (void)arg;

    while (1) {
        esp_websocket_client_config_t ws_cfg = {
            .uri = s_ws_url,
            .buffer_size = 2048,
            .task_stack = QQBOT_WS_STACK,
            .network_timeout_ms = 10000,
            .reconnect_timeout_ms = QQBOT_WS_RECONNECT_MS,
            .disable_auto_reconnect = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        s_ws_client = esp_websocket_client_init(&ws_cfg);
        if (!s_ws_client) {
            ESP_LOGE(TAG, "Failed to init QQ WebSocket client");
            vTaskDelay(pdMS_TO_TICKS(QQBOT_WS_RECONNECT_MS));
            continue;
        }

        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, qqbot_ws_event_handler, NULL);

        if (esp_websocket_client_start(s_ws_client) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start QQ WebSocket client");
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
            vTaskDelay(pdMS_TO_TICKS(QQBOT_WS_RECONNECT_MS));
            continue;
        }

        while (s_ws_client) {
            if (s_ws_connected && !s_registered) {
                qqbot_send_register();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            if (!esp_websocket_client_is_connected(s_ws_client) && !s_ws_connected) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(250));
        }

        if (s_ws_client) {
            esp_websocket_client_stop(s_ws_client);
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
        }

        s_ws_connected = false;
        s_registered = false;
        vTaskDelay(pdMS_TO_TICKS(QQBOT_WS_RECONNECT_MS));
    }
}

esp_err_t qqbot_bridge_init(void)
{
    nvs_handle_t nvs;

    if (!s_send_lock) {
        s_send_lock = xSemaphoreCreateMutex();
    }
    if (!s_route_lock) {
        s_route_lock = xSemaphoreCreateMutex();
    }
    if (!s_send_lock || !s_route_lock) {
        return ESP_ERR_NO_MEM;
    }

    qqbot_build_device_id();
    qqbot_reset_routes();

    if (nvs_open(MIMI_NVS_QQ, NVS_READONLY, &nvs) == ESP_OK) {
        qqbot_load_nvs_string(nvs, MIMI_NVS_KEY_QQ_APP_ID, s_qq_app_id, sizeof(s_qq_app_id), "qq_app_id");
        qqbot_load_nvs_string(
            nvs,
            MIMI_NVS_KEY_QQ_APP_SECRET,
            s_qq_app_secret,
            sizeof(s_qq_app_secret),
            "qq_app_secret");
        qqbot_load_nvs_string(
            nvs,
            MIMI_NVS_KEY_QQ_GATEWAY_WS_URL,
            s_ws_url,
            sizeof(s_ws_url),
            "qq_gateway_ws_url");
        nvs_close(nvs);
    }

    if (qqbot_bridge_configured()) {
        ESP_LOGI(TAG, "QQ bridge config ready (app_id=%s, ws=%s)", s_qq_app_id, s_ws_url);
    } else {
        ESP_LOGW(TAG, "QQ bridge not configured; app_id or ws_url missing");
    }

    return ESP_OK;
}

esp_err_t qqbot_bridge_start(void)
{
    BaseType_t ok;

    if (!qqbot_bridge_configured()) {
        ESP_LOGW(TAG, "Skipping QQ bridge start: incomplete configuration");
        return ESP_OK;
    }
    if (s_bridge_task) {
        ESP_LOGW(TAG, "QQ bridge task already running");
        return ESP_OK;
    }

    ok = xTaskCreatePinnedToCore(
        qqbot_bridge_task,
        "qqbot_ws",
        QQBOT_WS_STACK,
        NULL,
        QQBOT_WS_PRIO,
        &s_bridge_task,
        QQBOT_WS_CORE);

    if (ok != pdPASS) {
        s_bridge_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "QQ bridge task started");
    return ESP_OK;
}

esp_err_t qqbot_bridge_send_message(const char *chat_id, const char *text)
{
    qq_reply_route_t route = {0};
    cJSON *root = NULL;
    cJSON *payload = NULL;
    cJSON *reply_to = NULL;
    esp_err_t ret;

    if (!chat_id || !chat_id[0] || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registered) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!qqbot_lookup_reply_route(chat_id, &route)) {
        ESP_LOGW(TAG, "No cached QQ reply route for %s", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    root = cJSON_CreateObject();
    payload = cJSON_CreateObject();
    reply_to = cJSON_CreateObject();
    if (!root || !payload || !reply_to) {
        cJSON_Delete(root);
        cJSON_Delete(payload);
        cJSON_Delete(reply_to);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "outbound_reply");
    cJSON_AddStringToObject(payload, "content", text);
    cJSON_AddStringToObject(reply_to, "chat_type", route.chat_type);
    cJSON_AddStringToObject(reply_to, "msg_id", route.msg_id);
    if (strcmp(route.chat_type, "private") == 0) {
        cJSON_AddStringToObject(reply_to, "user_openid", route.user_openid);
    } else {
        cJSON_AddStringToObject(reply_to, "group_openid", route.group_openid);
    }
    cJSON_AddItemToObject(payload, "reply_to", reply_to);
    cJSON_AddItemToObject(root, "payload", payload);

    ret = qqbot_send_json(root);
    cJSON_Delete(root);
    return ret;
}
