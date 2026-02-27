/**
 * @file api_actuator.c
 * @brief Actuator (mightyZAP) API handlers
 *
 * Supports multiple actuators with scan, add, remove, control, and config.
 */

#include "actuator_task.h"
#include "handlers.h"

static const char *TAG = "API_ACTUATOR";

// Mutex timeout constants
#define BUS_MUTEX_TIMEOUT_CONTROL_MS  5000  // Quick operations (read/write single register)
#define BUS_MUTEX_TIMEOUT_SCAN_MS    30000  // Scan across multiple IDs

// ============================================================================
// Multi-actuator support
// ============================================================================

#define MAX_ACTUATORS 10

typedef struct {
    uint8_t id;
    mightyzap_handle_t handle;
    bool active;
} actuator_slot_t;

/*
 * Thread safety: s_actuators[] and s_num_actuators are accessed without a
 * mutex. This is safe because ESP-IDF's httpd runs all URI handlers on a single
 * task (core_id = tskNO_AFFINITY, max_open_sockets controls concurrency but
 * handlers are serialized on the httpd task). If httpd is ever configured with
 * multiple worker tasks, a mutex must be added here.
 */
static actuator_slot_t s_actuators[MAX_ACTUATORS] = {0};
static uint8_t s_num_actuators = 0;

// Cached status JSON for polling optimization
static char s_status_cache[1024] = {0};
static size_t s_status_cache_len = 0;
static bool s_status_dirty = true;

static void mark_status_dirty(void) { s_status_dirty = true; }

// Helper: Find actuator by ID
static actuator_slot_t *find_actuator(uint8_t id) {
    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].id == id) {
            return &s_actuators[i];
        }
    }
    return NULL;
}

// Helper: Add actuator
static esp_err_t add_actuator(uint8_t id) {
    if (find_actuator(id) != NULL)
        return ESP_OK; // Already exists
    if (g_modbus == NULL)
        return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (!s_actuators[i].active) {
            mightyzap_handle_t handle = NULL;
            esp_err_t ret = mightyzap_init(g_modbus, id, &handle);
            if (ret == ESP_OK) {
                s_actuators[i].id = id;
                s_actuators[i].handle = handle;
                s_actuators[i].active = true;
                s_num_actuators++;
                mark_status_dirty();
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
            mark_status_dirty();
            break;
        }
    }
}

// ============================================================================
// Public: Actuator handle lookup (used by control_loop)
// ============================================================================

mightyzap_handle_t *actuator_get_handle_by_id(uint8_t id) {
    actuator_slot_t *slot = find_actuator(id);
    if (slot != NULL && slot->handle != NULL) {
        return &slot->handle;
    }
    return NULL;
}

// ============================================================================
// Initialization
// ============================================================================

void actuator_handlers_init(void) {
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
            ESP_LOGW(TAG, "Failed to load actuator ID %d: %s", id,
               esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Loaded %d of %d saved actuators", loaded, count);
}

// ============================================================================
// API Handlers
// ============================================================================

// Minimum interval between status cache rebuilds (ms)
#define STATUS_CACHE_MIN_INTERVAL_MS 500
static TickType_t s_status_cache_tick = 0;

/**
 * @brief Rebuild the cached status JSON using snprintf (hot path optimization)
 */
static void rebuild_status_cache(void) {
    char *p = s_status_cache;
    char *end = s_status_cache + sizeof(s_status_cache) - 2;

    p += snprintf(p, end - p, "{\"actuators\":[");

    bool first = true;
    for (int i = 0; i < MAX_ACTUATORS && p < end; i++) {
        if (!s_actuators[i].active || !s_actuators[i].handle)
            continue;

        if (!first)
            *p++ = ',';
        first = false;

        const char *name = config_get_actuator_name(s_actuators[i].id);
        char name_buf[32];
        if (!name || strlen(name) == 0) {
            snprintf(name_buf, sizeof(name_buf), "Actuator #%d", s_actuators[i].id);
            name = name_buf;
        }

        mightyzap_status_t status;
        esp_err_t ret = mightyzap_get_status(s_actuators[i].handle, &status);

        if (ret == ESP_OK) {
            p += snprintf(
                    p, end - p,
                    "{\"id\":%d,\"name\":\"%s\",\"connected\":true,"
                    "\"position\":%d,\"current\":%d,\"voltage\":%.1f,\"moving\":%s}",
                    s_actuators[i].id, name, status.position, status.current,
                    status.voltage / 10.0, status.moving ? "true" : "false");
        } else {
            p += snprintf(p, end - p,
                                        "{\"id\":%d,\"name\":\"%s\",\"connected\":false}",
                                        s_actuators[i].id, name);
        }
    }

    p += snprintf(p, end - p, "],\"count\":%d}", s_num_actuators);

    s_status_cache_len = (size_t)(p - s_status_cache);
    s_status_dirty = false;
    s_status_cache_tick = xTaskGetTickCount();
}

// GET /api/actuator/status - Get status of all active actuators
esp_err_t api_actuator_status_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);

    // Rebuild cache if dirty or stale
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (now - s_status_cache_tick) * portTICK_PERIOD_MS;
    if (s_status_dirty || elapsed_ms >= STATUS_CACHE_MIN_INTERVAL_MS ||
            s_status_cache_len == 0) {
        rebuild_status_cache();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s_status_cache, s_status_cache_len);
    return ESP_OK;
}

