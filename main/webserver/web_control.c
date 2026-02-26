#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "control_loop.h"
#include "config_manager.h"

static const char *TAG = "WEB_CONTROL";

// GET /api/control/status
static esp_err_t api_control_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    control_status_t status;
    control_config_t config;
    control_loop_get_status(&status);
    control_loop_get_config(&config);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", status.running);
    cJSON_AddNumberToObject(root, "interval_ms", config.interval_ms);
    cJSON_AddNumberToObject(root, "measurement_index", config.measurement_index);
    cJSON_AddNumberToObject(root, "last_gap_value", status.last_gap_value);
    cJSON_AddNumberToObject(root, "last_quality", status.last_quality);
    cJSON_AddNumberToObject(root, "loop_count", status.loop_count);
    cJSON_AddNumberToObject(root, "error_count", status.error_count);

    cJSON *equations = cJSON_CreateArray();
    for (uint8_t i = 0; i < config.equation_count; i++) {
        cJSON *eq = cJSON_CreateObject();
        cJSON_AddNumberToObject(eq, "actuator_id", config.equations[i].actuator_id);
        cJSON_AddNumberToObject(eq, "a", config.equations[i].coeff_a);
        cJSON_AddNumberToObject(eq, "b", config.equations[i].coeff_b);
        cJSON_AddBoolToObject(eq, "enabled", config.equations[i].enabled);

        // Find computed position for this actuator
        int pos = -1;
        for (uint8_t j = 0; j < status.computed_count; j++) {
            if (status.computed_actuator_ids[j] == config.equations[i].actuator_id) {
                pos = status.computed_positions[j];
                break;
            }
        }
        if (pos >= 0) {
            cJSON_AddNumberToObject(eq, "computed_position", pos);
        } else {
            cJSON_AddNullToObject(eq, "computed_position");
        }

        cJSON_AddItemToArray(equations, eq);
    }
    cJSON_AddItemToObject(root, "equations", equations);

    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, str);
    free((void *)str);
    cJSON_Delete(root);

    return ESP_OK;
}

// POST /api/control/start
static esp_err_t api_control_start_handler(httpd_req_t *req)
{
    control_loop_start();
    config_set_control_running(true);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"running\":true}");
    return ESP_OK;
}

// POST /api/control/stop
static esp_err_t api_control_stop_handler(httpd_req_t *req)
{
    control_loop_stop();
    config_set_control_running(false);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"running\":false}");
    return ESP_OK;
}

// PUT /api/control/interval
static esp_err_t api_control_interval_handler(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ms = cJSON_GetObjectItem(root, "interval_ms");
    if (ms == NULL || !cJSON_IsNumber(ms)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'interval_ms'");
        return ESP_FAIL;
    }

    uint32_t interval = (uint32_t)ms->valueint;
    cJSON_Delete(root);

    esp_err_t ret = control_loop_set_interval(interval);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid interval (100-60000)");
        return ESP_FAIL;
    }

    config_set_control_interval(interval);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// PUT /api/control/equation
static esp_err_t api_control_equation_handler(httpd_req_t *req)
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

    cJSON *id_j = cJSON_GetObjectItem(root, "actuator_id");
    cJSON *a_j = cJSON_GetObjectItem(root, "a");
    cJSON *b_j = cJSON_GetObjectItem(root, "b");
    cJSON *en_j = cJSON_GetObjectItem(root, "enabled");

    if (!id_j || !cJSON_IsNumber(id_j) || !a_j || !cJSON_IsNumber(a_j) ||
        !b_j || !cJSON_IsNumber(b_j) || !en_j || !cJSON_IsBool(en_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                           "Required: actuator_id, a, b, enabled");
        return ESP_FAIL;
    }

    uint8_t actuator_id = (uint8_t)id_j->valueint;
    float a = (float)a_j->valuedouble;
    float b = (float)b_j->valuedouble;
    bool enabled = cJSON_IsTrue(en_j);
    cJSON_Delete(root);

    esp_err_t ret = control_loop_set_equation(actuator_id, a, b, enabled);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set equation");
        return ESP_FAIL;
    }

    // Persist all equations
    control_config_t cfg;
    control_loop_get_config(&cfg);
    config_control_equation_t eqs[CONFIG_MAX_EQUATIONS];
    for (uint8_t i = 0; i < cfg.equation_count; i++) {
        eqs[i].actuator_id = cfg.equations[i].actuator_id;
        eqs[i].coeff_a = cfg.equations[i].coeff_a;
        eqs[i].coeff_b = cfg.equations[i].coeff_b;
        eqs[i].enabled = cfg.equations[i].enabled;
    }
    config_set_control_equations(eqs, cfg.equation_count);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// PUT /api/control/measurement_index
static esp_err_t api_control_meas_index_handler(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *idx = cJSON_GetObjectItem(root, "measurement_index");
    if (idx == NULL || !cJSON_IsNumber(idx)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'measurement_index'");
        return ESP_FAIL;
    }

    uint8_t index = (uint8_t)idx->valueint;
    cJSON_Delete(root);

    esp_err_t ret = control_loop_set_measurement_index(index);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid index (0-3)");
        return ESP_FAIL;
    }

    config_set_control_measurement_index(index);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

void register_control_handlers(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/api/control/status",            .method = HTTP_GET,  .handler = api_control_status_handler },
        { .uri = "/api/control/start",             .method = HTTP_POST, .handler = api_control_start_handler },
        { .uri = "/api/control/stop",              .method = HTTP_POST, .handler = api_control_stop_handler },
        { .uri = "/api/control/interval",          .method = HTTP_PUT,  .handler = api_control_interval_handler },
        { .uri = "/api/control/equation",          .method = HTTP_PUT,  .handler = api_control_equation_handler },
        { .uri = "/api/control/measurement_index", .method = HTTP_PUT,  .handler = api_control_meas_index_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Control API handlers registered");
}
