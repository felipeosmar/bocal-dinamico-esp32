#include "control_loop.h"
#include "actuator_task.h"
#include "app_globals.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "CONTROL";

#define CONTROL_TASK_STACK_SIZE 4096
#define CONTROL_TASK_PRIORITY 5

// Defined in api_actuator.c
extern mightyzap_handle_t *actuator_get_handle_by_id(uint8_t id);

static struct {
  baumer_handle_t baumer;
  control_config_t config;
  control_status_t status;
  SemaphoreHandle_t mutex;
  TaskHandle_t task;
  uint8_t consecutive_errors;
} s_ctrl = {0};

static uint16_t clamp_position(float value) {
  if (value < 0.0f)
    return 0;
  if (value > 4095.0f)
    return 4095;
  return (uint16_t)(value + 0.5f); // Round to nearest
}

static uint16_t s_last_sent_position[CONTROL_MAX_EQUATIONS];

static void control_task(void *arg) {
  ESP_LOGI(TAG, "Control loop task started");
  memset(s_last_sent_position, 0xFF, sizeof(s_last_sent_position));

  while (1) {
    // Wait for configured interval
    uint32_t interval;
    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
    interval = s_ctrl.config.interval_ms;
    xSemaphoreGive(s_ctrl.mutex);

    vTaskDelay(pdMS_TO_TICKS(interval));

    // Always read sensor for monitoring (take bus 1 mutex to avoid conflict)
    baumer_measurement_t m;
    xSemaphoreTake(g_bus_mutex, portMAX_DELAY);
    esp_err_t ret = baumer_read_measurements(s_ctrl.baumer, &m);
    xSemaphoreGive(g_bus_mutex);

    xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);

    if (ret != ESP_OK) {
      s_ctrl.status.error_count++;
      s_ctrl.consecutive_errors++;
      ESP_LOGW(TAG, "Read error (%u consecutive)", s_ctrl.consecutive_errors);

      // Auto-stop after too many consecutive errors
      if (s_ctrl.config.running &&
          s_ctrl.consecutive_errors >= CONTROL_MAX_CONSECUTIVE_ERRORS) {
        s_ctrl.config.running = false;
        ESP_LOGE(TAG, "Stopped: %d consecutive errors",
                 CONTROL_MAX_CONSECUTIVE_ERRORS);
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
    if (meas_idx >= BAUMER_NUM_VALUES)
      meas_idx = 0;
    float gap = m.values[meas_idx];
    s_ctrl.status.last_gap_value = gap;

    // Validate float value
    if (isnan(gap) || isinf(gap)) {
      ESP_LOGW(TAG, "Invalid float from Baumer (NaN/Inf), skipping");
      xSemaphoreGive(s_ctrl.mutex);
      continue;
    }

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
        if (!cfg.equations[i].enabled)
          continue;

        float pos_f = cfg.equations[i].coeff_a * gap + cfg.equations[i].coeff_b;
        uint16_t position = clamp_position(pos_f);

        // Dedup: skip if position unchanged
        if (position != s_last_sent_position[i]) {
          uint8_t bus = cfg.equations[i].bus;
          if (bus == 2) {
            // Bus 2: broadcast to all sync actuators
            esp_err_t move_ret = actuator_move_sync_async(g_modbus_sync, position);
            if (move_ret != ESP_OK) {
              ESP_LOGW(TAG, "Failed to enqueue sync broadcast: %s",
                       esp_err_to_name(move_ret));
            }
          } else {
            // Bus 1 (default): individual actuator move
            mightyzap_handle_t *h =
                actuator_get_handle_by_id(cfg.equations[i].actuator_id);
            if (h != NULL) {
              esp_err_t move_ret = actuator_move_async(*h, position);
              if (move_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to enqueue actuator %u: %s",
                         cfg.equations[i].actuator_id, esp_err_to_name(move_ret));
              }
            }
          }
          s_last_sent_position[i] = position;
        }

        // Update computed positions in status
        xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
        if (computed_count < CONTROL_MAX_EQUATIONS) {
          s_ctrl.status.computed_positions[computed_count] = position;
          s_ctrl.status.computed_actuator_ids[computed_count] =
              cfg.equations[i].actuator_id;
          computed_count++;
        }
        s_ctrl.status.computed_count = computed_count;
        xSemaphoreGive(s_ctrl.mutex);
      }

      ESP_LOGD(TAG, "Loop #%lu: gap=%.3f -> %u actuators updated",
               (unsigned long)cfg.equation_count, gap, computed_count);
    } else if (should_control && !signal_ok) {
      ESP_LOGW(TAG, "Quality=%u (no signal), skipping actuator update",
               m.quality);
    } else if (should_control && !values_valid) {
      ESP_LOGW(TAG, "Values invalid (status=0x%04X), skipping actuator update",
               m.status);
    }
  }
}

