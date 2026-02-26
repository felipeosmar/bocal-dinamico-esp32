# Baumer OX100 Profilometer Integration - Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Integrate Baumer OX100 profilometer readings via Modbus RTU to drive closed-loop actuator position control with configurable linear equations per actuator.

**Architecture:** New `baumer/` component reads Input Registers (FC04) from the sensor, a `control_loop/` FreeRTOS task applies per-actuator linear equations to compute positions, and new REST endpoints + web tab expose monitoring and configuration.

**Tech Stack:** ESP-IDF (C), FreeRTOS, Modbus RTU over RS485, LittleFS config, vanilla JS web UI.

**Design doc:** `docs/plans/2026-02-26-baumer-ox100-design.md`

---

### Task 1: Add modbus_read_input_registers (FC04)

The Modbus component has FC04 in its enum but no implementation. The Baumer OX100 serves measurements via Input Registers (FC04), so this must be added first.

**Files:**
- Modify: `main/modbus/modbus_rtu.h`
- Modify: `main/modbus/modbus_rtu.c`

**Step 1: Add function declaration to header**

In `main/modbus/modbus_rtu.h`, after `modbus_read_holding_registers` declaration (line 107), add:

```c
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
```

**Step 2: Implement in source file**

In `main/modbus/modbus_rtu.c`, after `modbus_read_holding_registers` (line 415), add. This is identical to `modbus_read_holding_registers` except uses `MODBUS_FC_READ_INPUT_REGISTERS`:

```c
esp_err_t modbus_read_input_registers(modbus_handle_t handle,
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
    request[1] = MODBUS_FC_READ_INPUT_REGISTERS;
    request[2] = (start_reg >> 8) & 0xFF;
    request[3] = start_reg & 0xFF;
    request[4] = (num_regs >> 8) & 0xFF;
    request[5] = num_regs & 0xFF;

    uint16_t crc = modbus_crc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    ESP_LOGD(TAG, "Read input regs: addr=%u, start=0x%04X, count=%u",
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
```

**Step 3: Build to verify compilation**

Run: `idf.py build`
Expected: Clean build with no errors.

**Step 4: Commit**

```bash
git add main/modbus/modbus_rtu.h main/modbus/modbus_rtu.c
git commit -m "feat(modbus): add read input registers FC04 support"
```

---

### Task 2: Create Baumer OX100 component

**Files:**
- Create: `main/baumer/baumer_ox100.h`
- Create: `main/baumer/baumer_ox100.c`
- Create: `main/baumer/CMakeLists.txt`

**Step 1: Create CMakeLists.txt**

```cmake
# main/baumer/CMakeLists.txt is not needed — files are compiled via main/CMakeLists.txt
```

Note: This project compiles all source files from the main component's CMakeLists.txt. No separate CMakeLists.txt needed per subfolder. See Task 9 for CMakeLists.txt update.

**Step 2: Create header file `main/baumer/baumer_ox100.h`**

```c
#ifndef BAUMER_OX100_H
#define BAUMER_OX100_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "modbus_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAUMER_QUALITY_OK          0
#define BAUMER_QUALITY_WEAK_SIGNAL 1
#define BAUMER_QUALITY_NO_SIGNAL   2

#define BAUMER_STATUS_BIT_PARAM_MODE  (1 << 0)
#define BAUMER_STATUS_BIT_TIME_SYNC   (1 << 1)
#define BAUMER_STATUS_BIT_VALID       (1 << 2)
#define BAUMER_STATUS_BIT_ALARM       (1 << 3)

#define BAUMER_NUM_VALUES 4

/**
 * @brief Baumer OX100 measurement data
 */
typedef struct {
    uint16_t status;                    // Status word (see BAUMER_STATUS_BIT_*)
    uint8_t  quality;                   // 0=OK, 1=Weak signal, 2=No signal
    uint8_t  output;                    // Binary outputs (bit 0=out1, bit 1=out2)
    float    values[BAUMER_NUM_VALUES]; // Measured values 1-4
} baumer_measurement_t;

typedef struct baumer_ctx *baumer_handle_t;

/**
 * @brief Initialize Baumer OX100 sensor
 *
 * @param modbus Modbus handle (shared with other devices on the bus)
 * @param slave_id Modbus slave address of the sensor
 * @param handle Pointer to store the created handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t baumer_init(modbus_handle_t modbus, uint8_t slave_id, baumer_handle_t *handle);

/**
 * @brief Read measurements from sensor (blocks on RS485 bus)
 *
 * Reads FC04 Input Registers at address 200-210 (11 registers):
 * status, quality, output, and 4 Float32 measured values.
 * Also updates the internal cache.
 *
 * @param handle Baumer handle
 * @param out Measurement data (can be NULL to only update cache)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t baumer_read_measurements(baumer_handle_t handle, baumer_measurement_t *out);

/**
 * @brief Get last cached measurement (thread-safe, does not use RS485 bus)
 *
 * @param handle Baumer handle
 * @param out Measurement data
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if no data yet
 */
esp_err_t baumer_get_cached(baumer_handle_t handle, baumer_measurement_t *out);

/**
 * @brief Control sensor laser
 *
 * @param handle Baumer handle
 * @param on true=laser ON, false=laser OFF
 * @return esp_err_t ESP_OK on success
 */
esp_err_t baumer_set_laser(baumer_handle_t handle, bool on);

/**
 * @brief Free resources
 *
 * @param handle Baumer handle
 */
void baumer_deinit(baumer_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // BAUMER_OX100_H
```

**Step 3: Create implementation `main/baumer/baumer_ox100.c`**