// POST /api/actuator/control - Control specific actuator by ID
// Non-blocking: async commands are enqueued, sync commands take mutex briefly.
esp_err_t api_actuator_control_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
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

    // Check for force enable/disable (requires bus mutex briefly)
    cJSON *force_enable = cJSON_GetObjectItem(root, "force");
    if (cJSON_IsBool(force_enable)) {
        if (!cJSON_IsTrue(force_enable)) {
            err = actuator_stop_async(handle);
        } else {
            if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                err = mightyzap_set_force_enable(handle, true);
                xSemaphoreGive(g_bus_mutex);
            } else {
                err = ESP_ERR_TIMEOUT;
            }
        }
    }

    // Check for position (async — no mutex needed, goes through queue)
    cJSON *position = cJSON_GetObjectItem(root, "position");
    if (cJSON_IsNumber(position)) {
        int val = position->valueint;
        if (val >= 0 && val <= MZAP_MAX_POSITION) {
            err = actuator_move_async(handle, val);
        }
    }

    // Check for speed (requires bus mutex briefly)
    cJSON *speed = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(speed)) {
        int val = speed->valueint;
        if (val >= 0 && val <= MZAP_MAX_SPEED) {
            if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                err = mightyzap_set_speed(handle, val);
                xSemaphoreGive(g_bus_mutex);
            } else {
                err = ESP_ERR_TIMEOUT;
            }
        }
    }

    // Check for current (requires bus mutex briefly)
    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (cJSON_IsNumber(current)) {
        int val = current->valueint;
        if (val >= 0 && val <= MZAP_MAX_CURRENT) {
            if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                err = mightyzap_set_current(handle, val);
                xSemaphoreGive(g_bus_mutex);
            } else {
                err = ESP_ERR_TIMEOUT;
            }
        }
    }

    // Check for combined goal (requires bus mutex briefly)
    cJSON *goal = cJSON_GetObjectItem(root, "goal");
    if (cJSON_IsObject(goal)) {
        cJSON *g_pos = cJSON_GetObjectItem(goal, "position");
        cJSON *g_spd = cJSON_GetObjectItem(goal, "speed");
        cJSON *g_cur = cJSON_GetObjectItem(goal, "current");

        if (cJSON_IsNumber(g_pos) && cJSON_IsNumber(g_spd) &&
                cJSON_IsNumber(g_cur)) {
            if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                err = mightyzap_set_goal(handle, g_pos->valueint, g_spd->valueint,
                                   g_cur->valueint);
                xSemaphoreGive(g_bus_mutex);
            } else {
                err = ESP_ERR_TIMEOUT;
            }
        }
    }

    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message",
                                                    err == ESP_OK ? "OK" : "Command failed");
    if (err == ESP_OK)
        mark_status_dirty();

send_response:
    cJSON_Delete(root);
    return send_json(req, response);
}

