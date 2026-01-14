#include "web_server.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
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

static const char *TAG = "WEB_SRV";

// External globals from main
extern rs485_handle_t g_rs485;
extern modbus_handle_t g_modbus;
extern mightyzap_handle_t g_actuator;

// Server handle
static httpd_handle_t s_server = NULL;
static web_server_config_t s_config;
static bool s_running = false;

// Forward declarations
static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type);
static bool check_auth(httpd_req_t *req);

// ============================================================================
// Authentication
// ============================================================================

static bool check_auth(httpd_req_t *req)
{
    if (!s_config.auth_enabled) return true;

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
    snprintf(expected, sizeof(expected), "%s:%s", s_config.username, s_config.password);

    // Secure comparison
    bool match = (strlen(expected) == decoded_len) && 
                 (memcmp(decoded, expected, decoded_len) == 0);
    
    if (!match) {
        ESP_LOGW(TAG, "Authentication failed");
    }
    
    return match;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================================
// Static File Handlers
// ============================================================================

static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type)
{
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/index.html", "text/html");
}

static esp_err_t css_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/style.css", "text/css");
}

static esp_err_t js_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/core.js", "application/javascript");
}

// Generic handler for tabs HTML files
static esp_err_t tabs_html_handler(httpd_req_t *req)
{
    // Extract filename from URI (e.g., "/tabs/actuators.html" -> "actuators.html")
    const char *uri = req->uri;
    const char *filename = strrchr(uri, '/');
    if (!filename) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    filename++; // Skip the '/'

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/www/tabs/%s", filename);
    return serve_file(req, filepath, "text/html");
}

// Generic handler for tabs JS files
static esp_err_t tabs_js_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *filename = strrchr(uri, '/');
    if (!filename) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    filename++;

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/www/tabs/%s", filename);
    return serve_file(req, filepath, "application/javascript");
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/favicon.ico", "image/x-icon");
}

// ============================================================================
// API Handlers - File Manager
// ============================================================================

// Helper: Get partition base path from query parameter
static const char* get_partition_path(httpd_req_t *req, char *buf, size_t buf_len)
{
    char param[16] = {0};
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        if (httpd_query_key_value(buf, "partition", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "userdata") == 0) {
                return "/userdata";
            }
        }
    }
    return "/www";
}

// Helper: URL decode in place
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int val;
            char hex[3] = {src[1], src[2], 0};
            if (sscanf(hex, "%x", &val) == 1) {
                *dst++ = (char)val;
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// Helper: Validate path (security check)
static bool is_valid_path(const char *path)
{
    if (!path || strlen(path) == 0) return false;
    if (path[0] != '/') return false;
    if (strstr(path, "..") != NULL) return false;  // No path traversal
    if (strlen(path) > 128) return false;
    return true;
}

// Helper: Build full path
static void build_full_path(char *dest, size_t dest_size, const char *base, const char *path)
{
    if (strcmp(path, "/") == 0) {
        snprintf(dest, dest_size, "%s", base);
    } else {
        snprintf(dest, dest_size, "%s%s", base, path);
    }
}

// GET /api/files/list - List files in directory
static esp_err_t api_files_list_handler(httpd_req_t *req)
{
    char query_buf[256] = {0};
    char dir_param[128] = "/";

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    // Get dir parameter
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        httpd_query_key_value(query_buf, "dir", dir_param, sizeof(dir_param));
        url_decode(dir_param);
    }

    if (!is_valid_path(dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, dir_param);

    DIR *dir = opendir(full_path);
    if (!dir) {
        // If directory doesn't exist, return empty list
        cJSON *root = cJSON_CreateObject();
        cJSON *files = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "files", files);

        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        cJSON *file_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(file_obj, "name", entry->d_name);

        // Get file info
        char file_path[320];
        snprintf(file_path, sizeof(file_path), "%.159s/%.159s", full_path, entry->d_name);

        struct stat st;
        if (stat(file_path, &st) == 0) {
            cJSON_AddNumberToObject(file_obj, "size", st.st_size);
            cJSON_AddBoolToObject(file_obj, "isDir", S_ISDIR(st.st_mode));
        } else {
            cJSON_AddNumberToObject(file_obj, "size", 0);
            cJSON_AddBoolToObject(file_obj, "isDir", entry->d_type == DT_DIR);
        }

        cJSON_AddItemToArray(files, file_obj);
    }
    closedir(dir);

    cJSON_AddItemToObject(root, "files", files);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// GET /api/files/info - Get storage info for all partitions
static esp_err_t api_files_info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    size_t total, used;

    // WWW partition info
    if (esp_littlefs_info("www", &total, &used) == ESP_OK) {
        cJSON *www = cJSON_CreateObject();
        cJSON_AddNumberToObject(www, "total", total);
        cJSON_AddNumberToObject(www, "used", used);
        cJSON_AddItemToObject(root, "www", www);
    }

    // Userdata partition info
    if (esp_littlefs_info("userdata", &total, &used) == ESP_OK) {
        cJSON *userdata = cJSON_CreateObject();
        cJSON_AddNumberToObject(userdata, "total", total);
        cJSON_AddNumberToObject(userdata, "used", used);
        cJSON_AddItemToObject(root, "userdata", userdata);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// GET /api/files/download - Download a file
static esp_err_t api_files_download_handler(httpd_req_t *req)
{
    char query_buf[256] = {0};
    char file_param[128] = {0};

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }
    url_decode(file_param);

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // Get filename for Content-Disposition
    const char *filename = strrchr(file_param, '/');
    filename = filename ? filename + 1 : file_param;

    char header[256];
    snprintf(header, sizeof(header), "attachment; filename=\"%.200s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", header);
    httpd_resp_set_type(req, "application/octet-stream");

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

// GET /api/files/view - View file content inline
static esp_err_t api_files_view_handler(httpd_req_t *req)
{
    char query_buf[256] = {0};
    char file_param[128] = {0};

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }
    url_decode(file_param);

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    return serve_file(req, full_path, "text/plain");
}

// GET /api/files/read - Read file content for editing
static esp_err_t api_files_read_handler(httpd_req_t *req)
{
    char query_buf[256] = {0};
    char file_param[128] = {0};

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }
    url_decode(file_param);

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Limit to 50KB
    if (fsize > 50 * 1024) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 50KB)");
        return ESP_FAIL;
    }

    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    fread(content, 1, fsize, f);
    content[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "content", content);
    cJSON_AddNumberToObject(root, "size", fsize);

    free(content);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// POST /api/files/write - Write file content
