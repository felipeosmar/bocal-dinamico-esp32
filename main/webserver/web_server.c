#include "web_server.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"

#include "wifi_manager.h"
#include "config_manager.h"
#include "modbus_rtu.h"
#include "mightyzap.h"

static const char *TAG = "WEB_SRV";

// External globals from main
extern modbus_handle_t g_modbus;
extern mightyzap_handle_t g_actuator;

// Server handle
static httpd_handle_t s_server = NULL;
static web_server_config_t s_config;
static bool s_running = false;

// Remote slave register addresses - LED Control (0x0000-0x0002)
#define REG_LED_STATE       0x0000
#define REG_BLINK_MODE      0x0001
#define REG_BLINK_PERIOD    0x0002

// Remote slave register addresses - System/Config (0x0100-0x01FF)
#define REG_SLAVE_ID        0x0100
#define REG_FW_VERSION      0x0101
#define REG_SAVE_CONFIG     0x0102
#define REG_REBOOT          0x0103
#define REBOOT_MAGIC        0xBEEF

// Forward declarations
static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type);
static bool check_auth(httpd_req_t *req);

// ============================================================================
// Authentication
// ============================================================================

static bool check_auth(httpd_req_t *req)
{
    if (!s_config.auth_enabled) return true;

    char auth_header[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    // Parse "Basic base64credentials"
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;

    // For simplicity, just compare with expected credentials
    // In production, use proper base64 encoding
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", s_config.username, s_config.password);

    // Simple base64 decode check (simplified version)
    // This is a basic implementation - in production use mbedtls base64
    return true; // Simplified - implement proper auth if needed
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
    return serve_file(req, "/www/app.js", "application/javascript");
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
// API Handlers - LED Control (Remote Slave)
// ============================================================================

static esp_err_t api_led_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(root, "error", true);
        cJSON_AddStringToObject(root, "message", "Modbus not initialized");
    } else {
        uint16_t values[3];
        esp_err_t ret = modbus_read_holding_registers(g_modbus, config_get_modbus_slave_id(),
                                                       REG_LED_STATE, 3, values);
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(root, "error", false);
            cJSON_AddBoolToObject(root, "led_on", values[0] != 0);
            cJSON_AddBoolToObject(root, "blink_mode", values[1] != 0);
            cJSON_AddNumberToObject(root, "blink_period", values[2]);
        } else {
            cJSON_AddBoolToObject(root, "error", true);
            cJSON_AddStringToObject(root, "message", "Slave not responding");
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_led_control_handler(httpd_req_t *req)
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

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Modbus not initialized");
    } else {
        // Check for LED state
        cJSON *led_on = cJSON_GetObjectItem(root, "led_on");
        if (cJSON_IsBool(led_on)) {
            err = modbus_write_single_register(g_modbus, config_get_modbus_slave_id(),
                                               REG_LED_STATE, cJSON_IsTrue(led_on) ? 1 : 0);
        }

        // Check for blink mode
        cJSON *blink = cJSON_GetObjectItem(root, "blink_mode");
        if (cJSON_IsBool(blink)) {
            err = modbus_write_single_register(g_modbus, config_get_modbus_slave_id(),
                                               REG_BLINK_MODE, cJSON_IsTrue(blink) ? 1 : 0);
        }

        // Check for blink period
        cJSON *period = cJSON_GetObjectItem(root, "blink_period");
        if (cJSON_IsNumber(period)) {
            int val = period->valueint;
            if (val >= 100 && val <= 10000) {
                err = modbus_write_single_register(g_modbus, config_get_modbus_slave_id(),
                                                   REG_BLINK_PERIOD, val);
            }
        }

        cJSON_AddBoolToObject(response, "success", err == ESP_OK);
        cJSON_AddStringToObject(response, "message", err == ESP_OK ? "OK" : "Failed");
    }

    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// API Handlers - LED Modbus (Full Slave Protocol Support)
// ============================================================================

// GET /api/ledmodbus/status - Get full LED slave status
static esp_err_t api_ledmodbus_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    // Get slave ID from query parameter
    char query_buf[32] = {0};
    char id_param[8] = "2";  // Default
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        httpd_query_key_value(query_buf, "id", id_param, sizeof(id_param));
    }
    uint8_t slave_id = atoi(id_param);

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(root, "connected", false);
        cJSON_AddStringToObject(root, "error", "Modbus not initialized");
    } else {
        // Read LED registers (0x0000-0x0002)
        uint16_t led_values[3];
        esp_err_t ret = modbus_read_holding_registers(g_modbus, slave_id,
                                                       REG_LED_STATE, 3, led_values);
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(root, "connected", true);
            cJSON_AddNumberToObject(root, "slave_id", slave_id);
            cJSON_AddBoolToObject(root, "led_on", led_values[0] != 0);
            cJSON_AddBoolToObject(root, "blink_mode", led_values[1] != 0);
            cJSON_AddNumberToObject(root, "blink_period", led_values[2]);

            // Read firmware version (0x0101)
            uint16_t fw_version = 0;
            if (modbus_read_holding_registers(g_modbus, slave_id,
                                              REG_FW_VERSION, 1, &fw_version) == ESP_OK) {
                char fw_str[16];
                snprintf(fw_str, sizeof(fw_str), "v%d.%d", (fw_version >> 8) & 0xFF, fw_version & 0xFF);
                cJSON_AddStringToObject(root, "fw_version", fw_str);
            } else {
                cJSON_AddStringToObject(root, "fw_version", "unknown");
            }
        } else {
            cJSON_AddBoolToObject(root, "connected", false);
            cJSON_AddStringToObject(root, "error", "Slave not responding");
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/ledmodbus/control - Control LED state, blink mode, period
static esp_err_t api_ledmodbus_control_handler(httpd_req_t *req)
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

    // Get slave ID
    cJSON *id_json = cJSON_GetObjectItem(root, "slave_id");
    uint8_t slave_id = cJSON_IsNumber(id_json) ? id_json->valueint : config_get_modbus_slave_id();

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Modbus not initialized");
    } else {
        // Check for LED state
        cJSON *led_on = cJSON_GetObjectItem(root, "led_on");
        if (cJSON_IsBool(led_on)) {
            err = modbus_write_single_register(g_modbus, slave_id,
                                               REG_LED_STATE, cJSON_IsTrue(led_on) ? 1 : 0);
        }

        // Check for blink mode
        cJSON *blink = cJSON_GetObjectItem(root, "blink_mode");
        if (cJSON_IsBool(blink)) {
            err = modbus_write_single_register(g_modbus, slave_id,
                                               REG_BLINK_MODE, cJSON_IsTrue(blink) ? 1 : 0);
        }

        // Check for blink period
        cJSON *period = cJSON_GetObjectItem(root, "blink_period");
        if (cJSON_IsNumber(period)) {
            int val = period->valueint;
            if (val >= 100 && val <= 10000) {
                err = modbus_write_single_register(g_modbus, slave_id,
                                                   REG_BLINK_PERIOD, val);
            } else {
                cJSON_AddBoolToObject(response, "success", false);
                cJSON_AddStringToObject(response, "message", "Period must be 100-10000ms");
                goto send_response_ledmodbus;
            }
        }

        cJSON_AddBoolToObject(response, "success", err == ESP_OK);
        cJSON_AddStringToObject(response, "message", err == ESP_OK ? "OK" : "Command failed");
    }

