#ifndef MIGHTYZAP_H
#define MIGHTYZAP_H

#include "esp_err.h"
#include "modbus_rtu.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief mightyZAP Modbus register addresses (Holding Registers 40001+)
 *        Address in Modbus = Register - 40001
 */
typedef enum {
    // Non-Volatile Memory (stored in EEPROM) - FC_MODBUS Model
    MZAP_REG_MODEL_NUMBER = 0x0000,     // 40001 - Model Number (R)
    MZAP_REG_FIRMWARE_VERSION = 0x0001, // 40002 - Firmware Version (R)
    MZAP_REG_ID = 0x0002,        // 40003 - Servo ID (RW) [1-247, default 1]
    MZAP_REG_BAUD_RATE = 0x0003, // 40004 - Baud Rate (RW) [16=115200, 32=57600,
                               // 48=38400, 64=19200, 128=9600]
    MZAP_REG_PROTOCOL_TYPE =
            0x0004, // 40005 - Protocol (RW) [0=Modbus, 1=IRRobot]
    MZAP_REG_SHORT_STROKE_LIM =
            0x0005,                        // 40006 - Short Stroke Limit (RW) [0-4095]
    MZAP_REG_LONG_STROKE_LIM = 0x0006, // 40007 - Long Stroke Limit (RW) [0-4095]
    MZAP_REG_LOWEST_VOLTAGE =
            0x0007, // 40008 - Lowest Limit Voltage (R) [default 70]
    MZAP_REG_HIGHEST_VOLTAGE =
            0x0008, // 40009 - Highest Limit Voltage (R) [default 130]
    MZAP_REG_ALARM_LED = 0x0009,      // 40010 - Alarm LED (RW) [default 32]
    MZAP_REG_ALARM_SHUTDOWN = 0x000A, // 40011 - Alarm Shutdown (RW) [default 32]
    MZAP_REG_START_COMPLIANCE =
            0x000B, // 40012 - Start Compliance Margin (RW) [0-255, default 7]
    MZAP_REG_END_COMPLIANCE =
            0x000C, // 40013 - End Compliance Margin (RW) [0-255, default 2]
    MZAP_REG_SPEED_LIMIT =
            0x000D, // 40014 - Speed Limit (RW) [0-1023, default 1023]
    MZAP_REG_CURRENT_LIMIT =
            0x000E, // 40015 - Current Limit (RW) [0-1600, default 800]

    // Volatile Memory (RAM) - FC_MODBUS Model (Force Control)
    MZAP_REG_FORCE_ON_OFF = 0x0032,  // 40051 - Force Enable (RW) [0=Off, 1=On]
    MZAP_REG_LED_ON_OFF = 0x0033,    // 40052 - LED Control (RW)
    MZAP_REG_GOAL_POSITION = 0x0034, // 40053 - Goal Position (RW) [0-4095]
    MZAP_REG_GOAL_SPEED = 0x0035,    // 40054 - Goal Speed (RW) [0-1023]
    MZAP_REG_GOAL_CURRENT = 0x0036,  // 40055 - Goal Current/Force (RW) [0-1600]
    MZAP_REG_PRESENT_POSITION = 0x0037, // 40056 - Present Position (R) [0-4095]
    MZAP_REG_PRESENT_CURRENT = 0x0038,  // 40057 - Present Current (R) [0-1600]
    MZAP_REG_PRESENT_MOTOR_OP =
            0x0039, // 40058 - Motor Operating Rate (R) [0-2048]
    MZAP_REG_PRESENT_VOLTAGE = 0x003A, // 40059 - Present Voltage (R) [0-255]
    MZAP_REG_MOVING = 0x003B,          // 40060 - Moving Status (R) [0-1]
    MZAP_REG_HW_ERROR_STATE = 0x003C,  // 40061 - Hardware Error State (R)

    // Special commands - NOTE: These use SP Function Codes (0xF6, 0xF8) in
    // FC_MODBUS,
    // not regular registers. Current implementation may not work for FC_MODBUS
    // model.
    MZAP_REG_RESTART = 0x00FF,       // Placeholder - uses SP Function Code 0xF8
    MZAP_REG_FACTORY_RESET = 0x00FE, // Placeholder - uses SP Function Code 0xF6
} mightyzap_register_t;

/**
 * @brief Baud rate values for mightyZAP
 */
