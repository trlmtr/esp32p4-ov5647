/*
 * Camera Initialization Header
 * For OV5647 CSI Camera on ESP32-P4
 */
#pragma once

#include "esp_err.h"
#include "esp_cam_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Camera configuration structure
 */
typedef struct {
    int i2c_port;           // I2C port number
    int scl_pin;            // I2C SCL pin
    int sda_pin;            // I2C SDA pin
    int reset_pin;          // Camera reset pin (-1 if not used)
    int pwdn_pin;           // Camera power down pin (-1 if not used)
    uint32_t xclk_freq_hz;  // External clock frequency
} camera_config_t;

/**
 * @brief Initialize the OV5647 camera
 * 
 * @param config Camera configuration
 * @return Camera device handle or NULL on failure
 */
esp_cam_sensor_device_t *camera_init(const camera_config_t *config);

/**
 * @brief Get the file descriptor for the camera device
 * 
 * @return File descriptor or -1 on error
 */
int camera_get_fd(void);

/**
 * @brief Get camera frame buffer information
 * 
 * @param width Output: frame width
 * @param height Output: frame height
 * @param format Output: pixel format
 * @return ESP_OK on success
 */
esp_err_t camera_get_frame_info(uint32_t *width, uint32_t *height, uint32_t *format);

/**
 * @brief Deinitialize the camera
 */
void camera_deinit(void);

#ifdef __cplusplus
}
#endif
