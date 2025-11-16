/*
 * Camera Web Server Header
 * Provides HTTP endpoints for camera streaming and image capture
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the camera web server
 * 
 * @param camera_fd File descriptor of the camera device
 * @return ESP_OK on success
 */
esp_err_t camera_server_start(int camera_fd);

/**
 * @brief Stop the camera web server
 */
void camera_server_stop(void);

#ifdef __cplusplus
}
#endif
