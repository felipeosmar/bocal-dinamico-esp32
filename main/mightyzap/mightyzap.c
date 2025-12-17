#include "mightyzap.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MIGHTYZAP";

/**
 * @brief Internal mightyZAP structure
 */
struct mightyzap {
    modbus_handle_t modbus;
    uint8_t slave_id;
};

esp_err_t mightyzap_init(modbus_handle_t modbus, uint8_t slave_id, mightyzap_handle_t *handle)
{
    if (modbus == NULL || handle == NULL || slave_id == 0 || slave_id > 247) {
        return ESP_ERR_INVALID_ARG;
    }

    struct mightyzap *zap = calloc(1, sizeof(struct mightyzap));
    if (zap == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mightyZAP structure");
        return ESP_ERR_NO_MEM;
    }

    zap->modbus = modbus;
    zap->slave_id = slave_id;

    ESP_LOGI(TAG, "mightyZAP initialized, ID=%u", slave_id);

    *handle = zap;
    return ESP_OK;
}

esp_err_t mightyzap_deinit(mightyzap_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    free(handle);
    return ESP_OK;
}

esp_err_t mightyzap_get_model(mightyzap_handle_t handle, uint16_t *model)
{
    if (handle == NULL || model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                         MZAP_REG_MODEL_NUMBER, 1, model);
}

esp_err_t mightyzap_set_force_enable(mightyzap_handle_t handle, bool enable)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "ID=%u: Force %s", handle->slave_id, enable ? "ON" : "OFF");
    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_FORCE_ON_OFF, enable ? 1 : 0);
}

esp_err_t mightyzap_set_position(mightyzap_handle_t handle, uint16_t position)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "ID=%u: Set position=%u", handle->slave_id, position);
    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_GOAL_POSITION, position);
}

esp_err_t mightyzap_set_speed(mightyzap_handle_t handle, uint16_t speed)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "ID=%u: Set speed=%u", handle->slave_id, speed);
    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_GOAL_SPEED, speed);
}

esp_err_t mightyzap_set_current(mightyzap_handle_t handle, uint16_t current)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "ID=%u: Set current=%u", handle->slave_id, current);
    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_GOAL_CURRENT, current);
}

esp_err_t mightyzap_set_goal(mightyzap_handle_t handle, uint16_t position, uint16_t speed, uint16_t current)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "ID=%u: Set goal pos=%u, spd=%u, cur=%u",
             handle->slave_id, position, speed, current);

    // Write position first
    esp_err_t ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                                  MZAP_REG_GOAL_POSITION, position);
    if (ret != ESP_OK) return ret;

    ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_GOAL_SPEED, speed);
    if (ret != ESP_OK) return ret;

    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_GOAL_CURRENT, current);
}

esp_err_t mightyzap_get_position(mightyzap_handle_t handle, uint16_t *position)
{
    if (handle == NULL || position == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                         MZAP_REG_PRESENT_POSITION, 1, position);
}

esp_err_t mightyzap_get_status(mightyzap_handle_t handle, mightyzap_status_t *status)
{
    if (handle == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    // Read position
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_PRESENT_POSITION, 1, &status->position);
    if (ret != ESP_OK) return ret;

    // Read current
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_PRESENT_CURRENT, 1, &status->current);
    if (ret != ESP_OK) return ret;

    // Read voltage
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_PRESENT_VOLTAGE, 1, &status->voltage);
    if (ret != ESP_OK) return ret;

    // Read moving status
    uint16_t moving;
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_MOVING, 1, &moving);
    if (ret != ESP_OK) return ret;
    status->moving = moving & 0xFF;

    return ESP_OK;
}

esp_err_t mightyzap_is_moving(mightyzap_handle_t handle, bool *moving)
{
    if (handle == NULL || moving == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t value;
    esp_err_t ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                                  MZAP_REG_MOVING, 1, &value);
    if (ret != ESP_OK) return ret;

    *moving = (value != 0);
    return ESP_OK;
}

esp_err_t mightyzap_set_led(mightyzap_handle_t handle, uint8_t state)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_LED_ON_OFF, state);
}

esp_err_t mightyzap_set_id(mightyzap_handle_t handle, uint8_t new_id)
{
    if (handle == NULL || new_id == 0 || new_id > 247) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                                 MZAP_REG_ID, new_id);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ID changed from %u to %u (restart required)", handle->slave_id, new_id);
        handle->slave_id = new_id;
    }

    return ret;
}

esp_err_t mightyzap_restart(mightyzap_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "ID=%u: Restarting actuator", handle->slave_id);
    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_RESTART, 1);
}

esp_err_t mightyzap_factory_reset(mightyzap_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "ID=%u: Factory reset!", handle->slave_id);
    return modbus_write_single_register(handle->modbus, handle->slave_id,
                                        MZAP_REG_FACTORY_RESET, 1);
}