// GET /api/actuator/config - Get actuator configuration
esp_err_t api_actuator_config_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    // Parse query string for ID
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "400 Bad Request", "Missing id parameter");
    }

    char id_str[8] = {0};
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        return send_error_json(req, "400 Bad Request", "Missing id parameter");
    }

    uint8_t act_id = atoi(id_str);
    actuator_slot_t *slot = find_actuator(act_id);

    if (slot == NULL || slot->handle == NULL) {
        return send_error_json(req, "404 Not Found", "Actuator not found");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return send_json(req, NULL);
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
        cJSON_AddNumberToObject(config_obj, "short_stroke_limit",
                                                        config.short_stroke_limit);
        cJSON_AddNumberToObject(config_obj, "long_stroke_limit",
                                                        config.long_stroke_limit);
        cJSON_AddNumberToObject(config_obj, "speed_limit", config.speed_limit);
        cJSON_AddNumberToObject(config_obj, "current_limit", config.current_limit);
        cJSON_AddNumberToObject(config_obj, "start_compliance",
                                                        config.start_compliance);
        cJSON_AddNumberToObject(config_obj, "end_compliance",
                                                        config.end_compliance);
        cJSON_AddNumberToObject(config_obj, "alarm_led", config.alarm_led);
        cJSON_AddNumberToObject(config_obj, "alarm_shutdown",
                                                        config.alarm_shutdown);
        cJSON_AddItemToObject(root, "config", config_obj);
    } else {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "Failed to read config");
    }

    return send_json(req, root);
}

// POST /api/actuator/config - Set actuator configuration
esp_err_t api_actuator_config_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
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

    if ((item = cJSON_GetObjectItem(config_json, "slave_id")) &&
            cJSON_IsNumber(item)) {
        config.slave_id = item->valueint;
        mask |= MZAP_CONFIG_SLAVE_ID;
    }
    if ((item = cJSON_GetObjectItem(config_json, "baud_rate")) &&
            cJSON_IsNumber(item)) {
        config.baud_rate = item->valueint;
        mask |= MZAP_CONFIG_BAUD_RATE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "short_stroke_limit")) &&
            cJSON_IsNumber(item)) {
        config.short_stroke_limit = item->valueint;
        mask |= MZAP_CONFIG_SHORT_STROKE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "long_stroke_limit")) &&
            cJSON_IsNumber(item)) {
        config.long_stroke_limit = item->valueint;
        mask |= MZAP_CONFIG_LONG_STROKE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "speed_limit")) &&
            cJSON_IsNumber(item)) {
        config.speed_limit = item->valueint;
        mask |= MZAP_CONFIG_SPEED_LIMIT;
    }
    if ((item = cJSON_GetObjectItem(config_json, "current_limit")) &&
            cJSON_IsNumber(item)) {
        config.current_limit = item->valueint;
        mask |= MZAP_CONFIG_CURRENT_LIMIT;
    }
    if ((item = cJSON_GetObjectItem(config_json, "start_compliance")) &&
            cJSON_IsNumber(item)) {
        config.start_compliance = item->valueint;
        mask |= MZAP_CONFIG_START_COMPLIANCE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "end_compliance")) &&
            cJSON_IsNumber(item)) {
        config.end_compliance = item->valueint;
        mask |= MZAP_CONFIG_END_COMPLIANCE;
    }
    if ((item = cJSON_GetObjectItem(config_json, "alarm_led")) &&
            cJSON_IsNumber(item)) {
        config.alarm_led = item->valueint;
        mask |= MZAP_CONFIG_ALARM_LED;
    }
    if ((item = cJSON_GetObjectItem(config_json, "alarm_shutdown")) &&
            cJSON_IsNumber(item)) {
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
    cJSON_AddStringToObject(response, "message",
                                                    err == ESP_OK ? "Config saved"
                                                                                : "Failed to save config");

send_response:
    cJSON_Delete(root);
    return send_json(req, response);
}

// POST /api/actuator/restart - Restart actuator
esp_err_t api_actuator_restart_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsNumber(id_json)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "Missing actuator ID");
    }

    uint8_t act_id = id_json->valueint;
    actuator_slot_t *slot = find_actuator(act_id);
    cJSON_Delete(root);

    if (slot == NULL || slot->handle == NULL) {
        return send_error_json(req, "404 Not Found", "Actuator not found");
    }

    esp_err_t err = mightyzap_restart(slot->handle);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL)
        return send_json(req, NULL);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message",
                                                    err == ESP_OK ? "Actuator restarting"
                                                                                : "Restart failed");

    return send_json(req, response);
}

// POST /api/actuator/factory-reset - Factory reset actuator
esp_err_t api_actuator_factory_reset_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *confirm = cJSON_GetObjectItem(root, "confirm");

    if (!cJSON_IsNumber(id_json)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "Missing actuator ID");
    }

    if (!cJSON_IsTrue(confirm)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "Confirmation required (confirm: true)");
    }

    uint8_t act_id = id_json->valueint;
    actuator_slot_t *slot = find_actuator(act_id);
    cJSON_Delete(root);

    if (slot == NULL || slot->handle == NULL) {
        return send_error_json(req, "404 Not Found", "Actuator not found");
    }

    ESP_LOGW(TAG, "Factory reset requested for actuator ID=%u", act_id);
    esp_err_t err = mightyzap_factory_reset(slot->handle);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL)
        return send_json(req, NULL);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message",
                                                    err == ESP_OK
                                                            ? "Factory reset complete - actuator will restart"
                                                            : "Factory reset failed");

    return send_json(req, response);
}