typedef enum {
    MZAP_BAUD_9600 = 128,  // 0x80
    MZAP_BAUD_19200 = 64,  // 0x40
    MZAP_BAUD_38400 = 48,  // 0x30
    MZAP_BAUD_57600 = 32,  // 0x20 (default)
    MZAP_BAUD_115200 = 16, // 0x10
} mightyzap_baud_t;

/**
 * @brief mightyZAP actuator handle
 */
typedef struct mightyzap *mightyzap_handle_t;

/**
 * @brief mightyZAP status
 */
typedef struct {
    uint16_t position; // Present position (0-4095 typical)
    uint16_t current;  // Present current (mA)
    uint16_t voltage;  // Present voltage (0.1V units)
    uint8_t moving;    // Moving status (0=stopped, 1=moving)
} mightyzap_status_t;

/**
 * @brief mightyZAP device info (read-only)
 */
typedef struct {
    uint16_t model;       // Model number
    uint16_t firmware;    // Firmware version
    uint16_t voltage_min; // Lowest limit voltage (0.1V units)
    uint16_t voltage_max; // Highest limit voltage (0.1V units)
} mightyzap_info_t;

/**
 * @brief mightyZAP configuration (EEPROM)
 */
typedef struct {
    uint8_t slave_id;  // Modbus slave ID (1-247)
    uint8_t baud_rate; // Baud rate code (16=115200, 32=57600, 48=38400, 64=19200,
                     // 128=9600)
    uint16_t short_stroke_limit; // Short stroke limit (0-4095)
    uint16_t long_stroke_limit;  // Long stroke limit (0-4095)
    uint16_t speed_limit;        // Speed limit (0-1023)
    uint16_t current_limit;      // Current limit (0-1600 mA)
    uint8_t start_compliance;    // Start compliance margin (0-255)
    uint8_t end_compliance;      // End compliance margin (0-255)
    uint8_t alarm_led;           // Alarm LED bitmask
    uint8_t alarm_shutdown;      // Alarm shutdown bitmask
} mightyzap_config_t;

/**
 * @brief Initialize mightyZAP driver
 *
 * @param modbus Modbus RTU handle
 * @param slave_id Slave ID (1-247)
 * @param handle Pointer to store handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_init(modbus_handle_t modbus, uint8_t slave_id,
                         mightyzap_handle_t *handle);

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
 * @brief Set goal position directly using raw modbus handle
 * Useful for broadcast messages where no specific mightyZAP handle exists
 *
 * @param modbus Modbus RTU handle
 * @param slave_id Slave ID (use 0 for broadcast)
 * @param position Goal position (0-4095 typical)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_position_raw(modbus_handle_t modbus, uint8_t slave_id,
                                     uint16_t position);

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
esp_err_t mightyzap_set_goal(mightyzap_handle_t handle, uint16_t position,
                             uint16_t speed, uint16_t current);

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
esp_err_t mightyzap_get_status(mightyzap_handle_t handle,
                               mightyzap_status_t *status);

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

/**
 * @brief Get device info (read-only parameters)
 *
 * @param handle mightyZAP handle
 * @param info Pointer to store device info
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_get_info(mightyzap_handle_t handle, mightyzap_info_t *info);

/**
 * @brief Get full configuration (EEPROM parameters)
 *
 * @param handle mightyZAP handle
 * @param config Pointer to store configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_get_config(mightyzap_handle_t handle,
                               mightyzap_config_t *config);

/**
 * @brief Set configuration (writes to EEPROM)
 *
 * @param handle mightyZAP handle
 * @param config Configuration to write (only non-zero fields are written)
 * @param mask Bitmask of fields to update (use MZAP_CONFIG_* constants)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_set_config(mightyzap_handle_t handle,
                               const mightyzap_config_t *config, uint16_t mask);

// Configuration field masks for mightyzap_set_config()
#define MZAP_CONFIG_SLAVE_ID (1 << 0)
#define MZAP_CONFIG_BAUD_RATE (1 << 1)
#define MZAP_CONFIG_SHORT_STROKE (1 << 2)
#define MZAP_CONFIG_LONG_STROKE (1 << 3)
#define MZAP_CONFIG_SPEED_LIMIT (1 << 4)
#define MZAP_CONFIG_CURRENT_LIMIT (1 << 5)
#define MZAP_CONFIG_START_COMPLIANCE (1 << 6)
#define MZAP_CONFIG_END_COMPLIANCE (1 << 7)
#define MZAP_CONFIG_ALARM_LED (1 << 8)
#define MZAP_CONFIG_ALARM_SHUTDOWN (1 << 9)
#define MZAP_CONFIG_ALL (0x03FF)

// mightyZAP parameter limits
#define MZAP_MAX_POSITION 4095
#define MZAP_MAX_SPEED 1023
#define MZAP_MAX_CURRENT 1600
#define MODBUS_MAX_SLAVE_ADDR 247

// ============================================================================
// Synchronized Movement (Multi-Actuator)
// ============================================================================

#define MZAP_SYNC_MAX_ACTUATORS 4 // Max actuators in a sync group
#define MZAP_SYNC_DEFAULT_TOLERANCE                                            \
    20 // Default position tolerance (~0.13mm for 27mm stroke)

/**
 * @brief Sync group result codes
 */
