#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi manager configuration
 */
typedef struct {
    char sta_ssid[32];          // Station SSID
    char sta_password[64];      // Station password
    char ap_ssid[32];           // AP SSID
    char ap_password[64];       // AP password
    bool ap_mode_enabled;       // Start in AP mode
    uint32_t sta_timeout_ms;    // Station connection timeout
} wifi_manager_config_t;

/**
 * @brief WiFi connection status
 */
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_AP_MODE,
    WIFI_STATUS_ERROR
} wifi_status_t;

/**
 * @brief WiFi scan result
 */
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_scan_result_t;

/**
 * @brief Initialize WiFi manager
 * @param config Configuration (can be NULL for defaults)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_init(const wifi_manager_config_t *config);

/**
 * @brief Deinitialize WiFi manager
 */
void wifi_manager_deinit(void);

/**
 * @brief Start WiFi in station mode
 * @param ssid Network SSID
 * @param password Network password
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Disconnect from WiFi
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Start WiFi in AP mode
 * @param ssid AP SSID
 * @param password AP password (min 8 chars, or empty for open)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_start_ap(const char *ssid, const char *password);

/**
 * @brief Stop AP mode
 */
esp_err_t wifi_manager_stop_ap(void);

/**
 * @brief Get current WiFi status
 * @return wifi_status_t Current status
 */
wifi_status_t wifi_manager_get_status(void);

/**
 * @brief Check if connected to WiFi (STA mode)
 * @return true if connected
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get current IP address
 * @param ip_str Buffer to store IP string (min 16 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_get_ip(char *ip_str);

/**
 * @brief Get current SSID
 * @param ssid_str Buffer to store SSID (min 33 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_get_ssid(char *ssid_str);

/**
 * @brief Get RSSI
 * @return RSSI value or 0 if not connected
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief Scan for WiFi networks
 * @param results Array to store results
 * @param max_results Maximum number of results
 * @param found Pointer to store number of networks found
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_scan(wifi_scan_result_t *results, uint16_t max_results, uint16_t *found);

/**
 * @brief Get default configuration
 * @param config Pointer to store config
 */
void wifi_manager_get_default_config(wifi_manager_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
