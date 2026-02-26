#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "baumer_ox100.h"
#include "config_manager.h"

static const char *TAG = "WEB_BAUMER";

extern baumer_handle_t g_baumer;

static const char *quality_to_string(uint8_t quality)
{
    switch (quality) {
        case BAUMER_QUALITY_OK:          return "OK";
        case BAUMER_QUALITY_WEAK_SIGNAL: return "Weak signal";
        case BAUMER_QUALITY_NO_SIGNAL:   return "No signal";
        default:                         return "Unknown";
    }
}

// GET /api/baumer/status
static esp_err_t api_baumer_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (g_baumer == NULL) {
        httpd_resp_sendstr(req, "{\"connected\":false}");
        return ESP_OK;
    }

    baumer_measurement_t m;
    bool has_data = (baumer_get_cached(g_baumer, &m) == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", has_data);
    cJSON_AddNumberToObject(root, "slave_id", config_get_baumer_slave_id());

    if (has_data) {
        cJSON_AddNumberToObject(root, "status", m.status);
        cJSON_AddNumberToObject(root, "quality", m.quality);
        cJSON_AddStringToObject(root, "quality_text", quality_to_string(m.quality));
        cJSON_AddBoolToObject(root, "valid", (m.status & BAUMER_STATUS_BIT_VALID) != 0);

        cJSON *values = cJSON_CreateArray();
        for (int i = 0; i < BAUMER_NUM_VALUES; i++) {
            cJSON_AddItemToArray(values, cJSON_CreateNumber(m.values[i]));
        }
        cJSON_AddItemToObject(root, "values", values);
    }

    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, str);
    free((void *)str);
    cJSON_Delete(root);

    return ESP_OK;
}

// GET /api/baumer/config
static esp_err_t api_baumer_config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "slave_id", config_get_baumer_slave_id());
    cJSON_AddBoolToObject(root, "enabled", config_get_baumer_enabled());

    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, str);
    free((void *)str);
    cJSON_Delete(root);

    return ESP_OK;
}

// PUT /api/baumer/config
static esp_err_t api_baumer_config_put_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "slave_id")) && cJSON_IsNumber(item))
        config_set_baumer_slave_id((uint8_t)item->valueint);
    if ((item = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(item))
        config_set_baumer_enabled(cJSON_IsTrue(item));

    config_save();
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
    return ESP_OK;
}

// POST /api/baumer/laser
static esp_err_t api_baumer_laser_handler(httpd_req_t *req)
{
    if (g_baumer == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Baumer not initialized");
        return ESP_FAIL;
    }

    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *on = cJSON_GetObjectItem(root, "on");
    if (on == NULL || !cJSON_IsBool(on)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'on' field");
        return ESP_FAIL;
    }

    esp_err_t ret = baumer_set_laser(g_baumer, cJSON_IsTrue(on));
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed to control laser\"}");
    }

    return ESP_OK;
}

void register_baumer_handlers(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/api/baumer/status", .method = HTTP_GET,  .handler = api_baumer_status_handler },
        { .uri = "/api/baumer/config", .method = HTTP_GET,  .handler = api_baumer_config_get_handler },
        { .uri = "/api/baumer/config", .method = HTTP_PUT,  .handler = api_baumer_config_put_handler },
        { .uri = "/api/baumer/laser",  .method = HTTP_POST, .handler = api_baumer_laser_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Baumer API handlers registered");
}
