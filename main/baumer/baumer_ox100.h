#ifndef BAUMER_OX100_H
#define BAUMER_OX100_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "modbus_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAUMER_QUALITY_OK          0
#define BAUMER_QUALITY_WEAK_SIGNAL 1
#define BAUMER_QUALITY_NO_SIGNAL   2

#define BAUMER_STATUS_BIT_PARAM_MODE  (1 << 0)
#define BAUMER_STATUS_BIT_TIME_SYNC   (1 << 1)
#define BAUMER_STATUS_BIT_VALID       (1 << 2)
#define BAUMER_STATUS_BIT_ALARM       (1 << 3)

#define BAUMER_NUM_VALUES 4

/**
 * @brief Baumer OX100 measurement data
 */
typedef struct {
    uint16_t status;                    // Status word (see BAUMER_STATUS_BIT_*)
    uint8_t  quality;                   // 0=OK, 1=Weak signal, 2=No signal
    uint8_t  output;                    // Binary outputs (bit 0=out1, bit 1=out2)
    float    values[BAUMER_NUM_VALUES]; // Measured values 1-4
} baumer_measurement_t;

typedef struct baumer_ctx *baumer_handle_t;

/**
 * @brief Initialize Baumer OX100 sensor
 *
 * @param modbus Modbus handle (shared with other devices on the bus)
 * @param slave_id Modbus slave address of the sensor
 * @param handle Pointer to store the created handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t baumer_init(modbus_handle_t modbus, uint8_t slave_id, baumer_handle_t *handle);

/**
 * @brief Read measurements from sensor (blocks on RS485 bus)
 *
 * Reads FC04 Input Registers at address 200-210 (11 registers):
 * status, quality, output, and 4 Float32 measured values.
 * Also updates the internal cache.
 *
 * @param handle Baumer handle
 * @param out Measurement data (can be NULL to only update cache)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t baumer_read_measurements(baumer_handle_t handle, baumer_measurement_t *out);

/**
 * @brief Get last cached measurement (thread-safe, does not use RS485 bus)
 *
 * @param handle Baumer handle
 * @param out Measurement data
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if no data yet
 */
esp_err_t baumer_get_cached(baumer_handle_t handle, baumer_measurement_t *out);

/**
 * @brief Control sensor laser
 *
 * @param handle Baumer handle
 * @param on true=laser ON, false=laser OFF
 * @return esp_err_t ESP_OK on success
 */
esp_err_t baumer_set_laser(baumer_handle_t handle, bool on);

/**
 * @brief Free resources
 *
 * @param handle Baumer handle
 */
void baumer_deinit(baumer_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // BAUMER_OX100_H