// GET /api/actuator/scan - Scan for actuators and auto-add them
esp_err_t api_actuator_scan_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);

    if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_SCAN_MS)) != pdTRUE) {
        return send_error_json(req, "503 Service Unavailable", "Bus busy");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *found = cJSON_CreateArray();

    if (g_modbus == NULL) {
        xSemaphoreGive(g_bus_mutex);
        cJSON_AddItemToObject(root, "found", found);
        cJSON_AddNumberToObject(root, "count", 0);
        cJSON_AddStringToObject(root, "error", "Modbus not initialized");
        return send_json(req, root);
    }

    uint8_t max_id = config_get_scan_max_id();
    if (max_id < 1)
        max_id = 1;
    if (max_id > MODBUS_MAX_SLAVE_ADDR)
        max_id = MODBUS_MAX_SLAVE_ADDR;

    ESP_LOGI(TAG, "Scanning for mightyZAP actuators (IDs 1-%d)...", max_id);

    // Suppress timeout warnings during scan
    esp_log_level_set("RS485", ESP_LOG_ERROR);
    esp_log_level_set("MODBUS", ESP_LOG_ERROR);

    int count = 0;
    bool config_changed = false;
    for (uint8_t id = 1; id <= max_id; id++) {
        uint16_t model = 0;
        esp_err_t ret = modbus_read_holding_registers(
                g_modbus, id, MZAP_REG_MODEL_NUMBER, 1, &model);
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

    return send_json(req, root);
}

// POST /api/actuator/add - Add actuator by ID
esp_err_t api_actuator_add_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json) || id_json->valueint < 1 ||
            id_json->valueint > MODBUS_MAX_SLAVE_ADDR) {
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

    cJSON_Delete(root);
    return send_json(req, response);
}

// POST /api/actuator/remove - Remove actuator by ID
esp_err_t api_actuator_remove_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
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

    cJSON_Delete(root);
    return send_json(req, response);
}

// POST /api/actuator/set-name - Set friendly name for actuator
esp_err_t api_actuator_set_name_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *name_json = cJSON_GetObjectItem(root, "name");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json) || id_json->valueint < 1 ||
            id_json->valueint > MODBUS_MAX_SLAVE_ADDR) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid ID (1-247)");
    } else if (!cJSON_IsString(name_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message",
                                                        "Invalid name (must be string)");
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
            cJSON_AddStringToObject(response, "message",
                                                            "Actuator not found in config");
        }
    }

    cJSON_Delete(root);
    return send_json(req, response);
}

// ============================================================================
// Synchronized Movement Handlers
// ============================================================================

// Static sync group (persists between calls)
static mightyzap_sync_group_t s_sync_group = {0};
static bool s_sync_group_initialized = false;

