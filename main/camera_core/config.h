#pragma once

#include "esp_camera.h"

// Camera runtime settings
#define CAMERA_STARTUP_SELF_TEST 0
#define CAMERA_STREAM_FRAME_SIZE FRAMESIZE_VGA
#define CAMERA_STREAM_JPEG_QUALITY 8
#define CAMERA_FB_COUNT 1
#define CAMERA_GRAB_MODE CAMERA_GRAB_WHEN_EMPTY

// Sensor tuning
#define CAMERA_TUNE_BRIGHTNESS 1
#define CAMERA_TUNE_CONTRAST 1
#define CAMERA_TUNE_SATURATION 0
#define CAMERA_TUNE_SHARPNESS 2
#define CAMERA_TUNE_DENOISE 2

// Pin and clock mapping
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