static esp_err_t api_files_write_handler(httpd_req_t *req)
{
    char query_buf[64] = {0};
    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    // Read POST data
    int total_len = req->content_len;
    if (total_len > 60 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    // Parse multipart form data (simple parser)
    char file_param[128] = {0};
    char *content_start = NULL;
    size_t content_len = 0;

    // Find file parameter
    char *file_field = strstr(buf, "name=\"file\"");
    if (file_field) {
        char *value_start = strstr(file_field, "\r\n\r\n");
        if (value_start) {
            value_start += 4;
            char *value_end = strstr(value_start, "\r\n--");
            if (value_end) {
                size_t len = value_end - value_start;
                if (len < sizeof(file_param)) {
                    strncpy(file_param, value_start, len);
                    file_param[len] = '\0';
                }
            }
        }
    }

    // Find content parameter
    char *content_field = strstr(buf, "name=\"content\"");
    if (content_field) {
        content_start = strstr(content_field, "\r\n\r\n");
        if (content_start) {
            content_start += 4;
            char *content_end = strstr(content_start, "\r\n--");
            if (content_end) {
                content_len = content_end - content_start;
            } else {
                content_len = strlen(content_start);
            }
        }
    }

    if (!file_param[0] || !content_start) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing parameters");
        return ESP_FAIL;
    }

    if (!is_valid_path(file_param)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    FILE *f = fopen(full_path, "w");
    if (!f) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    fwrite(content_start, 1, content_len, f);
    fclose(f);
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// POST /api/files/delete - Delete file or folder
static esp_err_t api_files_delete_handler(httpd_req_t *req)
{
    char query_buf[64] = {0};
    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse file parameter from form data
    char file_param[128] = {0};
    char *file_field = strstr(buf, "name=\"file\"");
    if (file_field) {
        char *value_start = strstr(file_field, "\r\n\r\n");
        if (value_start) {
            value_start += 4;
            char *value_end = strstr(value_start, "\r\n");
            if (value_end) {
                size_t len = value_end - value_start;
                if (len < sizeof(file_param)) {
                    strncpy(file_param, value_start, len);
                    file_param[len] = '\0';
                }
            }
        }
    }

    if (!file_param[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    int result;
    if (S_ISDIR(st.st_mode)) {
        result = rmdir(full_path);
    } else {
        result = unlink(full_path);
    }

    if (result != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// POST /api/files/mkdir - Create directory
static esp_err_t api_files_mkdir_handler(httpd_req_t *req)
{
    char query_buf[64] = {0};
    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse dir parameter from form data
    char dir_param[128] = {0};
    char *dir_field = strstr(buf, "name=\"dir\"");
    if (dir_field) {
        char *value_start = strstr(dir_field, "\r\n\r\n");
        if (value_start) {
            value_start += 4;
            char *value_end = strstr(value_start, "\r\n");
            if (value_end) {
                size_t len = value_end - value_start;
                if (len < sizeof(dir_param)) {
                    strncpy(dir_param, value_start, len);
                    dir_param[len] = '\0';
                }
            }
        }
    }

    if (!dir_param[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing dir parameter");
        return ESP_FAIL;
    }

    if (!is_valid_path(dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, dir_param);

    if (mkdir(full_path, 0755) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create directory");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// POST /api/files/upload - Upload file
static esp_err_t api_files_upload_handler(httpd_req_t *req)
{
    char query_buf[256] = {0};
    char dir_param[128] = "/";

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    // Get dir parameter
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        httpd_query_key_value(query_buf, "dir", dir_param, sizeof(dir_param));
        url_decode(dir_param);
    }

    if (!is_valid_path(dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    // Read multipart data
    int total_len = req->content_len;
    if (total_len > 100 * 1024) {  // 100KB max
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 100KB)");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    // Parse filename from Content-Disposition
    char filename[64] = {0};
    char *filename_start = strstr(buf, "filename=\"");
    if (filename_start) {
        filename_start += 10;
        char *filename_end = strchr(filename_start, '"');
        if (filename_end && (filename_end - filename_start) < sizeof(filename)) {
            strncpy(filename, filename_start, filename_end - filename_start);
        }
    }

    if (!filename[0]) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_FAIL;
    }

    // Find file content (after double CRLF)
    char *content_start = strstr(buf, "\r\n\r\n");
    if (!content_start) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid format");
        return ESP_FAIL;
    }
    content_start += 4;

    // Find end boundary
    char *content_end = NULL;
    char *boundary_pos = content_start;
    while ((boundary_pos = strstr(boundary_pos, "\r\n--")) != NULL) {
        content_end = boundary_pos;
        boundary_pos += 4;
    }

    size_t content_len = content_end ? (content_end - content_start) : (buf + total_len - content_start);

    // Build full file path
    char full_path[192];
    if (strcmp(dir_param, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, filename);
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s/%s", base_path, dir_param, filename);
    }

    FILE *f = fopen(full_path, "w");
    if (!f) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    fwrite(content_start, 1, content_len, f);
    fclose(f);
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "File uploaded: %s", full_path);
    return ESP_OK;
}

// ============================================================================
// API Handlers - System
// ============================================================================

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    // System info
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_ms", xTaskGetTickCount() * portTICK_PERIOD_MS);

    // WiFi info
    char ip[16] = {0};
    char ssid[33] = {0};
    wifi_manager_get_ip(ip);
    wifi_manager_get_ssid(ssid);

    cJSON_AddStringToObject(root, "wifi_ip", ip);
    cJSON_AddStringToObject(root, "wifi_ssid", ssid);
    cJSON_AddNumberToObject(root, "wifi_rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "wifi_status", wifi_manager_get_status());

    // Modbus status
    cJSON_AddBoolToObject(root, "modbus_ready", g_modbus != NULL);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/tasks - Get FreeRTOS task statistics
static esp_err_t api_tasks_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    // System overview
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", xTaskGetTickCount() / configTICK_RATE_HZ);
    cJSON_AddNumberToObject(root, "task_count", uxTaskGetNumberOfTasks());
    
    // Get task statistics
    UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = malloc(num_tasks * sizeof(TaskStatus_t));
    
    if (task_array == NULL) {
        cJSON_AddStringToObject(root, "error", "Out of memory");
        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    configRUN_TIME_COUNTER_TYPE total_runtime;
    UBaseType_t tasks_returned = uxTaskGetSystemState(task_array, num_tasks, &total_runtime);
    
    cJSON *tasks = cJSON_CreateArray();
    
    for (UBaseType_t i = 0; i < tasks_returned; i++) {
        cJSON *task = cJSON_CreateObject();
        
        cJSON_AddStringToObject(task, "name", task_array[i].pcTaskName);
        cJSON_AddNumberToObject(task, "priority", task_array[i].uxCurrentPriority);
        cJSON_AddNumberToObject(task, "stack_hwm", task_array[i].usStackHighWaterMark);
        cJSON_AddNumberToObject(task, "task_num", task_array[i].xTaskNumber);
        
        // Task state
        const char *state_str;
        switch (task_array[i].eCurrentState) {
            case eRunning:  state_str = "Running"; break;
            case eReady:    state_str = "Ready"; break;
            case eBlocked:  state_str = "Blocked"; break;
            case eSuspended: state_str = "Suspended"; break;
            case eDeleted:  state_str = "Deleted"; break;
            default:        state_str = "Unknown"; break;
        }
        cJSON_AddStringToObject(task, "state", state_str);
        
        // CPU percentage (if runtime stats enabled)
        if (total_runtime > 0) {
            uint32_t cpu_percent = (uint32_t)((task_array[i].ulRunTimeCounter * 100) / total_runtime);
            cJSON_AddNumberToObject(task, "cpu_percent", cpu_percent);
            cJSON_AddNumberToObject(task, "runtime", (double)task_array[i].ulRunTimeCounter);
        } else {
            cJSON_AddNumberToObject(task, "cpu_percent", 0);
            cJSON_AddNumberToObject(task, "runtime", 0);
        }
        
        cJSON_AddItemToArray(tasks, task);
    }
    
    free(task_array);
    
    cJSON_AddItemToObject(root, "tasks", tasks);
    cJSON_AddNumberToObject(root, "total_runtime", total_runtime);
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// API Handlers - WiFi
// ============================================================================

static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_result_t results[20];
    uint16_t found = 0;

    esp_err_t ret = wifi_manager_scan(results, 20, &found);
    if (ret != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < found; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(net, "auth", results[i].authmode);
        cJSON_AddItemToArray(root, net);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_json = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
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
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "Connected" : "Failed to connect");

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    char ip[16] = {0};
    char ssid[33] = {0};
    wifi_manager_get_ip(ip);
    wifi_manager_get_ssid(ssid);

    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "status", wifi_manager_get_status());
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - Actuator Control (mightyZAP) - Multi-actuator support
// ============================================================================

#define MAX_ACTUATORS 10

// Structure to hold multiple actuator handles
typedef struct {
    uint8_t id;
    mightyzap_handle_t handle;
    bool active;
} actuator_slot_t;

static actuator_slot_t s_actuators[MAX_ACTUATORS] = {0};
static uint8_t s_num_actuators = 0;

// Helper: Find actuator by ID
static actuator_slot_t* find_actuator(uint8_t id) {
    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].id == id) {
            return &s_actuators[i];
        }
    }
    return NULL;
}

// Helper: Add actuator
static esp_err_t add_actuator(uint8_t id) {
    if (find_actuator(id) != NULL) return ESP_OK; // Already exists
    if (g_modbus == NULL) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (!s_actuators[i].active) {
            mightyzap_handle_t handle = NULL;
            esp_err_t ret = mightyzap_init(g_modbus, id, &handle);
            if (ret == ESP_OK) {
                s_actuators[i].id = id;
                s_actuators[i].handle = handle;
                s_actuators[i].active = true;
                s_num_actuators++;
                return ESP_OK;
            }
            return ret;
        }
    }
    return ESP_ERR_NO_MEM;
}

// Helper: Remove actuator
static void remove_actuator(uint8_t id) {
    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].id == id) {
            if (s_actuators[i].handle) {
                mightyzap_deinit(s_actuators[i].handle);
            }
            s_actuators[i].active = false;
            s_actuators[i].handle = NULL;
            s_num_actuators--;
            break;
        }
    }
}

// Helper: Load saved actuators from config on startup
static void load_saved_actuators(void) {
    uint8_t count = config_get_saved_actuator_count();
    if (count == 0) {
        ESP_LOGI(TAG, "No saved actuators to load");
        return;
    }

    const uint8_t *ids = config_get_saved_actuator_ids();
    if (ids == NULL) {
        ESP_LOGW(TAG, "Failed to get saved actuator IDs");
        return;
    }

    ESP_LOGI(TAG, "Loading %d saved actuators from config", count);

    int loaded = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t id = ids[i];
        esp_err_t ret = add_actuator(id);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Loaded saved actuator ID %d", id);
            loaded++;
        } else {
            ESP_LOGW(TAG, "Failed to load actuator ID %d: %s", id, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Loaded %d of %d saved actuators", loaded, count);
}

// GET /api/actuator/status - Get status of all active actuators
static esp_err_t api_actuator_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *actuators = cJSON_CreateArray();

    for (int i = 0; i < MAX_ACTUATORS; i++) {
        if (s_actuators[i].active && s_actuators[i].handle) {
            cJSON *act = cJSON_CreateObject();
            cJSON_AddNumberToObject(act, "id", s_actuators[i].id);

            mightyzap_status_t status;
            esp_err_t ret = mightyzap_get_status(s_actuators[i].handle, &status);

            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(act, "connected", true);
                cJSON_AddNumberToObject(act, "position", status.position);
                cJSON_AddNumberToObject(act, "current", status.current);
                cJSON_AddNumberToObject(act, "voltage", status.voltage / 10.0);
                cJSON_AddBoolToObject(act, "moving", status.moving != 0);
            } else {
                cJSON_AddBoolToObject(act, "connected", false);
            }
            cJSON_AddItemToArray(actuators, act);
        }
    }

    cJSON_AddItemToObject(root, "actuators", actuators);
    cJSON_AddNumberToObject(root, "count", s_num_actuators);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/control - Control specific actuator by ID
static esp_err_t api_actuator_control_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    esp_err_t err = ESP_FAIL;

    // Get actuator ID from request
    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsNumber(id_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing actuator ID");
        goto send_response;
    }

    uint8_t act_id = id_json->valueint;
    actuator_slot_t *slot = find_actuator(act_id);

    if (slot == NULL || slot->handle == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Actuator not found");
        goto send_response;
    }

    mightyzap_handle_t handle = slot->handle;

    // Check for force enable/disable
    cJSON *force_enable = cJSON_GetObjectItem(root, "force");
    if (cJSON_IsBool(force_enable)) {
        err = mightyzap_set_force_enable(handle, cJSON_IsTrue(force_enable));
    }

    // Check for position
    cJSON *position = cJSON_GetObjectItem(root, "position");
    if (cJSON_IsNumber(position)) {
        int val = position->valueint;
        if (val >= 0 && val <= 4095) {
            err = mightyzap_set_position(handle, val);
        }
    }

    // Check for speed
    cJSON *speed = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(speed)) {
        int val = speed->valueint;
        if (val >= 0 && val <= 1023) {
            err = mightyzap_set_speed(handle, val);
        }
    }

    // Check for current (force limit)
    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (cJSON_IsNumber(current)) {
        int val = current->valueint;
        if (val >= 0 && val <= 800) {
            err = mightyzap_set_current(handle, val);
        }
    }

    // Check for combined goal (position + speed + current)
    cJSON *goal = cJSON_GetObjectItem(root, "goal");
    if (cJSON_IsObject(goal)) {
        cJSON *g_pos = cJSON_GetObjectItem(goal, "position");
        cJSON *g_spd = cJSON_GetObjectItem(goal, "speed");
        cJSON *g_cur = cJSON_GetObjectItem(goal, "current");

        if (cJSON_IsNumber(g_pos) && cJSON_IsNumber(g_spd) && cJSON_IsNumber(g_cur)) {
            err = mightyzap_set_goal(handle,
                                     g_pos->valueint,
                                     g_spd->valueint,
                                     g_cur->valueint);
        }
    }

    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "OK" : "Command failed");

