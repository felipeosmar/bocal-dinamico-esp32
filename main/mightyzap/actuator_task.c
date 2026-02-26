#include "actuator_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

extern SemaphoreHandle_t g_bus_mutex;

static const char *TAG = "ACT_TASK";

#define ACTUATOR_TASK_STACK_SIZE 4096
#define ACTUATOR_TASK_PRIORITY 4
#define ACTUATOR_QUEUE_LENGTH 10

static QueueHandle_t s_actuator_queue = NULL;
static TaskHandle_t s_actuator_task_handle = NULL;

static void actuator_task_loop(void *arg) {
  ESP_LOGI(TAG, "Dedicated Actuator thread started");
  actuator_cmd_t cmd;

  while (1) {
    if (xQueueReceive(s_actuator_queue, &cmd, portMAX_DELAY) == pdTRUE) {
      if (cmd.handle == NULL) {
        ESP_LOGE(TAG, "Received command with NULL handle!");
        continue;
      }

      esp_err_t ret = ESP_OK;

      xSemaphoreTake(g_bus_mutex, portMAX_DELAY);

      switch (cmd.type) {
      case ACT_CMD_MOVE:
        ret = mightyzap_set_position(cmd.handle, cmd.value);
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Failed move async to %u: %s", cmd.value,
                   esp_err_to_name(ret));
        } else {
          ESP_LOGI(TAG, "Async move to %u successful", cmd.value);
        }
        break;
      case ACT_CMD_STOP:
        ret = mightyzap_set_force_enable(cmd.handle, false);
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Failed stop async: %s", esp_err_to_name(ret));
        }
        break;
      default:
        ESP_LOGW(TAG, "Unknown async command type %d", cmd.type);
        break;
      }

      xSemaphoreGive(g_bus_mutex);
    }
  }
}

esp_err_t actuator_task_init(void) {
  if (s_actuator_queue != NULL) {
    return ESP_OK; // Already initialized
  }

  s_actuator_queue =
      xQueueCreate(ACTUATOR_QUEUE_LENGTH, sizeof(actuator_cmd_t));
  if (s_actuator_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create actuator queue");
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ret = xTaskCreatePinnedToCore(
      actuator_task_loop, "actuator_worker", ACTUATOR_TASK_STACK_SIZE, NULL,
      ACTUATOR_TASK_PRIORITY, &s_actuator_task_handle,
      1 // Run on core 1
  );

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create actuator_worker task");
    vQueueDelete(s_actuator_queue);
    s_actuator_queue = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t actuator_move_async(mightyzap_handle_t handle, uint16_t position) {
  if (s_actuator_queue == NULL || handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  actuator_cmd_t cmd = {
      .type = ACT_CMD_MOVE, .handle = handle, .value = position};

  // 0 wait block time ensures we fail immediately instead of blocking
  // HTTP/control loops
  if (xQueueSend(s_actuator_queue, &cmd, 0) != pdPASS) {
    ESP_LOGW(TAG, "Queue full, move command dropped");
    return ESP_ERR_TIMEOUT;
  }

  return ESP_OK;
}

esp_err_t actuator_stop_async(mightyzap_handle_t handle) {
  if (s_actuator_queue == NULL || handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  actuator_cmd_t cmd = {.type = ACT_CMD_STOP, .handle = handle, .value = 0};

  if (xQueueSend(s_actuator_queue, &cmd, 0) != pdPASS) {
    ESP_LOGW(TAG, "Queue full, stop command dropped");
    return ESP_ERR_TIMEOUT;
  }

  return ESP_OK;
}
