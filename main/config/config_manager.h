#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration Manager
 *
 * Threading model:
 * - Getters return pointers to internal static data (s_config struct).
 *   These pointers remain valid for the lifetime of the application but
 *   the data they point to may change if a setter or config_load() is called.
 * - Setters and config_save() must only be called from the HTTP server task
 *   (single-writer model). Since all writes originate from HTTP API handlers
 *   and ESP-IDF's HTTP server dispatches handlers on a single task, this is
 *   safe in practice.
 * - config_init() and config_load() during startup run before the HTTP server
 *   starts, so there are no concurrent readers at that point.
 * - Do NOT call setters or config_save() from other tasks (e.g., timer tasks,
 *   health monitor) without adding proper synchronization.
 */

/**
 * @brief Initialize config manager and LittleFS
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

// Saved actuator name persistence
const char* config_get_saved_actuator_name(uint8_t index);
bool config_set_saved_actuator_name(uint8_t index, const char* name);

// Actuator name by ID (finds ID in saved_actuator_ids and returns/sets corresponding name)
const char* config_get_actuator_name(uint8_t id);
bool config_set_actuator_name(uint8_t id, const char *name);

// ============================================================================
// Role Mapping Configuration
// ============================================================================

typedef struct {
    uint8_t id;
    uint32_t baud;
} config_role_t;

config_role_t config_get_role_lens_a(void);
config_role_t config_get_role_lens_b(void);
config_role_t config_get_role_nozzle(void);

void config_set_role_lens_a(uint8_t id, uint32_t baud);
void config_set_role_lens_b(uint8_t id, uint32_t baud);
void config_set_role_nozzle(uint8_t id, uint32_t baud);

uint8_t config_get_role_lens_a_id(void);
uint8_t config_get_role_lens_b_id(void);
uint8_t config_get_role_nozzle_id(void);

// ============================================================================
// Web Server Configuration
// ============================================================================

const char* config_get_web_username(void);
const char* config_get_web_password(void);
bool config_get_web_auth_enabled(void);

void config_set_web_username(const char *username);
void config_set_web_password(const char *password);
void config_set_web_auth_enabled(bool enabled);

// ============================================================================
// Baumer OX100 Configuration
// ============================================================================

uint8_t  config_get_baumer_slave_id(void);
bool     config_get_baumer_enabled(void);
void     config_set_baumer_slave_id(uint8_t id);
void     config_set_baumer_enabled(bool enabled);

// ============================================================================
// Control Loop Configuration
// ============================================================================

bool     config_get_control_running(void);
uint32_t config_get_control_interval(void);
uint8_t  config_get_control_measurement_index(void);
void     config_set_control_running(bool running);
void     config_set_control_interval(uint32_t ms);
void     config_set_control_measurement_index(uint8_t idx);

// Forward declare to avoid circular include
typedef struct {
    uint8_t actuator_id;
    float coeff_a;
    float coeff_b;
    bool enabled;
} config_control_equation_t;

#define CONFIG_MAX_EQUATIONS 10

int  config_get_control_equations(config_control_equation_t *out, int max_count);
void config_set_control_equations(const config_control_equation_t *eqs, int count);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
