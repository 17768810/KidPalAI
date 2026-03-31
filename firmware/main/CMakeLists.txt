idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "audio_input.c"
        "audio_output.c"
        "voice_upload.c"
        "led.c"
        "config_store.c"
        "config_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_client
        esp_http_server
        driver
        nvs_flash
        esp_event
        esp_netif
    PRIV_REQUIRES
        esp_timer
        # esp_sr   # uncomment after installing ESP-SR component
        # esp_adf  # uncomment after installing ESP-ADF
)
