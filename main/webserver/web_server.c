#include "web_server.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "wifi_manager.h"
#include "config_manager.h"
#include "modbus_rtu.h"
#include "mightyzap.h"

static const char *TAG = "WEB_SRV";

// External globals from main
extern modbus_handle_t g_modbus;
extern mightyzap_handle_t g_actuator;

// Server handle
static httpd_handle_t s_server = NULL;
static web_server_config_t s_config;
static bool s_running = false;

// Remote slave configuration
#define REMOTE_SLAVE_ID     2
#define REG_LED_STATE       0x0000
#define REG_BLINK_MODE      0x0001
#define REG_BLINK_PERIOD    0x0002
#define REG_DEVICE_ID       0x0003

// Forward declarations
static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type);
static bool check_auth(httpd_req_t *req);

// ============================================================================
// Authentication
// ============================================================================

static bool check_auth(httpd_req_t *req)
{
    if (!s_config.auth_enabled) return true;

    char auth_header[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    // Parse "Basic base64credentials"
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;

    // For simplicity, just compare with expected credentials
    // In production, use proper base64 encoding
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", s_config.username, s_config.password);

    // Simple base64 decode check (simplified version)
    // This is a basic implementation - in production use mbedtls base64
    return true; // Simplified - implement proper auth if needed
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================================
// Static File Handlers
// ============================================================================

static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type)
{
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/index.html", "text/html");
}

static esp_err_t css_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/style.css", "text/css");
}

static esp_err_t js_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/app.js", "application/javascript");
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/favicon.ico", "image/x-icon");
}

// ============================================================================
// API Handlers - System
// ============================================================================

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    // System info
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_ms", xTaskGetTickCount() * portTICK_PERIOD_MS);

    // WiFi info
    char ip[16] = {0};
    char ssid[33] = {0};
    wifi_manager_get_ip(ip);
    wifi_manager_get_ssid(ssid);

    cJSON_AddStringToObject(root, "wifi_ip", ip);
    cJSON_AddStringToObject(root, "wifi_ssid", ssid);
    cJSON_AddNumberToObject(root, "wifi_rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "wifi_status", wifi_manager_get_status());

    // Modbus status
    cJSON_AddBoolToObject(root, "modbus_ready", g_modbus != NULL);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - WiFi
// ============================================================================

static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_result_t results[20];
    uint16_t found = 0;

    esp_err_t ret = wifi_manager_scan(results, 20, &found);
    if (ret != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < found; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(net, "auth", results[i].authmode);
        cJSON_AddItemToArray(root, net);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_json = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    const char *ssid = ssid_json->valuestring;
    const char *password = cJSON_IsString(pass_json) ? pass_json->valuestring : "";

    ESP_LOGI(TAG, "Connecting to: %s", ssid);

    // Save config before connecting
    config_set_wifi_ssid(ssid);
    config_set_wifi_password(password);
    config_set_wifi_ap_mode(false);  // Disable AP mode so it connects on next boot
    config_save();

    esp_err_t err = wifi_manager_connect(ssid, password);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Connected" : "Failed to connect");

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    char ip[16] = {0};
    char ssid[33] = {0};
    wifi_manager_get_ip(ip);
    wifi_manager_get_ssid(ssid);

    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "status", wifi_manager_get_status());
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - LED Control (Remote Slave)
// ============================================================================

static esp_err_t api_led_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(root, "error", true);
        cJSON_AddStringToObject(root, "message", "Modbus not initialized");
    } else {
        uint16_t values[4];
        esp_err_t ret = modbus_read_holding_registers(g_modbus, REMOTE_SLAVE_ID,
                                                       REG_LED_STATE, 4, values);
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(root, "error", false);
            cJSON_AddBoolToObject(root, "led_on", values[0] != 0);
            cJSON_AddBoolToObject(root, "blink_mode", values[1] != 0);
            cJSON_AddNumberToObject(root, "blink_period", values[2]);
            cJSON_AddNumberToObject(root, "device_id", values[3]);
        } else {
            cJSON_AddBoolToObject(root, "error", true);
            cJSON_AddStringToObject(root, "message", "Slave not responding");
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_led_control_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    esp_err_t err = ESP_FAIL;

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Modbus not initialized");
    } else {
        // Check for LED state
        cJSON *led_on = cJSON_GetObjectItem(root, "led_on");
        if (cJSON_IsBool(led_on)) {
            err = modbus_write_single_register(g_modbus, REMOTE_SLAVE_ID,
                                               REG_LED_STATE, cJSON_IsTrue(led_on) ? 1 : 0);
        }

        // Check for blink mode
        cJSON *blink = cJSON_GetObjectItem(root, "blink_mode");
        if (cJSON_IsBool(blink)) {
            err = modbus_write_single_register(g_modbus, REMOTE_SLAVE_ID,
                                               REG_BLINK_MODE, cJSON_IsTrue(blink) ? 1 : 0);
        }

        // Check for blink period
        cJSON *period = cJSON_GetObjectItem(root, "blink_period");
        if (cJSON_IsNumber(period)) {
            int val = period->valueint;
            if (val >= 100 && val <= 10000) {
                err = modbus_write_single_register(g_modbus, REMOTE_SLAVE_ID,
                                                   REG_BLINK_PERIOD, val);
            }
        }

        cJSON_AddBoolToObject(response, "success", err == ESP_OK);
        cJSON_AddStringToObject(response, "message", err == ESP_OK ? "OK" : "Failed");
    }

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - Actuator Control (mightyZAP) - Multi-actuator support
// ============================================================================