typedef enum {
    MZAP_SYNC_OK = 0,        // Movement completed successfully
    MZAP_SYNC_TIMEOUT,       // Timeout waiting for position
    MZAP_SYNC_DESYNC,        // Actuators desynchronized beyond limit
    MZAP_SYNC_COMM_ERROR,    // Communication error with one or more actuators
    MZAP_SYNC_INVALID_PARAM, // Invalid parameters
} mightyzap_sync_result_t;

/**
 * @brief Synchronized movement group
 */
typedef struct {
    modbus_handle_t modbus;               // Modbus handle
    uint8_t ids[MZAP_SYNC_MAX_ACTUATORS]; // Actuator IDs in the group
    uint8_t count;                        // Number of actuators in group
    uint16_t target_position;             // Target position for all
    uint16_t speed;                       // Movement speed
    uint16_t current;                     // Current/force limit
    uint16_t tolerance;                   // Position tolerance in steps
    uint16_t max_desync;                  // Max allowed desync between actuators
    uint16_t present_positions[MZAP_SYNC_MAX_ACTUATORS]; // Last read positions
} mightyzap_sync_group_t;

/**
 * @brief Initialize a sync group
 *
 * @param group Pointer to sync group structure
 * @param modbus Modbus handle
 * @param ids Array of actuator IDs
 * @param count Number of actuators (1-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_init(mightyzap_sync_group_t *group,
                                                            modbus_handle_t modbus, const uint8_t *ids,
                                                            uint8_t count);

/**
 * @brief Set movement parameters for sync group
 *
 * @param group Sync group
 * @param speed Movement speed (0-1023)
 * @param current Current limit (0-800)
 * @param tolerance Position tolerance in steps (default 20)
 * @param max_desync Max desync allowed between actuators (default 50)
 */
void mightyzap_sync_set_params(mightyzap_sync_group_t *group, uint16_t speed,
                               uint16_t current, uint16_t tolerance,
                               uint16_t max_desync);

/**
 * @brief Start synchronized movement to position
 *
 * Sends goal position to all actuators in sequence with minimal delay.
 *
 * @param group Sync group
 * @param position Target position
 * @return esp_err_t ESP_OK if commands sent successfully
 */
esp_err_t mightyzap_sync_move_start(mightyzap_sync_group_t *group,
                                                                        uint16_t position);

/**
 * @brief Start synchronized movement via broadcast (slave_addr=0)
 *
 * Sends speed, current, and position as broadcast frames (no response expected).
 * All actuators on the bus receive the same command simultaneously, eliminating
 * timing skew between actuators. Position is sent last to trigger movement.
 *
 * @param group Sync group (uses group->modbus, group->speed, group->current)
 * @param position Target position
 * @return esp_err_t ESP_OK if all broadcast frames sent successfully
 */
esp_err_t mightyzap_sync_move_broadcast(mightyzap_sync_group_t *group,
                                        uint16_t position);

/**
 * @brief Check if all actuators reached target position
 *
 * @param group Sync group
 * @param in_position Output: true if all actuators are within tolerance
 * @param all_stopped Output: true if all actuators stopped moving
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_check_position(mightyzap_sync_group_t *group,
                                                                                bool *in_position, bool *all_stopped);

/**
 * @brief Get maximum desync (difference between actuator positions)
 *
 * @param group Sync group
 * @param max_diff Output: maximum position difference between any two actuators
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_get_desync(mightyzap_sync_group_t *group,
                                                                        uint16_t *max_diff);

/**
 * @brief Execute synchronized move and wait for completion
 *
 * This is a blocking call that:
 * 1. Sends position commands to all actuators
 * 2. Polls positions until all reach target or timeout
 * 3. Checks for desynchronization
 *
 * @param group Sync group
 * @param position Target position
 * @param timeout_ms Timeout in milliseconds
 * @return mightyzap_sync_result_t Result code
 */
