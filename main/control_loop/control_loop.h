#ifndef CONTROL_LOOP_H
#define CONTROL_LOOP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "baumer_ox100.h"
#include "mightyzap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROL_MAX_EQUATIONS   10
#define CONTROL_MIN_INTERVAL_MS 100
#define CONTROL_MAX_INTERVAL_MS 60000
#define CONTROL_MAX_CONSECUTIVE_ERRORS 5

/**
 * @brief Per-actuator linear equation: position = a * gap + b
 */
typedef struct {
    uint8_t actuator_id;
    float coeff_a;
    float coeff_b;
    bool enabled;
} control_equation_t;

/**
 * @brief Control loop configuration
 */
typedef struct {
    bool running;
    uint32_t interval_ms;
    uint8_t measurement_index;   // Which value[] to use (0-3)
    control_equation_t equations[CONTROL_MAX_EQUATIONS];
    uint8_t equation_count;
} control_config_t;

/**
 * @brief Control loop runtime status (read-only snapshot)
 */
typedef struct {
    bool running;
    float last_gap_value;
    uint8_t last_quality;
    uint32_t loop_count;
    uint32_t error_count;
    int64_t last_run_timestamp_ms;
    uint16_t computed_positions[CONTROL_MAX_EQUATIONS];
    uint8_t computed_actuator_ids[CONTROL_MAX_EQUATIONS];
    uint8_t computed_count;
} control_status_t;

/**
 * @brief Initialize the control loop task
 *
 * Creates the FreeRTOS task but does not start the control loop.
 * The task always reads the Baumer sensor for monitoring.
 * Actuator control only runs when started via control_loop_start().
 *
 * @param baumer Baumer sensor handle
 * @param config Initial configuration (loaded from config.json)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t control_loop_init(baumer_handle_t baumer, const control_config_t *config);

/**
 * @brief Start automatic actuator control
 */
esp_err_t control_loop_start(void);

/**
 * @brief Stop automatic actuator control (monitoring continues)
 */
esp_err_t control_loop_stop(void);

/**
 * @brief Set loop interval
 *
 * @param interval_ms Interval in milliseconds (100-60000)
 */
esp_err_t control_loop_set_interval(uint32_t interval_ms);

/**
 * @brief Set equation for a specific actuator
 */
esp_err_t control_loop_set_equation(uint8_t actuator_id, float a, float b, bool enabled);

/**
 * @brief Get current configuration
 */
esp_err_t control_loop_get_config(control_config_t *out);

/**
 * @brief Get current status snapshot (thread-safe)
 */
esp_err_t control_loop_get_status(control_status_t *out);

/**
 * @brief Set measurement index (which value[] to use for control, 0-3)
 */
esp_err_t control_loop_set_measurement_index(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_LOOP_H