```c
#include "baumer_ox100.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "BAUMER";

// Baumer OX100 Input Register addresses (FC04)
#define BAUMER_REG_ALL_MEASUREMENTS  200  // 24 registers for all 32-bit values
#define BAUMER_REG_STATUS_OFFSET     0
#define BAUMER_REG_QUALITY_OFFSET    1
#define BAUMER_REG_OUTPUT_OFFSET     2
#define BAUMER_REG_VALUE1_OFFSET     3    // Float32: regs 3-4
#define BAUMER_REG_VALUE2_OFFSET     5    // Float32: regs 5-6
#define BAUMER_REG_VALUE3_OFFSET     7    // Float32: regs 7-8
#define BAUMER_REG_VALUE4_OFFSET     9    // Float32: regs 9-10
#define BAUMER_READ_NUM_REGS         11   // Status(1) + Quality(1) + Output(1) + 4*Float32(8)

// Baumer OX100 Holding Register addresses (FC06)
#define BAUMER_HREG_LASER            410  // 0=OFF, 1=ON

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
static float regs_to_float_le(uint16_t reg_low, uint16_t reg_high)
{
    uint32_t raw = ((uint32_t)reg_high << 16) | (uint32_t)reg_low;
    float value;
    memcpy(&value, &raw, sizeof(float));
    return value;
}

esp_err_t baumer_init(modbus_handle_t modbus, uint8_t slave_id, baumer_handle_t *handle)
{
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

esp_err_t baumer_read_measurements(baumer_handle_t handle, baumer_measurement_t *out)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t regs[BAUMER_READ_NUM_REGS];

    esp_err_t ret = modbus_read_input_registers(
        handle->modbus, handle->slave_id,
        BAUMER_REG_ALL_MEASUREMENTS, BAUMER_READ_NUM_REGS, regs);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read measurements: %s", esp_err_to_name(ret));
        return ret;
    }

    baumer_measurement_t m;
    m.status  = regs[BAUMER_REG_STATUS_OFFSET];
    m.quality = regs[BAUMER_REG_QUALITY_OFFSET] & 0xFF;
    m.output  = regs[BAUMER_REG_OUTPUT_OFFSET] & 0xFF;

    m.values[0] = regs_to_float_le(regs[BAUMER_REG_VALUE1_OFFSET], regs[BAUMER_REG_VALUE1_OFFSET + 1]);
    m.values[1] = regs_to_float_le(regs[BAUMER_REG_VALUE2_OFFSET], regs[BAUMER_REG_VALUE2_OFFSET + 1]);
    m.values[2] = regs_to_float_le(regs[BAUMER_REG_VALUE3_OFFSET], regs[BAUMER_REG_VALUE3_OFFSET + 1]);
    m.values[3] = regs_to_float_le(regs[BAUMER_REG_VALUE4_OFFSET], regs[BAUMER_REG_VALUE4_OFFSET + 1]);

    ESP_LOGD(TAG, "Measurement: status=0x%04X quality=%u values=[%.3f, %.3f, %.3f, %.3f]",
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

esp_err_t baumer_get_cached(baumer_handle_t handle, baumer_measurement_t *out)
{
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

esp_err_t baumer_set_laser(baumer_handle_t handle, bool on)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t value = on ? 1 : 0;
    esp_err_t ret = modbus_write_single_register(
        handle->modbus, handle->slave_id, BAUMER_HREG_LASER, value);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Laser %s", on ? "ON" : "OFF");
    } else {
        ESP_LOGE(TAG, "Failed to set laser: %s", esp_err_to_name(ret));
    }

    return ret;
}

void baumer_deinit(baumer_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->mutex) {
        vSemaphoreDelete(handle->mutex);
    }
    free(handle);
    ESP_LOGI(TAG, "Baumer OX100 deinitialized");
}
```

**Step 4: Build to verify compilation**

Build will fail until CMakeLists.txt is updated (Task 9). Skip build here, verify at Task 9.

**Step 5: Commit**

```bash
git add main/baumer/baumer_ox100.h main/baumer/baumer_ox100.c
git commit -m "feat(baumer): add Baumer OX100 profilometer component"
```

---

### Task 3: Create control loop component

**Files:**
- Create: `main/control_loop/control_loop.h`
- Create: `main/control_loop/control_loop.c`

**Step 1: Create header `main/control_loop/control_loop.h`**

```c
#ifndef CONTROL_LOOP_H
#define CONTROL_LOOP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "baumer_ox100.h"
#include "mightyzap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROL_MAX_EQUATIONS   10
#define CONTROL_MIN_INTERVAL_MS 100
#define CONTROL_MAX_INTERVAL_MS 60000
#define CONTROL_MAX_CONSECUTIVE_ERRORS 5

/**
 * @brief Per-actuator linear equation: position = a * gap + b
 */
typedef struct {
    uint8_t actuator_id;
    float coeff_a;
    float coeff_b;
    bool enabled;
} control_equation_t;

/**
 * @brief Control loop configuration
 */
typedef struct {
    bool running;
    uint32_t interval_ms;
    uint8_t measurement_index;   // Which value[] to use (0-3)
    control_equation_t equations[CONTROL_MAX_EQUATIONS];
    uint8_t equation_count;
} control_config_t;

/**
 * @brief Control loop runtime status (read-only snapshot)
 */
typedef struct {
    bool running;
    float last_gap_value;
    uint8_t last_quality;
    uint32_t loop_count;
    uint32_t error_count;
    int64_t last_run_timestamp_ms;
    uint16_t computed_positions[CONTROL_MAX_EQUATIONS];
    uint8_t computed_actuator_ids[CONTROL_MAX_EQUATIONS];
    uint8_t computed_count;
} control_status_t;

/**
 * @brief Initialize the control loop task
 *
 * Creates the FreeRTOS task but does not start the control loop.
 * The task always reads the Baumer sensor for monitoring.
 * Actuator control only runs when started via control_loop_start().
 *
 * @param baumer Baumer sensor handle
 * @param config Initial configuration (loaded from config.json)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t control_loop_init(baumer_handle_t baumer, const control_config_t *config);

/**
 * @brief Start automatic actuator control
 */
esp_err_t control_loop_start(void);

/**
 * @brief Stop automatic actuator control (monitoring continues)
 */
esp_err_t control_loop_stop(void);

/**
 * @brief Set loop interval
 *
 * @param interval_ms Interval in milliseconds (100-60000)
 */
esp_err_t control_loop_set_interval(uint32_t interval_ms);

/**
 * @brief Set equation for a specific actuator
 */
esp_err_t control_loop_set_equation(uint8_t actuator_id, float a, float b, bool enabled);

/**
 * @brief Get current configuration
 */
esp_err_t control_loop_get_config(control_config_t *out);

/**
 * @brief Get current status snapshot (thread-safe)
 */
esp_err_t control_loop_get_status(control_status_t *out);

/**
 * @brief Set measurement index (which value[] to use for control, 0-3)
 */
esp_err_t control_loop_set_measurement_index(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_LOOP_H
```

**Step 2: Create implementation `main/control_loop/control_loop.c`**

