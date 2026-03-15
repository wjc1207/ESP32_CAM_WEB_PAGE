idf_component_register(
    SRCS "take_picture.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES 
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        esp_psram
        esp32-camera
)