send_response_ledmodbus:
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

// POST /api/ledmodbus/config - Change slave ID, save config, reboot
static esp_err_t api_ledmodbus_config_handler(httpd_req_t *req)
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

    // Get current slave ID
    cJSON *id_json = cJSON_GetObjectItem(root, "slave_id");
    uint8_t slave_id = cJSON_IsNumber(id_json) ? id_json->valueint : config_get_modbus_slave_id();

    if (g_modbus == NULL) {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Modbus not initialized");
        goto send_config_response;
    }

    // Check for new slave ID change
    cJSON *new_id = cJSON_GetObjectItem(root, "new_slave_id");
    if (cJSON_IsNumber(new_id)) {
        int val = new_id->valueint;
        if (val >= 1 && val <= 247) {
            ESP_LOGI(TAG, "Changing slave ID from %d to %d", slave_id, val);
            err = modbus_write_single_register(g_modbus, slave_id, REG_SLAVE_ID, val);
            if (err == ESP_OK) {
                cJSON_AddBoolToObject(response, "success", true);
                cJSON_AddStringToObject(response, "message", "Slave ID changed. Device will reboot.");
                cJSON_AddNumberToObject(response, "new_id", val);
                goto send_config_response;
            }
        } else {
            cJSON_AddBoolToObject(response, "success", false);
            cJSON_AddStringToObject(response, "message", "Slave ID must be 1-247");
            goto send_config_response;
        }
    }

    // Check for save config command
    cJSON *save = cJSON_GetObjectItem(root, "save_config");
    if (cJSON_IsTrue(save)) {
        ESP_LOGI(TAG, "Sending save config command to slave %d", slave_id);
        err = modbus_write_single_register(g_modbus, slave_id, REG_SAVE_CONFIG, 1);
        if (err == ESP_OK) {
            cJSON_AddBoolToObject(response, "success", true);
            cJSON_AddStringToObject(response, "message", "Configuration saved to flash");
            goto send_config_response;
        }
    }

    // Check for reboot command
    cJSON *reboot = cJSON_GetObjectItem(root, "reboot");
    if (cJSON_IsTrue(reboot)) {
        ESP_LOGI(TAG, "Sending reboot command to slave %d", slave_id);
        err = modbus_write_single_register(g_modbus, slave_id, REG_REBOOT, REBOOT_MAGIC);
        if (err == ESP_OK) {
            cJSON_AddBoolToObject(response, "success", true);
            cJSON_AddStringToObject(response, "message", "Slave device rebooting...");
            goto send_config_response;
        }
    }

    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message", err == ESP_OK ? "OK" : "Command failed");

