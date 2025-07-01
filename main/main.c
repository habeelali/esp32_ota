#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#define WIFI_SSID      "155"
#define WIFI_PASS      "359F0E78"
#define OTA_URL        "http://192.168.100.170:8001/firmware.bin"
#define FIRMWARE_VERSION "v1.0.0"

static const char *TAG = "OTA_VERSION_EXAMPLE";

void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to WiFi...");
    esp_wifi_connect();
}

// Helper to print current firmware version
void print_running_version(void) {
    const esp_app_desc_t *app_desc = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running firmware version: %s", app_desc->version);
}

void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA task...");
    print_running_version();

    esp_http_client_config_t config = {
        .url = OTA_URL,
        .timeout_ms = 5000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed");
        vTaskDelete(NULL);
        return;
    }

    // Get new firmware's version
    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(https_ota_handle, &new_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

        const esp_app_desc_t *running_app_info = esp_ota_get_app_description();
        if (strcmp(new_app_info.version, running_app_info->version) == 0) {
            ESP_LOGI(TAG, "Firmware is already up to date. No update needed.");
            esp_https_ota_abort(https_ota_handle);
            vTaskDelete(NULL);
            return;
        }
    } else {
        ESP_LOGE(TAG, "Failed to get new firmware description");
        esp_https_ota_abort(https_ota_handle);
        vTaskDelete(NULL);
        return;
    }

    // Proceed with OTA update if version is different
    err = esp_https_ota_perform(https_ota_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful, restarting...");
        esp_https_ota_finish(https_ota_handle);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed");
        esp_https_ota_abort(https_ota_handle);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    wifi_init_sta();
    vTaskDelay(5000 / portTICK_PERIOD_MS); // Wait for WiFi

    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
}
