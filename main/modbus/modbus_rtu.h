#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rs485_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Modbus function codes
 */
typedef enum {
    MODBUS_FC_READ_HOLDING_REGISTERS = 0x03,    // Read Holding Registers
    MODBUS_FC_READ_INPUT_REGISTERS = 0x04,      // Read Input Registers
    MODBUS_FC_WRITE_SINGLE_REGISTER = 0x06,     // Write Single Register
    MODBUS_FC_WRITE_MULTIPLE_REGISTERS = 0x10,  // Write Multiple Registers
} modbus_function_code_t;

/**
 * @brief Modbus exception codes
 */
typedef enum {
    MODBUS_EX_NONE = 0x00,
    MODBUS_EX_ILLEGAL_FUNCTION = 0x01,
    MODBUS_EX_ILLEGAL_DATA_ADDRESS = 0x02,
    MODBUS_EX_ILLEGAL_DATA_VALUE = 0x03,
    MODBUS_EX_SLAVE_DEVICE_FAILURE = 0x04,
    MODBUS_EX_ACKNOWLEDGE = 0x05,
    MODBUS_EX_SLAVE_DEVICE_BUSY = 0x06,
    MODBUS_EX_MEMORY_PARITY_ERROR = 0x08,
    MODBUS_EX_GATEWAY_PATH_UNAVAILABLE = 0x0A,
    MODBUS_EX_GATEWAY_TARGET_FAILED = 0x0B,

    // mightyZAP proprietary exception codes (0x20-0x2F range)
    MODBUS_EX_MZAP_MOTOR_MOVING = 0x21,      // Motor is currently moving
    MODBUS_EX_MZAP_OVERLOAD = 0x22,          // Overload condition
    MODBUS_EX_MZAP_CHECKSUM_ERROR = 0x23,    // Internal checksum error
    MODBUS_EX_MZAP_RANGE_ERROR = 0x24,       // Value out of range
    MODBUS_EX_MZAP_INSTRUCTION_ERROR = 0x25, // Invalid instruction
} modbus_exception_t;

/**
 * @brief Modbus error classification for retry logic
 */
typedef enum {
    MODBUS_ERR_NONE = 0,           // No error
    MODBUS_ERR_TIMEOUT,            // Recoverable - no response
    MODBUS_ERR_CRC,                // Recoverable - noise/corruption
    MODBUS_ERR_SHORT_RESPONSE,     // Recoverable - incomplete frame
    MODBUS_ERR_EXCEPTION,          // Depends on exception code
    MODBUS_ERR_INVALID_RESPONSE,   // Non-recoverable - malformed response
} modbus_error_type_t;

/**
 * @brief Modbus RTU handle
 */
typedef struct modbus_rtu* modbus_handle_t;

/**
 * @brief Modbus RTU configuration
 */
typedef struct {
    rs485_handle_t rs485;       // RS485 handle
    uint32_t response_timeout;  // Response timeout in ms (default 100)
} modbus_config_t;

#define MODBUS_DEFAULT_CONFIG() {   \
    .rs485 = NULL,                  \
    .response_timeout = 100,        \
}

