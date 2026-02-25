/**
 * @file auth.c
 * @brief HTTP Basic Authentication handlers
 */

#include "handlers.h"

static const char *TAG = "WEB_AUTH";

// Rate limiting for failed auth attempts
static uint32_t s_fail_count = 0;
static TickType_t s_last_fail_tick = 0;

#define AUTH_MAX_DELAY_MS  10000
#define AUTH_FAIL_THRESHOLD 3
#define AUTH_RESET_PERIOD_MS 60000

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

        // Rate limiting: progressive delay after repeated failures
        s_fail_count++;
        s_last_fail_tick = xTaskGetTickCount();

        if (s_fail_count >= AUTH_FAIL_THRESHOLD) {
            uint32_t delay_ms = 1000 * (1 << (s_fail_count - AUTH_FAIL_THRESHOLD));
            if (delay_ms > AUTH_MAX_DELAY_MS) delay_ms = AUTH_MAX_DELAY_MS;
            ESP_LOGW(TAG, "Rate limiting: delaying %lu ms (attempt %lu)",
                     (unsigned long)delay_ms, (unsigned long)s_fail_count);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    } else {
        // Reset on successful auth
        s_fail_count = 0;
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