mightyzap_sync_result_t mightyzap_sync_move_wait(mightyzap_sync_group_t *group,
                                                 uint16_t position,
                                                 uint32_t timeout_ms);

/**
 * @brief Emergency stop all actuators in group
 *
 * Disables force on all actuators.
 *
 * @param group Sync group
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_stop(mightyzap_sync_group_t *group);

/**
 * @brief Enable force on all actuators in group
 *
 * @param group Sync group
 * @param enable true to enable, false to disable
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_set_force(mightyzap_sync_group_t *group, bool enable);

// ============================================================================
// Closed-Loop Sync Controller
// ============================================================================

/**
 * @brief Sync controller state machine states
 */
typedef enum {
    SYNC_CTRL_IDLE = 0,   // Waiting for command
    SYNC_CTRL_MOVING,     // Closed-loop control active
    SYNC_CTRL_DONE,       // Movement completed successfully
    SYNC_CTRL_ERROR,      // Movement failed (timeout/desync/comm)
} mightyzap_sync_ctrl_state_t;

/**
 * @brief Sync controller configuration (tunable parameters)
 */
typedef struct {
    uint16_t sync_interval_ms;       // Control loop period (default 40ms)
    uint16_t max_desync;             // Desync threshold for correction (default 50)
    float    correction_speed_factor; // Speed reduction factor for leader (default 0.5)
    uint16_t position_tolerance;     // "arrived" tolerance (default 30)
    uint32_t timeout_ms;             // Overall timeout (default 10000)
} mightyzap_sync_ctrl_config_t;

/**
 * @brief Sync controller runtime status (read by API)
 */
typedef struct {
    mightyzap_sync_ctrl_state_t state;
    mightyzap_sync_result_t     result;
    uint16_t target_position;
    uint16_t original_speed;
    uint16_t positions[MZAP_SYNC_MAX_ACTUATORS];
    uint16_t desync;
    uint8_t  count;
    uint8_t  ids[MZAP_SYNC_MAX_ACTUATORS];
    uint32_t elapsed_ms;
    uint16_t corrections_applied; // Number of speed corrections made
} mightyzap_sync_ctrl_status_t;

/**
 * @brief Sync controller command (sent via queue)
 */
typedef struct {
    mightyzap_sync_group_t      group;   // Copy of group config
    uint16_t                    position; // Target position
    mightyzap_sync_ctrl_config_t config;  // Controller params
} mightyzap_sync_ctrl_cmd_t;

/**
 * @brief Default sync controller configuration
 */
#define MZAP_SYNC_CTRL_CONFIG_DEFAULT() { \
    .sync_interval_ms = 40, \
    .max_desync = 50, \
    .correction_speed_factor = 0.5f, \
    .position_tolerance = 30, \
    .timeout_ms = 10000, \
}

/**
 * @brief Initialize the sync controller task
 *
 * Creates a FreeRTOS task and command queue for closed-loop sync control.
 * Must be called once at startup.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_ctrl_init(void);

/**
 * @brief Start a closed-loop synchronized movement
 *
 * Sends a command to the sync controller task. Non-blocking.
 *
 * @param group Sync group (copied internally)
 * @param position Target position
 * @param config Controller configuration (NULL for defaults)
 * @return esp_err_t ESP_OK if command enqueued
 */
esp_err_t mightyzap_sync_ctrl_move(mightyzap_sync_group_t *group,
                                   uint16_t position,
                                   const mightyzap_sync_ctrl_config_t *config);

/**
 * @brief Get current sync controller status (lock-free read)
 *
 * @param status Output status structure
 */
void mightyzap_sync_ctrl_get_status(mightyzap_sync_ctrl_status_t *status);

/**
 * @brief Check if sync controller is busy (movement in progress)
 *
 * @return true if a movement is in progress
 */
bool mightyzap_sync_ctrl_is_busy(void);

/**
 * @brief Abort current sync controller movement
 *
 * Stops actuators and returns to idle state.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mightyzap_sync_ctrl_abort(void);

#ifdef __cplusplus
}
#endif

#endif // MIGHTYZAP_H