/**
 * @brief Initialize Modbus RTU master
 *
 * @param config Configuration
 * @param handle Pointer to store handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modbus_init(const modbus_config_t *config, modbus_handle_t *handle);

/**
 * @brief Deinitialize Modbus RTU master
 *
 * @param handle Modbus handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modbus_deinit(modbus_handle_t handle);

/**
 * @brief Read holding registers (FC 0x03)
 *
 * @param handle Modbus handle
 * @param slave_addr Slave address (1-247)
 * @param start_reg Starting register address
 * @param num_regs Number of registers to read (1-125)
 * @param values Buffer to store read values
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modbus_read_holding_registers(modbus_handle_t handle,
                                        uint8_t slave_addr,
                                        uint16_t start_reg,
                                        uint16_t num_regs,
                                        uint16_t *values);

/**
 * @brief Read input registers (FC 0x04)
 *
 * @param handle Modbus handle
 * @param slave_addr Slave address (1-247)
 * @param start_reg Starting register address
 * @param num_regs Number of registers to read (1-125)
 * @param values Buffer to store read values
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modbus_read_input_registers(modbus_handle_t handle,
                                      uint8_t slave_addr,
                                      uint16_t start_reg,
                                      uint16_t num_regs,
                                      uint16_t *values);

/**
 * @brief Write single register (FC 0x06)
 *
 * @param handle Modbus handle
 * @param slave_addr Slave address (1-247)
 * @param reg_addr Register address
 * @param value Value to write
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modbus_write_single_register(modbus_handle_t handle,
                                       uint8_t slave_addr,
                                       uint16_t reg_addr,
                                       uint16_t value);

/**
 * @brief Write multiple registers (FC 0x10)
 *
 * @param handle Modbus handle
 * @param slave_addr Slave address (1-247)
 * @param start_reg Starting register address
 * @param num_regs Number of registers to write
 * @param values Values to write
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modbus_write_multiple_registers(modbus_handle_t handle,
                                          uint8_t slave_addr,
                                          uint16_t start_reg,
                                          uint16_t num_regs,
                                          const uint16_t *values);

/**
 * @brief Calculate Modbus CRC16
 *
 * @param data Data buffer
 * @param len Length of data
 * @return uint16_t CRC16 value
 */
uint16_t modbus_crc16(const uint8_t *data, size_t len);

/**
 * @brief Get last Modbus exception
 *
 * @param handle Modbus handle
 * @return modbus_exception_t Last exception code
 */
modbus_exception_t modbus_get_last_exception(modbus_handle_t handle);

/**
 * @brief Modbus communication statistics
 */
typedef struct {
    uint32_t tx_count;          // Total requests sent
    uint32_t rx_count;          // Total successful responses
    uint32_t error_count;       // Total errors
    uint32_t timeout_count;     // Timeout errors
    uint32_t crc_error_count;   // CRC errors
    uint32_t short_response_count; // Short response errors
    uint32_t retry_count;       // Total retries performed
    uint32_t exception_counts[16];      // Standard Modbus exceptions (0x00-0x0F)
    uint32_t mzap_exception_counts[16]; // mightyZAP exceptions (0x20-0x2F)
} modbus_stats_t;

/**
 * @brief Get Modbus communication statistics (direct pointer — not thread-safe)
 *
 * @return const modbus_stats_t* Pointer to stats structure
 */
const modbus_stats_t* modbus_get_stats(void);

/**
 * @brief Get an atomic snapshot of Modbus statistics
 *
 * Copies the stats inside a critical section so all fields are consistent.
 * Use this from contexts where stats may be updated concurrently (e.g. API handlers).
 *
 * @param[out] snapshot Destination for the snapshot copy
 */
void modbus_get_stats_snapshot(modbus_stats_t *snapshot);

/**
 * @brief Reset Modbus statistics
 */
void modbus_reset_stats(void);

/**
 * @brief Convert exception code to human-readable string
 *
 * @param ex Exception code
 * @return const char* Exception name string
 */
const char* modbus_exception_to_string(modbus_exception_t ex);

/**
 * @brief Check if an exception is retryable
 *
 * @param ex Exception code
 * @return bool True if the operation should be retried
 */
bool modbus_exception_is_retryable(modbus_exception_t ex);

/**
 * @brief Get last error type from a Modbus operation
 *
 * @param handle Modbus handle
 * @return modbus_error_type_t Last error classification
 */
modbus_error_type_t modbus_get_last_error_type(modbus_handle_t handle);

/**
 * @brief Set retry count override (0 = no retries, -1 = restore default)
 */
void modbus_set_retry_count(int8_t count);

#ifdef __cplusplus
}
#endif

#endif // MODBUS_RTU_H
