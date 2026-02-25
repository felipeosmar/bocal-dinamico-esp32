/**
 * @file static_files.c
 * @brief Static file serving handlers (HTML, CSS, JS)
 */

#include "handlers.h"

static const char *TAG = "WEB_STATIC";

esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type)
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

esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return serve_file(req, "/www/index.html", "text/html");
}

esp_err_t css_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return serve_file(req, "/www/style.css", "text/css");
}

esp_err_t js_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return serve_file(req, "/www/core.js", "application/javascript");
}

esp_err_t tabs_html_handler(httpd_req_t *req)
{
    // Extract filename from URI (e.g., "/tabs/actuators.html" -> "actuators.html")
    const char *uri = req->uri;
    const char *filename = strrchr(uri, '/');
    if (!filename) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    filename++; // Skip the '/'

    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/www/tabs/%s", filename);
    return serve_file(req, filepath, "text/html");
}

esp_err_t tabs_js_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *filename = strrchr(uri, '/');
    if (!filename) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    filename++;

    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/www/tabs/%s", filename);
    return serve_file(req, filepath, "application/javascript");
}

esp_err_t favicon_handler(httpd_req_t *req)
{
    return serve_file(req, "/www/favicon.ico", "image/x-icon");
}