```c
#include "control_loop.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "CONTROL";

#define CONTROL_TASK_STACK_SIZE 4096
#define CONTROL_TASK_PRIORITY   5

// Defined in web_server.c / actuator handlers
extern mightyzap_handle_t *actuator_get_handle_by_id(uint8_t id);

static struct {
    baumer_handle_t baumer;
    control_config_t config;
    control_status_t status;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    uint8_t consecutive_errors;
} s_ctrl = {0};

static uint16_t clamp_position(float value)
{
    if (value < 0.0f) return 0;
    if (value > 4095.0f) return 4095;
    return (uint16_t)(value + 0.5f);  // Round to nearest
}

static void control_task(void *arg)
{
    ESP_LOGI(TAG, "Control loop task started");

    while (1) {
        // Wait for configured interval
        uint32_t interval;
        xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
        interval = s_ctrl.config.interval_ms;
        xSemaphoreGive(s_ctrl.mutex);

        vTaskDelay(pdMS_TO_TICKS(interval));

        // Always read sensor for monitoring
        baumer_measurement_t m;
        esp_err_t ret = baumer_read_measurements(s_ctrl.baumer, &m);

        xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);

        if (ret != ESP_OK) {
            s_ctrl.status.error_count++;
            s_ctrl.consecutive_errors++;
            ESP_LOGW(TAG, "Read error (%u consecutive)", s_ctrl.consecutive_errors);

            // Auto-stop after too many consecutive errors
            if (s_ctrl.config.running &&
                s_ctrl.consecutive_errors >= CONTROL_MAX_CONSECUTIVE_ERRORS) {
                s_ctrl.config.running = false;
                ESP_LOGE(TAG, "Stopped: %d consecutive errors", CONTROL_MAX_CONSECUTIVE_ERRORS);
            }
            xSemaphoreGive(s_ctrl.mutex);
            continue;
        }

        // Successful read - reset error counter
        s_ctrl.consecutive_errors = 0;
        s_ctrl.status.last_quality = m.quality;
        s_ctrl.status.last_run_timestamp_ms = esp_timer_get_time() / 1000;
        s_ctrl.status.loop_count++;

        uint8_t meas_idx = s_ctrl.config.measurement_index;
        if (meas_idx >= BAUMER_NUM_VALUES) meas_idx = 0;
        float gap = m.values[meas_idx];
        s_ctrl.status.last_gap_value = gap;

        bool should_control = s_ctrl.config.running;
        bool values_valid = (m.status & BAUMER_STATUS_BIT_VALID) != 0;
        bool signal_ok = (m.quality != BAUMER_QUALITY_NO_SIGNAL);

        // Copy config for use outside mutex
        control_config_t cfg = s_ctrl.config;

        xSemaphoreGive(s_ctrl.mutex);

        // Control actuators if running AND signal is usable
        if (should_control && values_valid && signal_ok) {
            uint8_t computed_count = 0;

            for (uint8_t i = 0; i < cfg.equation_count; i++) {
                if (!cfg.equations[i].enabled) continue;

                float pos_f = cfg.equations[i].coeff_a * gap + cfg.equations[i].coeff_b;
                uint16_t position = clamp_position(pos_f);

                mightyzap_handle_t *h = actuator_get_handle_by_id(cfg.equations[i].actuator_id);
                if (h != NULL) {
                    esp_err_t move_ret = mightyzap_set_position(*h, position);
                    if (move_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to move actuator %u: %s",
                                 cfg.equations[i].actuator_id, esp_err_to_name(move_ret));
                    }
                }

                // Update computed positions in status
                xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
                if (computed_count < CONTROL_MAX_EQUATIONS) {
                    s_ctrl.status.computed_positions[computed_count] = position;
                    s_ctrl.status.computed_actuator_ids[computed_count] = cfg.equations[i].actuator_id;
                    computed_count++;
                }
                s_ctrl.status.computed_count = computed_count;
                xSemaphoreGive(s_ctrl.mutex);
            }

            ESP_LOGD(TAG, "Loop #%lu: gap=%.3f → %u actuators updated",
                     (unsigned long)cfg.equation_count, gap, computed_count);
        } else if (should_control && !signal_ok) {
            ESP_LOGW(TAG, "Quality=%u (no signal), skipping actuator update", m.quality);
        } else if (should_control && !values_valid) {
            ESP_LOGW(TAG, "Values invalid (status=0x%04X), skipping actuator update", m.status);
        }
    }
}

esp_err_t control_loop_init(baumer_handle_t baumer, const control_config_t *config)
{
    if (baumer == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ctrl.baumer = baumer;
    s_ctrl.config = *config;
    s_ctrl.consecutive_errors = 0;
    memset(&s_ctrl.status, 0, sizeof(s_ctrl.status));

    s_ctrl.mutex = xSemaphoreCreateMutex();
    if (s_ctrl.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Don't auto-start running here — that's done separately
    s_ctrl.config.running = false;

    BaseType_t ret = xTaskCreatePinnedToCore(
        control_task, "ctrl_loop",
        CONTROL_TASK_STACK_SIZE, NULL,
        CONTROL_TASK_PRIORITY, &s_ctrl.task, 1);

    if (ret != pdPASS) {
        vSemaphoreDelete(s_ctrl.mutex);
        ESP_LOGE(TAG, "Failed to create control task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Control loop initialized, interval=%lu ms, equations=%u",
             config->interval_ms, config->equation_count);
    return ESP_OK;
}

esp_err_t control_loop_start(void)
{
    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    s_ctrl.config.running = true;
    s_ctrl.consecutive_errors = 0;
    xSemaphoreGive(s_ctrl.mutex);

    s_ctrl.status.running = true;
    ESP_LOGI(TAG, "Control loop started");
    return ESP_OK;
}

esp_err_t control_loop_stop(void)
{
    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    s_ctrl.config.running = false;
    xSemaphoreGive(s_ctrl.mutex);

    s_ctrl.status.running = false;
    ESP_LOGI(TAG, "Control loop stopped");
    return ESP_OK;
}

esp_err_t control_loop_set_interval(uint32_t interval_ms)
{
    if (interval_ms < CONTROL_MIN_INTERVAL_MS || interval_ms > CONTROL_MAX_INTERVAL_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    s_ctrl.config.interval_ms = interval_ms;
    xSemaphoreGive(s_ctrl.mutex);

    ESP_LOGI(TAG, "Interval set to %lu ms", interval_ms);
    return ESP_OK;
}

esp_err_t control_loop_set_equation(uint8_t actuator_id, float a, float b, bool enabled)
{
    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);

    // Find existing or add new
    int found = -1;
    for (uint8_t i = 0; i < s_ctrl.config.equation_count; i++) {
        if (s_ctrl.config.equations[i].actuator_id == actuator_id) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        s_ctrl.config.equations[found].coeff_a = a;
        s_ctrl.config.equations[found].coeff_b = b;
        s_ctrl.config.equations[found].enabled = enabled;
    } else if (s_ctrl.config.equation_count < CONTROL_MAX_EQUATIONS) {
        uint8_t idx = s_ctrl.config.equation_count;
        s_ctrl.config.equations[idx].actuator_id = actuator_id;
        s_ctrl.config.equations[idx].coeff_a = a;
        s_ctrl.config.equations[idx].coeff_b = b;
        s_ctrl.config.equations[idx].enabled = enabled;
        s_ctrl.config.equation_count++;
    } else {
        xSemaphoreGive(s_ctrl.mutex);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_ctrl.mutex);

    ESP_LOGI(TAG, "Equation for actuator %u: pos = %.3f * gap + %.3f (%s)",
             actuator_id, a, b, enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t control_loop_get_config(control_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    *out = s_ctrl.config;
    xSemaphoreGive(s_ctrl.mutex);

    return ESP_OK;
}

esp_err_t control_loop_get_status(control_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    *out = s_ctrl.status;
    out->running = s_ctrl.config.running;
    xSemaphoreGive(s_ctrl.mutex);

    return ESP_OK;
}

esp_err_t control_loop_set_measurement_index(uint8_t index)
{
    if (index >= BAUMER_NUM_VALUES) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    s_ctrl.config.measurement_index = index;
    xSemaphoreGive(s_ctrl.mutex);

    ESP_LOGI(TAG, "Measurement index set to %u", index);
    return ESP_OK;
}
```

