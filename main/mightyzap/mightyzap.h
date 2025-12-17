#ifndef MIGHTYZAP_H
#define MIGHTYZAP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "modbus_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief mightyZAP Modbus register addresses (Holding Registers 40001+)
 *        Address in Modbus = Register - 40001
 */
typedef enum {
    // Non-Volatile Memory (stored in EEPROM)
    MZAP_REG_MODEL_NUMBER      = 0x0000,   // 40001 - Model Number (R)
    MZAP_REG_FIRMWARE_VERSION  = 0x0001,   // 40002 - Firmware Version (R)
    MZAP_REG_ID                = 0x0002,   // 40003 - Servo ID (RW) [1-247, default 1]
    MZAP_REG_BAUD_RATE         = 0x0003,   // 40004 - Baud Rate (RW) [16-128, default 32]
    MZAP_REG_PROTOCOL_TYPE     = 0x0004,   // 40005 - Protocol (RW) [0=Modbus, 1=IRRobot]
    MZAP_REG_SHORT_STROKE_LIM  = 0x0005,   // 40006 - Short Stroke Limit (RW)
    MZAP_REG_LONG_STROKE_LIM   = 0x0006,   // 40007 - Long Stroke Limit (RW)
    MZAP_REG_SPEED_LIMIT       = 0x000A,   // 40011 - Speed Limit (RW)
    MZAP_REG_CURRENT_LIMIT     = 0x000B,   // 40012 - Current Limit (RW)
    MZAP_REG_MIN_POSITION      = 0x000C,   // 40013 - Min Position Calibration (RW)
    MZAP_REG_MAX_POSITION      = 0x000D,   // 40014 - Max Position Calibration (RW)

    // Volatile Memory (RAM)
    MZAP_REG_FORCE_ON_OFF      = 0x0080,   // 40129 - Force Enable (RW) [0=Off, 1=On]
    MZAP_REG_LED_ON_OFF        = 0x0081,   // 40130 - LED Control (RW)
    MZAP_REG_GOAL_POSITION     = 0x0086,   // 40135 - Goal Position (RW)
    MZAP_REG_GOAL_SPEED        = 0x0087,   // 40136 - Goal Speed (RW)
    MZAP_REG_GOAL_CURRENT      = 0x0088,   // 40137 - Goal Current/Force (RW)
    MZAP_REG_PRESENT_POSITION  = 0x0096,   // 40151 - Present Position (R)
    MZAP_REG_PRESENT_CURRENT   = 0x0097,   // 40152 - Present Current (R)
    MZAP_REG_PRESENT_MOTOR_OP  = 0x0098,   // 40153 - Motor Operating Rate (R)
    MZAP_REG_PRESENT_VOLTAGE   = 0x009A,   // 40155 - Present Voltage (R)
    MZAP_REG_MOVING            = 0x009F,   // 40160 - Moving Status (R)
    MZAP_REG_ACTION_REQUEST    = 0x00A0,   // 40161 - Action Request (W)
    MZAP_REG_RESTART           = 0x00A5,   // 40166 - Restart (W)
    MZAP_REG_FACTORY_RESET     = 0x00A6,   // 40167 - Factory Reset (W)
} mightyzap_register_t;

/**
 * @brief Baud rate values for mightyZAP
 */
typedef enum {
    MZAP_BAUD_9600   = 16,    // 0x10
    MZAP_BAUD_19200  = 32,    // 0x20
    MZAP_BAUD_57600  = 64,    // 0x40
    MZAP_BAUD_115200 = 128,   // 0x80
} mightyzap_baud_t;

/**
 * @brief mightyZAP actuator handle
 */
typedef struct mightyzap* mightyzap_handle_t;

/**
 * @brief mightyZAP status
 */
typedef struct {
    uint16_t position;          // Present position (0-4095 typical)
    uint16_t current;           // Present current (mA)
    uint16_t voltage;           // Present voltage (0.1V units)
    uint8_t moving;             // Moving status (0=stopped, 1=moving)
} mightyzap_status_t;

/**
 * @brief Initialize mightyZAP driver
 *
 * @param modbus Modbus RTU handle
 * @param slave_id Slave ID (1-247)
 * @param handle Pointer to store handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_init(modbus_handle_t modbus, uint8_t slave_id, mightyzap_handle_t *handle);

/**
 * @brief Deinitialize mightyZAP driver
 *
 * @param handle mightyZAP handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_deinit(mightyzap_handle_t handle);

/**
 * @brief Read model number
 *
 * @param handle mightyZAP handle
 * @param model Pointer to store model number
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_get_model(mightyzap_handle_t handle, uint16_t *model);

/**
 * @brief Enable/disable motor force (torque)
 *
 * @param handle mightyZAP handle
 * @param enable true to enable, false to disable
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_force_enable(mightyzap_handle_t handle, bool enable);

/**
 * @brief Set goal position
 *
 * @param handle mightyZAP handle
 * @param position Goal position (0-4095 typical, depends on stroke)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_position(mightyzap_handle_t handle, uint16_t position);

/**
 * @brief Set goal speed
 *
 * @param handle mightyZAP handle
 * @param speed Goal speed (0-1023)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_speed(mightyzap_handle_t handle, uint16_t speed);

/**
 * @brief Set goal current (force control)
 *
 * @param handle mightyZAP handle
 * @param current Goal current (0-800 typical, in mA)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_current(mightyzap_handle_t handle, uint16_t current);

/**
 * @brief Set position, speed and current at once
 *
 * @param handle mightyZAP handle
 * @param position Goal position
 * @param speed Goal speed
 * @param current Goal current
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_goal(mightyzap_handle_t handle, uint16_t position, uint16_t speed, uint16_t current);

/**
 * @brief Get present position
 *
 * @param handle mightyZAP handle
 * @param position Pointer to store position
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_get_position(mightyzap_handle_t handle, uint16_t *position);

/**
 * @brief Get current status (position, current, voltage, moving)
 *
 * @param handle mightyZAP handle
 * @param status Pointer to store status
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_get_status(mightyzap_handle_t handle, mightyzap_status_t *status);

/**
 * @brief Check if motor is moving
 *
 * @param handle mightyZAP handle
 * @param moving Pointer to store moving status
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_is_moving(mightyzap_handle_t handle, bool *moving);

/**
 * @brief Set LED state
 *
 * @param handle mightyZAP handle
 * @param state LED state (0=off, 1=on, 2=blinking varies by model)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_led(mightyzap_handle_t handle, uint8_t state);

/**
 * @brief Set actuator ID
 *
 * @param handle mightyZAP handle
 * @param new_id New ID (1-247)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_id(mightyzap_handle_t handle, uint8_t new_id);

/**
 * @brief Restart actuator
 *
 * @param handle mightyZAP handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_restart(mightyzap_handle_t handle);

/**
 * @brief Factory reset actuator
 *
 * @param handle mightyZAP handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_factory_reset(mightyzap_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // MIGHTYZAP_H