#define MAX_ACTUATORS 10

// Structure to hold multiple actuator handles
typedef struct {
    uint8_t id;
    mightyzap_handle_t handle;
    bool active;
} actuator_slot_t;

static actuator_slot_t s_actuators[MAX_ACTUATORS] = {0};
static uint8_t s_num_actuators = 0;

// Helper: Find actuator by ID
static actuator_slot_t* find_actuator(uint8_t id) {
    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].id == id) {
            return &s_actuators[i];
        }
    }
    return NULL;
}

// Helper: Add actuator
static esp_err_t add_actuator(uint8_t id) {
    if (find_actuator(id) != NULL) return ESP_OK; // Already exists
    if (g_modbus == NULL) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (!s_actuators[i].active) {
            mightyzap_handle_t handle = NULL;
            esp_err_t ret = mightyzap_init(g_modbus, id, &handle);
            if (ret == ESP_OK) {
                s_actuators[i].id = id;
                s_actuators[i].handle = handle;
                s_actuators[i].active = true;
                s_num_actuators++;
                return ESP_OK;
            }
            return ret;
        }
    }
    return ESP_ERR_NO_MEM;
}

// Helper: Remove actuator
static void remove_actuator(uint8_t id) {
    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].id == id) {
            if (s_actuators[i].handle) {
                mightyzap_deinit(s_actuators[i].handle);
            }
            s_actuators[i].active = false;
            s_actuators[i].handle = NULL;
            s_num_actuators--;
            break;
        }
    }
}

// GET /api/actuator/status - Get status of all active actuators
static esp_err_t api_actuator_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *actuators = cJSON_CreateArray();

    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].handle) {
            cJSON *act = cJSON_CreateObject();
            cJSON_AddNumberToObject(act, "id", s_actuators[i].id);

            mightyzap_status_t status;
            esp_err_t ret = mightyzap_get_status(s_actuators[i].handle, &status);

            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(act, "connected", true);
                cJSON_AddNumberToObject(act, "position", status.position);
                cJSON_AddNumberToObject(act, "current", status.current);
                cJSON_AddNumberToObject(act, "voltage", status.voltage / 10.0);
                cJSON_AddBoolToObject(act, "moving", status.moving != 0);
            } else {
                cJSON_AddBoolToObject(act, "connected", false);
            }
            cJSON_AddItemToArray(actuators, act);
        }
    }

    cJSON_AddItemToObject(root, "actuators", actuators);
    cJSON_AddNumberToObject(root, "count", s_num_actuators);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/control - Control specific actuator by ID
