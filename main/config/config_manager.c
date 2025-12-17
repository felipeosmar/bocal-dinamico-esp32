#include "config_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"

static const char *TAG = "CONFIG";

#define CONFIG_FILE "/userdata/config.json"

// Configuration structure
typedef struct {
    // WiFi
    char wifi_ssid[32];
    char wifi_password[64];
    bool wifi_ap_mode;
    char ap_ssid[32];
    char ap_password[64];

    // RS485
    uint32_t rs485_baud;
    uint8_t rs485_tx_pin;
    uint8_t rs485_rx_pin;
    uint8_t rs485_de_pin;

    // Modbus
    uint8_t modbus_slave_id;
    uint32_t modbus_timeout;

    // Web
    char web_username[32];
    char web_password[64];
    bool web_auth_enabled;
} config_t;

static config_t s_config;
static bool s_initialized = false;

// ============================================================================
// LittleFS Setup
// ============================================================================

static esp_err_t init_littlefs(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS (userdata partition)");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/userdata",
        .partition_label = "userdata",
        .format_if_mount_failed = true,
        .dont_mount = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition 'userdata'");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info("userdata", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS userdata: total=%d, used=%d", total, used);
    }

    return ESP_OK;
}

// ============================================================================
// Default Configuration
// ============================================================================

void config_reset_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));

    // WiFi defaults
    strcpy(s_config.wifi_ssid, "");
    strcpy(s_config.wifi_password, "");
    s_config.wifi_ap_mode = true;  // Start in AP mode by default
    strcpy(s_config.ap_ssid, "ESP32-Master");
    strcpy(s_config.ap_password, "12345678");

    // RS485 defaults (safe pins)
    s_config.rs485_baud = 19200;
    s_config.rs485_tx_pin = 17;
    s_config.rs485_rx_pin = 5;
    s_config.rs485_de_pin = 18;

    // Modbus defaults
    s_config.modbus_slave_id = 2;      // Remote ESP32 Slave ID
    s_config.modbus_timeout = 500;

    // Web defaults
    strcpy(s_config.web_username, "admin");
    strcpy(s_config.web_password, "admin");
    s_config.web_auth_enabled = true;

    ESP_LOGI(TAG, "Configuration reset to defaults");
}

// ============================================================================
// Load/Save
// ============================================================================

