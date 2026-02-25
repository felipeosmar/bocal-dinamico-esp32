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
    uint16_t speed_limit;    // Cached speed limit
    uint16_t current_limit;  // Cached current limit
    bool limits_cached;      // Flag to indicate if limits are cached
};

esp_err_t mightyzap_init(modbus_handle_t modbus, uint8_t slave_id, mightyzap_handle_t *handle)
{
    if (modbus == NULL || handle == NULL || slave_id == 0 || slave_id > MODBUS_MAX_SLAVE_ADDR) {
        return ESP_ERR_INVALID_ARG;
    }

    struct mightyzap *zap = calloc(1, sizeof(struct mightyzap));
    if (zap == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mightyZAP structure");
        return ESP_ERR_NO_MEM;
    }

    zap->modbus = modbus;
    zap->slave_id = slave_id;
    zap->speed_limit = MZAP_MAX_SPEED;     // Default max
    zap->current_limit = MZAP_MAX_CURRENT;  // Default max
    zap->limits_cached = false;

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

/**
 * @brief Cache speed and current limits from actuator (called once)
 */
static void cache_limits(mightyzap_handle_t handle)
{
    if (handle->limits_cached) return;

    // Try to read limits - use defaults if fails
    if (modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                      MZAP_REG_SPEED_LIMIT, 1, &handle->speed_limit) != ESP_OK) {
        handle->speed_limit = MZAP_MAX_SPEED;
    }
    if (modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                      MZAP_REG_CURRENT_LIMIT, 1, &handle->current_limit) != ESP_OK) {
        handle->current_limit = MZAP_MAX_CURRENT;
    }

    handle->limits_cached = true;
    ESP_LOGI(TAG, "ID=%u: Cached limits - speed=%u, current=%u",
             handle->slave_id, handle->speed_limit, handle->current_limit);
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

    // Use cached limit
    cache_limits(handle);

    uint16_t original_speed = speed;
    if (speed > handle->speed_limit) {
        speed = handle->speed_limit;
        ESP_LOGW(TAG, "ID=%u: Speed clamped %u->%u (actuator limit)",
                 handle->slave_id, original_speed, speed);
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

    // Use cached limit
    cache_limits(handle);

    uint16_t original_current = current;
    if (current > handle->current_limit) {
        current = handle->current_limit;
        ESP_LOGW(TAG, "ID=%u: Current clamped %u->%u (actuator limit)",
                 handle->slave_id, original_current, current);
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

    // Use cached limits
    cache_limits(handle);

    // Clamp speed and current to their limits
    uint16_t original_speed = speed;
    uint16_t original_current = current;

    if (speed > handle->speed_limit) {
        speed = handle->speed_limit;
        ESP_LOGW(TAG, "ID=%u: Speed clamped %u->%u (actuator limit)",
                 handle->slave_id, original_speed, speed);
    }
    if (current > handle->current_limit) {
        current = handle->current_limit;
        ESP_LOGW(TAG, "ID=%u: Current clamped %u->%u (actuator limit)",
                 handle->slave_id, original_current, current);
    }

    ESP_LOGD(TAG, "ID=%u: Set goal pos=%u, spd=%u, cur=%u",
             handle->slave_id, position, speed, current);

    // Write registers individually (some actuators don't support FC16 write_multiple)
    // Order: speed first, then current, then position (position triggers movement)
    esp_err_t ret;
    
    ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                       MZAP_REG_GOAL_SPEED, speed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ID=%u: Failed to set speed", handle->slave_id);
        return ret;
    }
    
    ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                       MZAP_REG_GOAL_CURRENT, current);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ID=%u: Failed to set current", handle->slave_id);
        return ret;
    }
    
    // Position last - this triggers the movement
    ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                       MZAP_REG_GOAL_POSITION, position);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ID=%u: Failed to set position", handle->slave_id);
        return ret;
    }
    
    return ESP_OK;
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

    // Read all status registers in a single transaction (5 consecutive registers)
    // 0x0037: Position, 0x0038: Current, 0x0039: Motor Op, 0x003A: Voltage, 0x003B: Moving
    uint16_t regs[5];
    esp_err_t ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                                   MZAP_REG_PRESENT_POSITION, 5, regs);
    if (ret != ESP_OK) return ret;

    status->position = regs[0];  // 0x0037
    status->current = regs[1];   // 0x0038
    // regs[2] is Motor Operating Rate (0x0039) - not used in status struct
    status->voltage = regs[3];   // 0x003A
    status->moving = regs[4] & 0xFF;  // 0x003B

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
    if (handle == NULL || new_id == 0 || new_id > MODBUS_MAX_SLAVE_ADDR) {
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

esp_err_t mightyzap_get_info(mightyzap_handle_t handle, mightyzap_info_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    
    // Read model number
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_MODEL_NUMBER, 1, &info->model);
    if (ret != ESP_OK) return ret;
    
    // Read firmware version
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_FIRMWARE_VERSION, 1, &info->firmware);
    if (ret != ESP_OK) return ret;
    
    // Read voltage limits
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_LOWEST_VOLTAGE, 1, &info->voltage_min);
    if (ret != ESP_OK) return ret;
    
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_HIGHEST_VOLTAGE, 1, &info->voltage_max);
    if (ret != ESP_OK) return ret;
    
    ESP_LOGD(TAG, "ID=%u: Info - model=%u, fw=%u, voltage=%u-%u",
             handle->slave_id, info->model, info->firmware, 
             info->voltage_min, info->voltage_max);
    
    return ESP_OK;
}

