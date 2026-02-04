/**
 * @file auth.c
 * @brief HTTP Basic Authentication handlers
 */

#include "handlers.h"

static const char *TAG = "WEB_AUTH";

bool check_auth(httpd_req_t *req)
{
    if (!g_web_config.auth_enabled) return true;

    char auth_header[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        ESP_LOGD(TAG, "No Authorization header");
        return false;
    }

    // Parse "Basic base64credentials"
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        ESP_LOGD(TAG, "Not Basic auth");
        return false;
    }

    // Decode base64
    const char *b64_credentials = auth_header + 6;
    size_t b64_len = strlen(b64_credentials);
    
    unsigned char decoded[128] = {0};
    size_t decoded_len = 0;
    
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                    (const unsigned char*)b64_credentials, b64_len);
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to decode base64 credentials");
        return false;
    }
    decoded[decoded_len] = '\0';

    // Build expected "username:password" string
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", g_web_config.username, g_web_config.password);

    // Secure comparison
    bool match = (strlen(expected) == decoded_len) && 
                 (memcmp(decoded, expected, decoded_len) == 0);
    
    if (!match) {
        ESP_LOGW(TAG, "Authentication failed");
    }
    
    return match;
}

esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
