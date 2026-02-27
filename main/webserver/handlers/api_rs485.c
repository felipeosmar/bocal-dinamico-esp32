/**
 * @file api_rs485.c
 * @brief RS485/Modbus API handlers
 */

#include "handlers.h"

static const char *TAG = "API_RS485";

// ============================================================================
// Helper functions
// ============================================================================

// Helper: Add exception stats to JSON object
static void add_exception_stats_to_json(cJSON *parent, const modbus_stats_t *stats)
{
    cJSON *exception_stats = cJSON_CreateObject();

    // Standard Modbus exceptions (0x00-0x0F)
    static const struct {
        uint8_t code;
        const char *name;
    } std_exceptions[] = {
        {0x01, "Illegal Function"},
        {0x02, "Illegal Data Address"},
        {0x03, "Illegal Data Value"},
        {0x04, "Slave Device Failure"},
        {0x05, "Acknowledge"},
        {0x06, "Slave Device Busy"},
        {0x08, "Memory Parity Error"},
        {0x0A, "Gateway Path Unavailable"},
        {0x0B, "Gateway Target Failed"},
    };

    for (size_t i = 0; i < sizeof(std_exceptions) / sizeof(std_exceptions[0]); i++) {
        uint8_t code = std_exceptions[i].code;
        if (stats->exception_counts[code] > 0) {
            char key[8];
            snprintf(key, sizeof(key), "0x%02X", code);
            cJSON *ex_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(ex_obj, "count", stats->exception_counts[code]);
            cJSON_AddStringToObject(ex_obj, "name", std_exceptions[i].name);
            cJSON_AddItemToObject(exception_stats, key, ex_obj);
        }
    }

    // mightyZAP proprietary exceptions (0x20-0x2F)
    static const struct {
        uint8_t code;
        const char *name;
    } mzap_exceptions[] = {
        {0x21, "mightyZAP Motor Moving"},
        {0x22, "mightyZAP Overload"},
        {0x23, "mightyZAP Checksum Error"},
        {0x24, "mightyZAP Range Error"},
        {0x25, "mightyZAP Instruction Error"},
    };

    for (size_t i = 0; i < sizeof(mzap_exceptions) / sizeof(mzap_exceptions[0]); i++) {
        uint8_t code = mzap_exceptions[i].code;
        uint8_t idx = code - 0x20;
        if (stats->mzap_exception_counts[idx] > 0) {
            char key[8];
            snprintf(key, sizeof(key), "0x%02X", code);
            cJSON *ex_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(ex_obj, "count", stats->mzap_exception_counts[idx]);
            cJSON_AddStringToObject(ex_obj, "name", mzap_exceptions[i].name);
            cJSON_AddItemToObject(exception_stats, key, ex_obj);
        }
    }

    cJSON_AddItemToObject(parent, "exception_stats", exception_stats);
}

// ============================================================================
// API Handlers
// ============================================================================

// GET/POST /api/rs485/config
esp_err_t api_rs485_config_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) return send_json(req, NULL);

        cJSON_AddNumberToObject(root, "baud_rate", config_get_rs485_baud());
        cJSON_AddNumberToObject(root, "tx_pin", config_get_rs485_tx_pin());
        cJSON_AddNumberToObject(root, "rx_pin", config_get_rs485_rx_pin());
        cJSON_AddNumberToObject(root, "de_pin", config_get_rs485_de_pin());
        cJSON_AddNumberToObject(root, "slave_id", config_get_modbus_slave_id());

        return send_json(req, root);
    } else {
        // POST - update config
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

        cJSON *baud = cJSON_GetObjectItem(root, "baud_rate");
        if (cJSON_IsNumber(baud)) {
            config_set_rs485_baud(baud->valueint);
        }

        cJSON *slave_id = cJSON_GetObjectItem(root, "slave_id");
        if (cJSON_IsNumber(slave_id)) {
            config_set_modbus_slave_id(slave_id->valueint);
        }

        config_save();

        cJSON *response = cJSON_CreateObject();
        cJSON_Delete(root);
        if (response == NULL) return send_json(req, NULL);
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "Config saved. Restart to apply.");

        return send_json(req, response);
    }
    return ESP_OK;
}