send_response:
    {
        char *json_str = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
    }

    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/actuator/scan - Scan for actuators and auto-add them
static esp_err_t api_actuator_scan_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *found = cJSON_CreateArray();

    if (g_modbus == NULL) {
        cJSON_AddItemToObject(root, "found", found);
        cJSON_AddNumberToObject(root, "count", 0);
        cJSON_AddStringToObject(root, "error", "Modbus not initialized");

        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }

    uint8_t max_id = config_get_scan_max_id();
    if (max_id < 1) max_id = 1;
    if (max_id > 247) max_id = 247;

    ESP_LOGI(TAG, "Scanning for mightyZAP actuators (IDs 1-%d)...", max_id);

    // Suppress timeout warnings during scan
    esp_log_level_set("RS485", ESP_LOG_ERROR);
    esp_log_level_set("MODBUS", ESP_LOG_ERROR);

    int count = 0;
    bool config_changed = false;
    for (uint8_t id = 1; id <= max_id; id++) {
        uint16_t model = 0;
        esp_err_t ret = modbus_read_holding_registers(g_modbus, id,
                                                       MZAP_REG_MODEL_NUMBER, 1, &model);
        // mightyZAP models are typically > 100 (e.g., 350, 500, etc.)
        if (ret == ESP_OK && model > 100) {
            ESP_LOGI(TAG, "Found actuator at ID %d, model: %u", id, model);

            // Auto-add to active actuators
            add_actuator(id);

            // Persist to config (idempotent - won't duplicate)
            if (config_add_saved_actuator_id(id)) {
                ESP_LOGI(TAG, "Persisted actuator ID %d to config", id);
                config_changed = true;
            }

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddNumberToObject(item, "model", model);
            cJSON_AddItemToArray(found, item);
            count++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Restore log levels
    esp_log_level_set("RS485", ESP_LOG_WARN);
    esp_log_level_set("MODBUS", ESP_LOG_WARN);

    // Save config if any new actuators were persisted
    if (config_changed) {
        config_save();
        ESP_LOGI(TAG, "Saved actuator config with %d actuators", count);
    }

    cJSON_AddItemToObject(root, "found", found);
    cJSON_AddNumberToObject(root, "count", count);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/add - Add actuator by ID
static esp_err_t api_actuator_add_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json) || id_json->valueint < 1 || id_json->valueint > 247) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid ID (1-247)");
    } else {
        uint8_t new_id = id_json->valueint;
        esp_err_t err = add_actuator(new_id);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Actuator added with ID %d", new_id);
            cJSON_AddBoolToObject(response, "success", true);
            cJSON_AddStringToObject(response, "message", "Actuator added");
            cJSON_AddNumberToObject(response, "id", new_id);
        } else {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Failed to add actuator");
        }
    }

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/remove - Remove actuator by ID
static esp_err_t api_actuator_remove_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    cJSON *response = cJSON_CreateObject();

    if (!cJSON_IsNumber(id_json)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Invalid ID");
    } else {
        uint8_t id = id_json->valueint;
        remove_actuator(id);

        // Also remove from persisted config
        if (config_remove_saved_actuator_id(id)) {
            config_save();
            ESP_LOGI(TAG, "Actuator removed from config: ID %d", id);
        }

        ESP_LOGI(TAG, "Actuator removed: ID %d", id);
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "Actuator removed");
    }

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/actuator/sync_control - Synchronized control for actuators 1 and 2
static esp_err_t api_actuator_sync_control_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();

    // Parse goal object
    cJSON *goal = cJSON_GetObjectItem(root, "goal");
    if (!cJSON_IsObject(goal)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing goal object");
        goto send_sync_response;
    }

    cJSON *g_pos = cJSON_GetObjectItem(goal, "position");
    cJSON *g_spd = cJSON_GetObjectItem(goal, "speed");
    cJSON *g_cur = cJSON_GetObjectItem(goal, "current");

    if (!cJSON_IsNumber(g_pos) || !cJSON_IsNumber(g_spd) || !cJSON_IsNumber(g_cur)) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Missing position, speed, or current");
        goto send_sync_response;
    }

    // Get delay_ms with validation (default 20ms, clamp to 10-50ms)
    cJSON *delay_json = cJSON_GetObjectItem(root, "delay_ms");
    int delay_ms = 20;
    if (cJSON_IsNumber(delay_json)) {
        delay_ms = delay_json->valueint;
        if (delay_ms < 10) delay_ms = 10;
        if (delay_ms > 50) delay_ms = 50;
    }

    // Clamp position to valid range (0-4095)
    int position = g_pos->valueint;
    if (position < 0) position = 0;
    if (position > 4095) position = 4095;

    // Clamp speed (0-1023) and current (0-800)
    int speed = g_spd->valueint;
    if (speed < 0) speed = 0;
    if (speed > 1023) speed = 1023;

    int current = g_cur->valueint;
    if (current < 0) current = 0;
    if (current > 800) current = 800;

    ESP_LOGI(TAG, "Sync control: pos=%d, spd=%d, cur=%d, delay=%dms",
             position, speed, current, delay_ms);

    // Find actuators 1 and 2
    actuator_slot_t *slot1 = find_actuator(1);
    actuator_slot_t *slot2 = find_actuator(2);

    bool act1_success = false;
    bool act2_success = false;
    const char *act1_error = NULL;
    const char *act2_error = NULL;

    // Send command to actuator 1
    if (slot1 != NULL && slot1->handle != NULL) {
        esp_err_t err = mightyzap_set_goal(slot1->handle, position, speed, current);
        if (err == ESP_OK) {
            act1_success = true;
            ESP_LOGI(TAG, "ID=1: Set goal position=%d, speed=%d, current=%d", position, speed, current);
        } else {
            act1_error = "Command failed";
            ESP_LOGW(TAG, "ID=1: Set goal failed: %s", esp_err_to_name(err));
        }
    } else {
        act1_error = "Actuator not found";
        ESP_LOGW(TAG, "ID=1: Actuator not found");
    }

    // Delay between actuator commands
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    // Send command to actuator 2
    if (slot2 != NULL && slot2->handle != NULL) {
        esp_err_t err = mightyzap_set_goal(slot2->handle, position, speed, current);
        if (err == ESP_OK) {
            act2_success = true;
            ESP_LOGI(TAG, "ID=2: Set goal position=%d, speed=%d, current=%d", position, speed, current);
        } else {
            act2_error = "Command failed";
            ESP_LOGW(TAG, "ID=2: Set goal failed: %s", esp_err_to_name(err));
        }
    } else {
        act2_error = "Actuator not found";
        ESP_LOGW(TAG, "ID=2: Actuator not found");
    }

    // Build response
    bool overall_success = act1_success && act2_success;
    cJSON_AddBoolToObject(response, "success", overall_success);

    if (overall_success) {
        cJSON_AddStringToObject(response, "message", "Synchronized movement started");
    } else if (act1_success || act2_success) {
        // Partial success
        if (!act1_success) {
            cJSON_AddStringToObject(response, "message", "Actuator 1 failed");
        } else {
            cJSON_AddStringToObject(response, "message", "Actuator 2 failed");
        }
    } else {
        cJSON_AddStringToObject(response, "message", "Both actuators failed");
    }

    // Add individual actuator status
    cJSON *act1_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(act1_obj, "id", 1);
    cJSON_AddBoolToObject(act1_obj, "success", act1_success);
    if (act1_error) {
        cJSON_AddStringToObject(act1_obj, "error", act1_error);
    }
    cJSON_AddItemToObject(response, "actuator_1", act1_obj);

    cJSON *act2_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(act2_obj, "id", 2);
    cJSON_AddBoolToObject(act2_obj, "success", act2_success);
    if (act2_error) {
        cJSON_AddStringToObject(act2_obj, "error", act2_error);
    }
    cJSON_AddItemToObject(response, "actuator_2", act2_obj);

