/*
 * Camera Initialization Implementation
 * For OV5647 CSI Camera on ESP32-P4
 */
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_video_init.h"
#include "ov5647.h"
#include "camera_init.h"

static const char *TAG = "camera_init";

static esp_cam_sensor_device_t *s_cam_dev = NULL;
static int s_video_fd = -1;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_sccb_io_handle_t s_sccb_handle = NULL;
static bool s_video_initialized = false;

static esp_err_t init_i2c_bus(int port, int scl_pin, int sda_pin, i2c_master_bus_handle_t *i2c_bus_handle)
{
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .scl_io_num = scl_pin,
        .sda_io_num = sda_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&i2c_bus_conf, i2c_bus_handle);
}

esp_cam_sensor_device_t *camera_init(const camera_config_t *config)
{
    if (s_cam_dev) {
        ESP_LOGW(TAG, "Camera already initialized");
        return s_cam_dev;
    }

    ESP_LOGI(TAG, "Initializing OV5647 camera...");
    ESP_LOGI(TAG, "I2C port=%d, scl_pin=%d, sda_pin=%d", 
             config->i2c_port, config->scl_pin, config->sda_pin);

    // Initialize I2C bus for SCCB
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    esp_sccb_io_handle_t sccb_handle = NULL;
    esp_err_t ret = init_i2c_bus(config->i2c_port, config->scl_pin, config->sda_pin, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return NULL;
    }

    // Create SCCB handle
    sccb_i2c_config_t sccb_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV5647_SCCB_ADDR,
        .scl_speed_hz = 400000,
        .addr_bits_width = 16,
        .val_bits_width = 8,
    };

    ret = sccb_new_i2c_io(i2c_bus_handle, &sccb_config, &sccb_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SCCB handle: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Configure camera sensor
    esp_cam_sensor_config_t cam_config = {
        .sccb_handle = sccb_handle,
        .reset_pin = config->reset_pin,
        .pwdn_pin = config->pwdn_pin,
        .xclk_pin = -1,  // XCLK managed by CSI controller
        .xclk_freq_hz = config->xclk_freq_hz,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };

    // Detect and initialize OV5647
    s_cam_dev = ov5647_detect(&cam_config);
    if (!s_cam_dev) {
        ESP_LOGE(TAG, "Failed to detect OV5647 camera");
        i2c_del_master_bus(i2c_bus_handle);
        return NULL;
    }

    ESP_LOGI(TAG, "Detected camera: %s", esp_cam_sensor_get_name(s_cam_dev));

    // Query supported formats
    int desired_width = CONFIG_CAMERA_FRAME_WIDTH;
    int desired_height = CONFIG_CAMERA_FRAME_HEIGHT;
    ESP_LOGI(TAG, "Desired sensor resolution: %dx%d", desired_width, desired_height);

    esp_cam_sensor_format_array_t format_array = {0};
    ret = esp_cam_sensor_query_format(s_cam_dev, &format_array);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query camera formats");
        goto fail;
    }

    const esp_cam_sensor_format_t *selected_format = NULL;
    for (int i = 0; i < format_array.count; i++) {
        const esp_cam_sensor_format_t *fmt = &format_array.format_array[i];
        ESP_LOGI(TAG, "Format[%d]: %s, %dx%d, %dfps", 
                 i, fmt->name, fmt->width, fmt->height, fmt->fps);

        if (fmt->format != ESP_CAM_SENSOR_PIXFORMAT_RAW10) {
            continue;
        }

        if (desired_width > 0 && desired_height > 0 && fmt->width == desired_width && fmt->height == desired_height) {
            selected_format = fmt;
            break;
        }

        if (!selected_format) {
            selected_format = fmt;
        }
    }

    if (!selected_format) {
        ESP_LOGE(TAG, "No suitable format found");
        goto fail;
    }

    ESP_LOGI(TAG, "Selected format: %s", selected_format->name);

    // Set the format
    ret = esp_cam_sensor_set_format(s_cam_dev, selected_format);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set camera format");
        goto fail;
    }

    s_i2c_bus = i2c_bus_handle;
    s_sccb_handle = sccb_handle;

    if (!s_video_initialized) {
        esp_video_init_csi_config_t csi_cfg = {
            .sccb_config = {
                .init_sccb = false,
                .i2c_handle = s_i2c_bus,
                .freq = 400000,
            },
            .reset_pin = config->reset_pin,
            .pwdn_pin = config->pwdn_pin,
        };

        esp_video_init_config_t video_cfg = {
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
            .csi = &csi_cfg,
#endif
        };

        ret = esp_video_init(&video_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize video subsystem: %s", esp_err_to_name(ret));
            goto fail;
        }
        s_video_initialized = true;
    }

    // Open video device
    s_video_fd = open("/dev/video0", O_RDWR);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "Failed to open video device");
        goto fail;
    }

    ESP_LOGI(TAG, "Camera initialized successfully");
    return s_cam_dev;

fail:
    if (s_cam_dev) {
        esp_cam_sensor_del_dev(s_cam_dev);
        s_cam_dev = NULL;
    }
    if (sccb_handle) {
        esp_sccb_del_i2c_io(sccb_handle);
        sccb_handle = NULL;
        s_sccb_handle = NULL;
    }
    if (i2c_bus_handle) {
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
        s_i2c_bus = NULL;
    }
    return NULL;
}

int camera_get_fd(void)
{
    return s_video_fd;
}

esp_err_t camera_get_frame_info(uint32_t *width, uint32_t *height, uint32_t *format)
{
    if (!s_cam_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_cam_sensor_format_t current_format = {0};
    esp_err_t ret = esp_cam_sensor_get_format(s_cam_dev, &current_format);
    if (ret == ESP_OK) {
        if (width) *width = current_format.width;
        if (height) *height = current_format.height;
        if (format) *format = current_format.format;
    }

    return ret;
}

void camera_deinit(void)
{
    if (s_video_fd >= 0) {
        close(s_video_fd);
        s_video_fd = -1;
    }

    if (s_cam_dev) {
        esp_cam_sensor_del_dev(s_cam_dev);
        s_cam_dev = NULL;
    }

    if (s_sccb_handle) {
        esp_sccb_del_i2c_io(s_sccb_handle);
        s_sccb_handle = NULL;
    }

    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }

    if (s_video_initialized) {
        esp_video_deinit();
        s_video_initialized = false;
    }

    ESP_LOGI(TAG, "Camera deinitialized");
}
