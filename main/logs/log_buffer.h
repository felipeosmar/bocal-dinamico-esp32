/**
 * @file log_buffer.h
 * @brief Circular buffer for capturing ESP-IDF logs for web interface display
 *
 * Captures logs via esp_log_set_vprintf() hook and stores them in a circular
 * buffer accessible via REST API for real-time web display.
 */

#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log entry structure
 *
 * Compact representation of a single log entry.
 * Total size: ~120 bytes per entry
 */
typedef struct {
    uint32_t timestamp_ms;      /**< Timestamp in milliseconds since boot */
    uint32_t sequence;          /**< Monotonically increasing sequence number */
    uint8_t level;              /**< Log level (0=None, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Verbose) */
    char tag[16];               /**< Log tag (component name) */
    char message[96];           /**< Log message (truncated if longer) */
} log_entry_t;

/**
 * @brief Log buffer statistics
 */
typedef struct {
    uint32_t total_entries;     /**< Total entries ever captured */
    uint32_t dropped_entries;   /**< Entries dropped due to buffer full */
    uint32_t current_sequence;  /**< Current sequence number */
    uint16_t buffer_size;       /**< Maximum buffer capacity */
    uint16_t buffer_used;       /**< Current entries in buffer */
} log_buffer_stats_t;

/**
 * @brief Initialize log buffer and install vprintf hook
 *
 * Must be called early in app_main(), before any significant logging.
 *
 * @return ESP_OK on success
 */
esp_err_t log_buffer_init(void);

/**
 * @brief Get log entries since a sequence number
 *
 * Returns entries with sequence > since_sequence, up to max_entries.
 * Caller must provide a pre-allocated array.
 *
 * @param since_sequence Return entries after this sequence (0 for all)
 * @param entries Array to store entries
 * @param max_entries Maximum entries to return
 * @param count Output: actual number of entries returned
 * @return ESP_OK on success
 */
esp_err_t log_buffer_get_since(uint32_t since_sequence, log_entry_t *entries,
                                uint16_t max_entries, uint16_t *count);

/**
 * @brief Get current buffer statistics
 *
 * @param stats Output: buffer statistics
 * @return ESP_OK on success
 */
esp_err_t log_buffer_get_stats(log_buffer_stats_t *stats);

/**
 * @brief Clear all entries from buffer
 *
 * Does not reset sequence counter.
 *
 * @return ESP_OK on success
 */
esp_err_t log_buffer_clear(void);

/**
 * @brief Get latest sequence number
 *
 * Useful for clients to check if new logs are available without
 * fetching all entries.
 *
 * @return Current sequence number
 */
uint32_t log_buffer_get_sequence(void);

#ifdef __cplusplus
}
#endif

#endif // LOG_BUFFER_H