// POST /api/actuator/sync-move - Synchronized movement (uses Bus 2 with broadcast)
// Supports "closed_loop": true for closed-loop sync controller
esp_err_t api_actuator_sync_move_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);

    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
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
    cJSON *closed_loop_json = cJSON_GetObjectItem(root, "closed_loop");
    cJSON *sync_interval_json = cJSON_GetObjectItem(root, "sync_interval_ms");
    cJSON *correction_factor_json = cJSON_GetObjectItem(root, "correction_speed_factor");

    uint16_t speed = cJSON_IsNumber(speed_json) ? speed_json->valueint : 300;
    uint16_t current =
            cJSON_IsNumber(current_json) ? current_json->valueint : 400;
    uint16_t tolerance = cJSON_IsNumber(tolerance_json)
                           ? tolerance_json->valueint
                           : MZAP_SYNC_DEFAULT_TOLERANCE;
    uint16_t max_desync =
            cJSON_IsNumber(max_desync_json) ? max_desync_json->valueint : 150;
    uint32_t timeout_ms =
            cJSON_IsNumber(timeout_json) ? timeout_json->valueint : 10000;
    bool wait = cJSON_IsBool(wait_json) ? cJSON_IsTrue(wait_json) : false;
    bool closed_loop = cJSON_IsBool(closed_loop_json) ? cJSON_IsTrue(closed_loop_json) : false;

    // Check if sync Modbus bus is available (Bus 2)
    if (g_modbus_sync == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Sync bus (Modbus) not initialized");
        goto send_response;
    }

    // Initialize or reinitialize sync group on Bus 2
    esp_err_t err = mightyzap_sync_init(&s_sync_group, g_modbus_sync, ids, id_count);
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Failed to init sync group");
        goto send_response;
    }
    s_sync_group_initialized = true;

    // Set parameters
    mightyzap_sync_set_params(&s_sync_group, speed, current, tolerance,
                                                        max_desync);

    // ---- Closed-loop mode (non-blocking, uses dedicated task) ----
    if (closed_loop) {
        if (mightyzap_sync_ctrl_is_busy()) {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message",
                                    "Sync controller busy — abort first or wait");
            goto send_response;
        }

        mightyzap_sync_ctrl_config_t ctrl_cfg = MZAP_SYNC_CTRL_CONFIG_DEFAULT();
        ctrl_cfg.position_tolerance = tolerance;
        ctrl_cfg.max_desync = max_desync > 0 ? max_desync : 50;
        ctrl_cfg.timeout_ms = timeout_ms;
        if (cJSON_IsNumber(sync_interval_json))
            ctrl_cfg.sync_interval_ms = sync_interval_json->valueint;
        if (cJSON_IsNumber(correction_factor_json))
            ctrl_cfg.correction_speed_factor = (float)correction_factor_json->valuedouble;

        err = mightyzap_sync_ctrl_move(&s_sync_group, position, &ctrl_cfg);
        cJSON_AddBoolToObject(response, "success", err == ESP_OK);
        cJSON_AddStringToObject(response, "message",
                                err == ESP_OK
                                    ? "Closed-loop movement started — poll /api/actuator/sync-status"
                                    : "Failed to start closed-loop movement");
        cJSON_AddBoolToObject(response, "closed_loop", true);
        goto send_response;
    }

    // ---- Legacy mode (blocking or fire-and-forget) ----
    if (xSemaphoreTake(g_bus_sync_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_CONTROL_MS)) != pdTRUE) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Sync bus busy");
        goto send_response;
    }

    if (wait) {
        // Blocking move with wait
        mightyzap_sync_result_t result =
                mightyzap_sync_move_wait(&s_sync_group, position, timeout_ms);

        cJSON_AddBoolToObject(response, "success", result == MZAP_SYNC_OK);

        // Add result details
        const char *result_str;
        switch (result) {
        case MZAP_SYNC_OK:
            result_str = "Movement completed";
            break;
        case MZAP_SYNC_TIMEOUT:
            result_str = "Timeout";
            break;
        case MZAP_SYNC_DESYNC:
            result_str = "Desynchronization error";
            break;
        case MZAP_SYNC_COMM_ERROR:
            result_str = "Communication error";
            break;
        default:
            result_str = "Unknown error";
            break;
        }
        cJSON_AddStringToObject(response, "message", result_str);
        cJSON_AddNumberToObject(response, "result_code", result);

        // Add final positions
        cJSON *positions = cJSON_CreateArray();
        uint16_t final_desync = 0;
        mightyzap_sync_get_desync(&s_sync_group, &final_desync);

        for (int i = 0; i < id_count; i++) {
            cJSON *pos_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(pos_obj, "id", ids[i]);
            cJSON_AddNumberToObject(pos_obj, "position",
                                                            s_sync_group.present_positions[i]);
            cJSON_AddItemToArray(positions, pos_obj);
        }
        cJSON_AddItemToObject(response, "positions", positions);
        cJSON_AddNumberToObject(response, "desync", final_desync);
    } else {
        // Non-blocking: send movement commands to each actuator individually
        err = mightyzap_sync_move_start(&s_sync_group, position);
        cJSON_AddBoolToObject(response, "success", err == ESP_OK);
        cJSON_AddStringToObject(response, "message",
                                                        err == ESP_OK ? "Movement started"
                                                                                    : "Failed to start");
    }

    xSemaphoreGive(g_bus_sync_mutex);

send_response:
    cJSON_Delete(root);
    return send_json(req, response);
}

