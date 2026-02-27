/**
 * @file web_server.c
 * @brief HTTP Web Server - Core initialization and route registration
 *
 * Handlers are split into separate files in handlers/ directory.
 */

#include "web_server.h"
#include "handlers/handlers.h"

// From web_baumer.c
extern void register_baumer_handlers(httpd_handle_t server);
// From web_control.c
extern void register_control_handlers(httpd_handle_t server);

static const char *TAG = "WEB_SRV";

// Server handle
static httpd_handle_t s_server = NULL;
static bool s_running = false;

// Shared configuration (accessed by handlers via extern)
web_server_config_t g_web_config;

// ============================================================================
// Default Configuration
// ============================================================================

void web_server_get_default_config(web_server_config_t *config) {
  if (config == NULL)
    return;

  memset(config, 0, sizeof(*config));
  config->port = 80;
  strncpy(config->username, "admin", sizeof(config->username) - 1);
  strncpy(config->password, "admin", sizeof(config->password) - 1);
  config->auth_enabled = false;
}

// ============================================================================
// Server Initialization
// ============================================================================

esp_err_t web_server_init(const web_server_config_t *config) {
  if (s_running) {
    ESP_LOGW(TAG, "Already running");
    return ESP_OK;
  }

  // Initialize LittleFS for www partition (web interface files)
  esp_vfs_littlefs_conf_t www_conf = {.base_path = "/www",
                                      .partition_label = "www",
                                      .format_if_mount_failed = false,
                                      .dont_mount = false};

  esp_err_t ret = esp_vfs_littlefs_register(&www_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount www partition: %s", esp_err_to_name(ret));
    return ret;
  }

  size_t total = 0, used = 0;
  esp_littlefs_info("www", &total, &used);
  ESP_LOGI(TAG, "LittleFS www: total=%d, used=%d", total, used);

  // Store configuration
  if (config) {
    memcpy(&g_web_config, config, sizeof(web_server_config_t));
  } else {
    web_server_get_default_config(&g_web_config);
  }

  // Configure HTTP server
  // NOTE: HTTPD_DEFAULT_CONFIG uses a single task to process requests.
  // Handlers rely on this for thread safety (e.g. s_actuators[] in
  // api_actuator.c). Do NOT increase max_open_sockets or add worker tasks
  // without adding mutexes.
  httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
  http_config.server_port = g_web_config.port;
  http_config.max_uri_handlers = 128;
  http_config.stack_size = 8192;

  ESP_LOGI(TAG, "Starting server on port %d", http_config.server_port);

  ret = httpd_start(&s_server, &http_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
    return ret;
  }

  // ========================================================================
  // Register Routes - Static Files
  // ========================================================================

  httpd_uri_t index_uri = {
      .uri = "/", .method = HTTP_GET, .handler = index_handler};
  httpd_uri_t css_uri = {
      .uri = "/style.css", .method = HTTP_GET, .handler = css_handler};
  httpd_uri_t js_uri = {
      .uri = "/core.js", .method = HTTP_GET, .handler = js_handler};
  httpd_uri_t favicon_uri = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};

  httpd_register_uri_handler(s_server, &index_uri);
  httpd_register_uri_handler(s_server, &css_uri);
  httpd_register_uri_handler(s_server, &js_uri);
  httpd_register_uri_handler(s_server, &favicon_uri);

  // Tabs HTML
  httpd_uri_t tabs_actuators_html = {.uri = "/tabs/actuators.html",
                                     .method = HTTP_GET,
                                     .handler = tabs_html_handler};
  httpd_uri_t tabs_system_html = {.uri = "/tabs/system.html",
                                  .method = HTTP_GET,
                                  .handler = tabs_html_handler};
  httpd_uri_t tabs_config_html = {.uri = "/tabs/config.html",
                                  .method = HTTP_GET,
                                  .handler = tabs_html_handler};
  httpd_uri_t tabs_files_html = {.uri = "/tabs/files.html",
                                 .method = HTTP_GET,
                                 .handler = tabs_html_handler};
  httpd_register_uri_handler(s_server, &tabs_actuators_html);
  httpd_register_uri_handler(s_server, &tabs_system_html);
  httpd_register_uri_handler(s_server, &tabs_config_html);
  httpd_register_uri_handler(s_server, &tabs_files_html);

  httpd_uri_t tabs_setup_html = {.uri = "/tabs/setup.html",
                                 .method = HTTP_GET,
                                 .handler = tabs_html_handler};
  httpd_uri_t tabs_profiler_html = {.uri = "/tabs/profiler.html",
                                    .method = HTTP_GET,
                                    .handler = tabs_html_handler};
  httpd_register_uri_handler(s_server, &tabs_setup_html);
  httpd_register_uri_handler(s_server, &tabs_profiler_html);

  // Tabs JS
  httpd_uri_t tabs_actuators_js = {.uri = "/tabs/actuators.js",
                                   .method = HTTP_GET,
                                   .handler = tabs_js_handler};
  httpd_uri_t tabs_system_js = {
      .uri = "/tabs/system.js", .method = HTTP_GET, .handler = tabs_js_handler};
  httpd_uri_t tabs_config_js = {
      .uri = "/tabs/config.js", .method = HTTP_GET, .handler = tabs_js_handler};
  httpd_uri_t tabs_files_js = {
      .uri = "/tabs/files.js", .method = HTTP_GET, .handler = tabs_js_handler};
  httpd_register_uri_handler(s_server, &tabs_actuators_js);
  httpd_register_uri_handler(s_server, &tabs_system_js);
  httpd_register_uri_handler(s_server, &tabs_config_js);
  httpd_register_uri_handler(s_server, &tabs_files_js);

  httpd_uri_t tabs_setup_js = {
      .uri = "/tabs/setup.js", .method = HTTP_GET, .handler = tabs_js_handler};
  httpd_uri_t tabs_profiler_js = {.uri = "/tabs/profiler.js",
                                  .method = HTTP_GET,
                                  .handler = tabs_js_handler};
  httpd_register_uri_handler(s_server, &tabs_setup_js);
  httpd_register_uri_handler(s_server, &tabs_profiler_js);

  // ========================================================================
  // Register Routes - API: File Manager
  // ========================================================================

  httpd_uri_t files_list = {.uri = "/api/files/list",
                            .method = HTTP_GET,
                            .handler = api_files_list_handler};
  httpd_uri_t files_info = {.uri = "/api/files/info",
                            .method = HTTP_GET,
                            .handler = api_files_info_handler};
  httpd_uri_t files_download = {.uri = "/api/files/download",
                                .method = HTTP_GET,
                                .handler = api_files_download_handler};
  httpd_uri_t files_view = {.uri = "/api/files/view",
                            .method = HTTP_GET,
                            .handler = api_files_view_handler};
  httpd_uri_t files_read = {.uri = "/api/files/read",
                            .method = HTTP_GET,
                            .handler = api_files_read_handler};
  httpd_uri_t files_write = {.uri = "/api/files/write",
                             .method = HTTP_POST,
                             .handler = api_files_write_handler};
  httpd_uri_t files_delete = {.uri = "/api/files/delete",
                              .method = HTTP_POST,
                              .handler = api_files_delete_handler};
  httpd_uri_t files_mkdir = {.uri = "/api/files/mkdir",
                             .method = HTTP_POST,
                             .handler = api_files_mkdir_handler};
  httpd_uri_t files_upload = {.uri = "/api/files/upload",
                              .method = HTTP_POST,
                              .handler = api_files_upload_handler};

  httpd_register_uri_handler(s_server, &files_list);
  httpd_register_uri_handler(s_server, &files_info);
  httpd_register_uri_handler(s_server, &files_download);
  httpd_register_uri_handler(s_server, &files_view);
  httpd_register_uri_handler(s_server, &files_read);
  httpd_register_uri_handler(s_server, &files_write);
  httpd_register_uri_handler(s_server, &files_delete);
  httpd_register_uri_handler(s_server, &files_mkdir);
  httpd_register_uri_handler(s_server, &files_upload);

  // ========================================================================
  // Register Routes - API: System
  // ========================================================================

  httpd_uri_t status = {
      .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler};
  httpd_uri_t tasks = {
      .uri = "/api/tasks", .method = HTTP_GET, .handler = api_tasks_handler};
  httpd_uri_t restart = {.uri = "/api/restart",
                         .method = HTTP_POST,
                         .handler = api_restart_handler};
  httpd_uri_t logs = {
      .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler};
  httpd_uri_t logs_clear = {.uri = "/api/logs/clear",
                            .method = HTTP_POST,
                            .handler = api_logs_clear_handler};

  httpd_register_uri_handler(s_server, &status);
  httpd_register_uri_handler(s_server, &tasks);
  httpd_register_uri_handler(s_server, &restart);
  httpd_register_uri_handler(s_server, &logs);
  httpd_register_uri_handler(s_server, &logs_clear);

  // ========================================================================
  // Register Routes - API: WiFi
  // ========================================================================

  httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan",
                           .method = HTTP_GET,
                           .handler = api_wifi_scan_handler};
  httpd_uri_t wifi_connect = {.uri = "/api/wifi/connect",
                              .method = HTTP_POST,
                              .handler = api_wifi_connect_handler};
  httpd_uri_t wifi_status = {.uri = "/api/wifi/status",
                             .method = HTTP_GET,
                             .handler = api_wifi_status_handler};

  httpd_register_uri_handler(s_server, &wifi_scan);
  httpd_register_uri_handler(s_server, &wifi_connect);
  httpd_register_uri_handler(s_server, &wifi_status);

  // ========================================================================
  // Register Routes - API: RS485
  // ========================================================================

  httpd_uri_t rs485_config_get = {.uri = "/api/rs485/config",
                                  .method = HTTP_GET,
                                  .handler = api_rs485_config_handler};
  httpd_uri_t rs485_config_post = {.uri = "/api/rs485/config",
                                   .method = HTTP_POST,
                                   .handler = api_rs485_config_handler};
  httpd_uri_t rs485_diag = {.uri = "/api/rs485/diag",
                            .method = HTTP_GET,
                            .handler = api_rs485_diag_handler};
  httpd_uri_t rs485_test = {.uri = "/api/rs485/test",
                            .method = HTTP_POST,
                            .handler = api_rs485_test_handler};
  httpd_uri_t rs485_reset = {.uri = "/api/rs485/reset_stats",
                             .method = HTTP_POST,
                             .handler = api_rs485_reset_stats_handler};

  httpd_register_uri_handler(s_server, &rs485_config_get);
  httpd_register_uri_handler(s_server, &rs485_config_post);
  httpd_register_uri_handler(s_server, &rs485_diag);
  httpd_register_uri_handler(s_server, &rs485_test);
  httpd_register_uri_handler(s_server, &rs485_reset);

  // ========================================================================
  // Register Routes - API: Actuator
  // ========================================================================

  httpd_uri_t act_status = {.uri = "/api/actuator/status",
                            .method = HTTP_GET,
                            .handler = api_actuator_status_handler};
  httpd_uri_t act_control = {.uri = "/api/actuator/control",
                             .method = HTTP_POST,
                             .handler = api_actuator_control_handler};
  httpd_uri_t act_scan = {.uri = "/api/actuator/scan",
                          .method = HTTP_GET,
                          .handler = api_actuator_scan_handler};
  httpd_uri_t act_add = {.uri = "/api/actuator/add",
                         .method = HTTP_POST,
                         .handler = api_actuator_add_handler};
  httpd_uri_t act_remove = {.uri = "/api/actuator/remove",
                            .method = HTTP_POST,
                            .handler = api_actuator_remove_handler};
  httpd_uri_t act_set_name = {.uri = "/api/actuator/set-name",
                              .method = HTTP_POST,
                              .handler = api_actuator_set_name_handler};
  httpd_uri_t act_config_get = {.uri = "/api/actuator/config",
                                .method = HTTP_GET,
                                .handler = api_actuator_config_get_handler};
  httpd_uri_t act_config_post = {.uri = "/api/actuator/config",
                                 .method = HTTP_POST,
                                 .handler = api_actuator_config_post_handler};
  httpd_uri_t act_restart = {.uri = "/api/actuator/restart",
                             .method = HTTP_POST,
                             .handler = api_actuator_restart_handler};
  httpd_uri_t act_factory = {.uri = "/api/actuator/factory-reset",
                             .method = HTTP_POST,
                             .handler = api_actuator_factory_reset_handler};
  httpd_uri_t act_sync_move = {.uri = "/api/actuator/sync-move",
                               .method = HTTP_POST,
                               .handler = api_actuator_sync_move_handler};
  httpd_uri_t act_sync_status = {.uri = "/api/actuator/sync-status",
                                 .method = HTTP_GET,
                                 .handler = api_actuator_sync_status_handler};

  httpd_register_uri_handler(s_server, &act_status);
  httpd_register_uri_handler(s_server, &act_control);
  httpd_register_uri_handler(s_server, &act_scan);
  httpd_register_uri_handler(s_server, &act_add);
  httpd_register_uri_handler(s_server, &act_remove);
  httpd_register_uri_handler(s_server, &act_set_name);
  httpd_register_uri_handler(s_server, &act_config_get);
  httpd_register_uri_handler(s_server, &act_config_post);
  httpd_register_uri_handler(s_server, &act_restart);
  httpd_register_uri_handler(s_server, &act_factory);
  httpd_uri_t act_sync_abort = {.uri = "/api/actuator/sync-abort",
                               .method = HTTP_POST,
                               .handler = api_actuator_sync_abort_handler};
  httpd_register_uri_handler(s_server, &act_sync_move);
  httpd_register_uri_handler(s_server, &act_sync_status);
  httpd_register_uri_handler(s_server, &act_sync_abort);

  // ========================================================================
  // Register Routes - API: Setup Wizard
  // ========================================================================

  httpd_uri_t setup_smart_scan = {.uri = "/api/actuator/smart-scan",
                                  .method = HTTP_POST,
                                  .handler = api_actuator_smart_scan_handler};
  httpd_uri_t setup_scan_buses = {.uri = "/api/setup/scan-buses",
                                  .method = HTTP_POST,
                                  .handler = api_setup_scan_buses_handler};
  httpd_uri_t setup_roles_get = {.uri = "/api/actuator/roles",
                                 .method = HTTP_GET,
                                 .handler = api_actuator_roles_get_handler};
  httpd_uri_t setup_roles_post = {.uri = "/api/actuator/roles",
                                  .method = HTTP_POST,
                                  .handler = api_actuator_roles_post_handler};
  httpd_uri_t setup_jog = {.uri = "/api/actuator/jog",
                           .method = HTTP_POST,
                           .handler = api_actuator_jog_handler};
  httpd_uri_t setup_standardize = {.uri = "/api/actuator/standardize",
                                   .method = HTTP_POST,
                                   .handler = api_actuator_standardize_handler};

  httpd_register_uri_handler(s_server, &setup_smart_scan);
  httpd_register_uri_handler(s_server, &setup_scan_buses);
  httpd_register_uri_handler(s_server, &setup_roles_get);
  httpd_register_uri_handler(s_server, &setup_roles_post);
  httpd_register_uri_handler(s_server, &setup_jog);
  httpd_register_uri_handler(s_server, &setup_standardize);

  // ========================================================================
  // Register Routes - API: Baumer & Control Loop
  // ========================================================================

  register_baumer_handlers(s_server);
  register_control_handlers(s_server);

  // ========================================================================
  // Initialization Complete
  // ========================================================================

  s_running = true;
  ESP_LOGI(TAG, "Web server started");

  // Load saved actuators from config
  actuator_handlers_init();

  return ESP_OK;
}

// ============================================================================
// Server Control
// ============================================================================

void web_server_deinit(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  esp_vfs_littlefs_unregister("www");
  s_running = false;
}

bool web_server_is_running(void) { return s_running; }

esp_err_t web_server_set_auth(const char *username, const char *password) {
  if (username) {
    strncpy(g_web_config.username, username, sizeof(g_web_config.username) - 1);
    g_web_config.username[sizeof(g_web_config.username) - 1] = '\0';
  }
  if (password) {
    strncpy(g_web_config.password, password, sizeof(g_web_config.password) - 1);
    g_web_config.password[sizeof(g_web_config.password) - 1] = '\0';
  }
  return ESP_OK;
}