**Important note for implementation:** The `actuator_get_handle_by_id()` function must be exposed from the actuator handler code in `web_server.c` (or a separate actuator manager). This will be addressed in Task 7 when modifying main.c and the web server. If the current codebase stores actuator handles in an array within the web server handlers, a lookup function needs to be added.

**Step 3: Commit**

```bash
git add main/control_loop/control_loop.h main/control_loop/control_loop.c
git commit -m "feat(control): add closed-loop control task with per-actuator equations"
```

---

### Task 4: Extend config_manager

**Files:**
- Modify: `main/config/config_manager.h`
- Modify: `main/config/config_manager.c`

**Step 1: Add declarations to `main/config/config_manager.h`**

Add after the existing web auth section (before `#endif`):

```c
// --- Baumer OX100 ---
uint8_t  config_get_baumer_slave_id(void);
bool     config_get_baumer_enabled(void);
void     config_set_baumer_slave_id(uint8_t id);
void     config_set_baumer_enabled(bool enabled);

// --- Control Loop ---
bool     config_get_control_running(void);
uint32_t config_get_control_interval(void);
uint8_t  config_get_control_measurement_index(void);
void     config_set_control_running(bool running);
void     config_set_control_interval(uint32_t ms);
void     config_set_control_measurement_index(uint8_t idx);

// Forward declare to avoid circular include
typedef struct {
    uint8_t actuator_id;
    float coeff_a;
    float coeff_b;
    bool enabled;
} config_control_equation_t;

#define CONFIG_MAX_EQUATIONS 10

int  config_get_control_equations(config_control_equation_t *out, int max_count);
void config_set_control_equations(const config_control_equation_t *eqs, int count);
```

**Step 2: Add fields to internal config_t struct in `config_manager.c`**

Add to the `config_t` struct (inside the `typedef struct`):

```c
    // Baumer
    uint8_t baumer_slave_id;
    bool baumer_enabled;

    // Control loop
    bool control_running;
    uint32_t control_interval_ms;
    uint8_t control_measurement_index;
    config_control_equation_t control_equations[CONFIG_MAX_EQUATIONS];
    uint8_t control_equation_count;
```

**Step 3: Add default values in `config_reset_defaults()`**

```c
    s_config.baumer_slave_id = 8;
    s_config.baumer_enabled = true;
    s_config.control_running = false;
    s_config.control_interval_ms = 1000;
    s_config.control_measurement_index = 0;
    s_config.control_equation_count = 0;
```

**Step 4: Add JSON save logic in `config_save()`**

After existing sections, add:

```c
    // Baumer section
    cJSON *baumer = cJSON_CreateObject();
    cJSON_AddNumberToObject(baumer, "slave_id", s_config.baumer_slave_id);
    cJSON_AddBoolToObject(baumer, "enabled", s_config.baumer_enabled);
    cJSON_AddItemToObject(root, "baumer", baumer);

    // Control section
    cJSON *control = cJSON_CreateObject();
    cJSON_AddBoolToObject(control, "running", s_config.control_running);
    cJSON_AddNumberToObject(control, "interval_ms", s_config.control_interval_ms);
    cJSON_AddNumberToObject(control, "measurement_index", s_config.control_measurement_index);

    cJSON *equations = cJSON_CreateArray();
    for (uint8_t i = 0; i < s_config.control_equation_count; i++) {
        cJSON *eq = cJSON_CreateObject();
        cJSON_AddNumberToObject(eq, "actuator_id", s_config.control_equations[i].actuator_id);
        cJSON_AddNumberToObject(eq, "a", s_config.control_equations[i].coeff_a);
        cJSON_AddNumberToObject(eq, "b", s_config.control_equations[i].coeff_b);
        cJSON_AddBoolToObject(eq, "enabled", s_config.control_equations[i].enabled);
        cJSON_AddItemToArray(equations, eq);
    }
    cJSON_AddItemToObject(control, "equations", equations);
    cJSON_AddItemToObject(root, "control", control);
```

**Step 5: Add JSON load logic in `config_load()`**

After existing sections, add:

```c
    // Parse baumer section
    cJSON *baumer = cJSON_GetObjectItem(root, "baumer");
    if (baumer) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(baumer, "slave_id")) && cJSON_IsNumber(item))
            s_config.baumer_slave_id = (uint8_t)item->valueint;
        if ((item = cJSON_GetObjectItem(baumer, "enabled")) && cJSON_IsBool(item))
            s_config.baumer_enabled = cJSON_IsTrue(item);
    }

    // Parse control section
    cJSON *control = cJSON_GetObjectItem(root, "control");
    if (control) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(control, "running")) && cJSON_IsBool(item))
            s_config.control_running = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(control, "interval_ms")) && cJSON_IsNumber(item))
            s_config.control_interval_ms = (uint32_t)item->valueint;
        if ((item = cJSON_GetObjectItem(control, "measurement_index")) && cJSON_IsNumber(item))
            s_config.control_measurement_index = (uint8_t)item->valueint;

        cJSON *equations = cJSON_GetObjectItem(control, "equations");
        if (equations && cJSON_IsArray(equations)) {
            s_config.control_equation_count = 0;
            cJSON *eq;
            cJSON_ArrayForEach(eq, equations) {
                if (s_config.control_equation_count >= CONFIG_MAX_EQUATIONS) break;
                uint8_t idx = s_config.control_equation_count;

                cJSON *aid = cJSON_GetObjectItem(eq, "actuator_id");
                cJSON *a = cJSON_GetObjectItem(eq, "a");
                cJSON *b = cJSON_GetObjectItem(eq, "b");
                cJSON *en = cJSON_GetObjectItem(eq, "enabled");

                if (aid && cJSON_IsNumber(aid)) {
                    s_config.control_equations[idx].actuator_id = (uint8_t)aid->valueint;
                    s_config.control_equations[idx].coeff_a = (a && cJSON_IsNumber(a)) ? (float)a->valuedouble : 1.0f;
                    s_config.control_equations[idx].coeff_b = (b && cJSON_IsNumber(b)) ? (float)b->valuedouble : 0.0f;
                    s_config.control_equations[idx].enabled = (en && cJSON_IsBool(en)) ? cJSON_IsTrue(en) : false;
                    s_config.control_equation_count++;
                }
            }
        }
    }
```

**Step 6: Implement getter/setter functions**

Add at the end of `config_manager.c`:

```c
// --- Baumer ---
uint8_t config_get_baumer_slave_id(void) { return s_config.baumer_slave_id; }
bool    config_get_baumer_enabled(void) { return s_config.baumer_enabled; }
void    config_set_baumer_slave_id(uint8_t id) { s_config.baumer_slave_id = id; }
void    config_set_baumer_enabled(bool enabled) { s_config.baumer_enabled = enabled; }

// --- Control Loop ---
bool     config_get_control_running(void) { return s_config.control_running; }
uint32_t config_get_control_interval(void) { return s_config.control_interval_ms; }
uint8_t  config_get_control_measurement_index(void) { return s_config.control_measurement_index; }
void     config_set_control_running(bool running) { s_config.control_running = running; }
void     config_set_control_interval(uint32_t ms) { s_config.control_interval_ms = ms; }
void     config_set_control_measurement_index(uint8_t idx) { s_config.control_measurement_index = idx; }

int config_get_control_equations(config_control_equation_t *out, int max_count)
{
    int count = s_config.control_equation_count;
    if (count > max_count) count = max_count;
    if (out != NULL) {
        memcpy(out, s_config.control_equations, count * sizeof(config_control_equation_t));
    }
    return count;
}

void config_set_control_equations(const config_control_equation_t *eqs, int count)
{
    if (count > CONFIG_MAX_EQUATIONS) count = CONFIG_MAX_EQUATIONS;
    memcpy(s_config.control_equations, eqs, count * sizeof(config_control_equation_t));
    s_config.control_equation_count = (uint8_t)count;
}
```

**Step 7: Commit**

```bash
git add main/config/config_manager.h main/config/config_manager.c
git commit -m "feat(config): add Baumer and control loop configuration"
```

---

### Task 5: Add Baumer REST handlers

**Files:**
- Create: `main/webserver/web_baumer.c`

**Step 1: Create `main/webserver/web_baumer.c`**

```c
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "baumer_ox100.h"
#include "config_manager.h"

static const char *TAG = "WEB_BAUMER";

extern baumer_handle_t g_baumer;

static const char *quality_to_string(uint8_t quality)
{
    switch (quality) {
        case BAUMER_QUALITY_OK:          return "OK";
        case BAUMER_QUALITY_WEAK_SIGNAL: return "Weak signal";
        case BAUMER_QUALITY_NO_SIGNAL:   return "No signal";
        default:                         return "Unknown";
    }
}

// GET /api/baumer/status
static esp_err_t api_baumer_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (g_baumer == NULL) {
        httpd_resp_sendstr(req, "{\"connected\":false}");
        return ESP_OK;
    }

    baumer_measurement_t m;
    bool has_data = (baumer_get_cached(g_baumer, &m) == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", has_data);
    cJSON_AddNumberToObject(root, "slave_id", config_get_baumer_slave_id());

    if (has_data) {
        cJSON_AddNumberToObject(root, "status", m.status);
        cJSON_AddNumberToObject(root, "quality", m.quality);
        cJSON_AddStringToObject(root, "quality_text", quality_to_string(m.quality));
        cJSON_AddBoolToObject(root, "valid", (m.status & BAUMER_STATUS_BIT_VALID) != 0);

        cJSON *values = cJSON_CreateArray();
        for (int i = 0; i < BAUMER_NUM_VALUES; i++) {
            cJSON_AddItemToArray(values, cJSON_CreateNumber(m.values[i]));
        }
        cJSON_AddItemToObject(root, "values", values);
    }

    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, str);
    free((void *)str);
    cJSON_Delete(root);

    return ESP_OK;
}

// GET /api/baumer/config
static esp_err_t api_baumer_config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "slave_id", config_get_baumer_slave_id());
    cJSON_AddBoolToObject(root, "enabled", config_get_baumer_enabled());

    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, str);
    free((void *)str);
    cJSON_Delete(root);

    return ESP_OK;
}

// PUT /api/baumer/config
static esp_err_t api_baumer_config_put_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "slave_id")) && cJSON_IsNumber(item))
        config_set_baumer_slave_id((uint8_t)item->valueint);
    if ((item = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(item))
        config_set_baumer_enabled(cJSON_IsTrue(item));

    config_save();
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
    return ESP_OK;
}

// POST /api/baumer/laser
static esp_err_t api_baumer_laser_handler(httpd_req_t *req)
{
    if (g_baumer == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Baumer not initialized");
        return ESP_FAIL;
    }

    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *on = cJSON_GetObjectItem(root, "on");
    if (on == NULL || !cJSON_IsBool(on)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'on' field");
        return ESP_FAIL;
    }

    esp_err_t ret = baumer_set_laser(g_baumer, cJSON_IsTrue(on));
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to control laser");
    }

    return ESP_OK;
}

void register_baumer_handlers(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/api/baumer/status", .method = HTTP_GET,  .handler = api_baumer_status_handler },
        { .uri = "/api/baumer/config", .method = HTTP_GET,  .handler = api_baumer_config_get_handler },
        { .uri = "/api/baumer/config", .method = HTTP_PUT,  .handler = api_baumer_config_put_handler },
        { .uri = "/api/baumer/laser",  .method = HTTP_POST, .handler = api_baumer_laser_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Baumer API handlers registered");
}
```

**Step 2: Commit**

```bash
git add main/webserver/web_baumer.c
git commit -m "feat(web): add Baumer OX100 REST API handlers"
```

---

### Task 6: Add Control Loop REST handlers

**Files:**
- Create: `main/webserver/web_control.c`

**Step 1: Create `main/webserver/web_control.c`**

