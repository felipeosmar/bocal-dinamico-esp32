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
    // Non-Volatile Memory (stored in EEPROM) - FC_MODBUS Model
    MZAP_REG_MODEL_NUMBER      = 0x0000,   // 40001 - Model Number (R)
    MZAP_REG_FIRMWARE_VERSION  = 0x0001,   // 40002 - Firmware Version (R)
    MZAP_REG_ID                = 0x0002,   // 40003 - Servo ID (RW) [1-247, default 1]
    MZAP_REG_BAUD_RATE         = 0x0003,   // 40004 - Baud Rate (RW) [16-128, default 32]
    MZAP_REG_PROTOCOL_TYPE     = 0x0004,   // 40005 - Protocol (RW) [0=Modbus, 1=IRRobot]
    MZAP_REG_SHORT_STROKE_LIM  = 0x0005,   // 40006 - Short Stroke Limit (RW) [0-4095]
    MZAP_REG_LONG_STROKE_LIM   = 0x0006,   // 40007 - Long Stroke Limit (RW) [0-4095]
    MZAP_REG_LOWEST_VOLTAGE    = 0x0007,   // 40008 - Lowest Limit Voltage (R) [default 70]
    MZAP_REG_HIGHEST_VOLTAGE   = 0x0008,   // 40009 - Highest Limit Voltage (R) [default 130]
    MZAP_REG_ALARM_LED         = 0x0009,   // 40010 - Alarm LED (RW) [default 32]
    MZAP_REG_ALARM_SHUTDOWN    = 0x000A,   // 40011 - Alarm Shutdown (RW) [default 32]
    MZAP_REG_START_COMPLIANCE  = 0x000B,   // 40012 - Start Compliance Margin (RW) [0-255, default 7]
    MZAP_REG_END_COMPLIANCE    = 0x000C,   // 40013 - End Compliance Margin (RW) [0-255, default 2]
    MZAP_REG_SPEED_LIMIT       = 0x000D,   // 40014 - Speed Limit (RW) [0-1023, default 1023]
    MZAP_REG_CURRENT_LIMIT     = 0x000E,   // 40015 - Current Limit (RW) [0-1600, default 800]

    // Volatile Memory (RAM) - FC_MODBUS Model (Force Control)
    MZAP_REG_FORCE_ON_OFF      = 0x0032,   // 40051 - Force Enable (RW) [0=Off, 1=On]
    MZAP_REG_LED_ON_OFF        = 0x0033,   // 40052 - LED Control (RW)
    MZAP_REG_GOAL_POSITION     = 0x0034,   // 40053 - Goal Position (RW) [0-4095]
    MZAP_REG_GOAL_SPEED        = 0x0035,   // 40054 - Goal Speed (RW) [0-1023]
    MZAP_REG_GOAL_CURRENT      = 0x0036,   // 40055 - Goal Current/Force (RW) [0-1600]
    MZAP_REG_PRESENT_POSITION  = 0x0037,   // 40056 - Present Position (R) [0-4095]
    MZAP_REG_PRESENT_CURRENT   = 0x0038,   // 40057 - Present Current (R) [0-1600]
    MZAP_REG_PRESENT_MOTOR_OP  = 0x0039,   // 40058 - Motor Operating Rate (R) [0-2048]
    MZAP_REG_PRESENT_VOLTAGE   = 0x003A,   // 40059 - Present Voltage (R) [0-255]
    MZAP_REG_MOVING            = 0x003B,   // 40060 - Moving Status (R) [0-1]
    MZAP_REG_HW_ERROR_STATE    = 0x003C,   // 40061 - Hardware Error State (R)

    // Special commands - NOTE: These use SP Function Codes (0xF6, 0xF8) in FC_MODBUS,
    // not regular registers. Current implementation may not work for FC_MODBUS model.
    MZAP_REG_RESTART           = 0x00FF,   // Placeholder - uses SP Function Code 0xF8
    MZAP_REG_FACTORY_RESET     = 0x00FE,   // Placeholder - uses SP Function Code 0xF6
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