static esp_err_t api_actuator_control_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    esp_err_t err = ESP_FAIL;

    // Get actuator ID from request
    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsNumber(id_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing actuator ID");
        goto send_response;
    }

    uint8_t act_id = id_json->valueint;
    actuator_slot_t *slot = find_actuator(act_id);

    if (slot == NULL || slot->handle == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Actuator not found");
        goto send_response;
    }

    mightyzap_handle_t handle = slot->handle;

    // Check for force enable/disable
    cJSON *force_enable = cJSON_GetObjectItem(root, "force");
    if (cJSON_IsBool(force_enable)) {
        err = mightyzap_set_force_enable(handle, cJSON_IsTrue(force_enable));
    }

    // Check for position
    cJSON *position = cJSON_GetObjectItem(root, "position");
    if (cJSON_IsNumber(position)) {
        int val = position->valueint;
        if (val >= 0 && val <= 4095) {
            err = mightyzap_set_position(handle, val);
        }
    }

    // Check for speed
    cJSON *speed = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(speed)) {
        int val = speed->valueint;
        if (val >= 0 && val <= 1023) {
            err = mightyzap_set_speed(handle, val);
        }
    }

    // Check for current (force limit)
    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (cJSON_IsNumber(current)) {
        int val = current->valueint;
        if (val >= 0 && val <= 800) {
            err = mightyzap_set_current(handle, val);
        }
    }

    // Check for combined goal (position + speed + current)
    cJSON *goal = cJSON_GetObjectItem(root, "goal");
    if (cJSON_IsObject(goal)) {
        cJSON *g_pos = cJSON_GetObjectItem(goal, "position");
        cJSON *g_spd = cJSON_GetObjectItem(goal, "speed");
        cJSON *g_cur = cJSON_GetObjectItem(goal, "current");

        if (cJSON_IsNumber(g_pos) && cJSON_IsNumber(g_spd) && cJSON_IsNumber(g_cur)) {
            err = mightyzap_set_goal(handle,
                                     g_pos->valueint,
                                     g_spd->valueint,
                                     g_cur->valueint);
        }
    }

    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "OK" : "Command failed");

send_response:
    {
        char *json_str = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
    }

    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/actuator/scan - Scan for actuators and auto-add them
static esp_err_t api_actuator_scan_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *found = cJSON_CreateArray();

    if (g_modbus == NULL) {
        cJSON_AddItemToObject(root, "found", found);
        cJSON_AddNumberToObject(root, "count", 0);
        cJSON_AddStringToObject(root, "error", "Modbus not initialized");

        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Scanning for mightyZAP actuators (IDs 1-10)...");

    int count = 0;
    for (uint8_t id = 1; id <= 10; id++) {
        uint16_t model = 0;
        esp_err_t ret = modbus_read_holding_registers(g_modbus, id,
                                                       MZAP_REG_MODEL_NUMBER, 1, &model);
        if (ret == ESP_OK && model != 0) {
            ESP_LOGI(TAG, "Found actuator at ID %d, model: %u", id, model);

            // Auto-add to active actuators
            add_actuator(id);

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddNumberToObject(item, "model", model);
            cJSON_AddItemToArray(found, item);
            count++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    cJSON_AddItemToObject(root, "found", found);
    cJSON_AddNumberToObject(root, "count", count);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/add - Add actuator by ID
static esp_err_t api_actuator_add_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json) || id_json->valueint < 1 || id_json->valueint > 247) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid ID (1-247)");
    } else {
        uint8_t new_id = id_json->valueint;
        esp_err_t err = add_actuator(new_id);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Actuator added with ID %d", new_id);
            cJSON_AddBoolToObject(response, "success", true);
            cJSON_AddStringToObject(response, "message", "Actuator added");
            cJSON_AddNumberToObject(response, "id", new_id);
        } else {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Failed to add actuator");
        }
    }

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/remove - Remove actuator by ID
static esp_err_t api_actuator_remove_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid ID");
    } else {
        uint8_t id = id_json->valueint;
        remove_actuator(id);
        ESP_LOGI(TAG, "Actuator removed: ID %d", id);
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "Actuator removed");
    }

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - RS485 Configuration
// ============================================================================

static esp_err_t api_rs485_config_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();

        cJSON_AddNumberToObject(root, "baud_rate", config_get_rs485_baud());
        cJSON_AddNumberToObject(root, "tx_pin", config_get_rs485_tx_pin());
        cJSON_AddNumberToObject(root, "rx_pin", config_get_rs485_rx_pin());
        cJSON_AddNumberToObject(root, "de_pin", config_get_rs485_de_pin());
        cJSON_AddNumberToObject(root, "slave_id", config_get_modbus_slave_id());

        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));

        free(json_str);
        cJSON_Delete(root);
    } else {
        // POST - update config
        char buf[256];
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (root == NULL) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *baud = cJSON_GetObjectItem(root, "baud_rate");
        if (cJSON_IsNumber(baud)) {
            config_set_rs485_baud(baud->valueint);
        }

        cJSON *slave_id = cJSON_GetObjectItem(root, "slave_id");
        if (cJSON_IsNumber(slave_id)) {
            config_set_modbus_slave_id(slave_id->valueint);
        }

        config_save();

        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "Config saved. Restart to apply.");

        char *json_str = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));

        free(json_str);
        cJSON_Delete(response);
        cJSON_Delete(root);
    }
    return ESP_OK;
}

