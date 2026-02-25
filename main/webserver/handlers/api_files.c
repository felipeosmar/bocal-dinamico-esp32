/**
 * @file api_files.c
 * @brief File manager API handlers
 */

#include "handlers.h"

static const char *TAG = "API_FILES";

// ============================================================================
// Helper functions
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

// Helper: Check if partition is www (read-only for API writes)
static bool is_www_partition(const char *base_path)
{
    return (strcmp(base_path, "/www") == 0);
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

// ============================================================================
// API Handlers
// ============================================================================

// GET /api/files/list - List files in directory
esp_err_t api_files_list_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, dir_param);

    DIR *dir = opendir(full_path);
    if (!dir) {
        // If directory doesn't exist, return empty list
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) return send_json(req, NULL);
        cJSON *files = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "files", files);
        return send_json(req, root);
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

    return send_json(req, root);
}

// GET /api/files/info - Get storage info for all partitions
esp_err_t api_files_info_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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

    return send_json(req, root);
}

// GET /api/files/download - Download a file
esp_err_t api_files_download_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[256] = {0};
    char file_param[128] = {0};

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_OK;
    }
    url_decode(file_param);

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
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
esp_err_t api_files_view_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[256] = {0};
    char file_param[128] = {0};

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_OK;
    }
    url_decode(file_param);

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    return serve_file(req, full_path, "text/plain");
}

// GET /api/files/read - Read file content for editing
esp_err_t api_files_read_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[256] = {0};
    char file_param[128] = {0};

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) != ESP_OK ||
        httpd_query_key_value(query_buf, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_OK;
    }
    url_decode(file_param);

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Limit to 50KB
    if (fsize > 50 * 1024) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 50KB)");
        return ESP_OK;
    }

    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    fread(content, 1, fsize, f);
    content[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "content", content);
    cJSON_AddNumberToObject(root, "size", fsize);

    free(content);

    return send_json(req, root);
}

// POST /api/files/write - Write file content
esp_err_t api_files_write_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[64] = {0};
    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (is_www_partition(base_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "Write access to www partition is not allowed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Read POST data
    int total_len = req->content_len;
    if (total_len > 60 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_OK;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_OK;
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
        return ESP_OK;
    }

    if (!is_valid_path(file_param)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    FILE *f = fopen(full_path, "w");
    if (!f) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_OK;
    }

    fwrite(content_start, 1, content_len, f);
    fclose(f);
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    return send_json(req, root);
}

// POST /api/files/delete - Delete file or folder
esp_err_t api_files_delete_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[64] = {0};
    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (is_www_partition(base_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "Delete access to www partition is not allowed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_OK;
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
        return ESP_OK;
    }

    if (!is_valid_path(file_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, file_param);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    int result;
    if (S_ISDIR(st.st_mode)) {
        result = rmdir(full_path);
    } else {
        result = unlink(full_path);
    }

    if (result != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    return send_json(req, root);
}

// POST /api/files/mkdir - Create directory
esp_err_t api_files_mkdir_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[64] = {0};
    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (is_www_partition(base_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "Write access to www partition is not allowed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_OK;
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
        return ESP_OK;
    }

    if (!is_valid_path(dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    char full_path[160];
    build_full_path(full_path, sizeof(full_path), base_path, dir_param);

    if (mkdir(full_path, 0755) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create directory");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    return send_json(req, root);
}

// POST /api/files/upload - Upload file
esp_err_t api_files_upload_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char query_buf[256] = {0};
    char dir_param[128] = "/";

    const char *base_path = get_partition_path(req, query_buf, sizeof(query_buf));

    if (is_www_partition(base_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "Upload to www partition is not allowed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Get dir parameter
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        httpd_query_key_value(query_buf, "dir", dir_param, sizeof(dir_param));
        url_decode(dir_param);
    }

    if (!is_valid_path(dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    // Read multipart data
    int total_len = req->content_len;
    if (total_len > 100 * 1024) {  // 100KB max
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 100KB)");
        return ESP_OK;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_OK;
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
        return ESP_OK;
    }

    // Find file content (after double CRLF)
    char *content_start = strstr(buf, "\r\n\r\n");
    if (!content_start) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid format");
        return ESP_OK;
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
        return ESP_OK;
    }

    fwrite(content_start, 1, content_len, f);
    fclose(f);
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");

    send_json(req, root);

    ESP_LOGI(TAG, "File uploaded: %s", full_path);
    return ESP_OK;
}