send_sync_response:
    {
        char *json_str = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
    }

    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - RS485 Configuration
// ============================================================================

static esp_err_t api_rs485_config_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();

        cJSON_AddNumberToObject(root, "baud_rate", config_get_rs485_baud());
        cJSON_AddNumberToObject(root, "tx_pin", config_get_rs485_tx_pin());
        cJSON_AddNumberToObject(root, "rx_pin", config_get_rs485_rx_pin());
        cJSON_AddNumberToObject(root, "de_pin", config_get_rs485_de_pin());
        cJSON_AddNumberToObject(root, "slave_id", config_get_modbus_slave_id());

        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));

        free(json_str);
        cJSON_Delete(root);
    } else {
        // POST - update config
        char buf[256];
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (root == NULL) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
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
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "Config saved. Restart to apply.");

        char *json_str = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));

        free(json_str);
        cJSON_Delete(response);
        cJSON_Delete(root);
    }
    return ESP_OK;
}

// ============================================================================
// API Handlers - RS485 Diagnostics
// ============================================================================

// GET /api/rs485/diag - Get RS485/Modbus diagnostics
static esp_err_t api_rs485_diag_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

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

    // Modbus statistics
    const modbus_stats_t *stats = modbus_get_stats();
    if (stats) {
        cJSON *modbus_stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(modbus_stats, "tx_count", stats->tx_count);
        cJSON_AddNumberToObject(modbus_stats, "rx_count", stats->rx_count);
        cJSON_AddNumberToObject(modbus_stats, "error_count", stats->error_count);
        cJSON_AddNumberToObject(modbus_stats, "timeout_count", stats->timeout_count);
        cJSON_AddNumberToObject(modbus_stats, "crc_error_count", stats->crc_error_count);
        cJSON_AddNumberToObject(modbus_stats, "retry_count", stats->retry_count);

        // Calculate success rate
        if (stats->tx_count > 0) {
            double success_rate = (double)stats->rx_count / (double)stats->tx_count * 100.0;
            cJSON_AddNumberToObject(modbus_stats, "success_rate", success_rate);
        } else {
            cJSON_AddNumberToObject(modbus_stats, "success_rate", 0);
        }
        cJSON_AddItemToObject(root, "stats", modbus_stats);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/rs485/test - Test communication with a Modbus slave
static esp_err_t api_rs485_test_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
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

        // Get last Modbus exception if available
        modbus_exception_t ex = modbus_get_last_exception(g_modbus);
        if (ex != MODBUS_EX_NONE) {
            cJSON_AddNumberToObject(response, "exception_code", ex);
        }
    }

