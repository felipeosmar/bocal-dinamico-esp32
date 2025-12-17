#ifndef RS485_DRIVER_H
#define RS485_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RS485 configuration structure
 */
typedef struct {
    uart_port_t uart_num;       // UART port number (UART_NUM_1 or UART_NUM_2)
    int tx_pin;                 // TX pin (to MAX485 DI)
    int rx_pin;                 // RX pin (from MAX485 RO)
    int de_pin;                 // Direction Enable pin (to MAX485 DE/RE)
    int baud_rate;              // Baud rate (default 57600)
    int rx_buffer_size;         // RX buffer size (default 256)
    int tx_buffer_size;         // TX buffer size (default 256)
} rs485_config_t;

/**
 * @brief RS485 handle
 */
typedef struct rs485_driver* rs485_handle_t;

/**
 * @brief Default RS485 configuration
 */
#define RS485_DEFAULT_CONFIG() {            \
    .uart_num = UART_NUM_1,                 \
    .tx_pin = GPIO_NUM_17,                  \
    .rx_pin = GPIO_NUM_16,                  \
    .de_pin = GPIO_NUM_4,                   \
    .baud_rate = 57600,                     \
    .rx_buffer_size = 256,                  \
    .tx_buffer_size = 256,                  \
}

/**
 * @brief Initialize RS485 driver
 *
 * @param config Pointer to configuration structure
 * @param handle Pointer to store the handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rs485_init(const rs485_config_t *config, rs485_handle_t *handle);

/**
 * @brief Deinitialize RS485 driver
 *
 * @param handle RS485 handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rs485_deinit(rs485_handle_t handle);

/**
 * @brief Send data over RS485
 *
 * @param handle RS485 handle
 * @param data Pointer to data buffer
 * @param len Length of data to send
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rs485_send(rs485_handle_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief Receive data from RS485
 *
 * @param handle RS485 handle
 * @param data Pointer to buffer to store received data
 * @param max_len Maximum length to receive
 * @param received Pointer to store actual received length
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT if timeout
 */
esp_err_t rs485_receive(rs485_handle_t handle, uint8_t *data, size_t max_len, size_t *received, uint32_t timeout_ms);

/**
 * @brief Flush RX buffer
 *
 * @param handle RS485 handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rs485_flush_rx(rs485_handle_t handle);

/**
 * @brief Send data and wait for response (half-duplex transaction)
 *
 * @param handle RS485 handle
 * @param tx_data Data to send
 * @param tx_len Length of data to send
 * @param rx_data Buffer to store response
 * @param rx_max_len Maximum response length
 * @param rx_received Pointer to store actual received length
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rs485_transaction(rs485_handle_t handle,
                           const uint8_t *tx_data, size_t tx_len,
                           uint8_t *rx_data, size_t rx_max_len, size_t *rx_received,
                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // RS485_DRIVER_H
