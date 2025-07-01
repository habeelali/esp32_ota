#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include <string.h>
#include <stdlib.h>

#define WIFI_SSID   "155"
#define WIFI_PASS   "359F0E78"

#define VERSION_URL "https://github.com/habeelali/esp32_ota/releases/latest/download/version.txt"
#define OTA_URL     "https://github.com/habeelali/esp32_ota/releases/latest/download/project-name.bin"
#define CURRENT_VERSION "v2.2.1"
#define POLL_INTERVAL_MS (24 * 60 * 60 * 1000)   /* 24 h */

static const char *TAG = "ESP32_OTA";

/* Structure to hold HTTP response data */
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} http_response_t;

/* =================================================================== */
/*  HTTP Event Handler (captures response body)                        */
/* =================================================================== */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Allocate or expand buffer as needed
            if (!response->data) {
                response->capacity = evt->data_len + 1;
                response->data = malloc(response->capacity);
                if (!response->data) return ESP_FAIL;
                response->len = 0;
            } else if (response->len + evt->data_len >= response->capacity) {
                response->capacity = response->len + evt->data_len + 1;
                char *new_data = realloc(response->data, response->capacity);
                if (!new_data) {
                    free(response->data);
                    return ESP_FAIL;
                }
                response->data = new_data;
            }
            
            // Append new data
            memcpy(response->data + response->len, evt->data, evt->data_len);
            response->len += evt->data_len;
            response->data[response->len] = '\0';
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/* =================================================================== */
/*  Wi-Fi STA                                                          */
/* =================================================================== */
static void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wcfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to Wi-Fi …");
    esp_wifi_connect();
}

/* =================================================================== */
/*  Fetch version.txt (uses event handler)                             */
/* =================================================================== */
static esp_err_t fetch_latest_version(char *ver_buf, size_t buf_sz)
{
    http_response_t response = {0};
    
    esp_http_client_config_t cfg = {
        .url = VERSION_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &response,
        .buffer_size = 8192,
        .buffer_size_tx = 8192,
        .timeout_ms = 20000,
        .disable_auto_redirect = false
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            if (response.data && response.len > 0) {
                // Copy and clean version string
                size_t copy_len = (response.len < buf_sz) ? response.len : buf_sz - 1;
                strncpy(ver_buf, response.data, copy_len);
                ver_buf[copy_len] = '\0';
                
                // Remove trailing newline if exists
                char *newline = strchr(ver_buf, '\n');
                if (newline) *newline = '\0';
                
                ESP_LOGI(TAG, "Fetched version: \"%s\"", ver_buf);
            } else {
                ESP_LOGE(TAG, "Empty response body");
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "HTTP status: %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    if (response.data) free(response.data);
    esp_http_client_cleanup(client);
    return err;
}

/* =================================================================== */
/*  Semantic Version Comparison                                        */
/* =================================================================== */
static bool version_is_newer(const char *remote, const char *local)
{
    int rM, rm, rP, lM, lm, lP;
    if (sscanf(remote, "v%d.%d.%d", &rM, &rm, &rP) != 3) return false;
    if (sscanf(local, "v%d.%d.%d", &lM, &lm, &lP) != 3) return false;

    if (rM != lM) return rM > lM;
    if (rm != lm) return rm > lm;
    return rP > lP;
}

/* =================================================================== */
/*  OTA Polling Task                                                   */
/* =================================================================== */
static void ota_poll_task(void *arg)
{
    while (1) {
        ESP_LOGI(TAG, "–––– OTA check –––– (running %s)", CURRENT_VERSION);
        
        char latest[32] = {0};
        if (fetch_latest_version(latest, sizeof(latest)) != ESP_OK) {
            ESP_LOGE(TAG, "Version fetch failed – retry in 24 h");
        }
        else if (!version_is_newer(latest, CURRENT_VERSION)) {
            ESP_LOGI(TAG, "Remote %s not newer – skip", latest);
        }
        else {
            ESP_LOGI(TAG, "Remote %s newer → OTA", latest);
            
            esp_http_client_config_t hcfg = {
                .url = OTA_URL,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 8192,
                .buffer_size_tx = 8192,
                .timeout_ms = 30000,
                .disable_auto_redirect = false
            };
            esp_https_ota_config_t ocfg = { .http_config = &hcfg };
            
            esp_https_ota_handle_t h;
            if (esp_https_ota_begin(&ocfg, &h) == ESP_OK) {
                esp_err_t err = esp_https_ota_perform(h);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "OTA succeeded – rebooting");
                    esp_https_ota_finish(h);
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "OTA error: %s", esp_err_to_name(err));
                    esp_https_ota_abort(h);
                }
            } else {
                ESP_LOGE(TAG, "OTA begin failed");
            }
        }
        
        ESP_LOGI(TAG, "Next check in 24 h");
        vTaskDelay(POLL_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

/* =================================================================== */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    
    wifi_init_sta();
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    xTaskCreate(ota_poll_task, "ota_poll_task", 8192, NULL, 5, NULL);
}
