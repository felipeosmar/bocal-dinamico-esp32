/**
 * @file handlers.h
 * @brief Common definitions for all HTTP request handlers
 */

#ifndef HANDLERS_H
#define HANDLERS_H

#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "config_manager.h"
#include "rs485_driver.h"
#include "modbus_rtu.h"
#include "mightyzap.h"
#include "log_buffer.h"
#include "web_server.h"
#include "freertos/semphr.h"

// ============================================================================
// JSON response helper
// ============================================================================

/**
 * @brief Send a cJSON object as HTTP response, handling NULL and cleanup.
 *
 * If @p root is NULL (OOM), sends a 500 error. Otherwise prints to string,
 * sends the response, and frees both the string and the cJSON tree.
 *
 * @param req  HTTP request handle
 * @param root cJSON object to send (consumed — caller must NOT cJSON_Delete)
 * @return ESP_OK on success, ESP_FAIL on NULL root
 */
static inline esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        ESP_LOGE("JSON", "cJSON allocation failed (OOM)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        ESP_LOGE("JSON", "cJSON_PrintUnformatted failed (OOM)");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// External globals (from main.c)
// ============================================================================

extern rs485_handle_t g_rs485;
extern modbus_handle_t g_modbus;
extern mightyzap_handle_t g_actuator;

/** @brief Global RS485 bus mutex — take before any bus operation from API handlers */
extern SemaphoreHandle_t g_bus_mutex;

// ============================================================================
// Shared state (from web_server.c)
// ============================================================================

// Server configuration (set during init)
extern web_server_config_t g_web_config;

// ============================================================================
// Authentication (auth.c)
// ============================================================================

/**
 * @brief Check Basic Auth credentials
 * @param req HTTP request
 * @return true if authenticated or auth disabled
 */
bool check_auth(httpd_req_t *req);

/**
 * @brief Send 401 Unauthorized response
 */
esp_err_t send_unauthorized(httpd_req_t *req);

/**
 * @brief Macro to require authentication at the top of API handlers.
 * Returns 401 Unauthorized if auth check fails.
 */
#define REQUIRE_AUTH(req) do { if (!check_auth(req)) return send_unauthorized(req); } while(0)

// ============================================================================
// Static file handlers (static_files.c)
// ============================================================================

esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type);
esp_err_t index_handler(httpd_req_t *req);
esp_err_t css_handler(httpd_req_t *req);
esp_err_t js_handler(httpd_req_t *req);
esp_err_t tabs_html_handler(httpd_req_t *req);
esp_err_t tabs_js_handler(httpd_req_t *req);
esp_err_t favicon_handler(httpd_req_t *req);

// ============================================================================
// API - Files (api_files.c)
// ============================================================================

esp_err_t api_files_list_handler(httpd_req_t *req);
esp_err_t api_files_info_handler(httpd_req_t *req);
esp_err_t api_files_download_handler(httpd_req_t *req);
esp_err_t api_files_view_handler(httpd_req_t *req);
esp_err_t api_files_read_handler(httpd_req_t *req);
esp_err_t api_files_write_handler(httpd_req_t *req);
esp_err_t api_files_delete_handler(httpd_req_t *req);
esp_err_t api_files_mkdir_handler(httpd_req_t *req);
esp_err_t api_files_upload_handler(httpd_req_t *req);

// ============================================================================
// API - System (api_system.c)
// ============================================================================

esp_err_t api_status_handler(httpd_req_t *req);
esp_err_t api_tasks_handler(httpd_req_t *req);
esp_err_t api_restart_handler(httpd_req_t *req);
esp_err_t api_logs_handler(httpd_req_t *req);
esp_err_t api_logs_clear_handler(httpd_req_t *req);

// ============================================================================
// API - WiFi (api_wifi.c)
// ============================================================================

esp_err_t api_wifi_scan_handler(httpd_req_t *req);
esp_err_t api_wifi_connect_handler(httpd_req_t *req);
esp_err_t api_wifi_status_handler(httpd_req_t *req);

// ============================================================================
// API - Actuator (api_actuator.c)
// ============================================================================

/**
 * @brief Load saved actuators from config (call during server init)
 */
void actuator_handlers_init(void);

esp_err_t api_actuator_status_handler(httpd_req_t *req);
esp_err_t api_actuator_control_handler(httpd_req_t *req);
esp_err_t api_actuator_config_get_handler(httpd_req_t *req);
esp_err_t api_actuator_config_post_handler(httpd_req_t *req);
esp_err_t api_actuator_restart_handler(httpd_req_t *req);
esp_err_t api_actuator_factory_reset_handler(httpd_req_t *req);
esp_err_t api_actuator_scan_handler(httpd_req_t *req);
esp_err_t api_actuator_add_handler(httpd_req_t *req);
esp_err_t api_actuator_remove_handler(httpd_req_t *req);
esp_err_t api_actuator_set_name_handler(httpd_req_t *req);
esp_err_t api_actuator_sync_move_handler(httpd_req_t *req);
esp_err_t api_actuator_sync_status_handler(httpd_req_t *req);

// ============================================================================
// API - Setup Wizard (api_setup.c)
// ============================================================================

esp_err_t api_actuator_smart_scan_handler(httpd_req_t *req);
esp_err_t api_actuator_roles_get_handler(httpd_req_t *req);
esp_err_t api_actuator_roles_post_handler(httpd_req_t *req);
esp_err_t api_actuator_jog_handler(httpd_req_t *req);
esp_err_t api_actuator_standardize_handler(httpd_req_t *req);

// ============================================================================
// API - RS485 (api_rs485.c)
// ============================================================================

esp_err_t api_rs485_config_handler(httpd_req_t *req);
esp_err_t api_rs485_diag_handler(httpd_req_t *req);
esp_err_t api_rs485_test_handler(httpd_req_t *req);
esp_err_t api_rs485_reset_stats_handler(httpd_req_t *req);

#endif // HANDLERS_H