esp_err_t control_loop_init(baumer_handle_t baumer,
                            const control_config_t *config) {
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

  // Don't auto-start running here - that's done separately
  s_ctrl.config.running = false;

  BaseType_t ret = xTaskCreatePinnedToCore(
      control_task, "ctrl_loop", CONTROL_TASK_STACK_SIZE, NULL,
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

esp_err_t control_loop_start(void) {
  if (s_ctrl.mutex == NULL)
    return ESP_ERR_INVALID_STATE;

  xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
  s_ctrl.config.running = true;
  s_ctrl.consecutive_errors = 0;
  xSemaphoreGive(s_ctrl.mutex);

  s_ctrl.status.running = true;
  ESP_LOGI(TAG, "Control loop started");
  return ESP_OK;
}

esp_err_t control_loop_stop(void) {
  if (s_ctrl.mutex == NULL)
    return ESP_ERR_INVALID_STATE;

  xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
  s_ctrl.config.running = false;
  xSemaphoreGive(s_ctrl.mutex);

  s_ctrl.status.running = false;
  ESP_LOGI(TAG, "Control loop stopped");
  return ESP_OK;
}

esp_err_t control_loop_set_interval(uint32_t interval_ms) {
  if (s_ctrl.mutex == NULL)
    return ESP_ERR_INVALID_STATE;
  if (interval_ms < CONTROL_MIN_INTERVAL_MS ||
      interval_ms > CONTROL_MAX_INTERVAL_MS) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
  s_ctrl.config.interval_ms = interval_ms;
  xSemaphoreGive(s_ctrl.mutex);

  ESP_LOGI(TAG, "Interval set to %lu ms", interval_ms);
  return ESP_OK;
}

esp_err_t control_loop_set_equation(uint8_t actuator_id, float a, float b,
                                    bool enabled, uint8_t bus) {
  if (s_ctrl.mutex == NULL)
    return ESP_ERR_INVALID_STATE;
  if (bus == 0)
    bus = 1; // Default to bus 1

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
    s_ctrl.config.equations[found].bus = bus;
  } else if (s_ctrl.config.equation_count < CONTROL_MAX_EQUATIONS) {
    uint8_t idx = s_ctrl.config.equation_count;
    s_ctrl.config.equations[idx].actuator_id = actuator_id;
    s_ctrl.config.equations[idx].coeff_a = a;
    s_ctrl.config.equations[idx].coeff_b = b;
    s_ctrl.config.equations[idx].enabled = enabled;
    s_ctrl.config.equations[idx].bus = bus;
    s_ctrl.config.equation_count++;
  } else {
    xSemaphoreGive(s_ctrl.mutex);
    return ESP_ERR_NO_MEM;
  }

  xSemaphoreGive(s_ctrl.mutex);

  ESP_LOGI(TAG, "Equation for actuator %u (bus %u): pos = %.3f * gap + %.3f (%s)",
           actuator_id, bus, a, b, enabled ? "enabled" : "disabled");
  return ESP_OK;
}

esp_err_t control_loop_get_config(control_config_t *out) {
  if (out == NULL)
    return ESP_ERR_INVALID_ARG;
  if (s_ctrl.mutex == NULL) {
    memset(out, 0, sizeof(*out));
    return ESP_OK;
  }

  xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
  *out = s_ctrl.config;
  xSemaphoreGive(s_ctrl.mutex);

  return ESP_OK;
}

esp_err_t control_loop_get_status(control_status_t *out) {
  if (out == NULL)
    return ESP_ERR_INVALID_ARG;
  if (s_ctrl.mutex == NULL) {
    memset(out, 0, sizeof(*out));
    return ESP_OK;
  }

  xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
  *out = s_ctrl.status;
  out->running = s_ctrl.config.running;
  xSemaphoreGive(s_ctrl.mutex);

  return ESP_OK;
}

esp_err_t control_loop_set_measurement_index(uint8_t index) {
  if (s_ctrl.mutex == NULL)
    return ESP_ERR_INVALID_STATE;
  if (index >= BAUMER_NUM_VALUES) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(s_ctrl.mutex, portMAX_DELAY);
  s_ctrl.config.measurement_index = index;
  xSemaphoreGive(s_ctrl.mutex);

  ESP_LOGI(TAG, "Measurement index set to %u", index);
  return ESP_OK;
}
