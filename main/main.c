#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include "esp_psram.h"

#include "camera_core.h"
#include "wifi_sta.h"

static const char *TAG = "camera_sta";


void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM size: %d bytes", psram_size);

    if (ESP_OK != camera_core_init()) {
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    wifi_init_sta();
}
