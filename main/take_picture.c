#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "esp_camera.h"
#include "esp_psram.h"
#include "img_converters.h"

#define WIFI_SSID      "HUAWEI-1B9KZ3"
#define WIFI_PASS      "Availability"

static const char *TAG = "camera_sta";

// Avoid long blocking capture attempts during boot to reduce WDT reset risk.
#define CAMERA_STARTUP_SELF_TEST 0

// Image tuning for better perceived sharpness on OV2640.
#define CAM_TUNE_BRIGHTNESS 0
#define CAM_TUNE_CONTRAST 1
#define CAM_TUNE_SATURATION 0
#define CAM_TUNE_SHARPNESS 2
#define CAM_TUNE_DENOISE 0

/* ================= CAMERA PINS ================= */

/*
 * OV2640 needs a master clock input (XVCLK/XCLK).
 * - If your camera module has an onboard oscillator, set CAM_EXTERNAL_XCLK_OSC to 1.
 * - Otherwise keep it 0 and wire ESP32 XCLK to camera XVCLK.
 */
#define CAM_EXTERNAL_XCLK_OSC 1

#define CAM_PIN_PWDN 10
#define CAM_PIN_RESET 42
#if CAM_EXTERNAL_XCLK_OSC
#define CAM_PIN_XCLK -1
#define CAM_XCLK_FREQ_HZ 12000000
#else
#define CAM_PIN_XCLK 10
#define CAM_XCLK_FREQ_HZ 10000000
#endif
#define CAM_PIN_SIOD 40
#define CAM_PIN_SIOC 39
#define CAM_PIN_D7 41
#define CAM_PIN_D6 11
#define CAM_PIN_D5 12
#define CAM_PIN_D4 14
#define CAM_PIN_D3 16
#define CAM_PIN_D2 18
#define CAM_PIN_D1 17
#define CAM_PIN_D0 15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 13

/* ================= CAMERA CONFIG ================= */

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA, // 640x480
    .jpeg_quality = 8,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static camera_fb_t *camera_fb_get_retry(int retry_count, int delay_ms);
static void apply_sensor_tuning(sensor_t *s);

/* ================= CAMERA INIT ================= */

static esp_err_t init_camera(void)
{
    if (camera_config.pin_xclk < 0) {
        ESP_LOGW(TAG, "XCLK pin disabled. Make sure camera module has onboard oscillator.");
    } else {
        ESP_LOGI(TAG, "Using ESP32 output XCLK on GPIO %d @ %d Hz",
                 camera_config.pin_xclk, camera_config.xclk_freq_hz);
    }

    if (camera_config.pin_pwdn < 0) {
        ESP_LOGW(TAG, "PWDN pin is not controlled by ESP. Ensure OV_PWDN is hard-wired LOW.");
    }
    if (camera_config.pin_reset < 0) {
        ESP_LOGW(TAG, "RESET pin is not controlled by ESP. Ensure OV_RESET is hard-wired HIGH.");
    }

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "Sensor detected PID=0x%04X", s->id.PID);
        s->set_framesize(s, FRAMESIZE_VGA);
        s->set_quality(s, 8);
        apply_sensor_tuning(s);
    }

#if CAMERA_STARTUP_SELF_TEST
    camera_fb_t *test_fb = camera_fb_get_retry(2, 40);
    if (!test_fb) {
        ESP_LOGW(TAG, "Startup frame self-test failed. Check PCLK/VSYNC/HREF wiring and XCLK source.");
    } else {
        ESP_LOGI(TAG, "Startup frame self-test OK: %ux%u len=%u", test_fb->width, test_fb->height, (unsigned)test_fb->len);
        esp_camera_fb_return(test_fb);
    }
#endif

    return ESP_OK;
}

