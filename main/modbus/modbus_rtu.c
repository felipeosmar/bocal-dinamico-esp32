#include "modbus_rtu.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MODBUS";

#define MODBUS_MAX_PDU_SIZE 256
#define MODBUS_RETRY_COUNT 3
#define MODBUS_RETRY_BASE_DELAY_MS 100

/**
 * @brief Internal Modbus RTU structure
 */
struct modbus_rtu {
    rs485_handle_t rs485;
    uint32_t response_timeout;
    modbus_exception_t last_exception;
};

// Global statistics for diagnostics
static modbus_stats_t s_modbus_stats = {0};

// CRC16 lookup table for Modbus
static const uint16_t crc_table[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        uint8_t index = (crc ^ data[i]) & 0xFF;
        crc = (crc >> 8) ^ crc_table[index];
    }

    return crc;
}

esp_err_t modbus_init(const modbus_config_t *config, modbus_handle_t *handle)
{
    if (config == NULL || handle == NULL || config->rs485 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct modbus_rtu *mb = calloc(1, sizeof(struct modbus_rtu));
    if (mb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate Modbus structure");
        return ESP_ERR_NO_MEM;
    }

    mb->rs485 = config->rs485;
    mb->response_timeout = config->response_timeout > 0 ? config->response_timeout : 100;
    mb->last_exception = MODBUS_EX_NONE;

    ESP_LOGI(TAG, "Modbus RTU master initialized, timeout=%lu ms", mb->response_timeout);

    *handle = mb;
    return ESP_OK;
}

esp_err_t modbus_deinit(modbus_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    free(handle);
    return ESP_OK;
}

modbus_exception_t modbus_get_last_exception(modbus_handle_t handle)
{
    if (handle == NULL) {
        return MODBUS_EX_NONE;
    }
    return handle->last_exception;
}

const modbus_stats_t* modbus_get_stats(void)
{
    return &s_modbus_stats;
}

void modbus_reset_stats(void)
{
    memset(&s_modbus_stats, 0, sizeof(s_modbus_stats));
}

static esp_err_t modbus_send_receive(modbus_handle_t handle,
                                     const uint8_t *request, size_t req_len,
                                     uint8_t *response, size_t *resp_len,
                                     size_t expected_len)
{
    esp_err_t ret;
    size_t received = 0;

    s_modbus_stats.tx_count++;

    // Send request and receive response
    ret = rs485_transaction(handle->rs485,
                           request, req_len,
                           response, MODBUS_MAX_PDU_SIZE, &received,
                           handle->response_timeout);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RS485 transaction failed: %s", esp_err_to_name(ret));
        s_modbus_stats.error_count++;
        if (ret == ESP_ERR_TIMEOUT) {
            s_modbus_stats.timeout_count++;
        }
        return ret;
    }

    // Check minimum response length (addr + fc + crc)
    if (received < 4) {
        ESP_LOGE(TAG, "Response too short: %u bytes", received);
        s_modbus_stats.error_count++;
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Verify CRC
    uint16_t recv_crc = (response[received - 1] << 8) | response[received - 2];
    uint16_t calc_crc = modbus_crc16(response, received - 2);

    if (recv_crc != calc_crc) {
        ESP_LOGE(TAG, "CRC mismatch: recv=0x%04X, calc=0x%04X", recv_crc, calc_crc);
        s_modbus_stats.error_count++;
        s_modbus_stats.crc_error_count++;
        return ESP_ERR_INVALID_CRC;
    }

    // Check for exception response
    if (response[1] & 0x80) {
        handle->last_exception = response[2];
        ESP_LOGE(TAG, "Modbus exception: 0x%02X", response[2]);
        s_modbus_stats.error_count++;
        return ESP_ERR_INVALID_RESPONSE;
    }

    handle->last_exception = MODBUS_EX_NONE;
    s_modbus_stats.rx_count++;
    *resp_len = received;
    return ESP_OK;
}

esp_err_t modbus_read_holding_registers(modbus_handle_t handle,
                                        uint8_t slave_addr,
                                        uint16_t start_reg,
                                        uint16_t num_regs,
                                        uint16_t *values)
{
    if (handle == NULL || values == NULL || num_regs == 0 || num_regs > 125) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[8];
    uint8_t response[MODBUS_MAX_PDU_SIZE];
    size_t resp_len;

    // Build request: [Addr][FC][StartHi][StartLo][NumHi][NumLo][CRCLo][CRCHi]
    request[0] = slave_addr;
    request[1] = MODBUS_FC_READ_HOLDING_REGISTERS;
    request[2] = (start_reg >> 8) & 0xFF;
    request[3] = start_reg & 0xFF;
    request[4] = (num_regs >> 8) & 0xFF;
    request[5] = num_regs & 0xFF;

    uint16_t crc = modbus_crc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    ESP_LOGD(TAG, "Read regs: addr=%u, start=0x%04X, count=%u",
             slave_addr, start_reg, num_regs);

    esp_err_t ret = modbus_send_receive(handle, request, 8, response, &resp_len,
                                        5 + num_regs * 2);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: [Addr][FC][ByteCount][Data...][CRC]
    uint8_t byte_count = response[2];
    if (byte_count != num_regs * 2) {
        ESP_LOGE(TAG, "Unexpected byte count: %u (expected %u)", byte_count, num_regs * 2);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extract register values (big-endian in Modbus)
    for (uint16_t i = 0; i < num_regs; i++) {
        values[i] = (response[3 + i * 2] << 8) | response[4 + i * 2];
    }

    return ESP_OK;
}

esp_err_t modbus_write_single_register(modbus_handle_t handle,
                                       uint8_t slave_addr,
                                       uint16_t reg_addr,
                                       uint16_t value)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[8];
    uint8_t response[MODBUS_MAX_PDU_SIZE];
    size_t resp_len;

    // Build request: [Addr][FC][RegHi][RegLo][ValHi][ValLo][CRCLo][CRCHi]
    request[0] = slave_addr;
    request[1] = MODBUS_FC_WRITE_SINGLE_REGISTER;
    request[2] = (reg_addr >> 8) & 0xFF;
    request[3] = reg_addr & 0xFF;
    request[4] = (value >> 8) & 0xFF;
    request[5] = value & 0xFF;

    uint16_t crc = modbus_crc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    ESP_LOGD(TAG, "Write reg: addr=%u, reg=0x%04X, value=0x%04X",
             slave_addr, reg_addr, value);

    esp_err_t ret = modbus_send_receive(handle, request, 8, response, &resp_len, 8);
    if (ret != ESP_OK) {
        return ret;
    }

    // Response should echo request (without CRC comparison already done)
    if (response[0] != slave_addr || response[1] != MODBUS_FC_WRITE_SINGLE_REGISTER) {
        ESP_LOGE(TAG, "Unexpected response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t modbus_write_multiple_registers(modbus_handle_t handle,
                                          uint8_t slave_addr,
                                          uint16_t start_reg,
                                          uint16_t num_regs,
                                          const uint16_t *values)
{
    if (handle == NULL || values == NULL || num_regs == 0 || num_regs > 123) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[MODBUS_MAX_PDU_SIZE];
    uint8_t response[MODBUS_MAX_PDU_SIZE];
    size_t resp_len;

    // Build request: [Addr][FC][StartHi][StartLo][NumHi][NumLo][ByteCount][Data...][CRC]
    request[0] = slave_addr;
    request[1] = MODBUS_FC_WRITE_MULTIPLE_REGISTERS;
    request[2] = (start_reg >> 8) & 0xFF;
    request[3] = start_reg & 0xFF;
    request[4] = (num_regs >> 8) & 0xFF;
    request[5] = num_regs & 0xFF;
    request[6] = num_regs * 2;  // Byte count

    // Add register values (big-endian)
    for (uint16_t i = 0; i < num_regs; i++) {
        request[7 + i * 2] = (values[i] >> 8) & 0xFF;
        request[8 + i * 2] = values[i] & 0xFF;
    }

    size_t req_len = 7 + num_regs * 2;
    uint16_t crc = modbus_crc16(request, req_len);
    request[req_len] = crc & 0xFF;
    request[req_len + 1] = (crc >> 8) & 0xFF;

    ESP_LOGD(TAG, "Write multi regs: addr=%u, start=0x%04X, count=%u",
             slave_addr, start_reg, num_regs);

    esp_err_t ret = modbus_send_receive(handle, request, req_len + 2, response, &resp_len, 8);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}
