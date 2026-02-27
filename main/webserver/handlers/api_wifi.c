/**
 * @file api_wifi.c
 * @brief WiFi API handlers
 */

#include "handlers.h"

static const char *TAG = "API_WIFI";

// GET /api/wifi/scan
esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    wifi_scan_result_t results[20];
    uint16_t found = 0;

    esp_err_t ret = wifi_manager_scan(results, 20, &found);
    if (ret != ESP_OK) {
        return send_error_json(req, "500 Internal Server Error", "WiFi scan failed");
    }

    cJSON *root = cJSON_CreateArray();
    if (root == NULL) return send_json(req, NULL);
    for (int i = 0; i < found; i++) {
        cJSON *net = cJSON_CreateObject();
        if (net == NULL) continue;
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(net, "auth", results[i].authmode);
        cJSON_AddItemToArray(root, net);
    }

    return send_json(req, root);
}

// POST /api/wifi/connect
esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char buf[256];
    if (!validate_content_length(req, sizeof(buf) - 1)) return ESP_OK;

    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error_json(req, "500 Internal Server Error", "Failed to receive data");
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "Invalid JSON");
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_json = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_json)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "Missing SSID");
    }

    const char *ssid = ssid_json->valuestring;
    const char *password = cJSON_IsString(pass_json) ? pass_json->valuestring : "";

    ESP_LOGI(TAG, "Connecting to: %s", ssid);

    // Save config before connecting
    config_set_wifi_ssid(ssid);
    config_set_wifi_password(password);
    config_set_wifi_ap_mode(false);  // Disable AP mode so it connects on next boot
    config_save();

    esp_err_t err = wifi_manager_connect(ssid, password);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) { cJSON_Delete(root); return send_json(req, NULL); }
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Connected" : "Failed to connect");

    cJSON_Delete(root);
    return send_json(req, response);
}

// GET /api/wifi/status
esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return send_json(req, NULL);

    char ip[16] = {0};
    char ssid[33] = {0};
    wifi_manager_get_ip(ip);
    wifi_manager_get_ssid(ssid);

    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "status", wifi_manager_get_status());
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());

    return send_json(req, root);
}