static camera_fb_t *camera_fb_get_retry(int retry_count, int delay_ms)
{
    for (int i = 0; i < retry_count; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb && fb->len > 0) {
            return fb;
        }

        if (fb) {
            ESP_LOGW(TAG, "Invalid frame len=0 on attempt %d", i + 1);
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "Failed to get frame: timeout (attempt %d)", i + 1);
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return NULL;
}

static void apply_sensor_tuning(sensor_t *s)
{
    if (!s) {
        return;
    }

    if (s->set_brightness) {
        s->set_brightness(s, CAM_TUNE_BRIGHTNESS);
    }
    if (s->set_contrast) {
        s->set_contrast(s, CAM_TUNE_CONTRAST);
    }
    if (s->set_saturation) {
        s->set_saturation(s, CAM_TUNE_SATURATION);
    }
    if (s->set_sharpness) {
        s->set_sharpness(s, CAM_TUNE_SHARPNESS);
    }
    if (s->set_denoise) {
        s->set_denoise(s, CAM_TUNE_DENOISE);
    }
    if (s->set_whitebal) {
        s->set_whitebal(s, 1);
    }
    if (s->set_awb_gain) {
        s->set_awb_gain(s, 1);
    }
    if (s->set_gain_ctrl) {
        s->set_gain_ctrl(s, 1);
    }
    if (s->set_exposure_ctrl) {
        s->set_exposure_ctrl(s, 1);
    }
    if (s->set_aec2) {
        s->set_aec2(s, 1);
    }
    if (s->set_ae_level) {
        s->set_ae_level(s, 0);
    }

    ESP_LOGI(TAG, "Sensor tuning applied: sharpness=%d contrast=%d quality=%d",
             CAM_TUNE_SHARPNESS, CAM_TUNE_CONTRAST, 8);
}

/* ================= STREAM HANDLER ================= */

static esp_err_t stream_handler(httpd_req_t *req)
{
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=frame";
    static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

    while (true) {

        camera_fb_t *fb = camera_fb_get_retry(3, 30);
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            return ESP_FAIL;
        }

        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;

        if (fb->format == PIXFORMAT_JPEG) {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        } else {
            if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                         fb->format, 80, &jpg_buf, &jpg_len)) {
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                return ESP_FAIL;
            }
        }

        char part_buf[64];
        size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_len);

        if (httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)) != ESP_OK ||
            httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len) != ESP_OK) {
            if (fb->format != PIXFORMAT_JPEG) {
                free(jpg_buf);
            }
            esp_camera_fb_return(fb);
            break;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            free(jpg_buf);
        }
        esp_camera_fb_return(fb);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = camera_fb_get_retry(3, 30);
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;

    if (fb->format == PIXFORMAT_JPEG) {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
    } else {
        if (!fmt2jpg(fb->buf, fb->len,
                     fb->width, fb->height,
                     fb->format,
                     90,
                     &jpg_buf, &jpg_len)) {

            ESP_LOGE(TAG, "JPEG compression failed");
            esp_camera_fb_return(fb);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

    esp_err_t res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);

    if (fb->format != PIXFORMAT_JPEG) {
        free(jpg_buf);
    }
    esp_camera_fb_return(fb);

    return res;
}


/* ================= HTTP SERVER ================= */

static void start_camera_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {

        httpd_uri_t stream_uri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);

        httpd_uri_t capture_uri = {
            .uri       = "/capture",
            .method    = HTTP_GET,
            .handler   = capture_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &capture_uri);
    }
}


/* ================= WIFI EVENT HANDLER ================= */

static void wifi_event_handler(void* arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to WiFi...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t* event =
            (wifi_event_sta_disconnected_t*) event_data;

        ESP_LOGI(TAG, "Disconnected, reason: %d", event->reason);

        esp_wifi_connect();
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_camera_server();
    }
}


/* ================= WIFI INIT (STA) ================= */

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ================= MAIN ================= */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM size: %d bytes", psram_size);

    if (ESP_OK != init_camera()) {
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    wifi_init_sta();
}