send_test_response:
    {
        char *json_str = cJSON_PrintUnformatted(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
    }

    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/rs485/reset_stats - Reset Modbus statistics
static esp_err_t api_rs485_reset_stats_handler(httpd_req_t *req)
{
    modbus_reset_stats();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Statistics reset");

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

// ============================================================================
// API Handlers - System Control
// ============================================================================

static esp_err_t api_restart_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "Restarting...");

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);

    // Delay before restart
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ============================================================================
// Server Setup
// ============================================================================

void web_server_get_default_config(web_server_config_t *config)
{
    if (config == NULL) return;

    config->port = 80;
    config->username = "admin";
    config->password = "admin";
    config->auth_enabled = false;
}

esp_err_t web_server_init(const web_server_config_t *config)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    // Initialize LittleFS for www partition (web interface files)
    esp_vfs_littlefs_conf_t www_conf = {
        .base_path = "/www",
        .partition_label = "www",
        .format_if_mount_failed = false,  // Don't format - should have files from flash
        .dont_mount = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&www_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount www partition: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("www", &total, &used);
    ESP_LOGI(TAG, "LittleFS www: total=%d, used=%d", total, used);

    if (config) {
        memcpy(&s_config, config, sizeof(web_server_config_t));
    } else {
        web_server_get_default_config(&s_config);
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = s_config.port;
    http_config.max_uri_handlers = 50;
    http_config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting server on port %d", http_config.server_port);

    ret = httpd_start(&s_server, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Static files
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t css_uri = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = css_handler,
    };
    httpd_register_uri_handler(s_server, &css_uri);

    httpd_uri_t js_uri = {
        .uri = "/core.js",
        .method = HTTP_GET,
        .handler = js_handler,
    };
    httpd_register_uri_handler(s_server, &js_uri);

    // Tabs HTML files
    httpd_uri_t tabs_actuators_html = { .uri = "/tabs/actuators.html", .method = HTTP_GET, .handler = tabs_html_handler };
    httpd_uri_t tabs_system_html = { .uri = "/tabs/system.html", .method = HTTP_GET, .handler = tabs_html_handler };
    httpd_uri_t tabs_config_html = { .uri = "/tabs/config.html", .method = HTTP_GET, .handler = tabs_html_handler };
    httpd_uri_t tabs_files_html = { .uri = "/tabs/files.html", .method = HTTP_GET, .handler = tabs_html_handler };
    httpd_uri_t tabs_tasks_html = { .uri = "/tabs/tasks.html", .method = HTTP_GET, .handler = tabs_html_handler };
    httpd_register_uri_handler(s_server, &tabs_actuators_html);
    httpd_register_uri_handler(s_server, &tabs_system_html);
    httpd_register_uri_handler(s_server, &tabs_config_html);
    httpd_register_uri_handler(s_server, &tabs_files_html);
    httpd_register_uri_handler(s_server, &tabs_tasks_html);

    // Tabs JS files
    httpd_uri_t tabs_actuators_js = { .uri = "/tabs/actuators.js", .method = HTTP_GET, .handler = tabs_js_handler };
    httpd_uri_t tabs_system_js = { .uri = "/tabs/system.js", .method = HTTP_GET, .handler = tabs_js_handler };
    httpd_uri_t tabs_config_js = { .uri = "/tabs/config.js", .method = HTTP_GET, .handler = tabs_js_handler };
    httpd_uri_t tabs_files_js = { .uri = "/tabs/files.js", .method = HTTP_GET, .handler = tabs_js_handler };
    httpd_uri_t tabs_tasks_js = { .uri = "/tabs/tasks.js", .method = HTTP_GET, .handler = tabs_js_handler };
    httpd_register_uri_handler(s_server, &tabs_actuators_js);
    httpd_register_uri_handler(s_server, &tabs_system_js);
    httpd_register_uri_handler(s_server, &tabs_config_js);
    httpd_register_uri_handler(s_server, &tabs_files_js);
    httpd_register_uri_handler(s_server, &tabs_tasks_js);

    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };
    httpd_register_uri_handler(s_server, &favicon_uri);

    // API - File Manager
    httpd_uri_t files_list_uri = {
        .uri = "/api/files/list",
        .method = HTTP_GET,
        .handler = api_files_list_handler,
    };
    httpd_register_uri_handler(s_server, &files_list_uri);

    httpd_uri_t files_info_uri = {
        .uri = "/api/files/info",
        .method = HTTP_GET,
        .handler = api_files_info_handler,
    };
    httpd_register_uri_handler(s_server, &files_info_uri);

    httpd_uri_t files_download_uri = {
        .uri = "/api/files/download",
        .method = HTTP_GET,
        .handler = api_files_download_handler,
    };
    httpd_register_uri_handler(s_server, &files_download_uri);

    httpd_uri_t files_view_uri = {
        .uri = "/api/files/view",
        .method = HTTP_GET,
        .handler = api_files_view_handler,
    };
    httpd_register_uri_handler(s_server, &files_view_uri);

    httpd_uri_t files_read_uri = {
        .uri = "/api/files/read",
        .method = HTTP_GET,
        .handler = api_files_read_handler,
    };
    httpd_register_uri_handler(s_server, &files_read_uri);

    httpd_uri_t files_write_uri = {
        .uri = "/api/files/write",
        .method = HTTP_POST,
        .handler = api_files_write_handler,
    };
    httpd_register_uri_handler(s_server, &files_write_uri);

    httpd_uri_t files_delete_uri = {
        .uri = "/api/files/delete",
        .method = HTTP_POST,
        .handler = api_files_delete_handler,
    };
    httpd_register_uri_handler(s_server, &files_delete_uri);

    httpd_uri_t files_mkdir_uri = {
        .uri = "/api/files/mkdir",
        .method = HTTP_POST,
        .handler = api_files_mkdir_handler,
    };
    httpd_register_uri_handler(s_server, &files_mkdir_uri);

    httpd_uri_t files_upload_uri = {
        .uri = "/api/files/upload",
        .method = HTTP_POST,
        .handler = api_files_upload_handler,
    };
    httpd_register_uri_handler(s_server, &files_upload_uri);

    // API - System
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t tasks_uri = {
        .uri = "/api/tasks",
        .method = HTTP_GET,
        .handler = api_tasks_handler,
    };
    httpd_register_uri_handler(s_server, &tasks_uri);

    httpd_uri_t restart_uri = {
        .uri = "/api/restart",
        .method = HTTP_POST,
        .handler = api_restart_handler,
    };
    httpd_register_uri_handler(s_server, &restart_uri);

    // API - WiFi
    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_scan_uri);

    httpd_uri_t wifi_connect_uri = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = api_wifi_connect_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_connect_uri);

    httpd_uri_t wifi_status_uri = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = api_wifi_status_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_status_uri);

    // API - RS485 Config
    httpd_uri_t rs485_config_get_uri = {
        .uri = "/api/rs485/config",
        .method = HTTP_GET,
        .handler = api_rs485_config_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_config_get_uri);

    httpd_uri_t rs485_config_post_uri = {
        .uri = "/api/rs485/config",
        .method = HTTP_POST,
        .handler = api_rs485_config_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_config_post_uri);

    // API - RS485 Diagnostics
    httpd_uri_t rs485_diag_uri = {
        .uri = "/api/rs485/diag",
        .method = HTTP_GET,
        .handler = api_rs485_diag_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_diag_uri);

    httpd_uri_t rs485_test_uri = {
        .uri = "/api/rs485/test",
        .method = HTTP_POST,
        .handler = api_rs485_test_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_test_uri);

    httpd_uri_t rs485_reset_stats_uri = {
        .uri = "/api/rs485/reset_stats",
        .method = HTTP_POST,
        .handler = api_rs485_reset_stats_handler,
    };
    httpd_register_uri_handler(s_server, &rs485_reset_stats_uri);

    // API - Actuator Control (Multi-actuator)
    httpd_uri_t actuator_status_uri = {
        .uri = "/api/actuator/status",
        .method = HTTP_GET,
        .handler = api_actuator_status_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_status_uri);

    httpd_uri_t actuator_control_uri = {
        .uri = "/api/actuator/control",
        .method = HTTP_POST,
        .handler = api_actuator_control_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_control_uri);

    httpd_uri_t actuator_scan_uri = {
        .uri = "/api/actuator/scan",
        .method = HTTP_GET,
        .handler = api_actuator_scan_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_scan_uri);

    httpd_uri_t actuator_add_uri = {
        .uri = "/api/actuator/add",
        .method = HTTP_POST,
        .handler = api_actuator_add_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_add_uri);

    httpd_uri_t actuator_remove_uri = {
        .uri = "/api/actuator/remove",
        .method = HTTP_POST,
        .handler = api_actuator_remove_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_remove_uri);

    httpd_uri_t actuator_sync_control_uri = {
        .uri = "/api/actuator/sync_control",
        .method = HTTP_POST,
        .handler = api_actuator_sync_control_handler,
    };
    httpd_register_uri_handler(s_server, &actuator_sync_control_uri);

    s_running = true;
    ESP_LOGI(TAG, "Web server started");

    // Load saved actuators from config
    load_saved_actuators();

    return ESP_OK;
}

void web_server_deinit(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_vfs_littlefs_unregister("www");
    s_running = false;
}

bool web_server_is_running(void)
{
    return s_running;
}

esp_err_t web_server_set_auth(const char *username, const char *password)
{
    if (username) s_config.username = username;
    if (password) s_config.password = password;
    return ESP_OK;
}