send_config_response:
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

    ESP_LOGI(TAG, "Scanning for mightyZAP actuators (IDs 1-10)...");

    int count = 0;
    for (uint8_t id = 1; id <= 10; id++) {
        uint16_t model = 0;
        esp_err_t ret = modbus_read_holding_registers(g_modbus, id,
                                                       MZAP_REG_MODEL_NUMBER, 1, &model);
        // mightyZAP models are typically > 100 (e.g., 350, 500, etc.)
        // This filters out false positives from LED slaves where reg 0x0000 is LED state (0 or 1)
        if (ret == ESP_OK && model > 100) {
            ESP_LOGI(TAG, "Found actuator at ID %d, model: %u", id, model);

            // Auto-add to active actuators
            add_actuator(id);

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddNumberToObject(item, "model", model);
            cJSON_AddItemToArray(found, item);
            count++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
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
    http_config.max_uri_handlers = 35;
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
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = js_handler,
    };
    httpd_register_uri_handler(s_server, &js_uri);

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

    // API - LED Control
    httpd_uri_t led_status_uri = {
        .uri = "/api/led/status",
        .method = HTTP_GET,
        .handler = api_led_status_handler,
    };
    httpd_register_uri_handler(s_server, &led_status_uri);

    httpd_uri_t led_control_uri = {
        .uri = "/api/led/control",
        .method = HTTP_POST,
        .handler = api_led_control_handler,
    };
    httpd_register_uri_handler(s_server, &led_control_uri);

    // API - LED Modbus (Full Protocol Support)
    httpd_uri_t ledmodbus_status_uri = {
        .uri = "/api/ledmodbus/status",
        .method = HTTP_GET,
        .handler = api_ledmodbus_status_handler,
    };
    httpd_register_uri_handler(s_server, &ledmodbus_status_uri);

    httpd_uri_t ledmodbus_control_uri = {
        .uri = "/api/ledmodbus/control",
        .method = HTTP_POST,
        .handler = api_ledmodbus_control_handler,
    };
    httpd_register_uri_handler(s_server, &ledmodbus_control_uri);

    httpd_uri_t ledmodbus_config_uri = {
        .uri = "/api/ledmodbus/config",
        .method = HTTP_POST,
        .handler = api_ledmodbus_config_handler,
    };
    httpd_register_uri_handler(s_server, &ledmodbus_config_uri);

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

    s_running = true;
    ESP_LOGI(TAG, "Web server started");
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