```c
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "control_loop.h"
#include "config_manager.h"

static const char *TAG = "WEB_CONTROL";

// GET /api/control/status
static esp_err_t api_control_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    control_status_t status;
    control_config_t config;
    control_loop_get_status(&status);
    control_loop_get_config(&config);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", status.running);
    cJSON_AddNumberToObject(root, "interval_ms", config.interval_ms);
    cJSON_AddNumberToObject(root, "measurement_index", config.measurement_index);
    cJSON_AddNumberToObject(root, "last_gap_value", status.last_gap_value);
    cJSON_AddNumberToObject(root, "last_quality", status.last_quality);
    cJSON_AddNumberToObject(root, "loop_count", status.loop_count);
    cJSON_AddNumberToObject(root, "error_count", status.error_count);

    cJSON *equations = cJSON_CreateArray();
    for (uint8_t i = 0; i < config.equation_count; i++) {
        cJSON *eq = cJSON_CreateObject();
        cJSON_AddNumberToObject(eq, "actuator_id", config.equations[i].actuator_id);
        cJSON_AddNumberToObject(eq, "a", config.equations[i].coeff_a);
        cJSON_AddNumberToObject(eq, "b", config.equations[i].coeff_b);
        cJSON_AddBoolToObject(eq, "enabled", config.equations[i].enabled);

        // Find computed position for this actuator
        int pos = -1;
        for (uint8_t j = 0; j < status.computed_count; j++) {
            if (status.computed_actuator_ids[j] == config.equations[i].actuator_id) {
                pos = status.computed_positions[j];
                break;
            }
        }
        if (pos >= 0) {
            cJSON_AddNumberToObject(eq, "computed_position", pos);
        } else {
            cJSON_AddNullToObject(eq, "computed_position");
        }

        cJSON_AddItemToArray(equations, eq);
    }
    cJSON_AddItemToObject(root, "equations", equations);

    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, str);
    free((void *)str);
    cJSON_Delete(root);

    return ESP_OK;
}

// POST /api/control/start
static esp_err_t api_control_start_handler(httpd_req_t *req)
{
    control_loop_start();
    config_set_control_running(true);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"running\":true}");
    return ESP_OK;
}

// POST /api/control/stop
static esp_err_t api_control_stop_handler(httpd_req_t *req)
{
    control_loop_stop();
    config_set_control_running(false);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"running\":false}");
    return ESP_OK;
}

// PUT /api/control/interval
static esp_err_t api_control_interval_handler(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ms = cJSON_GetObjectItem(root, "interval_ms");
    if (ms == NULL || !cJSON_IsNumber(ms)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'interval_ms'");
        return ESP_FAIL;
    }

    uint32_t interval = (uint32_t)ms->valueint;
    cJSON_Delete(root);

    esp_err_t ret = control_loop_set_interval(interval);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid interval (100-60000)");
        return ESP_FAIL;
    }

    config_set_control_interval(interval);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// PUT /api/control/equation
static esp_err_t api_control_equation_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_j = cJSON_GetObjectItem(root, "actuator_id");
    cJSON *a_j = cJSON_GetObjectItem(root, "a");
    cJSON *b_j = cJSON_GetObjectItem(root, "b");
    cJSON *en_j = cJSON_GetObjectItem(root, "enabled");

    if (!id_j || !cJSON_IsNumber(id_j) || !a_j || !cJSON_IsNumber(a_j) ||
        !b_j || !cJSON_IsNumber(b_j) || !en_j || !cJSON_IsBool(en_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                           "Required: actuator_id, a, b, enabled");
        return ESP_FAIL;
    }

    uint8_t actuator_id = (uint8_t)id_j->valueint;
    float a = (float)a_j->valuedouble;
    float b = (float)b_j->valuedouble;
    bool enabled = cJSON_IsTrue(en_j);
    cJSON_Delete(root);

    esp_err_t ret = control_loop_set_equation(actuator_id, a, b, enabled);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set equation");
        return ESP_FAIL;
    }

    // Persist all equations
    control_config_t cfg;
    control_loop_get_config(&cfg);
    config_control_equation_t eqs[CONFIG_MAX_EQUATIONS];
    for (uint8_t i = 0; i < cfg.equation_count; i++) {
        eqs[i].actuator_id = cfg.equations[i].actuator_id;
        eqs[i].coeff_a = cfg.equations[i].coeff_a;
        eqs[i].coeff_b = cfg.equations[i].coeff_b;
        eqs[i].enabled = cfg.equations[i].enabled;
    }
    config_set_control_equations(eqs, cfg.equation_count);
    config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

void register_control_handlers(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/api/control/status",   .method = HTTP_GET,  .handler = api_control_status_handler },
        { .uri = "/api/control/start",    .method = HTTP_POST, .handler = api_control_start_handler },
        { .uri = "/api/control/stop",     .method = HTTP_POST, .handler = api_control_stop_handler },
        { .uri = "/api/control/interval", .method = HTTP_PUT,  .handler = api_control_interval_handler },
        { .uri = "/api/control/equation", .method = HTTP_PUT,  .handler = api_control_equation_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Control API handlers registered");
}
```

**Step 2: Commit**

```bash
git add main/webserver/web_control.c
git commit -m "feat(web): add control loop REST API handlers"
```

---

### Task 7: Modify main.c and web_server.c

**Files:**
- Modify: `main/main.c` — add g_baumer, Baumer + control loop initialization
- Modify: `main/webserver/web_server.c` — register new handlers, expose actuator lookup

**Step 1: Add global handle and includes in `main/main.c`**

Add to the existing global handles section:

```c
#include "baumer_ox100.h"
#include "control_loop.h"

baumer_handle_t g_baumer = NULL;
```

**Step 2: Add Baumer/control init in `app_main()`**

After `init_communication()` and mightyZAP initialization, before `web_server_init()`:

```c
    // Initialize Baumer OX100 profilometer
    if (config_get_baumer_enabled()) {
        esp_err_t bret = baumer_init(g_modbus, config_get_baumer_slave_id(), &g_baumer);
        if (bret == ESP_OK) {
            ESP_LOGI(TAG, "Baumer OX100 initialized (ID=%u)", config_get_baumer_slave_id());

            // Initialize control loop
            control_config_t ctrl_cfg = {
                .running = false,
                .interval_ms = config_get_control_interval(),
                .measurement_index = config_get_control_measurement_index(),
                .equation_count = 0,
            };

            // Load equations from config
            config_control_equation_t ceqs[CONFIG_MAX_EQUATIONS];
            int eq_count = config_get_control_equations(ceqs, CONFIG_MAX_EQUATIONS);
            for (int i = 0; i < eq_count; i++) {
                ctrl_cfg.equations[i].actuator_id = ceqs[i].actuator_id;
                ctrl_cfg.equations[i].coeff_a = ceqs[i].coeff_a;
                ctrl_cfg.equations[i].coeff_b = ceqs[i].coeff_b;
                ctrl_cfg.equations[i].enabled = ceqs[i].enabled;
            }
            ctrl_cfg.equation_count = (uint8_t)eq_count;

            if (control_loop_init(g_baumer, &ctrl_cfg) == ESP_OK) {
                if (config_get_control_running()) {
                    control_loop_start();
                    ESP_LOGI(TAG, "Control loop auto-started from config");
                }
            }
        } else {
            ESP_LOGW(TAG, "Baumer OX100 init failed: %s", esp_err_to_name(bret));
        }
    }
```

**Step 3: Register new handlers in `web_server.c`**

Add forward declarations at the top of `web_server.c`:

```c
// From web_baumer.c
extern void register_baumer_handlers(httpd_handle_t server);
// From web_control.c
extern void register_control_handlers(httpd_handle_t server);
```

In the server init function, after existing route registrations:

```c
    // Baumer and control loop APIs
    register_baumer_handlers(s_server);
    register_control_handlers(s_server);
```

**Step 4: Expose actuator handle lookup**

Check how actuator handles are stored in the web server handlers. Add a function to look up a handle by actuator ID. This depends on the existing implementation — the function `actuator_get_handle_by_id()` referenced in `control_loop.c` needs to be implemented where the actuator array lives (likely in the actuator handler code).

If actuators are stored in an array like:
```c
static mightyzap_handle_t s_actuators[MAX_ACTUATORS];
static uint8_t s_actuator_ids[MAX_ACTUATORS];
static uint8_t s_actuator_count;
```

