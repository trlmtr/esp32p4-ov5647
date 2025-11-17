#pragma once

#include "esp_err.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t face_detect_start(int camera_fd, esp_mqtt_client_handle_t mqtt_client);
void face_detect_stop(void);

#ifdef __cplusplus
}
#endif