// GET /api/rs485/diag - Get RS485/Modbus diagnostics
esp_err_t api_rs485_diag_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);

    // RS485 status
    cJSON_AddBoolToObject(root, "rs485_ready", g_rs485 != NULL);
    cJSON_AddBoolToObject(root, "modbus_ready", g_modbus != NULL);

    // Configuration
    cJSON *config = cJSON_CreateObject();
    cJSON_AddNumberToObject(config, "baud_rate", config_get_rs485_baud());
    cJSON_AddNumberToObject(config, "tx_pin", config_get_rs485_tx_pin());
    cJSON_AddNumberToObject(config, "rx_pin", config_get_rs485_rx_pin());
    cJSON_AddNumberToObject(config, "de_pin", config_get_rs485_de_pin());
    cJSON_AddNumberToObject(config, "timeout_ms", config_get_modbus_timeout());
    cJSON_AddItemToObject(root, "config", config);

    // Modbus statistics (atomic snapshot for consistent reads)
    modbus_stats_t stats_snap;
    modbus_get_stats_snapshot(&stats_snap);
    {
        const modbus_stats_t *stats = &stats_snap;
        cJSON *modbus_stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(modbus_stats, "tx_count", stats->tx_count);
        cJSON_AddNumberToObject(modbus_stats, "rx_count", stats->rx_count);
        cJSON_AddNumberToObject(modbus_stats, "error_count", stats->error_count);
        cJSON_AddNumberToObject(modbus_stats, "timeout_count", stats->timeout_count);
        cJSON_AddNumberToObject(modbus_stats, "crc_error_count", stats->crc_error_count);
        cJSON_AddNumberToObject(modbus_stats, "short_response_count", stats->short_response_count);
        cJSON_AddNumberToObject(modbus_stats, "retry_count", stats->retry_count);

        // Calculate success rate
        if (stats->tx_count > 0) {
            double success_rate = (double)stats->rx_count / (double)stats->tx_count * 100.0;
            cJSON_AddNumberToObject(modbus_stats, "success_rate", success_rate);
        } else {
            cJSON_AddNumberToObject(modbus_stats, "success_rate", 0);
        }
        cJSON_AddItemToObject(root, "stats", modbus_stats);

        // Add detailed exception statistics
        add_exception_stats_to_json(root, stats);
    }

    return send_json(req, root);
}

// POST /api/rs485/test - Test communication with a Modbus slave
esp_err_t api_rs485_test_handler(httpd_req_t *req)
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

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "error", "Modbus not initialized");
        goto send_test_response;
    }

    // Get slave ID (default 1 for mightyZAP)
    cJSON *id_json = cJSON_GetObjectItem(root, "slave_id");
    uint8_t slave_id = cJSON_IsNumber(id_json) ? id_json->valueint : 1;

    // Get register to read (default 0x0000 for model number)
    cJSON *reg_json = cJSON_GetObjectItem(root, "register");
    uint16_t reg_addr = cJSON_IsNumber(reg_json) ? reg_json->valueint : 0x0000;

    // Get number of registers (default 1)
    cJSON *count_json = cJSON_GetObjectItem(root, "count");
    uint16_t count = cJSON_IsNumber(count_json) ? count_json->valueint : 1;
    if (count > 10) count = 10;  // Limit to 10 registers

    ESP_LOGI(TAG, "RS485 Test: slave=%d, reg=0x%04X, count=%d", slave_id, reg_addr, count);

    uint16_t values[10] = {0};
    esp_err_t err = modbus_read_holding_registers(g_modbus, slave_id, reg_addr, count, values);

    cJSON_AddNumberToObject(response, "slave_id", slave_id);
    cJSON_AddNumberToObject(response, "register", reg_addr);
    cJSON_AddNumberToObject(response, "count", count);

    if (err == ESP_OK) {
        cJSON_AddBoolToObject(response, "success", true);
        cJSON *data = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            cJSON_AddItemToArray(data, cJSON_CreateNumber(values[i]));
        }
        cJSON_AddItemToObject(response, "data", data);

        // Show hex representation too
        char hex_str[64] = {0};
        size_t pos = 0;
        for (int i = 0; i < count && pos < sizeof(hex_str) - 6; i++) {
            pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "0x%04X ", values[i]);
        }
        cJSON_AddStringToObject(response, "hex", hex_str);
    } else {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "error", esp_err_to_name(err));

        // Get last Modbus exception and error type if available
        modbus_exception_t ex = modbus_get_last_exception(g_modbus);
        modbus_error_type_t err_type = modbus_get_last_error_type(g_modbus);

        if (ex != MODBUS_EX_NONE) {
            cJSON_AddNumberToObject(response, "exception_code", ex);
            cJSON_AddStringToObject(response, "exception_name", modbus_exception_to_string(ex));
            cJSON_AddBoolToObject(response, "retryable", modbus_exception_is_retryable(ex));
        }

        // Add error classification
        const char *err_type_str;
        switch (err_type) {
            case MODBUS_ERR_TIMEOUT:        err_type_str = "timeout"; break;
            case MODBUS_ERR_CRC:            err_type_str = "crc_error"; break;
            case MODBUS_ERR_SHORT_RESPONSE: err_type_str = "short_response"; break;
            case MODBUS_ERR_EXCEPTION:      err_type_str = "exception"; break;
            case MODBUS_ERR_INVALID_RESPONSE: err_type_str = "invalid_response"; break;
            default:                        err_type_str = "unknown"; break;
        }
        cJSON_AddStringToObject(response, "error_type", err_type_str);
    }

send_test_response:
    cJSON_Delete(root);
    return send_json(req, response);
}

// POST /api/rs485/reset_stats - Reset Modbus statistics
esp_err_t api_rs485_reset_stats_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    modbus_reset_stats();

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) return send_json(req, NULL);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Statistics reset");

    return send_json(req, response);
}
