# Code Review — Bocal Dinâmico ESP32

**Date:** 2026-02-25  
**Reviewer:** Automated analysis (Claude)  
**Scope:** Full project — firmware, web frontend, build system

---

## Executive Summary

The project is well-structured for an ESP32 industrial control system. The layered architecture (RS485 → Modbus → mightyZAP → API handlers) is clean, and the opaque handle pattern provides good encapsulation. The Modbus layer has solid retry/backoff logic. However, there are several **thread safety issues**, **missing authentication on API endpoints**, and some **potential buffer overflows** that should be addressed before production deployment.

---

## 🔴 CRITICAL — Bugs & Safety Issues

### C1. Authentication bypass — No auth check on ANY API endpoint
**File:** `main/webserver/handlers/*.c` (all API handlers)  
**Problem:** The `check_auth()` function exists in `auth.c` but is **never called** by any handler. All API endpoints (`/api/restart`, `/api/files/delete`, `/api/actuator/factory-reset`, etc.) are completely unauthenticated despite `web_auth_enabled` being `true` in config.  
**Impact:** Anyone on the network can restart the device, delete files, factory-reset actuators, or change WiFi credentials.  
**Fix:** Add `if (!check_auth(req)) return send_unauthorized(req);` at the top of every API handler, or use a pre-request hook. Consider a macro:
```c
#define REQUIRE_AUTH(req) do { if (!check_auth(req)) return send_unauthorized(req); } while(0)
```

### C2. Race condition — Config getters return pointers to unprotected static data
**File:** `main/config/config_manager.c`, lines ~300-310 (getters)  
**Problem:** Functions like `config_get_wifi_ssid()` return `const char*` pointers directly into `s_config` static memory without holding the mutex. If another task calls a setter or `config_load()` concurrently, the caller reads corrupted/partial data.  
**Impact:** Garbled WiFi credentials, truncated strings, potential crashes.  
**Fix:** Either (a) copy into caller-provided buffers under mutex, or (b) since ESP32 is single-writer (web server task), document the threading model and ensure setters+save are only called from one task.

### C3. Race condition — Global handles accessed without synchronization
**File:** `main/main.c` (globals), `main/webserver/handlers/*.c` (consumers)  
**Problem:** `g_rs485`, `g_modbus`, `g_actuator` are written in `app_main()` and read from the HTTP server task without any memory barrier or synchronization. While currently safe because `app_main` sets them before starting the server, the `api_setup.c` handlers call `rs485_set_baud()` which changes UART state while other handlers may be polling actuator status.  
**Impact:** The smart-scan and standardize endpoints change baud rate while the actuators tab polls status on a 3-second interval, causing bus collisions.  
**Fix:** Add a global mutex for RS485 bus access at the API layer, or use the existing RS485 driver mutex consistently. The frontend command lock only prevents UI-initiated collisions, not server-side ones from different browser sessions.

### C4. Static buffer in vprintf hook — Not thread-safe
**File:** `main/logs/log_buffer.c`, line ~115  
**Problem:** `static char line_buffer[256]` in `log_vprintf_hook()` is shared across all tasks since `esp_log` can be called from any context. Two tasks logging simultaneously will corrupt each other's output.  
**Impact:** Garbled log entries, potential buffer overflow if vsnprintf output is read while being overwritten.  
**Fix:** Use a stack-allocated buffer instead:
```c
char line_buffer[256];  // Remove 'static'
```
Stack usage is ~256 bytes per logging call, acceptable for most tasks.

### C5. web_server_set_auth stores dangling pointers
**File:** `main/webserver/web_server.c`, `web_server_set_auth()`  
**Problem:** The function stores raw `const char*` pointers into `g_web_config.username`/`.password`. If the caller's string is freed or goes out of scope, these become dangling pointers. In `web_server_init()`, the pointers come from `config_get_web_username()` which returns pointers to static config memory — safe as long as config isn't reloaded.  
**Impact:** If config is reloaded while auth is in use, credentials comparison uses stale memory.  
**Fix:** Copy strings into `g_web_config` owned buffers, or change the struct to use `char[]` arrays.

### C6. flash.sh uses wrong www partition offset
**File:** `flash.sh`, line for `flash_www()`  
**Problem:** The script uses `0x1D0000` for the www partition, but `partitions.csv` shows www at offset `0x1C0000`.  
**Impact:** Flashing www data to wrong offset corrupts the www or userdata partition.  
**Fix:** Change `0x1D0000` to `0x1C0000` in both `flash_www()` and `flash_update()`.

