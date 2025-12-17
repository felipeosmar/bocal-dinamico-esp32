#include "wifi_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_MGR";

// Event group bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// Maximum retry attempts
#define MAX_RETRY_COUNT     5

// Static variables
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static int s_retry_count = 0;
static bool s_initialized = false;
static wifi_manager_config_t s_config;

// Event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Station started, connecting...");
                s_status = WIFI_STATUS_CONNECTING;
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_retry_count < MAX_RETRY_COUNT) {
                    esp_wifi_connect();
                    s_retry_count++;
                    ESP_LOGW(TAG, "Retry %d/%d", s_retry_count, MAX_RETRY_COUNT);
                } else {
                    ESP_LOGE(TAG, "Failed to connect after %d attempts", MAX_RETRY_COUNT);
                    s_status = WIFI_STATUS_DISCONNECTED;
                    if (s_wifi_event_group) {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                break;

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                s_status = WIFI_STATUS_AP_MODE;
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station connected: %02x:%02x:%02x:%02x:%02x:%02x",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5]);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5]);
                break;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_status = WIFI_STATUS_CONNECTED;
            s_retry_count = 0;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
    }
}

void wifi_manager_get_default_config(wifi_manager_config_t *config)
{
    if (config == NULL) return;

    memset(config, 0, sizeof(wifi_manager_config_t));
    strcpy(config->ap_ssid, "ESP32-Master");
    strcpy(config->ap_password, "12345678");
    config->ap_mode_enabled = false;
    config->sta_timeout_ms = 15000;
}

esp_err_t wifi_manager_init(const wifi_manager_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Use default config if none provided
    if (config) {
        memcpy(&s_config, config, sizeof(wifi_manager_config_t));
    } else {
        wifi_manager_get_default_config(&s_config);
    }

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default network interfaces
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Set WiFi storage to RAM (don't save to NVS automatically)
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");

    return ESP_OK;
}

void wifi_manager_deinit(void)
{
    if (!s_initialized) return;

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_initialized = false;
    s_status = WIFI_STATUS_DISCONNECTED;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to: %s", ssid);

    // Stop any existing connection
    esp_wifi_stop();

    // Configure station mode
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = password && strlen(password) > 0 ?
                                         WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Clear event bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;
    s_status = WIFI_STATUS_CONNECTING;

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(s_config.sta_timeout_ms)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        // Save successful connection
        strncpy(s_config.sta_ssid, ssid, sizeof(s_config.sta_ssid) - 1);
        if (password) {
            strncpy(s_config.sta_password, password, sizeof(s_config.sta_password) - 1);
        }
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Connection failed");
        s_status = WIFI_STATUS_DISCONNECTED;
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_wifi_disconnect();
    s_status = WIFI_STATUS_DISCONNECTED;
    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const char *ap_ssid = ssid ? ssid : s_config.ap_ssid;
    const char *ap_pass = password ? password : s_config.ap_password;

    ESP_LOGI(TAG, "Starting AP: %s", ap_ssid);

    // Stop any existing connection
    esp_wifi_stop();

    // Configure AP mode
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;

    if (ap_pass && strlen(ap_pass) >= 8) {
        strncpy((char *)wifi_config.ap.password, ap_pass, sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_status = WIFI_STATUS_AP_MODE;

    // Get AP IP
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));

    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_wifi_stop();
    s_status = WIFI_STATUS_DISCONNECTED;
    return ESP_OK;
}

wifi_status_t wifi_manager_get_status(void)
{
    return s_status;
}

bool wifi_manager_is_connected(void)
{
    return s_status == WIFI_STATUS_CONNECTED;
}

esp_err_t wifi_manager_get_ip(char *ip_str)
{
    if (!s_initialized || ip_str == NULL) return ESP_ERR_INVALID_ARG;

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = (s_status == WIFI_STATUS_AP_MODE) ? s_ap_netif : s_sta_netif;

    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret == ESP_OK) {
        sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
    }
    return ret;
}

esp_err_t wifi_manager_get_ssid(char *ssid_str)
{
    if (!s_initialized || ssid_str == NULL) return ESP_ERR_INVALID_ARG;

    wifi_ap_record_t ap_info;
    if (s_status == WIFI_STATUS_CONNECTED && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strcpy(ssid_str, (char *)ap_info.ssid);
        return ESP_OK;
    } else if (s_status == WIFI_STATUS_AP_MODE) {
        strcpy(ssid_str, s_config.ap_ssid);
        return ESP_OK;
    }
    ssid_str[0] = '\0';
    return ESP_FAIL;
}

int8_t wifi_manager_get_rssi(void)
{
    if (s_status != WIFI_STATUS_CONNECTED) return 0;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

esp_err_t wifi_manager_scan(wifi_scan_result_t *results, uint16_t max_results, uint16_t *found)
{
    if (!s_initialized || results == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting WiFi scan...");

    // If in AP mode, need to switch temporarily
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else if (current_mode == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }

    // Configure scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Get scan results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        *found = 0;
        return ESP_OK;
    }

    uint16_t to_get = (ap_count < max_results) ? ap_count : max_results;
    wifi_ap_record_t *ap_records = malloc(to_get * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&to_get, ap_records);
    if (ret == ESP_OK) {
        for (uint16_t i = 0; i < to_get; i++) {
            strncpy(results[i].ssid, (char *)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
            results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
            results[i].rssi = ap_records[i].rssi;
            results[i].authmode = ap_records[i].authmode;
        }
        *found = to_get;
        ESP_LOGI(TAG, "Found %d networks", to_get);
    }

    free(ap_records);

    // Restore mode if needed
    if (current_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    return ret;
}