esp_err_t mightyzap_get_config(mightyzap_handle_t handle, mightyzap_config_t *config)
{
    if (handle == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    uint16_t val;
    
    // Read slave ID
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_ID, 1, &val);
    if (ret != ESP_OK) return ret;
    config->slave_id = val;
    
    // Read baud rate
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_BAUD_RATE, 1, &val);
    if (ret != ESP_OK) return ret;
    config->baud_rate = val;
    
    // Read stroke limits
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_SHORT_STROKE_LIM, 1, &config->short_stroke_limit);
    if (ret != ESP_OK) return ret;
    
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_LONG_STROKE_LIM, 1, &config->long_stroke_limit);
    if (ret != ESP_OK) return ret;
    
    // Read performance limits
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_SPEED_LIMIT, 1, &config->speed_limit);
    if (ret != ESP_OK) return ret;
    
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_CURRENT_LIMIT, 1, &config->current_limit);
    if (ret != ESP_OK) return ret;
    
    // Read compliance margins
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_START_COMPLIANCE, 1, &val);
    if (ret != ESP_OK) return ret;
    config->start_compliance = val;
    
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_END_COMPLIANCE, 1, &val);
    if (ret != ESP_OK) return ret;
    config->end_compliance = val;
    
    // Read alarm settings
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_ALARM_LED, 1, &val);
    if (ret != ESP_OK) return ret;
    config->alarm_led = val;
    
    ret = modbus_read_holding_registers(handle->modbus, handle->slave_id,
                                        MZAP_REG_ALARM_SHUTDOWN, 1, &val);
    if (ret != ESP_OK) return ret;
    config->alarm_shutdown = val;
    
    ESP_LOGI(TAG, "ID=%u: Config loaded - speed_lim=%u, current_lim=%u",
             handle->slave_id, config->speed_limit, config->current_limit);
    
    return ESP_OK;
}