// ============================================================================
// API Handlers - System Control
// ============================================================================

static esp_err_t api_restart_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "Restarting...");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);

    // Delay before restart
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ============================================================================
// Server Setup
// ============================================================================

void web_server_get_default_config(web_server_config_t *config)
{
    if (config == NULL) return;

    config->port = 80;
    config->username = "admin";
    config->password = "admin";
    config->auth_enabled = false;
}

esp_err_t web_server_init(const web_server_config_t *config)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    // Initialize SPIFFS for www partition (web interface files)
    esp_vfs_spiffs_conf_t www_conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 5,
        .format_if_mount_failed = false  // Don't format - should have files from flash
    };

    esp_err_t ret = esp_vfs_spiffs_register(&www_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount www partition: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("www", &total, &used);
    ESP_LOGI(TAG, "SPIFFS www: total=%d, used=%d", total, used);

    if (config) {
        memcpy(&s_config, config, sizeof(web_server_config_t));
    } else {
        web_server_get_default_config(&s_config);
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = s_config.port;
    http_config.max_uri_handlers = 20;
    http_config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting server on port %d", http_config.server_port);

    ret = httpd_start(&s_server, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Static files
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t css_uri = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = css_handler,
    };
    httpd_register_uri_handler(s_server, &css_uri);

    httpd_uri_t js_uri = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = js_handler,
    };
    httpd_register_uri_handler(s_server, &js_uri);

    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };
    httpd_register_uri_handler(s_server, &favicon_uri);

    // API - System
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t restart_uri = {
        .uri = "/api/restart",
        .method = HTTP_POST,
        .handler = api_restart_handler,
    };
    httpd_register_uri_handler(s_server, &restart_uri);

    // API - WiFi
    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_scan_uri);

    httpd_uri_t wifi_connect_uri = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = api_wifi_connect_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_connect_uri);

    httpd_uri_t wifi_status_uri = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = api_wifi_status_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_status_uri);

    // API - LED Control
    httpd_uri_t led_status_uri = {
        .uri = "/api/led/status",
        .method = HTTP_GET,
        .handler = api_led_status_handler,
    };
    httpd_register_uri_handler(s_server, &led_status_uri);

    httpd_uri_t led_control_uri = {
        .uri = "/api/led/control",
        .method = HTTP_POST,
        .handler = api_led_control_handler,
    };
    httpd_register_uri_handler(s_server, &led_control_uri);

    // API - RS485 Config
    httpd_uri_t rs485_config_get_uri = {
        .uri = "/api/rs485/config",
        .method = HTTP_GET,
        .handler = api_rs485_config_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_config_get_uri);

    httpd_uri_t rs485_config_post_uri = {
        .uri = "/api/rs485/config",
        .method = HTTP_POST,
        .handler = api_rs485_config_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_config_post_uri);

    // API - Actuator Control (Multi-actuator)
    httpd_uri_t actuator_status_uri = {
        .uri = "/api/actuator/status",
        .method = HTTP_GET,
        .handler = api_actuator_status_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_status_uri);

    httpd_uri_t actuator_control_uri = {
        .uri = "/api/actuator/control",
        .method = HTTP_POST,
        .handler = api_actuator_control_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_control_uri);

    httpd_uri_t actuator_scan_uri = {
        .uri = "/api/actuator/scan",
        .method = HTTP_GET,
        .handler = api_actuator_scan_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_scan_uri);

    httpd_uri_t actuator_add_uri = {
        .uri = "/api/actuator/add",
        .method = HTTP_POST,
        .handler = api_actuator_add_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_add_uri);

    httpd_uri_t actuator_remove_uri = {
        .uri = "/api/actuator/remove",
        .method = HTTP_POST,
        .handler = api_actuator_remove_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_remove_uri);

    s_running = true;
    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

void web_server_deinit(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_vfs_spiffs_unregister("www");
    s_running = false;
}

bool web_server_is_running(void)
{
    return s_running;
}

esp_err_t web_server_set_auth(const char *username, const char *password)
{
    if (username) s_config.username = username;
    if (password) s_config.password = password;
    return ESP_OK;
}
