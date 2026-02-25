/**
 * @file api_actuator.c
 * @brief Actuator (mightyZAP) API handlers
 * 
 * Supports multiple actuators with scan, add, remove, control, and config.
 */

#include "handlers.h"

static const char *TAG = "API_ACTUATOR";

// ============================================================================
// Multi-actuator support
// ============================================================================

#define MAX_ACTUATORS 10

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

// ============================================================================
// Initialization
// ============================================================================

void actuator_handlers_init(void)
{
    uint8_t count = config_get_saved_actuator_count();
    if (count == 0) {
        ESP_LOGI(TAG, "No saved actuators to load");
        return;
    }

    const uint8_t *ids = config_get_saved_actuator_ids();
    if (ids == NULL) {
        ESP_LOGW(TAG, "Failed to get saved actuator IDs");
        return;
    }

    ESP_LOGI(TAG, "Loading %d saved actuators from config", count);

    int loaded = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t id = ids[i];
        esp_err_t ret = add_actuator(id);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Loaded saved actuator ID %d", id);
            loaded++;
        } else {
            ESP_LOGW(TAG, "Failed to load actuator ID %d: %s", id, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Loaded %d of %d saved actuators", loaded, count);
}

// ============================================================================
// API Handlers
// ============================================================================

// GET /api/actuator/status - Get status of all active actuators
esp_err_t api_actuator_status_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    cJSON *actuators = cJSON_CreateArray();

    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].handle) {
            cJSON *act = cJSON_CreateObject();
            cJSON_AddNumberToObject(act, "id", s_actuators[i].id);

            // Add actuator name (or default if not set)
            const char *name = config_get_actuator_name(s_actuators[i].id);
            if (name && strlen(name) > 0) {
                cJSON_AddStringToObject(act, "name", name);
            } else {
                char default_name[32];
                snprintf(default_name, sizeof(default_name), "Actuator #%d", s_actuators[i].id);
                cJSON_AddStringToObject(act, "name", default_name);
            }

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
esp_err_t api_actuator_control_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Bus busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

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
    xSemaphoreGive(g_bus_mutex);
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

// GET /api/actuator/config - Get actuator configuration
esp_err_t api_actuator_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    // Parse query string for ID
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id parameter");
        return ESP_FAIL;
    }
    
    char id_str[8] = {0};
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id parameter");
        return ESP_FAIL;
    }
    
    uint8_t act_id = atoi(id_str);
    actuator_slot_t *slot = find_actuator(act_id);
    
    if (slot == NULL || slot->handle == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Actuator not found");
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "id", act_id);
    
    // Get device info
    mightyzap_info_t info = {0};
    if (mightyzap_get_info(slot->handle, &info) == ESP_OK) {
        cJSON *info_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(info_obj, "model", info.model);
        cJSON_AddNumberToObject(info_obj, "firmware", info.firmware);
        cJSON_AddNumberToObject(info_obj, "voltage_min", info.voltage_min);
        cJSON_AddNumberToObject(info_obj, "voltage_max", info.voltage_max);
        cJSON_AddItemToObject(root, "info", info_obj);
    }
    
    // Get configuration
    mightyzap_config_t config = {0};
    if (mightyzap_get_config(slot->handle, &config) == ESP_OK) {
        cJSON *config_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(config_obj, "slave_id", config.slave_id);
        cJSON_AddNumberToObject(config_obj, "baud_rate", config.baud_rate);
        cJSON_AddNumberToObject(config_obj, "short_stroke_limit", config.short_stroke_limit);
        cJSON_AddNumberToObject(config_obj, "long_stroke_limit", config.long_stroke_limit);
        cJSON_AddNumberToObject(config_obj, "speed_limit", config.speed_limit);
        cJSON_AddNumberToObject(config_obj, "current_limit", config.current_limit);
        cJSON_AddNumberToObject(config_obj, "start_compliance", config.start_compliance);
        cJSON_AddNumberToObject(config_obj, "end_compliance", config.end_compliance);
        cJSON_AddNumberToObject(config_obj, "alarm_led", config.alarm_led);
        cJSON_AddNumberToObject(config_obj, "alarm_shutdown", config.alarm_shutdown);
        cJSON_AddItemToObject(root, "config", config_obj);
    } else {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "Failed to read config");
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/config - Set actuator configuration
esp_err_t api_actuator_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char buf[512];
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
    
    // Get actuator ID
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
    
    // Parse config fields
    cJSON *config_json = cJSON_GetObjectItem(root, "config");
    if (!cJSON_IsObject(config_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing config object");
        goto send_response;
    }
    
    mightyzap_config_t config = {0};
    uint16_t mask = 0;
    cJSON *item;
    
    if ((item = cJSON_GetObjectItem(config_json, "slave_id")) && cJSON_IsNumber(item)) {
        config.slave_id = item->valueint;
        mask |= MZAP_CONFIG_SLAVE_ID;
    }
    if ((item = cJSON_GetObjectItem(config_json, "baud_rate")) && cJSON_IsNumber(item)) {
        config.baud_rate = item->valueint;
        mask |= MZAP_CONFIG_BAUD_RATE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "short_stroke_limit")) && cJSON_IsNumber(item)) {
        config.short_stroke_limit = item->valueint;
        mask |= MZAP_CONFIG_SHORT_STROKE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "long_stroke_limit")) && cJSON_IsNumber(item)) {
        config.long_stroke_limit = item->valueint;
        mask |= MZAP_CONFIG_LONG_STROKE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "speed_limit")) && cJSON_IsNumber(item)) {
        config.speed_limit = item->valueint;
        mask |= MZAP_CONFIG_SPEED_LIMIT;
    }
    if ((item = cJSON_GetObjectItem(config_json, "current_limit")) && cJSON_IsNumber(item)) {
        config.current_limit = item->valueint;
        mask |= MZAP_CONFIG_CURRENT_LIMIT;
    }
    if ((item = cJSON_GetObjectItem(config_json, "start_compliance")) && cJSON_IsNumber(item)) {
        config.start_compliance = item->valueint;
        mask |= MZAP_CONFIG_START_COMPLIANCE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "end_compliance")) && cJSON_IsNumber(item)) {
        config.end_compliance = item->valueint;
        mask |= MZAP_CONFIG_END_COMPLIANCE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "alarm_led")) && cJSON_IsNumber(item)) {
        config.alarm_led = item->valueint;
        mask |= MZAP_CONFIG_ALARM_LED;
    }
    if ((item = cJSON_GetObjectItem(config_json, "alarm_shutdown")) && cJSON_IsNumber(item)) {
        config.alarm_shutdown = item->valueint;
        mask |= MZAP_CONFIG_ALARM_SHUTDOWN;
    }
    
    if (mask == 0) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "No config fields to update");
        goto send_response;
    }
    
    esp_err_t err = mightyzap_set_config(slot->handle, &config, mask);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Config saved" : "Failed to save config");
    
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

