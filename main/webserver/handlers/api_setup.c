/**
 * @file api_setup.c
 * @brief Setup Wizard API handlers - Smart scan, role assignment, standardize
 */

#include "handlers.h"

static const char *TAG = "API_SETUP";

// Mutex timeout constants
#define BUS_MUTEX_TIMEOUT_CONTROL_MS  5000
#define BUS_MUTEX_TIMEOUT_SCAN_MS    30000

// Baud rates to scan (most common mightyZAP rates)
static const int SCAN_BAUD_RATES[] = { 9600, 19200, 38400, 57600, 115200 };
static const int SCAN_BAUD_COUNT = sizeof(SCAN_BAUD_RATES) / sizeof(SCAN_BAUD_RATES[0]);

// mightyZAP baud rate code to actual baud rate
static uint32_t mzap_baud_code_to_rate(uint8_t code)
{
    switch (code) {
        case 16:  return 115200;
        case 32:  return 57600;
        case 48:  return 38400;
        case 64:  return 19200;
        case 128: return 9600;
        default:  return 57600;
    }
}

// Actual baud rate to mightyZAP baud rate code
static uint8_t mzap_rate_to_baud_code(uint32_t rate)
{
    switch (rate) {
        case 115200: return 16;
        case 57600:  return 32;
        case 38400:  return 48;
        case 19200:  return 64;
        case 9600:   return 128;
        default:     return 32;
    }
}

// ============================================================================
// POST /api/setup/scan-buses - Dual-bus discovery
// ============================================================================