esp_err_t config_load(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "Config file not found, using defaults");
        config_reset_defaults();
        return config_save();
    }

    // Read file
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json_str = malloc(fsize + 1);
    if (json_str == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    fread(json_str, 1, fsize, f);
    json_str[fsize] = '\0';
    fclose(f);

    // Parse JSON
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse config file");
        config_reset_defaults();
        return ESP_FAIL;
    }

    // WiFi section
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(wifi, "ssid")) && cJSON_IsString(item)) {
            strncpy(s_config.wifi_ssid, item->valuestring, sizeof(s_config.wifi_ssid) - 1);
        }
        if ((item = cJSON_GetObjectItem(wifi, "password")) && cJSON_IsString(item)) {
            strncpy(s_config.wifi_password, item->valuestring, sizeof(s_config.wifi_password) - 1);
        }
        if ((item = cJSON_GetObjectItem(wifi, "ap_mode")) && cJSON_IsBool(item)) {
            s_config.wifi_ap_mode = cJSON_IsTrue(item);
        }
        if ((item = cJSON_GetObjectItem(wifi, "ap_ssid")) && cJSON_IsString(item)) {
            strncpy(s_config.ap_ssid, item->valuestring, sizeof(s_config.ap_ssid) - 1);
        }
        if ((item = cJSON_GetObjectItem(wifi, "ap_password")) && cJSON_IsString(item)) {
            strncpy(s_config.ap_password, item->valuestring, sizeof(s_config.ap_password) - 1);
        }
    }

    // RS485 section
    cJSON *rs485 = cJSON_GetObjectItem(root, "rs485");
    if (rs485) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(rs485, "baud")) && cJSON_IsNumber(item)) {
            s_config.rs485_baud = item->valueint;
        }
        if ((item = cJSON_GetObjectItem(rs485, "tx_pin")) && cJSON_IsNumber(item)) {
            s_config.rs485_tx_pin = item->valueint;
        }
        if ((item = cJSON_GetObjectItem(rs485, "rx_pin")) && cJSON_IsNumber(item)) {
            s_config.rs485_rx_pin = item->valueint;
        }
        if ((item = cJSON_GetObjectItem(rs485, "de_pin")) && cJSON_IsNumber(item)) {
            s_config.rs485_de_pin = item->valueint;
        }
    }

    // Modbus section
    cJSON *modbus = cJSON_GetObjectItem(root, "modbus");
    if (modbus) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(modbus, "slave_id")) && cJSON_IsNumber(item)) {
            s_config.modbus_slave_id = item->valueint;
        }
        if ((item = cJSON_GetObjectItem(modbus, "timeout")) && cJSON_IsNumber(item)) {
            s_config.modbus_timeout = item->valueint;
        }
    }

    // Web section
    cJSON *web = cJSON_GetObjectItem(root, "web");
    if (web) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(web, "username")) && cJSON_IsString(item)) {
            strncpy(s_config.web_username, item->valuestring, sizeof(s_config.web_username) - 1);
        }
        if ((item = cJSON_GetObjectItem(web, "password")) && cJSON_IsString(item)) {
            strncpy(s_config.web_password, item->valuestring, sizeof(s_config.web_password) - 1);
        }
        if ((item = cJSON_GetObjectItem(web, "auth_enabled")) && cJSON_IsBool(item)) {
            s_config.web_auth_enabled = cJSON_IsTrue(item);
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Configuration loaded");
    return ESP_OK;
}

