#include "baumer_ox100.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "BAUMER";

// Baumer OX100 Input Register addresses (FC04)
// The sensor requires reading the COMPLETE block — partial reads return
// Illegal Data Address (0x02).  Documentation §7.3 example: 24 registers.
#define BAUMER_REG_ALL_MEASUREMENTS 200 // 24 registers for all 32-bit values
#define BAUMER_READ_NUM_REGS 24         // Must read full block (doc §7.3)
#define BAUMER_REG_STATUS_OFFSET 0
#define BAUMER_REG_QUALITY_OFFSET 1
#define BAUMER_REG_OUTPUT_OFFSET 2
#define BAUMER_REG_VALUE1_OFFSET 3 // Float32: regs 3-4
#define BAUMER_REG_VALUE2_OFFSET 5 // Float32: regs 5-6
#define BAUMER_REG_VALUE3_OFFSET 7 // Float32: regs 7-8
#define BAUMER_REG_VALUE4_OFFSET 9 // Float32: regs 9-10

// Baumer OX100 Holding Register addresses (FC06)
#define BAUMER_HREG_LASER 410 // 0=OFF, 1=ON

struct baumer_ctx {
  modbus_handle_t modbus;
  uint8_t slave_id;
  baumer_measurement_t cached;
  bool has_data;
  SemaphoreHandle_t mutex;
};

/**
 * @brief Convert two Modbus registers to Float32 (Baumer Little Endian)
 *
 * Baumer stores Float32 with less significant word at lower address:
 *   Register N   = low 16 bits
 *   Register N+1 = high 16 bits
 */
static float regs_to_float_le(uint16_t reg_low, uint16_t reg_high) {
  uint32_t raw = ((uint32_t)reg_high << 16) | (uint32_t)reg_low;
  float value;
  memcpy(&value, &raw, sizeof(float));
  return value;
}

esp_err_t baumer_init(modbus_handle_t modbus, uint8_t slave_id,
                      baumer_handle_t *handle) {
  if (modbus == NULL || handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  struct baumer_ctx *ctx = calloc(1, sizeof(struct baumer_ctx));
  if (ctx == NULL) {
    ESP_LOGE(TAG, "Failed to allocate Baumer context");
    return ESP_ERR_NO_MEM;
  }

  ctx->modbus = modbus;
  ctx->slave_id = slave_id;
  ctx->has_data = false;
  ctx->mutex = xSemaphoreCreateMutex();
  if (ctx->mutex == NULL) {
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Baumer OX100 initialized, slave_id=%u", slave_id);
  *handle = ctx;
  return ESP_OK;
}

esp_err_t baumer_read_measurements(baumer_handle_t handle,
                                   baumer_measurement_t *out) {
  if (handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  uint16_t regs[BAUMER_READ_NUM_REGS];

  esp_err_t ret = modbus_read_input_registers(handle->modbus, handle->slave_id,
                                              BAUMER_REG_ALL_MEASUREMENTS,
                                              BAUMER_READ_NUM_REGS, regs);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read measurements: %s", esp_err_to_name(ret));
    return ret;
  }

  baumer_measurement_t m;
  m.status = regs[BAUMER_REG_STATUS_OFFSET];
  m.quality = regs[BAUMER_REG_QUALITY_OFFSET] & 0xFF;
  m.output = regs[BAUMER_REG_OUTPUT_OFFSET] & 0xFF;

  m.values[0] = regs_to_float_le(regs[BAUMER_REG_VALUE1_OFFSET],
                                 regs[BAUMER_REG_VALUE1_OFFSET + 1]);
  m.values[1] = regs_to_float_le(regs[BAUMER_REG_VALUE2_OFFSET],
                                 regs[BAUMER_REG_VALUE2_OFFSET + 1]);
  m.values[2] = regs_to_float_le(regs[BAUMER_REG_VALUE3_OFFSET],
                                 regs[BAUMER_REG_VALUE3_OFFSET + 1]);
  m.values[3] = regs_to_float_le(regs[BAUMER_REG_VALUE4_OFFSET],
                                 regs[BAUMER_REG_VALUE4_OFFSET + 1]);

  ESP_LOGD(
      TAG,
      "Measurement: status=0x%04X quality=%u values=[%.3f, %.3f, %.3f, %.3f]",
      m.status, m.quality, m.values[0], m.values[1], m.values[2], m.values[3]);

  // Update cache
  xSemaphoreTake(handle->mutex, portMAX_DELAY);
  handle->cached = m;
  handle->has_data = true;
  xSemaphoreGive(handle->mutex);

  if (out != NULL) {
    *out = m;
  }

  return ESP_OK;
}

esp_err_t baumer_get_cached(baumer_handle_t handle, baumer_measurement_t *out) {
  if (handle == NULL || out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(handle->mutex, portMAX_DELAY);
  if (!handle->has_data) {
    xSemaphoreGive(handle->mutex);
    return ESP_ERR_INVALID_STATE;
  }
  *out = handle->cached;
  xSemaphoreGive(handle->mutex);

  return ESP_OK;
}

esp_err_t baumer_set_laser(baumer_handle_t handle, bool on) {
  if (handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // Baumer OX100 requires entering parameterization mode before changing laser
  // state. Address 0: Request parameterization mode (write random value)
  esp_err_t ret =
      modbus_write_single_register(handle->modbus, handle->slave_id, 0, 1);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enter parameterization mode: %s",
             esp_err_to_name(ret));
    return ret;
  }

  // Small delay to allow sensor to process state change
  vTaskDelay(pdMS_TO_TICKS(50));

  uint16_t value = on ? 1 : 0;
  ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                     BAUMER_HREG_LASER, value);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Laser %s", on ? "ON" : "OFF");
  } else {
    ESP_LOGE(TAG, "Failed to set laser: %s", esp_err_to_name(ret));
  }

  // Address 2: Exit parameterization mode (write random value)
  esp_err_t exit_ret =
      modbus_write_single_register(handle->modbus, handle->slave_id, 2, 1);
  if (exit_ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to exit parameterization mode: %s",
             esp_err_to_name(exit_ret));
  }

  return ret;
}

void baumer_deinit(baumer_handle_t handle) {
  if (handle == NULL) {
    return;
  }

  if (handle->mutex) {
    vSemaphoreDelete(handle->mutex);
  }
  free(handle);
  ESP_LOGI(TAG, "Baumer OX100 deinitialized");
}