// POST /api/actuator/restart - Restart actuator
esp_err_t api_actuator_restart_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char buf[64];
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
    if (!cJSON_IsNumber(id_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing actuator ID");
        return ESP_FAIL;
    }
    
    uint8_t act_id = id_json->valueint;
    actuator_slot_t *slot = find_actuator(act_id);
    cJSON_Delete(root);
    
    if (slot == NULL || slot->handle == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Actuator not found");
        return ESP_FAIL;
    }
    
    esp_err_t err = mightyzap_restart(slot->handle);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Actuator restarting" : "Restart failed");
    
    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

// POST /api/actuator/factory-reset - Factory reset actuator
esp_err_t api_actuator_factory_reset_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char buf[64];
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
    cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    
    if (!cJSON_IsNumber(id_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing actuator ID");
        return ESP_FAIL;
    }
    
    if (!cJSON_IsTrue(confirm)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Confirmation required (confirm: true)");
        return ESP_FAIL;
    }
    
    uint8_t act_id = id_json->valueint;
    actuator_slot_t *slot = find_actuator(act_id);
    cJSON_Delete(root);
    
    if (slot == NULL || slot->handle == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Actuator not found");
        return ESP_FAIL;
    }
    
    ESP_LOGW(TAG, "Factory reset requested for actuator ID=%u", act_id);
    esp_err_t err = mightyzap_factory_reset(slot->handle);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Factory reset complete - actuator will restart" : "Factory reset failed");
    
    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

// GET /api/actuator/scan - Scan for actuators and auto-add them
esp_err_t api_actuator_scan_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Bus busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *found = cJSON_CreateArray();

    if (g_modbus == NULL) {
        xSemaphoreGive(g_bus_mutex);
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

    uint8_t max_id = config_get_scan_max_id();
    if (max_id < 1) max_id = 1;
    if (max_id > 247) max_id = 247;

    ESP_LOGI(TAG, "Scanning for mightyZAP actuators (IDs 1-%d)...", max_id);

    // Suppress timeout warnings during scan
    esp_log_level_set("RS485", ESP_LOG_ERROR);
    esp_log_level_set("MODBUS", ESP_LOG_ERROR);

    int count = 0;
    bool config_changed = false;
    for (uint8_t id = 1; id <= max_id; id++) {
        uint16_t model = 0;
        esp_err_t ret = modbus_read_holding_registers(g_modbus, id,
                                                       MZAP_REG_MODEL_NUMBER, 1, &model);
        // mightyZAP models are typically > 100 (e.g., 350, 500, etc.)
        if (ret == ESP_OK && model > 100) {
            ESP_LOGI(TAG, "Found actuator at ID %d, model: %u", id, model);

            // Auto-add to active actuators
            add_actuator(id);

            // Persist to config (idempotent - won't duplicate)
            if (config_add_saved_actuator_id(id)) {
                ESP_LOGI(TAG, "Persisted actuator ID %d to config", id);
                config_changed = true;
            }

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddNumberToObject(item, "model", model);
            cJSON_AddItemToArray(found, item);
            count++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Restore log levels
    esp_log_level_set("RS485", ESP_LOG_WARN);
    esp_log_level_set("MODBUS", ESP_LOG_WARN);

    // Save config if any new actuators were persisted
    if (config_changed) {
        config_save();
        ESP_LOGI(TAG, "Saved actuator config with %d actuators", count);
    }

    xSemaphoreGive(g_bus_mutex);

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
esp_err_t api_actuator_add_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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
esp_err_t api_actuator_remove_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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

        // Also remove from persisted config
        if (config_remove_saved_actuator_id(id)) {
            config_save();
            ESP_LOGI(TAG, "Actuator removed from config: ID %d", id);
        }

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

// POST /api/actuator/set-name - Set friendly name for actuator
esp_err_t api_actuator_set_name_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *name_json = cJSON_GetObjectItem(root, "name");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json) || id_json->valueint < 1 || id_json->valueint > 247) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid ID (1-247)");
    } else if (!cJSON_IsString(name_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid name (must be string)");
    } else {
        uint8_t id = id_json->valueint;
        const char *name = name_json->valuestring;

        // Set the actuator name
        bool success = config_set_actuator_name(id, name);

        if (success) {
            // Persist to config
            config_save();
            ESP_LOGI(TAG, "Actuator name set: ID %d -> '%s'", id, name);
            cJSON_AddBoolToObject(response, "success", true);
            cJSON_AddStringToObject(response, "message", "Name set successfully");
            cJSON_AddNumberToObject(response, "id", id);
            cJSON_AddStringToObject(response, "name", name);
        } else {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Actuator not found in config");
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

// ============================================================================
// Synchronized Movement Handlers
// ============================================================================

// Static sync group (persists between calls)
static mightyzap_sync_group_t s_sync_group = {0};
static bool s_sync_group_initialized = false;

// POST /api/actuator/sync-move - Synchronized movement
esp_err_t api_actuator_sync_move_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Bus busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        xSemaphoreGive(g_bus_mutex);
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

    // Parse IDs array
    cJSON *ids_json = cJSON_GetObjectItem(root, "ids");
    if (!cJSON_IsArray(ids_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing 'ids' array");
        goto send_response;
    }

    int id_count = cJSON_GetArraySize(ids_json);
    if (id_count < 1 || id_count > MZAP_SYNC_MAX_ACTUATORS) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "ids must have 1-4 elements");
        goto send_response;
    }

    uint8_t ids[MZAP_SYNC_MAX_ACTUATORS];
    for (int i = 0; i < id_count; i++) {
        cJSON *id_item = cJSON_GetArrayItem(ids_json, i);
        if (!cJSON_IsNumber(id_item)) {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Invalid ID in array");
            goto send_response;
        }
        ids[i] = id_item->valueint;
    }

    // Parse position
    cJSON *position_json = cJSON_GetObjectItem(root, "position");
    if (!cJSON_IsNumber(position_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing 'position'");
        goto send_response;
    }
    uint16_t position = position_json->valueint;

    // Parse optional parameters
    cJSON *speed_json = cJSON_GetObjectItem(root, "speed");
    cJSON *current_json = cJSON_GetObjectItem(root, "current");
    cJSON *tolerance_json = cJSON_GetObjectItem(root, "tolerance");
    cJSON *max_desync_json = cJSON_GetObjectItem(root, "max_desync");
    cJSON *timeout_json = cJSON_GetObjectItem(root, "timeout");
    cJSON *wait_json = cJSON_GetObjectItem(root, "wait");

    uint16_t speed = cJSON_IsNumber(speed_json) ? speed_json->valueint : 300;
    uint16_t current = cJSON_IsNumber(current_json) ? current_json->valueint : 400;
    uint16_t tolerance = cJSON_IsNumber(tolerance_json) ? tolerance_json->valueint : MZAP_SYNC_DEFAULT_TOLERANCE;
    uint16_t max_desync = cJSON_IsNumber(max_desync_json) ? max_desync_json->valueint : 150;
    uint32_t timeout_ms = cJSON_IsNumber(timeout_json) ? timeout_json->valueint : 10000;
    bool wait = cJSON_IsBool(wait_json) ? cJSON_IsTrue(wait_json) : true;

    // Check if Modbus is available
    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Modbus not initialized");
        goto send_response;
    }

    // Initialize or reinitialize sync group
    esp_err_t err = mightyzap_sync_init(&s_sync_group, g_modbus, ids, id_count);
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Failed to init sync group");
        goto send_response;
    }
    s_sync_group_initialized = true;

    // Set parameters
    mightyzap_sync_set_params(&s_sync_group, speed, current, tolerance, max_desync);

    if (wait) {
        // Blocking move with wait
        mightyzap_sync_result_t result = mightyzap_sync_move_wait(&s_sync_group, position, timeout_ms);
        
        cJSON_AddBoolToObject(response, "success", result == MZAP_SYNC_OK);
        
        // Add result details
        const char *result_str;
        switch (result) {
            case MZAP_SYNC_OK:        result_str = "Movement completed"; break;
            case MZAP_SYNC_TIMEOUT:   result_str = "Timeout"; break;
            case MZAP_SYNC_DESYNC:    result_str = "Desynchronization error"; break;
            case MZAP_SYNC_COMM_ERROR: result_str = "Communication error"; break;
            default:                   result_str = "Unknown error"; break;
        }
        cJSON_AddStringToObject(response, "message", result_str);
        cJSON_AddNumberToObject(response, "result_code", result);

        // Add final positions
        cJSON *positions = cJSON_CreateArray();
        uint16_t max_desync = 0;
        mightyzap_sync_get_desync(&s_sync_group, &max_desync);
        
        for (int i = 0; i < id_count; i++) {
            cJSON *pos_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(pos_obj, "id", ids[i]);
            cJSON_AddNumberToObject(pos_obj, "position", s_sync_group.present_positions[i]);
            cJSON_AddItemToArray(positions, pos_obj);
        }
        cJSON_AddItemToObject(response, "positions", positions);
        cJSON_AddNumberToObject(response, "desync", max_desync);
    } else {
        // Non-blocking: just start the movement
        err = mightyzap_sync_move_start(&s_sync_group, position);
        cJSON_AddBoolToObject(response, "success", err == ESP_OK);
        cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Movement started" : "Failed to start");
    }

send_response:
    xSemaphoreGive(g_bus_mutex);
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

// GET /api/actuator/sync-status - Get sync group status
esp_err_t api_actuator_sync_status_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();

    if (!s_sync_group_initialized || g_modbus == NULL) {
        cJSON_AddBoolToObject(root, "initialized", false);
        cJSON_AddStringToObject(root, "message", "Sync group not initialized");
        
        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "initialized", true);
    cJSON_AddNumberToObject(root, "count", s_sync_group.count);
    cJSON_AddNumberToObject(root, "target", s_sync_group.target_position);
    cJSON_AddNumberToObject(root, "speed", s_sync_group.speed);
    cJSON_AddNumberToObject(root, "current", s_sync_group.current);
    cJSON_AddNumberToObject(root, "tolerance", s_sync_group.tolerance);

    // Read current positions
    bool in_position = false;
    bool all_stopped = false;
    esp_err_t err = mightyzap_sync_check_position(&s_sync_group, &in_position, &all_stopped);

    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "in_position", in_position);
        cJSON_AddBoolToObject(root, "all_stopped", all_stopped);

        uint16_t desync = 0;
        mightyzap_sync_get_desync(&s_sync_group, &desync);
        cJSON_AddNumberToObject(root, "desync", desync);

        cJSON *actuators = cJSON_CreateArray();
        for (uint8_t i = 0; i < s_sync_group.count; i++) {
            cJSON *act = cJSON_CreateObject();
            cJSON_AddNumberToObject(act, "id", s_sync_group.ids[i]);
            cJSON_AddNumberToObject(act, "position", s_sync_group.present_positions[i]);
            
            int32_t diff = (int32_t)s_sync_group.present_positions[i] - (int32_t)s_sync_group.target_position;
            cJSON_AddNumberToObject(act, "error", diff);
            
            cJSON_AddItemToArray(actuators, act);
        }
        cJSON_AddItemToObject(root, "actuators", actuators);
    } else {
        cJSON_AddStringToObject(root, "error", "Failed to read positions");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}