esp_err_t mightyzap_set_config(mightyzap_handle_t handle, const mightyzap_config_t *config, uint16_t mask)
{
    if (handle == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    
    ESP_LOGI(TAG, "ID=%u: Setting config (mask=0x%04X)", handle->slave_id, mask);
    
    if (mask & MZAP_CONFIG_SLAVE_ID) {
        if (config->slave_id == 0 || config->slave_id > MODBUS_MAX_SLAVE_ADDR) {
            ESP_LOGE(TAG, "Invalid slave ID: %u", config->slave_id);
            return ESP_ERR_INVALID_ARG;
        }
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_ID, config->slave_id);
        if (ret != ESP_OK) return ret;
        ESP_LOGW(TAG, "Slave ID changed to %u - restart required", config->slave_id);
    }
    
    if (mask & MZAP_CONFIG_BAUD_RATE) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_BAUD_RATE, config->baud_rate);
        if (ret != ESP_OK) return ret;
    }
    
    if (mask & MZAP_CONFIG_SHORT_STROKE) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_SHORT_STROKE_LIM, config->short_stroke_limit);
        if (ret != ESP_OK) return ret;
    }
    
    if (mask & MZAP_CONFIG_LONG_STROKE) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_LONG_STROKE_LIM, config->long_stroke_limit);
        if (ret != ESP_OK) return ret;
    }
    
    if (mask & MZAP_CONFIG_SPEED_LIMIT) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_SPEED_LIMIT, config->speed_limit);
        if (ret != ESP_OK) return ret;
        // Update cached value
        handle->speed_limit = config->speed_limit;
        handle->limits_cached = true;
    }
    
    if (mask & MZAP_CONFIG_CURRENT_LIMIT) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_CURRENT_LIMIT, config->current_limit);
        if (ret != ESP_OK) return ret;
        // Update cached value
        handle->current_limit = config->current_limit;
        handle->limits_cached = true;
    }
    
    if (mask & MZAP_CONFIG_START_COMPLIANCE) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_START_COMPLIANCE, config->start_compliance);
        if (ret != ESP_OK) return ret;
    }
    
    if (mask & MZAP_CONFIG_END_COMPLIANCE) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_END_COMPLIANCE, config->end_compliance);
        if (ret != ESP_OK) return ret;
    }
    
    if (mask & MZAP_CONFIG_ALARM_LED) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_ALARM_LED, config->alarm_led);
        if (ret != ESP_OK) return ret;
    }
    
    if (mask & MZAP_CONFIG_ALARM_SHUTDOWN) {
        ret = modbus_write_single_register(handle->modbus, handle->slave_id,
                                           MZAP_REG_ALARM_SHUTDOWN, config->alarm_shutdown);
        if (ret != ESP_OK) return ret;
    }
    
    ESP_LOGI(TAG, "ID=%u: Config saved to EEPROM", handle->slave_id);
    return ESP_OK;
}

// ============================================================================
// Synchronized Movement Implementation
// ============================================================================

