#include "rs485_driver.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "RS485";

// Enable verbose hex dump of TX/RX data (set to 1 for debugging)
#ifndef RS485_DEBUG_HEX_DUMP
#define RS485_DEBUG_HEX_DUMP 1
#endif

#if RS485_DEBUG_HEX_DUMP
static void hex_dump(const char *prefix, const uint8_t *data, size_t len)
{
    if (len == 0) return;

    char buf[128];
    size_t pos = 0;

    for (size_t i = 0; i < len && pos < sizeof(buf) - 4; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
    }

    ESP_LOGI(TAG, "%s [%u bytes]: %s", prefix, (unsigned)len, buf);
}
#else
#define hex_dump(prefix, data, len) do {} while(0)
#endif

/**
 * @brief Internal RS485 driver structure
 */
struct rs485_driver {
    uart_port_t uart_num;
    gpio_num_t de_pin;
    SemaphoreHandle_t mutex;
    int baud_rate;
};

esp_err_t rs485_init(const rs485_config_t *config, rs485_handle_t *handle)
{
    if (config == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate driver structure
    struct rs485_driver *drv = calloc(1, sizeof(struct rs485_driver));
    if (drv == NULL) {
        ESP_LOGE(TAG, "Failed to allocate driver structure");
        return ESP_ERR_NO_MEM;
    }

    drv->uart_num = config->uart_num;
    drv->de_pin = config->de_pin;
    drv->baud_rate = config->baud_rate;

    // Create mutex for thread safety
    drv->mutex = xSemaphoreCreateMutex();
    if (drv->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(drv);
        return ESP_ERR_NO_MEM;
    }

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(config->uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(ret));
        vSemaphoreDelete(drv->mutex);
        free(drv);
        return ret;
    }

    // Set UART pins
    ret = uart_set_pin(config->uart_num, config->tx_pin, config->rx_pin,
                       config->de_pin, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        vSemaphoreDelete(drv->mutex);
        free(drv);
        return ret;
    }

    // Install UART driver
    ret = uart_driver_install(config->uart_num, config->rx_buffer_size,
                              config->tx_buffer_size, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        vSemaphoreDelete(drv->mutex);
        free(drv);
        return ret;
    }

    // Set RS485 half-duplex mode
    ret = uart_set_mode(config->uart_num, UART_MODE_RS485_HALF_DUPLEX);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set RS485 mode: %s", esp_err_to_name(ret));
        uart_driver_delete(config->uart_num);
        vSemaphoreDelete(drv->mutex);
        free(drv);
        return ret;
    }

    ESP_LOGI(TAG, "RS485 initialized: UART%d, TX=%d, RX=%d, DE=%d, Baud=%d",
             config->uart_num, config->tx_pin, config->rx_pin,
             config->de_pin, config->baud_rate);

    *handle = drv;
    return ESP_OK;
}

esp_err_t rs485_deinit(rs485_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct rs485_driver *drv = handle;

    uart_driver_delete(drv->uart_num);
    vSemaphoreDelete(drv->mutex);
    free(drv);

    return ESP_OK;
}

esp_err_t rs485_send(rs485_handle_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct rs485_driver *drv = handle;

    hex_dump("TX", data, len);

    int written = uart_write_bytes(drv->uart_num, data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "UART write failed");
        return ESP_FAIL;
    }

    // Wait for transmission to complete
    esp_err_t ret = uart_wait_tx_done(drv->uart_num, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART TX timeout");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t rs485_receive(rs485_handle_t handle, uint8_t *data, size_t max_len, size_t *received, uint32_t timeout_ms)
{
    if (handle == NULL || data == NULL || max_len == 0 || received == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct rs485_driver *drv = handle;
    *received = 0;

    int len = uart_read_bytes(drv->uart_num, data, max_len, pdMS_TO_TICKS(timeout_ms));
    if (len < 0) {
        ESP_LOGE(TAG, "UART read failed");
        return ESP_FAIL;
    }

    if (len == 0) {
        ESP_LOGW(TAG, "RX timeout - no response from slave");
        return ESP_ERR_TIMEOUT;
    }

    hex_dump("RX", data, len);

    *received = len;
    return ESP_OK;
}

esp_err_t rs485_flush_rx(rs485_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct rs485_driver *drv = handle;
    return uart_flush_input(drv->uart_num);
}

esp_err_t rs485_transaction(rs485_handle_t handle,
                           const uint8_t *tx_data, size_t tx_len,
                           uint8_t *rx_data, size_t rx_max_len, size_t *rx_received,
                           uint32_t timeout_ms)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct rs485_driver *drv = handle;
    esp_err_t ret;

    // Take mutex for thread safety
    if (xSemaphoreTake(drv->mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    // Flush RX buffer before transaction
    uart_flush_input(drv->uart_num);

    // Send request
    ret = rs485_send(handle, tx_data, tx_len, timeout_ms);
    if (ret != ESP_OK) {
        xSemaphoreGive(drv->mutex);
        return ret;
    }

    // Small delay for slave to process (Modbus requires 3.5 char times minimum)
    // At 57600 baud, 3.5 chars ~= 0.6ms, we use 2ms for safety
    vTaskDelay(pdMS_TO_TICKS(2));

    // Receive response
    if (rx_data != NULL && rx_max_len > 0 && rx_received != NULL) {
        ret = rs485_receive(handle, rx_data, rx_max_len, rx_received, timeout_ms);
    }

    xSemaphoreGive(drv->mutex);
    return ret;
}