esp_err_t api_setup_scan_buses_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);

    // Suppress noisy logs and disable retries during scan
    esp_log_level_set("RS485", ESP_LOG_ERROR);
    esp_log_level_set("MODBUS", ESP_LOG_ERROR);
    modbus_set_retry_count(0);

    // ---- Bus 1 (Primary): actuators + Baumer ----
    cJSON *bus1 = cJSON_CreateObject();
    cJSON *bus1_actuators = cJSON_CreateArray();
    cJSON *bus1_baumer = cJSON_CreateObject();

    if (g_rs485 == NULL || g_modbus == NULL) {
        cJSON_AddStringToObject(bus1, "error", "Bus 1 not initialized");
    } else {
        if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_SCAN_MS)) != pdTRUE) {
            cJSON_AddStringToObject(bus1, "error", "Bus 1 busy");
        } else {
            int original_baud = rs485_get_baud(g_rs485);

            for (int b = 0; b < SCAN_BAUD_COUNT; b++) {
                int baud = SCAN_BAUD_RATES[b];
                esp_err_t ret = rs485_set_baud(g_rs485, baud);
                if (ret != ESP_OK) continue;
                vTaskDelay(pdMS_TO_TICKS(50));

                // Scan IDs 1-10 for mightyZAP actuators
                for (uint8_t id = 1; id <= 10; id++) {
                    uint16_t model = 0;
                    ret = modbus_read_holding_registers(g_modbus, id,
                                                         MZAP_REG_MODEL_NUMBER, 1, &model);
                    if (ret == ESP_OK && model > 100) {
                        ESP_LOGI(TAG, "Bus1: Found actuator ID=%d, baud=%d, model=%u", id, baud, model);
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddNumberToObject(item, "id", id);
                        cJSON_AddNumberToObject(item, "baud_rate", baud);
                        cJSON_AddNumberToObject(item, "model", model);
                        cJSON_AddItemToArray(bus1_actuators, item);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            // Check for Baumer sensor at configured baud (always at target baud)
            rs485_set_baud(g_rs485, original_baud);
            vTaskDelay(pdMS_TO_TICKS(50));

            uint8_t baumer_id = config_get_baumer_slave_id();
            uint16_t baumer_reg = 0;
            esp_err_t ret = modbus_read_input_registers(g_modbus, baumer_id, 200, 1, &baumer_reg);
            bool baumer_detected = (ret == ESP_OK);
            cJSON_AddNumberToObject(bus1_baumer, "id", baumer_id);
            cJSON_AddBoolToObject(bus1_baumer, "detected", baumer_detected);
            ESP_LOGI(TAG, "Bus1: Baumer ID=%d %s", baumer_id,
                     baumer_detected ? "detected" : "not found");

            // Restore baud
            rs485_set_baud(g_rs485, original_baud);
            xSemaphoreGive(g_bus_mutex);
        }
    }
    cJSON_AddItemToObject(bus1, "actuators", bus1_actuators);
    cJSON_AddItemToObject(bus1, "baumer", bus1_baumer);
    cJSON_AddItemToObject(root, "bus1", bus1);

    // ---- Bus 2 (Sync): actuators only ----
    cJSON *bus2 = cJSON_CreateObject();
    cJSON *bus2_actuators = cJSON_CreateArray();

    if (g_rs485_sync == NULL || g_modbus_sync == NULL) {
        cJSON_AddStringToObject(bus2, "error", "Bus 2 not initialized");
    } else {
        if (xSemaphoreTake(g_bus_sync_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_SCAN_MS)) != pdTRUE) {
            cJSON_AddStringToObject(bus2, "error", "Bus 2 busy");
        } else {
            int original_baud = rs485_get_baud(g_rs485_sync);

            for (int b = 0; b < SCAN_BAUD_COUNT; b++) {
                int baud = SCAN_BAUD_RATES[b];
                esp_err_t ret = rs485_set_baud(g_rs485_sync, baud);
                if (ret != ESP_OK) continue;
                vTaskDelay(pdMS_TO_TICKS(50));

                // Scan IDs 1-10 for mightyZAP actuators
                for (uint8_t id = 1; id <= 10; id++) {
                    uint16_t model = 0;
                    ret = modbus_read_holding_registers(g_modbus_sync, id,
                                                         MZAP_REG_MODEL_NUMBER, 1, &model);
                    if (ret == ESP_OK && model > 100) {
                        ESP_LOGI(TAG, "Bus2: Found actuator ID=%d, baud=%d, model=%u", id, baud, model);
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddNumberToObject(item, "id", id);
                        cJSON_AddNumberToObject(item, "baud_rate", baud);
                        cJSON_AddNumberToObject(item, "model", model);
                        cJSON_AddItemToArray(bus2_actuators, item);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            // Restore baud
            rs485_set_baud(g_rs485_sync, original_baud);
            xSemaphoreGive(g_bus_sync_mutex);
        }
    }
    cJSON_AddItemToObject(bus2, "actuators", bus2_actuators);
    cJSON_AddItemToObject(root, "bus2", bus2);

    // Restore log levels and retry count
    esp_log_level_set("RS485", ESP_LOG_WARN);
    esp_log_level_set("MODBUS", ESP_LOG_WARN);
    modbus_set_retry_count(-1);

    cJSON_AddBoolToObject(root, "success", true);
    return send_json(req, root);
}

// ============================================================================
// POST /api/actuator/smart-scan - Multi-baud scan (legacy, Bus 1 only)
// ============================================================================

esp_err_t api_actuator_smart_scan_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_SCAN_MS)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Bus busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (g_rs485 == NULL || g_modbus == NULL) {
        xSemaphoreGive(g_bus_mutex);
        cJSON *err_obj = cJSON_CreateObject();
        if (err_obj == NULL) return send_json(req, NULL);
        cJSON_AddBoolToObject(err_obj, "success", false);
        cJSON_AddStringToObject(err_obj, "error", "RS485/Modbus not initialized");
        return send_json(req, err_obj);
    }

    // Save original baud rate to restore later
    int original_baud = rs485_get_baud(g_rs485);

    cJSON *root = cJSON_CreateObject();
    cJSON *actuators = cJSON_CreateArray();

    ESP_LOGI(TAG, "Starting multi-baud smart scan...");

    // Suppress noisy logs and disable retries during scan
    esp_log_level_set("RS485", ESP_LOG_ERROR);
    esp_log_level_set("MODBUS", ESP_LOG_ERROR);
    modbus_set_retry_count(0);

    int total_found = 0;

    for (int b = 0; b < SCAN_BAUD_COUNT; b++) {
        int baud = SCAN_BAUD_RATES[b];

        ESP_LOGI(TAG, "Scanning at %d baud...", baud);

        // Change UART baud rate
        esp_err_t ret = rs485_set_baud(g_rs485, baud);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set baud %d, skipping", baud);
            continue;
        }

        // Small delay for UART to settle
        vTaskDelay(pdMS_TO_TICKS(50));

        // Scan IDs 1-3
        for (uint8_t id = 1; id <= 3; id++) {
            uint16_t model = 0;
            ret = modbus_read_holding_registers(g_modbus, id,
                                                 MZAP_REG_MODEL_NUMBER, 1, &model);

            if (ret == ESP_OK && model > 100) {
                ESP_LOGI(TAG, "Found actuator: ID=%d, baud=%d, model=%u", id, baud, model);

                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", id);
                cJSON_AddNumberToObject(item, "baud_rate", baud);
                cJSON_AddNumberToObject(item, "model", model);
                cJSON_AddStringToObject(item, "protocol", "modbus");
                cJSON_AddItemToArray(actuators, item);
                total_found++;
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Restore original baud rate
    rs485_set_baud(g_rs485, original_baud);

    // Restore log levels and retry count
    esp_log_level_set("RS485", ESP_LOG_WARN);
    esp_log_level_set("MODBUS", ESP_LOG_WARN);
    modbus_set_retry_count(-1);

    ESP_LOGI(TAG, "Smart scan complete: found %d actuator(s)", total_found);

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddItemToObject(root, "actuators", actuators);
    cJSON_AddNumberToObject(root, "count", total_found);

    xSemaphoreGive(g_bus_mutex);

    return send_json(req, root);
}

// ============================================================================
// GET /api/actuator/roles - Get current role mapping
// ============================================================================

esp_err_t api_actuator_roles_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);
    cJSON_AddBoolToObject(root, "success", true);

    config_role_t la = config_get_role_lens_a();
    config_role_t lb = config_get_role_lens_b();
    config_role_t nz = config_get_role_nozzle();

    cJSON *lens_a = cJSON_CreateObject();
    cJSON_AddNumberToObject(lens_a, "id", la.id);
    cJSON_AddNumberToObject(lens_a, "baud", la.baud);
    cJSON_AddItemToObject(root, "lens_a", lens_a);

    cJSON *lens_b = cJSON_CreateObject();
    cJSON_AddNumberToObject(lens_b, "id", lb.id);
    cJSON_AddNumberToObject(lens_b, "baud", lb.baud);
    cJSON_AddItemToObject(root, "lens_b", lens_b);

    cJSON *nozzle = cJSON_CreateObject();
    cJSON_AddNumberToObject(nozzle, "id", nz.id);
    cJSON_AddNumberToObject(nozzle, "baud", nz.baud);
    cJSON_AddItemToObject(root, "nozzle", nozzle);

    return send_json(req, root);
}

// ============================================================================
// POST /api/actuator/roles - Save role mapping
// ============================================================================

esp_err_t api_actuator_roles_post_handler(httpd_req_t *req)
{
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
    cJSON *item;

    cJSON *la = cJSON_GetObjectItem(root, "lens_a");
    if (la && cJSON_IsObject(la)) {
        uint8_t id = 1;
        uint32_t baud = 57600;
        if ((item = cJSON_GetObjectItem(la, "id")) && cJSON_IsNumber(item)) id = item->valueint;
        if ((item = cJSON_GetObjectItem(la, "baud")) && cJSON_IsNumber(item)) baud = item->valueint;
        config_set_role_lens_a(id, baud);
    }

    cJSON *lb = cJSON_GetObjectItem(root, "lens_b");
    if (lb && cJSON_IsObject(lb)) {
        uint8_t id = 2;
        uint32_t baud = 57600;
        if ((item = cJSON_GetObjectItem(lb, "id")) && cJSON_IsNumber(item)) id = item->valueint;
        if ((item = cJSON_GetObjectItem(lb, "baud")) && cJSON_IsNumber(item)) baud = item->valueint;
        config_set_role_lens_b(id, baud);
    }

    cJSON *nz = cJSON_GetObjectItem(root, "nozzle");
    if (nz && cJSON_IsObject(nz)) {
        uint8_t id = 3;
        uint32_t baud = 57600;
        if ((item = cJSON_GetObjectItem(nz, "id")) && cJSON_IsNumber(item)) id = item->valueint;
        if ((item = cJSON_GetObjectItem(nz, "baud")) && cJSON_IsNumber(item)) baud = item->valueint;
        config_set_role_nozzle(id, baud);
    }

    config_save();

    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Roles saved");

    cJSON_Delete(root);
    return send_json(req, response);
}

// ============================================================================
// POST /api/actuator/jog - Jog actuator for identification
// ============================================================================

esp_err_t api_actuator_jog_handler(httpd_req_t *req)
{
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
    bool bus_locked = false;

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *baud_json = cJSON_GetObjectItem(root, "baud_rate");

    if (!cJSON_IsNumber(id_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing actuator ID");
        goto cleanup;
    }

    uint8_t act_id = id_json->valueint;
    int target_baud = cJSON_IsNumber(baud_json) ? baud_json->valueint : 0;

    // Select bus (default Bus 1)
    cJSON *bus_json = cJSON_GetObjectItem(root, "bus");
    int bus_num = cJSON_IsNumber(bus_json) ? bus_json->valueint : 1;

    rs485_handle_t rs485_h = (bus_num == 2) ? g_rs485_sync : g_rs485;
    modbus_handle_t modbus_h = (bus_num == 2) ? g_modbus_sync : g_modbus;
    SemaphoreHandle_t bus_mtx = (bus_num == 2) ? g_bus_sync_mutex : g_bus_mutex;

    if (rs485_h == NULL || modbus_h == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "RS485/Modbus not initialized for selected bus");
        goto cleanup;
    }

    if (xSemaphoreTake(bus_mtx, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_CONTROL_MS)) != pdTRUE) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Bus busy");
        goto cleanup;
    }
    bus_locked = true;

    {
        // Save current baud, switch if needed
        int original_baud = rs485_get_baud(rs485_h);
        if (target_baud > 0 && target_baud != original_baud) {
            rs485_set_baud(rs485_h, target_baud);
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Read current position
        uint16_t cur_pos = 0;
        esp_err_t err = modbus_read_holding_registers(modbus_h, act_id,
                                                       MZAP_REG_PRESENT_POSITION, 1, &cur_pos);
        if (err != ESP_OK) {
            // Restore baud
            if (target_baud > 0 && target_baud != original_baud) {
                rs485_set_baud(rs485_h, original_baud);
            }
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Cannot read actuator position");
            goto cleanup;
        }

        // Enable force
        modbus_write_single_register(modbus_h, act_id, MZAP_REG_FORCE_ON_OFF, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        // Set speed
        modbus_write_single_register(modbus_h, act_id, MZAP_REG_GOAL_SPEED, 300);

        // Move +200 from current position (clamped to max)
        uint16_t jog_pos = (cur_pos + 200 > MZAP_MAX_POSITION) ? cur_pos - 200 : cur_pos + 200;
        modbus_write_single_register(modbus_h, act_id, MZAP_REG_GOAL_POSITION, jog_pos);

        // Wait for movement
        vTaskDelay(pdMS_TO_TICKS(800));

        // Move back
        modbus_write_single_register(modbus_h, act_id, MZAP_REG_GOAL_POSITION, cur_pos);
        vTaskDelay(pdMS_TO_TICKS(800));

        // Restore baud
        if (target_baud > 0 && target_baud != original_baud) {
            rs485_set_baud(rs485_h, original_baud);
        }

        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "Jog complete");
    }

cleanup:
    if (bus_locked) xSemaphoreGive(bus_mtx);
    cJSON_Delete(root);
    return send_json(req, response);
}