// GET /api/actuator/sync-status - Get sync group status + closed-loop controller status
esp_err_t api_actuator_sync_status_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();

    // Auto-initialize sync group from saved roles if needed
    if (!s_sync_group_initialized && g_modbus_sync != NULL) {
        uint8_t la_id = config_get_role_lens_a_id();
        uint8_t lb_id = config_get_role_lens_b_id();
        if (la_id > 0 && lb_id > 0) {
            uint8_t ids[2] = { la_id, lb_id };
            if (mightyzap_sync_init(&s_sync_group, g_modbus_sync, ids, 2) == ESP_OK) {
                s_sync_group_initialized = true;
                ESP_LOGI(TAG, "Auto-initialized sync group from roles: IDs %d, %d", la_id, lb_id);
            }
        }
    }

    if (!s_sync_group_initialized || g_modbus_sync == NULL) {
        cJSON_AddBoolToObject(root, "initialized", false);
        cJSON_AddStringToObject(root, "message", "Sync group not initialized");
    } else {
        // Take Bus 2 mutex for position reads
        if (xSemaphoreTake(g_bus_sync_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_CONTROL_MS)) != pdTRUE) {
            cJSON_AddBoolToObject(root, "initialized", true);
            cJSON_AddStringToObject(root, "error", "Sync bus busy");
        } else {
            cJSON_AddBoolToObject(root, "initialized", true);
            cJSON_AddNumberToObject(root, "count", s_sync_group.count);
            cJSON_AddNumberToObject(root, "target", s_sync_group.target_position);
            cJSON_AddNumberToObject(root, "speed", s_sync_group.speed);
            cJSON_AddNumberToObject(root, "current", s_sync_group.current);
            cJSON_AddNumberToObject(root, "tolerance", s_sync_group.tolerance);

            // Read current positions
            bool in_position = false;
            bool all_stopped = false;
            esp_err_t err =
                    mightyzap_sync_check_position(&s_sync_group, &in_position, &all_stopped);

            xSemaphoreGive(g_bus_sync_mutex);

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
                    cJSON_AddNumberToObject(act, "position",
                                                                    s_sync_group.present_positions[i]);

                    int32_t diff = (int32_t)s_sync_group.present_positions[i] -
                             (int32_t)s_sync_group.target_position;
                    cJSON_AddNumberToObject(act, "error", diff);

                    cJSON_AddItemToArray(actuators, act);
                }
                cJSON_AddItemToObject(root, "actuators", actuators);
            } else {
                cJSON_AddStringToObject(root, "error", "Failed to read positions");
            }
        }
    }

    // Always include closed-loop controller status
    mightyzap_sync_ctrl_status_t ctrl_status;
    mightyzap_sync_ctrl_get_status(&ctrl_status);

    cJSON *ctrl = cJSON_CreateObject();
    const char *state_str;
    switch (ctrl_status.state) {
    case SYNC_CTRL_IDLE:   state_str = "idle"; break;
    case SYNC_CTRL_MOVING: state_str = "moving"; break;
    case SYNC_CTRL_DONE:   state_str = "done"; break;
    case SYNC_CTRL_ERROR:  state_str = "error"; break;
    default:               state_str = "unknown"; break;
    }
    cJSON_AddStringToObject(ctrl, "state", state_str);
    cJSON_AddNumberToObject(ctrl, "result_code", ctrl_status.result);
    cJSON_AddNumberToObject(ctrl, "target", ctrl_status.target_position);
    cJSON_AddNumberToObject(ctrl, "original_speed", ctrl_status.original_speed);
    cJSON_AddNumberToObject(ctrl, "desync", ctrl_status.desync);
    cJSON_AddNumberToObject(ctrl, "elapsed_ms", ctrl_status.elapsed_ms);
    cJSON_AddNumberToObject(ctrl, "corrections", ctrl_status.corrections_applied);

    if (ctrl_status.count > 0) {
        cJSON *ctrl_positions = cJSON_CreateArray();
        for (uint8_t i = 0; i < ctrl_status.count; i++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "id", ctrl_status.ids[i]);
            cJSON_AddNumberToObject(p, "position", ctrl_status.positions[i]);
            cJSON_AddItemToArray(ctrl_positions, p);
        }
        cJSON_AddItemToObject(ctrl, "positions", ctrl_positions);
    }

    cJSON_AddItemToObject(root, "controller", ctrl);

    return send_json(req, root);
}

// POST /api/actuator/sync-abort - Abort closed-loop sync movement
esp_err_t api_actuator_sync_abort_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);

    esp_err_t err = mightyzap_sync_ctrl_abort();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message",
                            err == ESP_OK ? "Aborted" : "Abort timeout");

    return send_json(req, response);
}
