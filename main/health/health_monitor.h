/**
 * @file health_monitor.h
 * @brief System health monitoring for industrial 24/7 operation
 */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief System health status
 */
typedef struct {
    // System
    uint32_t uptime_seconds;
    uint32_t free_heap;
    uint32_t min_free_heap;
    int reset_reason;
    
    // Subsystems
    bool wifi_connected;
    bool modbus_active;
    bool filesystem_ok;
    
    // Errors
    uint32_t total_error_count;
} system_health_t;

/**
 * @brief Initialize health monitor and start monitoring task
 * @return esp_err_t ESP_OK on success
 */
esp_err_t health_monitor_init(void);

/**
 * @brief Deinitialize health monitor
 */
void health_monitor_deinit(void);

/**
 * @brief Get current system health status
 * @return const system_health_t* Pointer to health status
 */
const system_health_t* health_monitor_get_status(void);

/**
 * @brief Check if system is healthy
 * @return true if all subsystems are OK
 */
bool health_monitor_is_healthy(void);

/**
 * @brief Log current health status
 */
void health_monitor_log_status(void);

#ifdef __cplusplus
}
#endif

#endif // HEALTH_MONITOR_H