// ============================================================================
// POST /api/actuator/standardize - Reconfigure all actuators to same baud/IDs
// ============================================================================

esp_err_t api_actuator_standardize_handler(httpd_req_t *req)
{
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
    bool bus_locked = false;

    if (g_rs485 == NULL || g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "RS485/Modbus not initialized");
        goto cleanup;
    }

    if (xSemaphoreTake(g_bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_SCAN_MS)) != pdTRUE) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Bus busy");
        goto cleanup;
    }
    bus_locked = true;

    {
        cJSON *target_baud_json = cJSON_GetObjectItem(root, "target_baud");
        uint32_t target_baud = cJSON_IsNumber(target_baud_json) ? target_baud_json->valueint : 57600;
        uint8_t target_baud_code = mzap_rate_to_baud_code(target_baud);

        // Parse actuators to standardize: array of {id, baud_rate, new_id}
        cJSON *acts = cJSON_GetObjectItem(root, "actuators");
        if (!cJSON_IsArray(acts)) {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Missing actuators array");
            goto cleanup;
        }

        cJSON *results = cJSON_CreateArray();
        int success_count = 0;
        int act_count = cJSON_GetArraySize(acts);

        // We need both bus mutexes since actuators can be on either bus
        // Bus 1 mutex is already held. Take Bus 2 mutex as well.
        bool bus2_locked = false;
        if (g_rs485_sync != NULL && g_modbus_sync != NULL) {
            if (xSemaphoreTake(g_bus_sync_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_SCAN_MS)) == pdTRUE) {
                bus2_locked = true;
            }
        }

        for (int i = 0; i < act_count; i++) {
            cJSON *act = cJSON_GetArrayItem(acts, i);
            cJSON *id_j = cJSON_GetObjectItem(act, "id");
            cJSON *baud_j = cJSON_GetObjectItem(act, "baud_rate");
            cJSON *new_id_j = cJSON_GetObjectItem(act, "new_id");
            cJSON *bus_j = cJSON_GetObjectItem(act, "bus");

            if (!cJSON_IsNumber(id_j) || !cJSON_IsNumber(new_id_j)) continue;

            uint8_t old_id = id_j->valueint;
            uint8_t new_id = new_id_j->valueint;
            int act_baud = cJSON_IsNumber(baud_j) ? baud_j->valueint : (int)target_baud;
            int bus_num = cJSON_IsNumber(bus_j) ? bus_j->valueint : 1;

            // Select the correct RS485/Modbus handles based on bus
            rs485_handle_t rs485_h = (bus_num == 2) ? g_rs485_sync : g_rs485;
            modbus_handle_t modbus_h = (bus_num == 2) ? g_modbus_sync : g_modbus;

            cJSON *result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "old_id", old_id);
            cJSON_AddNumberToObject(result, "new_id", new_id);
            cJSON_AddNumberToObject(result, "bus", bus_num);

            if (rs485_h == NULL || modbus_h == NULL) {
                cJSON_AddStringToObject(result, "status", "bus_not_available");
                cJSON_AddItemToArray(results, result);
                continue;
            }

            if (bus_num == 2 && !bus2_locked) {
                cJSON_AddStringToObject(result, "status", "bus2_busy");
                cJSON_AddItemToArray(results, result);
                continue;
            }

            // Switch to actuator's current baud
            rs485_set_baud(rs485_h, act_baud);
            vTaskDelay(pdMS_TO_TICKS(50));

            esp_err_t err;
            bool changed_something = false;

            // Change baud rate first (if needed)
            if ((uint32_t)act_baud != target_baud) {
                err = modbus_write_single_register(modbus_h, old_id,
                                                    MZAP_REG_BAUD_RATE, target_baud_code);
                if (err != ESP_OK) {
                    cJSON_AddStringToObject(result, "status", "baud_change_failed");
                    cJSON_AddItemToArray(results, result);
                    continue;
                }
                changed_something = true;

                // After changing baud, switch UART to new baud to continue communication
                rs485_set_baud(rs485_h, target_baud);
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            // Change ID (if needed)
            if (old_id != new_id) {
                err = modbus_write_single_register(modbus_h, old_id,
                                                    MZAP_REG_ID, new_id);
                if (err != ESP_OK) {
                    cJSON_AddStringToObject(result, "status", "id_change_failed");
                    cJSON_AddItemToArray(results, result);
                    continue;
                }
                changed_something = true;
            }

            if (changed_something) {
                // Restart actuator to apply EEPROM changes
                uint8_t restart_id = (old_id != new_id) ? new_id : old_id;
                rs485_set_baud(rs485_h, target_baud);
                vTaskDelay(pdMS_TO_TICKS(50));
                modbus_write_single_register(modbus_h, restart_id,
                                              MZAP_REG_RESTART, 1);
                // Wait for restart
                vTaskDelay(pdMS_TO_TICKS(1500));
            }

            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddItemToArray(results, result);
            success_count++;
        }

        // Restore UARTs to target baud
        rs485_set_baud(g_rs485, target_baud);
        if (bus2_locked) {
            rs485_set_baud(g_rs485_sync, target_baud);
            xSemaphoreGive(g_bus_sync_mutex);
        }

        // Update RS485 config to match new baud
        config_set_rs485_baud(target_baud);
        config_save();

        cJSON_AddBoolToObject(response, "success", success_count == act_count);
        cJSON_AddStringToObject(response, "message",
                                success_count == act_count ? "Standardization complete" : "Some actuators failed");
        cJSON_AddItemToObject(response, "results", results);
        cJSON_AddNumberToObject(response, "success_count", success_count);
        cJSON_AddNumberToObject(response, "total", act_count);
    }

cleanup:
    if (bus_locked) xSemaphoreGive(g_bus_mutex);
    cJSON_Delete(root);
    return send_json(req, response);
}
