/**
 * @file health_monitor.c
 * @brief System health monitoring implementation
 */

#include "health_monitor.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_littlefs.h"

#include "wifi_manager.h"
#include "modbus_rtu.h"

static const char *TAG = "HEALTH";

// Health check interval (seconds)
#define HEALTH_CHECK_INTERVAL_S 10

// Heap warning threshold
#define HEAP_WARNING_THRESHOLD 20000

// Health status
static system_health_t s_health = {0};
static TaskHandle_t s_health_task_handle = NULL;
static bool s_initialized = false;

/**
 * @brief Health monitoring task
 */
static void health_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Health monitor task started");
    
    // Add this task to watchdog
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add task to WDT: %s", esp_err_to_name(wdt_err));
    }
    
    TickType_t last_wake = xTaskGetTickCount();
    int check_counter = 0;
    
    while (1) {
        // Feed watchdog every iteration (every 2 seconds)
        esp_task_wdt_reset();
        
        // Increment counter, do full check every 5 iterations (10 seconds)
        check_counter++;
        
        if (check_counter >= 5) {
            check_counter = 0;
            
            // Update uptime
            s_health.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000);
            
            // Update heap info
            s_health.free_heap = esp_get_free_heap_size();
            s_health.min_free_heap = esp_get_minimum_free_heap_size();
            
            // Check WiFi
            s_health.wifi_connected = wifi_manager_is_connected();
            
            // Check Modbus (consider active if any TX in last period)
            const modbus_stats_t *mb_stats = modbus_get_stats();
            static uint32_t last_tx_count = 0;
            s_health.modbus_active = (mb_stats->tx_count > last_tx_count);
            last_tx_count = mb_stats->tx_count;
            s_health.total_error_count = mb_stats->error_count;
            
            // Check filesystem
            size_t total, used;
            s_health.filesystem_ok = (esp_littlefs_info("userdata", &total, &used) == ESP_OK);
            
            // Log warnings
            if (s_health.free_heap < HEAP_WARNING_THRESHOLD) {
                ESP_LOGW(TAG, "Low heap warning: %lu bytes free", s_health.free_heap);
            }
            
            // Periodic status log (every 6 full checks = 1 minute)
            static int log_counter = 0;
            if (++log_counter >= 6) {
                log_counter = 0;
                health_monitor_log_status();
            }
        }
        
        // Wait 2 seconds (well under the 10s WDT timeout)
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(2000));
    }
}

esp_err_t health_monitor_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    
    // Initialize health status
    memset(&s_health, 0, sizeof(s_health));
    s_health.reset_reason = esp_reset_reason();
    
    // Log reset reason
    const char *reset_reasons[] = {
        "Unknown", "Power-on", "External", "SW", "Panic",
        "Int WDT", "Task WDT", "WDT", "Deep Sleep", "Brownout", "SDIO"
    };
    int reason = s_health.reset_reason;
    if (reason >= 0 && reason < sizeof(reset_reasons)/sizeof(reset_reasons[0])) {
        ESP_LOGI(TAG, "Reset reason: %s", reset_reasons[reason]);
    }
    
    // Create health monitoring task
    BaseType_t ret = xTaskCreate(
        health_monitor_task,
        "health_mon",
        4096,
        NULL,
        tskIDLE_PRIORITY + 2,  // Low priority
        &s_health_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create health monitor task");
        return ESP_FAIL;
    }
    
    s_initialized = true;
    ESP_LOGI(TAG, "Health monitor initialized");
    return ESP_OK;
}

void health_monitor_deinit(void)
{
    if (s_health_task_handle != NULL) {
        esp_task_wdt_delete(s_health_task_handle);
        vTaskDelete(s_health_task_handle);
        s_health_task_handle = NULL;
    }
    s_initialized = false;
}

const system_health_t* health_monitor_get_status(void)
{
    return &s_health;
}

bool health_monitor_is_healthy(void)
{
    return s_health.filesystem_ok && 
           (s_health.free_heap > HEAP_WARNING_THRESHOLD);
}

void health_monitor_log_status(void)
{
    ESP_LOGI(TAG, "=== System Health ===");
    ESP_LOGI(TAG, "Uptime: %lu seconds", s_health.uptime_seconds);
    ESP_LOGI(TAG, "Heap: %lu free, %lu min", s_health.free_heap, s_health.min_free_heap);
    ESP_LOGI(TAG, "WiFi: %s, Modbus: %s, FS: %s",
             s_health.wifi_connected ? "OK" : "DISC",
             s_health.modbus_active ? "ACTIVE" : "IDLE",
             s_health.filesystem_ok ? "OK" : "ERR");
    ESP_LOGI(TAG, "Errors: %lu", s_health.total_error_count);
}