Then add:
```c
mightyzap_handle_t *actuator_get_handle_by_id(uint8_t id)
{
    for (uint8_t i = 0; i < s_actuator_count; i++) {
        if (s_actuator_ids[i] == id && s_actuators[i] != NULL) {
            return &s_actuators[i];
        }
    }
    return NULL;
}
```

**Step 5: Add static file routes for profiler tab**

In `web_server.c`, add routes for the new tab files alongside existing tab routes:

```c
    // Profiler tab
    { .uri = "/tabs/profiler.html", .method = HTTP_GET, .handler = tabs_html_handler },
    { .uri = "/tabs/profiler.js",   .method = HTTP_GET, .handler = tabs_js_handler },
```

**Step 6: Commit**

```bash
git add main/main.c main/webserver/web_server.c
git commit -m "feat: integrate Baumer and control loop into main init and web server"
```

---

### Task 8: Create web interface - Profiler tab

**Files:**
- Create: `main/www/tabs/profiler.html`
- Create: `main/www/tabs/profiler.js`
- Modify: `main/www/index.html`

**Step 1: Create `main/www/tabs/profiler.html`**

```html
<div class="profiler-grid">
  <!-- Measured Values Panel -->
  <div class="card">
    <h3>Measured Values</h3>
    <div class="profiler-values">
      <div class="value-row">
        <span class="label">Value 1:</span>
        <span id="pf-val1" class="value">--</span>
        <span class="unit">mm</span>
      </div>
      <div class="value-row">
        <span class="label">Value 2:</span>
        <span id="pf-val2" class="value">--</span>
        <span class="unit">mm</span>
      </div>
      <div class="value-row">
        <span class="label">Value 3:</span>
        <span id="pf-val3" class="value">--</span>
        <span class="unit">mm</span>
      </div>
      <div class="value-row">
        <span class="label">Value 4:</span>
        <span id="pf-val4" class="value">--</span>
        <span class="unit">mm</span>
      </div>
    </div>
    <div class="status-row">
      <span>Quality: <span id="pf-quality" class="badge">--</span></span>
      <span>Laser: <span id="pf-laser-status">--</span>
        <button id="pf-laser-btn" class="btn btn-sm" onclick="profilerToggleLaser()">Toggle</button>
      </span>
    </div>
  </div>

  <!-- Control Loop Panel -->
  <div class="card">
    <h3>Control Loop</h3>
    <div class="control-status">
      <div class="status-row">
        <span>Status:</span>
        <span id="pf-ctrl-status" class="badge">--</span>
      </div>
      <div class="status-row">
        <span>Interval (ms):</span>
        <input type="number" id="pf-interval" min="100" max="60000" value="1000" class="input-sm">
        <button class="btn btn-sm" onclick="profilerSetInterval()">Set</button>
      </div>
      <div class="status-row">
        <span>Control value:</span>
        <select id="pf-meas-index" class="input-sm" onchange="profilerSetMeasIndex()">
          <option value="0">Value 1</option>
          <option value="1">Value 2</option>
          <option value="2">Value 3</option>
          <option value="3">Value 4</option>
        </select>
      </div>
      <div class="status-row">
        <span>Loop count: <span id="pf-loop-count">0</span></span>
        <span>Errors: <span id="pf-error-count">0</span></span>
      </div>
      <div class="status-row">
        <span>Gap value: <span id="pf-gap-value">--</span> mm</span>
      </div>
      <div class="btn-group">
        <button class="btn btn-success" onclick="profilerStart()">Start</button>
        <button class="btn btn-danger" onclick="profilerStop()">Stop</button>
      </div>
    </div>
  </div>
</div>

<!-- Equations Table -->
<div class="card">
  <h3>Equations per Actuator</h3>
  <p class="info-text">position = a &times; gap + b</p>
  <table class="table" id="pf-equations-table">
    <thead>
      <tr>
        <th>Actuator</th>
        <th>a (slope)</th>
        <th>b (offset)</th>
        <th>Enabled</th>
        <th>Computed Position</th>
        <th>Action</th>
      </tr>
    </thead>
    <tbody id="pf-equations-body">
    </tbody>
  </table>
</div>

<!-- Baumer Config -->
<div class="card">
  <h3>Baumer Configuration</h3>
  <div class="status-row">
    <span>Slave ID:</span>
    <input type="number" id="pf-slave-id" min="1" max="247" value="8" class="input-sm">
    <span>Enabled:</span>
    <input type="checkbox" id="pf-baumer-enabled" checked>
    <button class="btn btn-sm" onclick="profilerSaveBaumerConfig()">Save (requires restart)</button>
  </div>
</div>
```

**Step 2: Create `main/www/tabs/profiler.js`**

