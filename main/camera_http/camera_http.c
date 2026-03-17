#include "camera_http.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "esp_camera.h"
#include "img_converters.h"

#include "secrets.h"
#include "camera_core.h"

static const char *TAG = "camera_sta";

static bool check_auth(httpd_req_t *req)
{
    const char *expected = CAM_API_TOKEN;
    if (expected[0] == '\0') {
        return true;
    }

    char auth_buf[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) == ESP_OK) {
        if (strncmp(auth_buf, "Bearer ", 7) == 0 && strcmp(auth_buf + 7, expected) == 0) {
            return true;
        }
    }

    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char token_val[160];
        if (httpd_query_key_value(query, "token", token_val, sizeof(token_val)) == ESP_OK &&
            strcmp(token_val, expected) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=frame";
    static const char *_STREAM_BOUNDARY = "\r\n--frame\r\n";
    static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    while (true) {
        camera_fb_t *fb = NULL;
        esp_err_t acq = camera_core_acquire_fb(&fb, 3, 30, pdMS_TO_TICKS(1000));
        if (acq != ESP_OK || fb == NULL) {
            ESP_LOGE(TAG, "Camera capture failed");
            return ESP_FAIL;
        }

        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;

        if (fb->format == PIXFORMAT_JPEG) {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        } else {
            if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 80, &jpg_buf, &jpg_len)) {
                ESP_LOGE(TAG, "JPEG compression failed");
                camera_core_release_fb(fb);
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
            camera_core_release_fb(fb);
            break;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            free(jpg_buf);
        }
        camera_core_release_fb(fb);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    camera_fb_t *fb = NULL;
    esp_err_t acq = camera_core_acquire_fb_latest(&fb, pdMS_TO_TICKS(1000));
    if (acq != ESP_OK || fb == NULL) {
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
        if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 90, &jpg_buf, &jpg_len)) {
            ESP_LOGE(TAG, "JPEG compression failed");
            camera_core_release_fb(fb);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    esp_err_t res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);

    if (fb->format != PIXFORMAT_JPEG) {
        free(jpg_buf);
    }
    camera_core_release_fb(fb);

    return res;
}

static esp_err_t capture_human_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    camera_fb_t *fb = NULL;
    esp_err_t acq = camera_core_acquire_fb_human(&fb, pdMS_TO_TICKS(2000));
    if (acq != ESP_OK || fb == NULL) {
        ESP_LOGE(TAG, "Human capture failed, err=0x%x", (unsigned int)acq);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Human capture failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture_human.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_core_release_fb_human(fb);
    return res;
}

void camera_http_start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &stream_uri);

        httpd_uri_t capture_uri = {
            .uri = "/capture",
            .method = HTTP_GET,
            .handler = capture_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &capture_uri);

        httpd_uri_t capture_human_uri = {
            .uri = "/capture_human",
            .method = HTTP_GET,
            .handler = capture_human_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &capture_human_uri);
    }
}