### C7. config_load() mutex release inconsistency on error paths
**File:** `main/config/config_manager.c`, `config_load()`  
**Problem:** When JSON parsing fails (line ~"Failed to parse config file"), the function returns `ESP_FAIL` **without releasing the mutex**. The mutex is acquired at the top but only released on success paths.  
**Impact:** Deadlock — all subsequent config operations will timeout.  
**Fix:** Add `if (s_config_mutex) xSemaphoreGive(s_config_mutex);` before the `return ESP_FAIL;` after parse failure.

---

## 🟠 IMPORTANT — Architecture & Correctness Issues

### I1. Modbus stats are global statics without synchronization
**File:** `main/modbus/modbus_rtu.c`  
**Problem:** `s_modbus_stats` is accessed from the Modbus transaction path (which holds the RS485 mutex) AND from API handlers (`api_rs485_diag_handler`) without synchronization. Reads of multi-word fields (like `tx_count` at 32 bits on a 32-bit MCU) are atomic, but compound reads (reading tx_count then rx_count) may see inconsistent snapshots.  
**Fix:** Accept as low-risk for diagnostic counters, or add a quick mutex/critical section around stat reads.

### I2. mightyzap_sync_move_wait() blocks the HTTP server task
**File:** `main/mightyzap/mightyzap.c`, `mightyzap_sync_move_wait()`; called from `api_actuator_sync_move_handler()`  
**Problem:** This function blocks with `vTaskDelay()` in a loop for up to `timeout_ms` (default 10s). Since ESP-IDF HTTP server runs handlers on its own task, this blocks ALL other HTTP requests for the duration.  
**Impact:** Web UI becomes unresponsive during sync moves. Health monitor and other HTTP clients timeout.  
**Fix:** Either (a) run sync moves in a separate task with status polling from the API, or (b) document that `wait=true` blocks the server and recommend `wait=false` + `/api/actuator/sync-status` polling from the frontend.

### I3. Actuator slot array not thread-safe
**File:** `main/webserver/handlers/api_actuator.c`  
**Problem:** `s_actuators[]` array and `s_num_actuators` are modified by `add_actuator()`/`remove_actuator()` and read by `api_actuator_status_handler()` without any locking. Multiple concurrent HTTP requests (scan + status poll) can corrupt the array.  
**Fix:** Add a mutex around actuator slot operations, or ensure all HTTP handlers run on the same task (ESP-IDF default is single-threaded HTTP server, which makes this safe in practice — but document the assumption).

### I4. config.json committed with real WiFi credentials
**File:** `config.json` (root)  
**Problem:** Contains real WiFi SSID "Escalador" and password "Escalador@123". This file appears to be a reference/default config, but if committed to version control, it leaks credentials.  
**Fix:** Either gitignore `config.json`, or replace with placeholder values. The actual config is stored on LittleFS `/userdata/config.json`.

### I5. cJSON allocations not checked for NULL
**File:** Multiple API handlers  
**Problem:** `cJSON_CreateObject()`, `cJSON_PrintUnformatted()`, etc. can return NULL on OOM. Most handlers don't check. Example: `api_actuator_status_handler()` calls `cJSON_PrintUnformatted(root)` and immediately passes to `strlen()` — NULL dereference crash.  
**Impact:** ESP32 panics and reboots when heap is low.  
**Fix:** Check return values, especially `cJSON_Print*()`. On failure, send a simple error response.

### I6. HTTP request body not fully consumed on early returns
**File:** Multiple POST handlers (e.g., `api_actuator_control_handler`)  
**Problem:** When `httpd_req_recv()` fails or JSON parsing fails, some handlers return `ESP_FAIL` without consuming the full request body. ESP-IDF's HTTP server may misbehave with unconsumed request data.  
**Fix:** Always consume the full request body or call `httpd_resp_send_err()` which handles cleanup.

### I7. `tasks.js` redefines `formatBytes()` and `formatUptime()` already in `core.js`
**File:** `main/www/tabs/tasks.js`  
**Problem:** These utility functions are duplicated with slightly different implementations (core.js uses ms, tasks.js uses seconds for uptime). The tasks.js versions shadow the global ones.  
**Impact:** Inconsistent formatting, wasted flash space.  
**Fix:** Remove duplicates from `tasks.js` and adapt to use the core.js versions (convert seconds to ms for `formatUptime`).

