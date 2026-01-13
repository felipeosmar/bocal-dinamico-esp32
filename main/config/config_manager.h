#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize config manager and SPIFFS
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_init(void);

/**
 * @brief Deinitialize config manager
 */
void config_deinit(void);

/**
 * @brief Load configuration from file
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_load(void);

/**
 * @brief Save configuration to file
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_save(void);

/**
 * @brief Reset configuration to defaults
 */
void config_reset_defaults(void);

// ============================================================================
// WiFi Configuration
// ============================================================================

const char* config_get_wifi_ssid(void);
const char* config_get_wifi_password(void);
bool config_get_wifi_ap_mode(void);
const char* config_get_ap_ssid(void);
const char* config_get_ap_password(void);

void config_set_wifi_ssid(const char *ssid);
void config_set_wifi_password(const char *password);
void config_set_wifi_ap_mode(bool ap_mode);
void config_set_ap_ssid(const char *ssid);
void config_set_ap_password(const char *password);

// ============================================================================
// RS485 Configuration
// ============================================================================

uint32_t config_get_rs485_baud(void);
uint8_t config_get_rs485_tx_pin(void);
uint8_t config_get_rs485_rx_pin(void);
uint8_t config_get_rs485_de_pin(void);

void config_set_rs485_baud(uint32_t baud);
void config_set_rs485_tx_pin(uint8_t pin);
void config_set_rs485_rx_pin(uint8_t pin);
void config_set_rs485_de_pin(uint8_t pin);

// ============================================================================
// Modbus Configuration
// ============================================================================

uint8_t config_get_modbus_slave_id(void);
uint32_t config_get_modbus_timeout(void);

void config_set_modbus_slave_id(uint8_t id);
void config_set_modbus_timeout(uint32_t timeout_ms);

// ============================================================================
// Actuator Configuration
// ============================================================================

#define MAX_SAVED_ACTUATORS 10

uint8_t config_get_scan_max_id(void);
void config_set_scan_max_id(uint8_t max_id);

// Saved actuator ID persistence
uint8_t config_get_saved_actuator_count(void);
const uint8_t* config_get_saved_actuator_ids(void);
bool config_add_saved_actuator_id(uint8_t id);
bool config_remove_saved_actuator_id(uint8_t id);
void config_clear_saved_actuators(void);

// ============================================================================
// Web Server Configuration
// ============================================================================

const char* config_get_web_username(void);
const char* config_get_web_password(void);
bool config_get_web_auth_enabled(void);

void config_set_web_username(const char *username);
void config_set_web_password(const char *password);
void config_set_web_auth_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