esp_err_t config_save(void)
{
    cJSON *root = cJSON_CreateObject();

    // WiFi section
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", s_config.wifi_ssid);
    cJSON_AddStringToObject(wifi, "password", s_config.wifi_password);
    cJSON_AddBoolToObject(wifi, "ap_mode", s_config.wifi_ap_mode);
    cJSON_AddStringToObject(wifi, "ap_ssid", s_config.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_password", s_config.ap_password);
    cJSON_AddItemToObject(root, "wifi", wifi);

    // RS485 section
    cJSON *rs485 = cJSON_CreateObject();
    cJSON_AddNumberToObject(rs485, "baud", s_config.rs485_baud);
    cJSON_AddNumberToObject(rs485, "tx_pin", s_config.rs485_tx_pin);
    cJSON_AddNumberToObject(rs485, "rx_pin", s_config.rs485_rx_pin);
    cJSON_AddNumberToObject(rs485, "de_pin", s_config.rs485_de_pin);
    cJSON_AddItemToObject(root, "rs485", rs485);

    // Modbus section
    cJSON *modbus = cJSON_CreateObject();
    cJSON_AddNumberToObject(modbus, "slave_id", s_config.modbus_slave_id);
    cJSON_AddNumberToObject(modbus, "timeout", s_config.modbus_timeout);
    cJSON_AddItemToObject(root, "modbus", modbus);

    // Web section
    cJSON *web = cJSON_CreateObject();
    cJSON_AddStringToObject(web, "username", s_config.web_username);
    cJSON_AddStringToObject(web, "password", s_config.web_password);
    cJSON_AddBoolToObject(web, "auth_enabled", s_config.web_auth_enabled);
    cJSON_AddItemToObject(root, "web", web);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(CONFIG_FILE, "w");
    if (f == NULL) {
        free(json_str);
        ESP_LOGE(TAG, "Failed to open config file for writing");
        return ESP_FAIL;
    }

    fprintf(f, "%s", json_str);
    fclose(f);
    free(json_str);

    ESP_LOGI(TAG, "Configuration saved");
    return ESP_OK;
}

// ============================================================================
// Init/Deinit
// ============================================================================

esp_err_t config_init(void)
{
    if (s_initialized) return ESP_OK;

    config_reset_defaults();

    esp_err_t ret = init_littlefs();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = config_load();
    s_initialized = true;
    return ret;
}

void config_deinit(void)
{
    esp_vfs_littlefs_unregister("userdata");
    s_initialized = false;
}

// ============================================================================
// Getters - WiFi
// ============================================================================

const char* config_get_wifi_ssid(void) { return s_config.wifi_ssid; }
const char* config_get_wifi_password(void) { return s_config.wifi_password; }
bool config_get_wifi_ap_mode(void) { return s_config.wifi_ap_mode; }
const char* config_get_ap_ssid(void) { return s_config.ap_ssid; }
const char* config_get_ap_password(void) { return s_config.ap_password; }

// ============================================================================
// Setters - WiFi
// ============================================================================

void config_set_wifi_ssid(const char *ssid) {
    if (ssid) strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
}

void config_set_wifi_password(const char *password) {
    if (password) strncpy(s_config.wifi_password, password, sizeof(s_config.wifi_password) - 1);
}

void config_set_wifi_ap_mode(bool ap_mode) {
    s_config.wifi_ap_mode = ap_mode;
}

void config_set_ap_ssid(const char *ssid) {
    if (ssid) strncpy(s_config.ap_ssid, ssid, sizeof(s_config.ap_ssid) - 1);
}

void config_set_ap_password(const char *password) {
    if (password) strncpy(s_config.ap_password, password, sizeof(s_config.ap_password) - 1);
}

// ============================================================================
// Getters - RS485
// ============================================================================

uint32_t config_get_rs485_baud(void) { return s_config.rs485_baud; }
uint8_t config_get_rs485_tx_pin(void) { return s_config.rs485_tx_pin; }
uint8_t config_get_rs485_rx_pin(void) { return s_config.rs485_rx_pin; }
uint8_t config_get_rs485_de_pin(void) { return s_config.rs485_de_pin; }

// ============================================================================
// Setters - RS485
// ============================================================================

void config_set_rs485_baud(uint32_t baud) { s_config.rs485_baud = baud; }
void config_set_rs485_tx_pin(uint8_t pin) { s_config.rs485_tx_pin = pin; }
void config_set_rs485_rx_pin(uint8_t pin) { s_config.rs485_rx_pin = pin; }
void config_set_rs485_de_pin(uint8_t pin) { s_config.rs485_de_pin = pin; }

// ============================================================================
// Getters - Modbus
// ============================================================================

uint8_t config_get_modbus_slave_id(void) { return s_config.modbus_slave_id; }
uint32_t config_get_modbus_timeout(void) { return s_config.modbus_timeout; }

// ============================================================================
// Setters - Modbus
// ============================================================================

void config_set_modbus_slave_id(uint8_t id) { s_config.modbus_slave_id = id; }
void config_set_modbus_timeout(uint32_t timeout_ms) { s_config.modbus_timeout = timeout_ms; }

// ============================================================================
// Getters - Web
// ============================================================================

const char* config_get_web_username(void) { return s_config.web_username; }
const char* config_get_web_password(void) { return s_config.web_password; }
bool config_get_web_auth_enabled(void) { return s_config.web_auth_enabled; }

// ============================================================================
// Setters - Web
// ============================================================================

void config_set_web_username(const char *username) {
    if (username) strncpy(s_config.web_username, username, sizeof(s_config.web_username) - 1);
}

void config_set_web_password(const char *password) {
    if (password) strncpy(s_config.web_password, password, sizeof(s_config.web_password) - 1);
}

void config_set_web_auth_enabled(bool enabled) {
    s_config.web_auth_enabled = enabled;
}
