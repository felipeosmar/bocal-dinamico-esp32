/**
 * @file handlers_common.c
 * @brief Common HTTP response helpers shared across all API handlers
 */

#include "handlers.h"

static const char *TAG = "API";

esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON allocation failed (OOM)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (OOM)");
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

esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"success\":false,\"error\":\"%s\"}", msg);
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

esp_err_t send_success_json(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"success\":true,\"message\":\"%s\"}", msg);
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}