---

## 🟡 NICE-TO-HAVE — Code Quality & Best Practices

### N1. Magic numbers throughout
**Files:** Various  
- `4095` (max position) — should be `MZAP_MAX_POSITION`
- `1023` (max speed) — should be `MZAP_MAX_SPEED`
- `1600`/`800` (max current) — should be `MZAP_MAX_CURRENT`
- `247` (max Modbus ID) — should be `MODBUS_MAX_SLAVE_ADDR`
- `400` speed clamp in JS — should come from config/API
- `0xF6`, `0xF8` mentioned in comments for restart/factory reset but using regular FC06 writes

### N2. RS485 debug hex dump enabled in production
**File:** `main/rs485/rs485_driver.c`, line 11  
**Problem:** `RS485_DEBUG_HEX_DUMP` is set to `1`, causing every TX/RX to be logged at INFO level. This floods the log buffer and wastes CPU.  
**Fix:** Set to `0` or use `ESP_LOGD` instead of `ESP_LOGI`.

### N3. `SPIFFS` referenced in config_manager.h docs but LittleFS is used
**File:** `main/config/config_manager.h`, line 9: "Initialize config manager and SPIFFS"  
**Fix:** Update comment to "Initialize config manager and LittleFS".

### N4. CMakeLists.txt REQUIRES includes `spiffs` but project uses LittleFS
**File:** `main/CMakeLists.txt`  
**Problem:** `spiffs` is in REQUIRES but the project uses `esp_littlefs`. Should be `esp_littlefs` instead.  
**Fix:** Replace `spiffs` with `esp_littlefs` in REQUIRES.

### N5. Inconsistent error handling patterns in API handlers
Some handlers use `goto send_response` pattern (good), others use early returns with `httpd_resp_send_err()`. The goto pattern is safer for cleanup.  
**Fix:** Standardize on one pattern. The goto approach with a response JSON object is cleaner.

### N6. No CORS headers
**File:** All API handlers  
**Problem:** No `Access-Control-Allow-Origin` headers. If the web UI is ever served from a different origin (e.g., during development), API calls fail.  
**Fix:** Add CORS headers at least for development, or add a global pre-request handler.

### N7. tasks.js doesn't use the module registration pattern
**File:** `main/www/tabs/tasks.js`  
**Problem:** Unlike other modules, tasks.js calls `initTasksTab()` directly via DOM events instead of using `registerModule('tasks', initTasksTab)`. The auto-refresh interval is never cleaned up when leaving the tab.  
**Fix:** Use `registerModule('tasks', initTasksTab)` and add cleanup to stop the interval.

### N8. Frontend doesn't handle auth 401 responses
**File:** `main/www/core.js`, `api()` function  
**Problem:** If auth is enabled and credentials are wrong, the API returns 401 but the frontend doesn't detect this or prompt for login.  
**Fix:** Check `res.status === 401` in the `api()` function and show a login prompt.

### N9. No input validation on pin numbers
**File:** `main/config/config_manager.c`, setters  
**Problem:** `config_set_rs485_tx_pin()` etc. accept any uint8_t value without validating it's a valid GPIO number. Invalid pins could crash the UART driver on next boot.  
**Fix:** Validate against valid ESP32 GPIO range (0-39, excluding 6-11 for flash, 20, 24, 28-31).

### N10. Health monitor task stack might be tight
**File:** `main/health/health_monitor.c`  
**Problem:** Task stack is 2048 bytes. The task calls `esp_littlefs_info()` and `ESP_LOGI()` (which uses vprintf → the log hook with a 256-byte stack buffer if C4 is fixed). Could be tight.  
**Fix:** Increase to 3072 or monitor stack high water mark via the tasks API.

### N11. `mightyzap_sync_move_handler` has variable shadowing
**File:** `main/webserver/handlers/api_actuator.c`, ~line 480  
**Problem:** Local variable `uint16_t max_desync` shadows the parameter `max_desync` parsed from JSON earlier in the same function.  
**Fix:** Rename the local to `actual_desync` or `final_desync`.

### N12. No cache headers on static files
**File:** `main/webserver/handlers/static_files.c`  
**Problem:** CSS, JS, and HTML files are served without `Cache-Control` headers. Browsers re-fetch them on every page load.  
**Fix:** Add `httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600")` for CSS/JS files.

