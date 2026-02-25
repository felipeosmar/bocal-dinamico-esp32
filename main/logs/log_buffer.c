/**
 * @file log_buffer.c
 * @brief Circular buffer implementation for ESP-IDF log capture
 */

#include "log_buffer.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

// Buffer configuration
#define LOG_BUFFER_SIZE     100     // Number of entries (100 * ~120 bytes = ~12KB)
#define MUTEX_TIMEOUT_MS    10      // Short timeout to avoid blocking

static const char *TAG = "LOGBUF";

// Circular buffer storage
static log_entry_t s_buffer[LOG_BUFFER_SIZE];
static uint16_t s_head = 0;         // Next write position
static uint16_t s_count = 0;        // Current number of entries
static uint32_t s_sequence = 0;     // Monotonically increasing sequence
static uint32_t s_dropped = 0;      // Dropped entries count

// Thread safety
static SemaphoreHandle_t s_mutex = NULL;

// Original vprintf function (for passthrough)
static vprintf_like_t s_original_vprintf = NULL;

// Flag to prevent recursion
static bool s_in_hook = false;

/**
 * @brief Parse ESP-IDF log format and extract level, tag, message
 *
 * ESP-IDF format: "I (12345) TAG: message\n"
 * Level chars: E, W, I, D, V
 */
static bool parse_log_line(const char *line, uint8_t *level, char *tag, size_t tag_size,
                           char *message, size_t msg_size, uint32_t *timestamp)
{
    // Minimum valid log: "I (0) T: m"
    if (strlen(line) < 10) return false;

    // Parse level character
    char level_char = line[0];
    switch (level_char) {
        case 'E': *level = ESP_LOG_ERROR; break;
        case 'W': *level = ESP_LOG_WARN; break;
        case 'I': *level = ESP_LOG_INFO; break;
        case 'D': *level = ESP_LOG_DEBUG; break;
        case 'V': *level = ESP_LOG_VERBOSE; break;
        default: return false;  // Not a standard log line
    }

    // Expect " (" after level
    if (line[1] != ' ' || line[2] != '(') return false;

    // Parse timestamp
    const char *ts_start = line + 3;
    char *ts_end = NULL;
    unsigned long ts = strtoul(ts_start, &ts_end, 10);
    if (ts_end == ts_start || *ts_end != ')') return false;
    *timestamp = (uint32_t)ts;

    // Skip ") "
    const char *tag_start = ts_end + 2;

    // Find tag end (": ")
    const char *tag_end = strstr(tag_start, ": ");
    if (tag_end == NULL) return false;

    // Copy tag
    size_t tag_len = tag_end - tag_start;
    if (tag_len >= tag_size) tag_len = tag_size - 1;
    strncpy(tag, tag_start, tag_len);
    tag[tag_len] = '\0';

    // Copy message (after ": ")
    const char *msg_start = tag_end + 2;
    size_t msg_len = strlen(msg_start);

    // Remove trailing newline
    while (msg_len > 0 && (msg_start[msg_len - 1] == '\n' || msg_start[msg_len - 1] == '\r')) {
        msg_len--;
    }

    if (msg_len >= msg_size) msg_len = msg_size - 1;
    strncpy(message, msg_start, msg_len);
    message[msg_len] = '\0';

    return true;
}

/**
 * @brief Add entry to circular buffer
 */
static void add_entry(uint8_t level, const char *tag, const char *message, uint32_t timestamp)
{
    if (s_mutex == NULL) return;

    // Try to take mutex with short timeout
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        s_dropped++;
        return;
    }

    // Write to current position
    log_entry_t *entry = &s_buffer[s_head];
    entry->timestamp_ms = timestamp;
    entry->sequence = ++s_sequence;
    entry->level = level;
    strncpy(entry->tag, tag, sizeof(entry->tag) - 1);
    entry->tag[sizeof(entry->tag) - 1] = '\0';
    strncpy(entry->message, message, sizeof(entry->message) - 1);
    entry->message[sizeof(entry->message) - 1] = '\0';

    // Advance head
    s_head = (s_head + 1) % LOG_BUFFER_SIZE;
    if (s_count < LOG_BUFFER_SIZE) {
        s_count++;
    }

    xSemaphoreGive(s_mutex);
}

/**
 * @brief Custom vprintf hook for capturing logs
 */
static int log_vprintf_hook(const char *fmt, va_list args)
{
    // Prevent recursion
    if (s_in_hook) {
        if (s_original_vprintf) {
            return s_original_vprintf(fmt, args);
        }
        return vprintf(fmt, args);
    }

    s_in_hook = true;

    // Format the message (stack-allocated for thread safety)
    char line_buffer[256];
    int len = vsnprintf(line_buffer, sizeof(line_buffer), fmt, args);

    // Try to parse and store
    uint8_t level;
    char tag[16];
    char message[96];
    uint32_t timestamp;

    if (parse_log_line(line_buffer, &level, tag, sizeof(tag),
                       message, sizeof(message), &timestamp)) {
        add_entry(level, tag, message, timestamp);
    }

    s_in_hook = false;

    // Pass through to original handler
    if (s_original_vprintf) {
        // We need to re-format since we consumed args
        return printf("%s", line_buffer);
    }
    return printf("%s", line_buffer);
}

esp_err_t log_buffer_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;  // Already initialized
    }

    // Create mutex
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Clear buffer
    memset(s_buffer, 0, sizeof(s_buffer));
    s_head = 0;
    s_count = 0;
    s_sequence = 0;
    s_dropped = 0;

    // Install vprintf hook
    s_original_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    ESP_LOGI(TAG, "Log buffer initialized (capacity: %d entries)", LOG_BUFFER_SIZE);

    return ESP_OK;
}

esp_err_t log_buffer_get_since(uint32_t since_sequence, log_entry_t *entries,
                                uint16_t max_entries, uint16_t *count)
{
    if (entries == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        *count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        *count = 0;
        return ESP_ERR_TIMEOUT;
    }

    *count = 0;

    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // Calculate start position (oldest entry)
    uint16_t start;
    if (s_count < LOG_BUFFER_SIZE) {
        start = 0;
    } else {
        start = s_head;  // Oldest is at head in full buffer
    }

    // Iterate through buffer
    for (uint16_t i = 0; i < s_count && *count < max_entries; i++) {
        uint16_t idx = (start + i) % LOG_BUFFER_SIZE;
        if (s_buffer[idx].sequence > since_sequence) {
            memcpy(&entries[*count], &s_buffer[idx], sizeof(log_entry_t));
            (*count)++;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t log_buffer_get_stats(log_buffer_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    stats->total_entries = s_sequence;
    stats->dropped_entries = s_dropped;
    stats->current_sequence = s_sequence;
    stats->buffer_size = LOG_BUFFER_SIZE;
    stats->buffer_used = s_count;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t log_buffer_clear(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_head = 0;
    s_count = 0;
    // Don't reset sequence - clients may still reference old sequences

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Log buffer cleared");
    return ESP_OK;
}

uint32_t log_buffer_get_sequence(void)
{
    return s_sequence;
}
