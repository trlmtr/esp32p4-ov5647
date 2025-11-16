/*
 * ESP32-P4 OV5647 Camera Application
 * Main application for camera capture with web server
 */
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "esp_hosted.h"
#include "camera_init.h"
#include "camera_server.h"

static const char *TAG = "app_main";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_hosted_ready = false;

// Camera configuration for ESP32-P4 with OV5647
#define CAMERA_I2C_PORT     CONFIG_CAMERA_I2C_PORT
#define CAMERA_SCL_PIN      CONFIG_CAMERA_SCL_PIN
#define CAMERA_SDA_PIN      CONFIG_CAMERA_SDA_PIN
#define CAMERA_RESET_PIN    -1  // Not used
#define CAMERA_PWDN_PIN     -1  // Not used
#define CAMERA_XCLK_FREQ    24000000

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t ensure_hosted_transport(void)
{
    if (s_hosted_ready) {
        return ESP_OK;
    }

    int ret = esp_hosted_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP-Hosted stack (%d)", ret);
        return ret;
    }

    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to ESP-Hosted co-processor (%d)", ret);
        return ret;
    }

    s_hosted_ready = true;
    ESP_LOGI(TAG, "ESP-Hosted transport ready");
    return ESP_OK;
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", CONFIG_ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", CONFIG_ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

static void initialize_mdns(void)
{
    ESP_LOGI(TAG, "Initializing mDNS");
    
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS Init failed: %d", err);
        return;
    }

    mdns_hostname_set(CONFIG_MDNS_HOSTNAME);
    mdns_instance_name_set(CONFIG_MDNS_INSTANCE);
    
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    
    ESP_LOGI(TAG, "mDNS initialized, hostname: %s", CONFIG_MDNS_HOSTNAME);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-P4 OV5647 Camera Application");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi station mode
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", CONFIG_ESP_WIFI_SSID);
    ESP_ERROR_CHECK(ensure_hosted_transport());
    wifi_init_sta();

    // Initialize mDNS after WiFi connection
    initialize_mdns();

    // Initialize camera
    ESP_LOGI(TAG, "Initializing camera...");
    camera_config_t cam_config = {
        .i2c_port = CAMERA_I2C_PORT,
        .scl_pin = CAMERA_SCL_PIN,
        .sda_pin = CAMERA_SDA_PIN,
        .reset_pin = CAMERA_RESET_PIN,
        .pwdn_pin = CAMERA_PWDN_PIN,
        .xclk_freq_hz = CAMERA_XCLK_FREQ,
    };

    esp_cam_sensor_device_t *cam_dev = camera_init(&cam_config);
    if (!cam_dev) {
        ESP_LOGE(TAG, "Failed to initialize camera");
        return;
    }

    int camera_fd = camera_get_fd();
    if (camera_fd < 0) {
        ESP_LOGE(TAG, "Failed to get camera file descriptor");
        camera_deinit();
        return;
    }

    // Start web server
    ESP_LOGI(TAG, "Starting camera web server...");
    ret = camera_server_start(camera_fd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start camera web server");
        camera_deinit();
        return;
    }

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Camera web server is running!");
    ESP_LOGI(TAG, "Open your browser and navigate to:");
    ESP_LOGI(TAG, "  http://%s.local", CONFIG_MDNS_HOSTNAME);
    ESP_LOGI(TAG, "  or use the IP address shown above");
    ESP_LOGI(TAG, "===========================================");

    // Keep the application running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