### N13. File upload doesn't handle binary files correctly
**File:** `main/webserver/handlers/api_files.c`, `api_files_upload_handler()`  
**Problem:** The multipart parser uses `strstr()` to find boundaries, which fails for binary files containing null bytes. The `buf[total_len] = '\0'` also truncates binary content.  
**Fix:** Use proper boundary-based parsing with `memchr()`/`memmem()` for binary support, or document text-only upload limitation.

### N14. WiFi scan in AP mode doesn't restore STA connection
**File:** `main/wifi/wifi_manager.c`, `wifi_manager_scan()`  
**Problem:** When in `WIFI_MODE_NULL`, the function starts STA mode for scanning but never stops it. When in AP mode, it switches to APSTA and back to AP, but doesn't rejoin the STA network if previously connected.  
**Fix:** Track and restore the full previous state.

---

## 🔒 Security

### S1. Passwords stored in plaintext
**File:** `main/config/config_manager.c`  
**Problem:** WiFi password, AP password, and web auth password are stored as plaintext in `/userdata/config.json` on LittleFS.  
**Impact:** Anyone with physical access can read the flash and extract credentials.  
**Mitigation:** For web auth, store a hash. For WiFi passwords, plaintext is necessary for the driver. Consider NVS encrypted storage.

### S2. Basic Auth credentials sent in cleartext
**File:** `main/webserver/handlers/auth.c`  
**Problem:** HTTP Basic Auth sends base64-encoded (not encrypted) credentials. No HTTPS support.  
**Impact:** Credentials are visible to anyone sniffing the network.  
**Mitigation:** Document this limitation. Consider session tokens with cookie-based auth to reduce credential exposure, or add HTTPS (requires more flash/RAM).

### S3. File manager allows writing to www partition
**File:** `main/webserver/handlers/api_files.c`  
**Problem:** The file write/upload/delete APIs work on both `www` and `userdata` partitions. An attacker can overwrite `index.html` or `core.js` to inject malicious code (XSS persistence).  
**Fix:** Make www partition read-only via the API (it's flashed at build time), or require re-authentication for www writes.

### S4. No rate limiting on authentication
**File:** `main/webserver/handlers/auth.c`  
**Problem:** No rate limiting or lockout on failed auth attempts. Brute-force attacks are trivial.  
**Fix:** Add a delay or lockout after N failed attempts.

---

## ⚡ Performance

### P1. Log viewer re-renders entire DOM on every poll
**File:** `main/www/core.js`, `renderLogs()`  
**Problem:** Every 1.5s, the entire `log-entries` innerHTML is rebuilt from all filtered entries. With 200 entries, this causes layout thrashing.  
**Fix:** Append only new entries and remove old ones from the DOM incrementally.

### P2. cJSON_Print used for all responses (formatted)
**File:** Most API handlers use `cJSON_PrintUnformatted()` ✓  
This is already correct — good job using unformatted output to save bandwidth.

### P3. Status polling creates cJSON trees even when data hasn't changed
**File:** `api_actuator_status_handler()`  
**Problem:** Every 3s poll builds a full cJSON tree, serializes, and frees. On ESP32, frequent malloc/free causes heap fragmentation.  
**Fix:** Consider a pre-formatted response buffer that's only rebuilt when state changes, or use a simpler snprintf-based JSON builder for hot paths.

### P4. Actuator scan delay too conservative
**File:** `main/webserver/handlers/api_actuator.c`, `api_actuator_scan_handler()`  
**Problem:** 10ms delay between each ID scan. With max_id=20, that's 200ms of pure delay, plus Modbus timeouts for non-responding IDs.  
**Fix:** The delay is fine for bus stability but could be reduced to 5ms since the Modbus transaction already includes inter-frame timing.

---

## Summary Table

| Priority | Count | Categories |
|----------|-------|------------|
| 🔴 Critical | 7 | Auth bypass, race conditions, deadlock, wrong flash offset |
| 🟠 Important | 7 | Blocking handlers, thread safety, credential leak, OOM crashes |
| 🟡 Nice-to-have | 14 | Code quality, caching, input validation, naming |
| 🔒 Security | 4 | Plaintext passwords, no HTTPS, XSS risk, no rate limiting |
| ⚡ Performance | 4 | DOM thrashing, heap fragmentation |

**Top 3 actions for immediate impact:**
1. **Add authentication to all API endpoints** (C1) — security critical
2. **Fix config_load mutex deadlock** (C7) — reliability critical  
3. **Fix flash.sh www partition offset** (C6) — deployment critical
