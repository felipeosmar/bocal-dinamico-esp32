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
} modbus_exception_t;

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

#ifdef __cplusplus
}
#endif

#endif // MODBUS_RTU_H