```js
let profilerTimer = null;

function profilerInit() {
    profilerLoadConfig();
    profilerPoll();
    profilerTimer = setInterval(profilerPoll, 1000);
}

function profilerCleanup() {
    if (profilerTimer) {
        clearInterval(profilerTimer);
        profilerTimer = null;
    }
}

async function profilerPoll() {
    try {
        const [baumer, control] = await Promise.all([
            api('baumer/status'),
            api('control/status')
        ]);

        // Update measured values
        if (baumer.connected && baumer.values) {
            for (let i = 0; i < 4; i++) {
                const el = document.getElementById('pf-val' + (i + 1));
                if (el) el.textContent = baumer.values[i] != null ? baumer.values[i].toFixed(3) : '--';
            }

            const qEl = document.getElementById('pf-quality');
            if (qEl) {
                qEl.textContent = baumer.quality_text || '--';
                qEl.className = 'badge ' + (baumer.quality === 0 ? 'badge-ok' :
                                             baumer.quality === 1 ? 'badge-warn' : 'badge-err');
            }
        }

        // Update control status
        const statusEl = document.getElementById('pf-ctrl-status');
        if (statusEl) {
            statusEl.textContent = control.running ? 'Running' : 'Stopped';
            statusEl.className = 'badge ' + (control.running ? 'badge-ok' : 'badge-off');
        }

        document.getElementById('pf-loop-count').textContent = control.loop_count || 0;
        document.getElementById('pf-error-count').textContent = control.error_count || 0;
        document.getElementById('pf-gap-value').textContent =
            control.last_gap_value != null ? control.last_gap_value.toFixed(3) : '--';
        document.getElementById('pf-interval').value = control.interval_ms || 1000;
        document.getElementById('pf-meas-index').value = control.measurement_index || 0;

        // Update equations table
        profilerUpdateEquationsTable(control.equations || []);

    } catch (e) {
        console.error('Profiler poll error:', e);
    }
}

function profilerUpdateEquationsTable(equations) {
    const tbody = document.getElementById('pf-equations-body');
    if (!tbody) return;

    // Preserve user edits if count hasn't changed
    if (tbody.children.length === equations.length) {
        // Just update computed positions
        for (let i = 0; i < equations.length; i++) {
            const row = tbody.children[i];
            const posCell = row.querySelector('.computed-pos');
            if (posCell) {
                posCell.textContent = equations[i].computed_position != null ?
                    equations[i].computed_position : '--';
            }
        }
        return;
    }

    tbody.innerHTML = '';
    for (const eq of equations) {
        const tr = document.createElement('tr');
        tr.innerHTML =
            '<td>ID ' + eq.actuator_id + '</td>' +
            '<td><input type="number" step="0.001" value="' + eq.a + '" class="input-sm eq-a" data-id="' + eq.actuator_id + '"></td>' +
            '<td><input type="number" step="0.001" value="' + eq.b + '" class="input-sm eq-b" data-id="' + eq.actuator_id + '"></td>' +
            '<td><input type="checkbox" class="eq-enabled" data-id="' + eq.actuator_id + '"' + (eq.enabled ? ' checked' : '') + '></td>' +
            '<td class="computed-pos">' + (eq.computed_position != null ? eq.computed_position : '--') + '</td>' +
            '<td><button class="btn btn-sm" onclick="profilerSaveEquation(' + eq.actuator_id + ')">Save</button></td>';
        tbody.appendChild(tr);
    }
}

async function profilerSaveEquation(actuatorId) {
    const a = parseFloat(document.querySelector('.eq-a[data-id="' + actuatorId + '"]').value);
    const b = parseFloat(document.querySelector('.eq-b[data-id="' + actuatorId + '"]').value);
    const enabled = document.querySelector('.eq-enabled[data-id="' + actuatorId + '"]').checked;

    try {
        await api('control/equation', 'PUT', {
            actuator_id: actuatorId, a: a, b: b, enabled: enabled
        });
    } catch (e) {
        alert('Failed to save equation: ' + e.message);
    }
}

async function profilerStart() {
    try { await api('control/start', 'POST'); } catch (e) { alert('Error: ' + e.message); }
}

async function profilerStop() {
    try { await api('control/stop', 'POST'); } catch (e) { alert('Error: ' + e.message); }
}

async function profilerSetInterval() {
    const ms = parseInt(document.getElementById('pf-interval').value);
    try {
        await api('control/interval', 'PUT', { interval_ms: ms });
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

async function profilerSetMeasIndex() {
    const idx = parseInt(document.getElementById('pf-meas-index').value);
    try {
        await api('control/interval', 'PUT', { measurement_index: idx });
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

async function profilerToggleLaser() {
    try {
        const status = await api('baumer/status');
        // Toggle based on current quality (if reading, laser is on)
        await api('baumer/laser', 'POST', { on: !(status.connected) });
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

async function profilerLoadConfig() {
    try {
        const cfg = await api('baumer/config');
        document.getElementById('pf-slave-id').value = cfg.slave_id || 8;
        document.getElementById('pf-baumer-enabled').checked = cfg.enabled !== false;
    } catch (e) {
        console.error('Failed to load Baumer config:', e);
    }
}

async function profilerSaveBaumerConfig() {
    const slaveId = parseInt(document.getElementById('pf-slave-id').value);
    const enabled = document.getElementById('pf-baumer-enabled').checked;
    try {
        await api('baumer/config', 'PUT', { slave_id: slaveId, enabled: enabled });
        alert('Saved. Restart required for changes to take effect.');
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

registerModule('profiler', profilerInit);
```

**Step 3: Modify `main/www/index.html`**

Add the Profiler tab button in the navigation:

```html
<button class="nav-btn" data-tab="profiler">Profiler</button>
```

Add the tab container:

```html
<section id="tab-profiler" class="tab"></section>
```

**Step 4: Add profiler to module registry in `main/www/core.js`**

In the `modules` object:

```js
    profiler: { loaded: false, init: null },
```

**Step 5: Commit**

```bash
git add main/www/tabs/profiler.html main/www/tabs/profiler.js main/www/index.html main/www/core.js
git commit -m "feat(web): add Profiler tab for Baumer monitoring and control loop"
```

---

### Task 9: Update CMakeLists.txt and build

**Files:**
- Modify: `main/CMakeLists.txt`

**Step 1: Add new source files to CMakeLists.txt**

In the `SRCS` list of `idf_component_register()`, add:

```cmake
    "baumer/baumer_ox100.c"
    "control_loop/control_loop.c"
    "webserver/web_baumer.c"
    "webserver/web_control.c"
```

In the `INCLUDE_DIRS` list, add:

```cmake
    "baumer"
    "control_loop"
```

**Step 2: Full build**

Run: `idf.py build`
Expected: Clean build with no errors. Fix any compilation issues.

**Step 3: Commit**

```bash
git add main/CMakeLists.txt
git commit -m "build: add Baumer and control loop components to build"
```

---

### Task 10: Integration verification

**Step 1: Flash and test**

```bash
./flash.sh update
idf.py -p /dev/ttyUSB0 monitor
```

**Step 2: Verify boot sequence**

Expected log output:
```
I (xxx) BAUMER: Baumer OX100 initialized, slave_id=8
I (xxx) CONTROL: Control loop initialized, interval=1000 ms, equations=0
I (xxx) WEB_BAUMER: Baumer API handlers registered
I (xxx) WEB_CONTROL: Control API handlers registered
```

**Step 3: Test REST APIs**

```bash
# Test Baumer status (should return measurement data if sensor connected)
curl http://<esp32-ip>/api/baumer/status

# Test Baumer config
curl http://<esp32-ip>/api/baumer/config

# Test control status
curl http://<esp32-ip>/api/control/status

# Test laser control
curl -X POST http://<esp32-ip>/api/baumer/laser -d '{"on":true}'

# Set an equation
curl -X PUT http://<esp32-ip>/api/control/equation \
  -d '{"actuator_id":1,"a":120.5,"b":-30.0,"enabled":true}'

# Start control loop
curl -X POST http://<esp32-ip>/api/control/start

# Check status again
curl http://<esp32-ip>/api/control/status
```

**Step 4: Test web interface**

Open browser, navigate to the Profiler tab. Verify:
- 4 measured values display and update
- Quality indicator shows correct color
- Control loop start/stop works
- Equations table populated with connected actuators
- Interval change works

**Step 5: Commit any fixes**

```bash
git add -A
git commit -m "fix: integration adjustments for Baumer and control loop"
```

---

## Summary

| Task | Description | Est. Files |
|------|-------------|------------|
| 1 | Add FC04 to Modbus component | 2 modified |
| 2 | Create Baumer OX100 component | 2 created |
| 3 | Create control loop component | 2 created |
| 4 | Extend config_manager | 2 modified |
| 5 | Add Baumer REST handlers | 1 created |
| 6 | Add Control REST handlers | 1 created |
| 7 | Modify main.c + web_server.c | 2 modified |
| 8 | Create Profiler web tab | 4 modified/created |
| 9 | Update CMakeLists.txt + build | 1 modified |
| 10 | Integration verification | 0 (testing only) |

**Total: 8 new files, 7 modified files, 10 tasks**
