idf_component_register(
    SRCS "main.c"
        "camera_core/camera_core.c"
        "camera_http/camera_http.c"
        "wifi_sta/wifi_sta.c"
    INCLUDE_DIRS "."
              "camera_core"
              "camera_http"
              "wifi_sta"
    PRIV_REQUIRES 
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        esp_psram
        esp32-camera
)
