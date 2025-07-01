#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include <string.h>

#define WIFI_SSID      "phone"
#define WIFI_PASS      "pakistan1337"

// URL to your version file (raw file in your repo)
#define VERSION_URL " https://github.com/habeelali/esp32_ota/releases/latest/download/version.txt"
// URL to your latest firmware release binary
#define OTA_URL        "https://github.com/habeelali/esp32_ota/releases/latest/download/firmware.bin"
// The version string for this build (update for each release)
#define CURRENT_VERSION "v1.0.0"

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

// Print the running firmware version
void print_running_version(void) {
    const esp_app_desc_t *app_desc = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running firmware version: %s", app_desc->version);
}

// Fetch latest version string from GitHub
esp_err_t fetch_latest_version(char *version_buf, size_t buf_len) {
    esp_http_client_config_t config = {
        .url = VERSION_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int content_length = esp_http_client_get_content_length(client);
            if (content_length > 0 && content_length < buf_len) {
                int read_len = esp_http_client_read(client, version_buf, content_length);
                if (read_len > 0) {
                    version_buf[read_len] = '\0'; // Null terminate
                    ESP_LOGI(TAG, "Fetched latest version: %s", version_buf);
                    esp_http_client_cleanup(client);
                    return ESP_OK;
                }
            }
        } else {
            ESP_LOGE(TAG, "HTTP status code: %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA task...");
    print_running_version();

    char latest_version[32] = {0};
    if (fetch_latest_version(latest_version, sizeof(latest_version)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch latest version");
        vTaskDelete(NULL);
        return;
    }

    if (strcmp(latest_version, CURRENT_VERSION) == 0) {
        ESP_LOGI(TAG, "Firmware is already up to date. No update needed.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "New firmware version available: %s. Starting OTA...", latest_version);

    esp_http_client_config_t config = {
        .url = OTA_URL,
        .timeout_ms = 10000,
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