esp_err_t mightyzap_sync_init(mightyzap_sync_group_t *group, modbus_handle_t modbus,
                               const uint8_t *ids, uint8_t count)
{
    if (group == NULL || modbus == NULL || ids == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (count == 0 || count > MZAP_SYNC_MAX_ACTUATORS) {
        ESP_LOGE(TAG, "Sync group count must be 1-%d, got %d", MZAP_SYNC_MAX_ACTUATORS, count);
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(group, 0, sizeof(mightyzap_sync_group_t));
    group->modbus = modbus;
    group->count = count;
    
    for (uint8_t i = 0; i < count; i++) {
        if (ids[i] == 0 || ids[i] > MODBUS_MAX_SLAVE_ADDR) {
            ESP_LOGE(TAG, "Invalid actuator ID: %d", ids[i]);
            return ESP_ERR_INVALID_ARG;
        }
        group->ids[i] = ids[i];
    }
    
    // Set defaults
    group->speed = 300;
    group->current = 400;
    group->tolerance = MZAP_SYNC_DEFAULT_TOLERANCE;
    group->max_desync = 150;  // ~1mm for 27mm stroke (tolerance for final position)
    
    ESP_LOGI(TAG, "Sync group initialized with %d actuators: [%d, %d%s%s]",
             count, ids[0], count > 1 ? ids[1] : 0,
             count > 2 ? ", ..." : "", count > 2 ? "" : "");
    
    return ESP_OK;
}

void mightyzap_sync_set_params(mightyzap_sync_group_t *group, uint16_t speed,
                                uint16_t current, uint16_t tolerance, uint16_t max_desync)
{
    if (group == NULL) return;
    
    group->speed = speed;
    group->current = current;
    group->tolerance = tolerance > 0 ? tolerance : MZAP_SYNC_DEFAULT_TOLERANCE;
    group->max_desync = max_desync > 0 ? max_desync : 50;
    
    ESP_LOGD(TAG, "Sync params: speed=%d, current=%d, tolerance=%d, max_desync=%d",
             speed, current, tolerance, max_desync);
}

esp_err_t mightyzap_sync_move_start(mightyzap_sync_group_t *group, uint16_t position)
{
    if (group == NULL || group->modbus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    group->target_position = position;
    esp_err_t ret;
    esp_err_t first_error = ESP_OK;
    
    ESP_LOGI(TAG, "Sync move start: position=%d, speed=%d, current=%d",
             position, group->speed, group->current);
    
    // Send goal to all actuators as fast as possible
    // Using individual FC06 writes for each parameter
    for (uint8_t i = 0; i < group->count; i++) {
        uint8_t id = group->ids[i];
        
        // Write speed
        ret = modbus_write_single_register(group->modbus, id, MZAP_REG_GOAL_SPEED, group->speed);
        if (ret != ESP_OK && first_error == ESP_OK) first_error = ret;
        
        // Write current
        ret = modbus_write_single_register(group->modbus, id, MZAP_REG_GOAL_CURRENT, group->current);
        if (ret != ESP_OK && first_error == ESP_OK) first_error = ret;
        
        // Write position (this triggers movement)
        ret = modbus_write_single_register(group->modbus, id, MZAP_REG_GOAL_POSITION, position);
        if (ret != ESP_OK && first_error == ESP_OK) first_error = ret;
        
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Sync move: actuator ID=%d command failed: %s", id, esp_err_to_name(ret));
        }
    }
    
    return first_error;
}

esp_err_t mightyzap_sync_check_position(mightyzap_sync_group_t *group,
                                         bool *in_position, bool *all_stopped)
{
    if (group == NULL || group->modbus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    bool all_in_pos = true;
    bool all_stop = true;
    esp_err_t ret;
    
    for (uint8_t i = 0; i < group->count; i++) {
        uint8_t id = group->ids[i];
        uint16_t pos = 0;
        uint16_t moving = 0;
        
        // Read current position
        ret = modbus_read_holding_registers(group->modbus, id, MZAP_REG_PRESENT_POSITION, 1, &pos);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Sync check: failed to read position from ID=%d", id);
            return ret;
        }
        group->present_positions[i] = pos;
        
        // Read moving status
        ret = modbus_read_holding_registers(group->modbus, id, MZAP_REG_MOVING, 1, &moving);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Sync check: failed to read moving status from ID=%d", id);
            return ret;
        }
        
        // Check if in tolerance
        int32_t diff = (int32_t)pos - (int32_t)group->target_position;
        if (diff < 0) diff = -diff;
        
        if (diff > group->tolerance) {
            all_in_pos = false;
        }
        
        if (moving != 0) {
            all_stop = false;
        }
    }
    
    if (in_position) *in_position = all_in_pos;
    if (all_stopped) *all_stopped = all_stop;
    
    return ESP_OK;
}

esp_err_t mightyzap_sync_get_desync(mightyzap_sync_group_t *group, uint16_t *max_diff)
{
    if (group == NULL || max_diff == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (group->count < 2) {
        *max_diff = 0;
        return ESP_OK;
    }
    
    uint16_t max_pos = 0;
    uint16_t min_pos = 0xFFFF;
    
    for (uint8_t i = 0; i < group->count; i++) {
        uint16_t pos = group->present_positions[i];
        if (pos > max_pos) max_pos = pos;
        if (pos < min_pos) min_pos = pos;
    }
    
    *max_diff = max_pos - min_pos;
    return ESP_OK;
}

mightyzap_sync_result_t mightyzap_sync_move_wait(mightyzap_sync_group_t *group,
                                                  uint16_t position, uint32_t timeout_ms)
{
    if (group == NULL || group->modbus == NULL) {
        return MZAP_SYNC_INVALID_PARAM;
    }
    
    ESP_LOGI(TAG, "Sync move wait: target=%d, timeout=%lums", position, (unsigned long)timeout_ms);
    
    // Start movement
    esp_err_t ret = mightyzap_sync_move_start(group, position);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sync move start failed: %s", esp_err_to_name(ret));
        return MZAP_SYNC_COMM_ERROR;
    }
    
    // Wait for completion
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t poll_interval = 50;  // 50ms between polls
    
    while (1) {
        // Check timeout
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start_time;
        if (elapsed > timeout_ms) {
            ESP_LOGW(TAG, "Sync move timeout after %lums", (unsigned long)elapsed);
            return MZAP_SYNC_TIMEOUT;
        }
        
        // Check positions
        bool in_position = false;
        bool all_stopped = false;
        ret = mightyzap_sync_check_position(group, &in_position, &all_stopped);
        if (ret != ESP_OK) {
            return MZAP_SYNC_COMM_ERROR;
        }
        
        // Get current desync
        uint16_t desync = 0;
        mightyzap_sync_get_desync(group, &desync);
        
        // Success condition: all in position
        if (in_position) {
            ESP_LOGI(TAG, "Sync move complete: desync=%d, elapsed=%lums", desync, (unsigned long)elapsed);
            return MZAP_SYNC_OK;
        }
        
        // Check when all stopped
        if (all_stopped) {
            // Check final desync - only error if stopped AND desync too high
            if (desync > group->max_desync) {
                ESP_LOGE(TAG, "Sync desync at stop: %d > %d", desync, group->max_desync);
                for (uint8_t i = 0; i < group->count; i++) {
                    ESP_LOGE(TAG, "  ID=%d: pos=%d", group->ids[i], group->present_positions[i]);
                }
                return MZAP_SYNC_DESYNC;
            }
            
            // Check if we're close enough to target
            bool close_enough = true;
            for (uint8_t i = 0; i < group->count; i++) {
                int32_t diff = (int32_t)group->present_positions[i] - (int32_t)position;
                if (diff < 0) diff = -diff;
                if (diff > group->tolerance * 2) {
                    close_enough = false;
                    break;
                }
            }
            if (close_enough) {
                ESP_LOGI(TAG, "Sync move complete (stopped): desync=%d, elapsed=%lums", desync, (unsigned long)elapsed);
                return MZAP_SYNC_OK;
            }
        }
        
        // Log desync during movement (debug level, not error)
        if (desync > group->max_desync) {
            ESP_LOGD(TAG, "Sync moving desync: %d (will check at stop)", desync);
        }
        
        vTaskDelay(pdMS_TO_TICKS(poll_interval));
    }
}

esp_err_t mightyzap_sync_stop(mightyzap_sync_group_t *group)
{
    if (group == NULL || group->modbus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGW(TAG, "Sync emergency stop");
    return mightyzap_sync_set_force(group, false);
}

esp_err_t mightyzap_sync_set_force(mightyzap_sync_group_t *group, bool enable)
{
    if (group == NULL || group->modbus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    esp_err_t first_error = ESP_OK;
    
    for (uint8_t i = 0; i < group->count; i++) {
        ret = modbus_write_single_register(group->modbus, group->ids[i],
                                           MZAP_REG_FORCE_ON_OFF, enable ? 1 : 0);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    
    ESP_LOGI(TAG, "Sync force %s for %d actuators", enable ? "enabled" : "disabled", group->count);
    return first_error;
}
