/**
 * @file api_system.c
 * @brief System API handlers (/api/status, /api/tasks, /api/restart, /api/logs)
 */

#include "handlers.h"

static const char *TAG = "API_SYSTEM";

// GET /api/status
esp_err_t api_status_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);

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

    return send_json(req, root);
}

// GET /api/tasks - Get FreeRTOS task statistics
esp_err_t api_tasks_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);
    
    // System overview
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", xTaskGetTickCount() / configTICK_RATE_HZ);
    cJSON_AddNumberToObject(root, "task_count", uxTaskGetNumberOfTasks());
    
    // Get task statistics
    UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = malloc(num_tasks * sizeof(TaskStatus_t));
    
    if (task_array == NULL) {
        cJSON_AddStringToObject(root, "error", "Out of memory");
        return send_json(req, root);
    }
    
    configRUN_TIME_COUNTER_TYPE total_runtime;
    UBaseType_t tasks_returned = uxTaskGetSystemState(task_array, num_tasks, &total_runtime);
    
    cJSON *tasks = cJSON_CreateArray();
    
    for (UBaseType_t i = 0; i < tasks_returned; i++) {
        cJSON *task = cJSON_CreateObject();
        
        cJSON_AddStringToObject(task, "name", task_array[i].pcTaskName);
        cJSON_AddNumberToObject(task, "priority", task_array[i].uxCurrentPriority);
        cJSON_AddNumberToObject(task, "stack_hwm", task_array[i].usStackHighWaterMark);
        cJSON_AddNumberToObject(task, "task_num", task_array[i].xTaskNumber);
        
        // Task state
        const char *state_str;
        switch (task_array[i].eCurrentState) {
            case eRunning:  state_str = "Running"; break;
            case eReady:    state_str = "Ready"; break;
            case eBlocked:  state_str = "Blocked"; break;
            case eSuspended: state_str = "Suspended"; break;
            case eDeleted:  state_str = "Deleted"; break;
            default:        state_str = "Unknown"; break;
        }
        cJSON_AddStringToObject(task, "state", state_str);
        
        // CPU percentage (if runtime stats enabled)
        if (total_runtime > 0) {
            uint32_t cpu_percent = (uint32_t)((task_array[i].ulRunTimeCounter * 100) / total_runtime);
            cJSON_AddNumberToObject(task, "cpu_percent", cpu_percent);
            cJSON_AddNumberToObject(task, "runtime", (double)task_array[i].ulRunTimeCounter);
        } else {
            cJSON_AddNumberToObject(task, "cpu_percent", 0);
            cJSON_AddNumberToObject(task, "runtime", 0);
        }
        
        cJSON_AddItemToArray(tasks, task);
    }
    
    free(task_array);
    
    cJSON_AddItemToObject(root, "tasks", tasks);
    cJSON_AddNumberToObject(root, "total_runtime", total_runtime);
    
    return send_json(req, root);
}

// POST /api/restart
esp_err_t api_restart_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "Restarting...");

    send_json(req, root);

    // Delay before restart
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// GET /api/logs?since=N - Get logs since sequence N
esp_err_t api_logs_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[64] = {0};
    uint32_t since_sequence = 0;

    // Parse 'since' parameter
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char param[16] = {0};
        if (httpd_query_key_value(query_buf, "since", param, sizeof(param)) == ESP_OK) {
            since_sequence = (uint32_t)strtoul(param, NULL, 10);
        }
    }

    // Allocate buffer for log entries (max 50 at a time to limit response size)
    #define MAX_LOG_RESPONSE 50
    log_entry_t *entries = malloc(MAX_LOG_RESPONSE * sizeof(log_entry_t));
    if (entries == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    uint16_t count = 0;
    esp_err_t ret = log_buffer_get_since(since_sequence, entries, MAX_LOG_RESPONSE, &count);

    if (ret != ESP_OK) {
        free(entries);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get logs");
        return ESP_FAIL;
    }

    // Get buffer stats
    log_buffer_stats_t stats;
    log_buffer_get_stats(&stats);

    // Build JSON response
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "sequence", stats.current_sequence);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "total_capacity", stats.buffer_size);
    cJSON_AddNumberToObject(root, "buffer_used", stats.buffer_used);
    cJSON_AddNumberToObject(root, "dropped", stats.dropped_entries);

    cJSON *logs_array = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "seq", entries[i].sequence);
        cJSON_AddNumberToObject(entry, "ts", entries[i].timestamp_ms);
        cJSON_AddNumberToObject(entry, "lvl", entries[i].level);
        cJSON_AddStringToObject(entry, "tag", entries[i].tag);
        cJSON_AddStringToObject(entry, "msg", entries[i].message);
        cJSON_AddItemToArray(logs_array, entry);
    }
    cJSON_AddItemToObject(root, "logs", logs_array);

    free(entries);

    return send_json(req, root);
}

// POST /api/logs/clear - Clear log buffer
esp_err_t api_logs_clear_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    esp_err_t ret = log_buffer_clear();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);
    cJSON_AddBoolToObject(root, "success", ret == ESP_OK);
    cJSON_AddStringToObject(root, "message", ret == ESP_OK ? "Log buffer cleared" : "Failed to clear");

    return send_json(req, root);
}
