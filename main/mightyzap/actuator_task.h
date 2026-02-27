#ifndef ACTUATOR_TASK_H
#define ACTUATOR_TASK_H

#include "esp_err.h"
#include "mightyzap.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACT_CMD_MOVE,
    ACT_CMD_STOP,
    ACT_CMD_SET_SPEED,
    ACT_CMD_SET_CURRENT,
    ACT_CMD_SYNC_MOVE
} actuator_cmd_type_t;

typedef struct {
    actuator_cmd_type_t type;
    union {
        mightyzap_handle_t actuator; // For ACT_CMD_MOVE, ACT_CMD_STOP
        modbus_handle_t modbus;      // For ACT_CMD_SYNC_MOVE
    } handle;
    uint16_t value; // Can be position, speed, or current
} actuator_cmd_t;

/**
 * @brief Initialize the dedicated actuator FreeRTOS thread and queue.
 */
esp_err_t actuator_task_init(void);

/**
 * @brief Enqueue an asynchronous move command.
 *
 * @param handle Actuator handle
 * @param position Target position (0-4095)
 * @return esp_err_t ESP_OK if enqueued successfully, ESP_ERR_TIMEOUT if queue
 * is full
 */
esp_err_t actuator_move_async(mightyzap_handle_t handle, uint16_t position);

/**
 * @brief Enqueue a stop (force disable) command asynchronously.
 */
esp_err_t actuator_stop_async(mightyzap_handle_t handle);

/**
 * @brief Enqueue a synchronous move command (Broadcast to ID 0)
 *
 * @param modbus Modbus handle for the secondary bus containing sync actuators
 * @param position Target position (0-4095)
 * @return esp_err_t ESP_OK if enqueued successfully
 */
esp_err_t actuator_move_sync_async(modbus_handle_t modbus,
                                   uint16_t position);

#ifdef __cplusplus
}
#endif

#endif // ACTUATOR_TASK_H
