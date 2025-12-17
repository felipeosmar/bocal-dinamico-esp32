#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Web server configuration
 */
typedef struct {
    uint16_t port;              // HTTP port (default 80)
    const char *username;       // Basic auth username
    const char *password;       // Basic auth password
    bool auth_enabled;          // Enable authentication
} web_server_config_t;

/**
 * @brief Initialize and start web server
 * @param config Configuration (can be NULL for defaults)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_init(const web_server_config_t *config);

/**
 * @brief Stop and deinitialize web server
 */
void web_server_deinit(void);

/**
 * @brief Check if web server is running
 * @return true if running
 */
bool web_server_is_running(void);

/**
 * @brief Get default configuration
 * @param config Pointer to store config
 */
void web_server_get_default_config(web_server_config_t *config);

/**
 * @brief Update authentication credentials
 * @param username New username
 * @param password New password
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_set_auth(const char *username, const char *password);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
