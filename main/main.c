/**
 * @file main.c
 * @brief ESP32 Master - RS485 Modbus RTU with Web Interface
 *
 * This application provides:
 * - RS485 communication with ESP32 Slave (LED control)
 * - RS485 communication with mightyZAP actuators
 * - Web interface for configuration and control
 * - WiFi AP/STA mode
 * - JSON-based configuration storage
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"

#include "rs485_driver.h"
#include "modbus_rtu.h"
#include "mightyzap.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "config_manager.h"
#include "health_monitor.h"

static const char *TAG = "MASTER";

// Global handles (accessible from web_server.c)
rs485_handle_t g_rs485 = NULL;
modbus_handle_t g_modbus = NULL;
mightyzap_handle_t g_actuator = NULL;

// Actuator configuration
#define ACTUATOR_SLAVE_ID   1   // mightyZAP default ID

// Remote ESP32 Slave register addresses (slave ID from config)
#define REG_LED_STATE       0x0000
#define REG_BLINK_MODE      0x0001
#define REG_BLINK_PERIOD    0x0002

/**
 * @brief Initialize RS485 and Modbus using config
 */
static esp_err_t init_communication(void)
{
    esp_err_t ret;

    // Get configuration
    uint32_t baud = config_get_rs485_baud();
    uint8_t tx_pin = config_get_rs485_tx_pin();
    uint8_t rx_pin = config_get_rs485_rx_pin();
    uint8_t de_pin = config_get_rs485_de_pin();
    uint32_t timeout = config_get_modbus_timeout();

    ESP_LOGI(TAG, "RS485 Config: TX=%d, RX=%d, DE=%d, Baud=%lu",
             tx_pin, rx_pin, de_pin, baud);

    // Initialize RS485
    rs485_config_t rs485_cfg = {
        .uart_num = UART_NUM_1,
        .tx_pin = tx_pin,
        .rx_pin = rx_pin,
        .de_pin = de_pin,
        .baud_rate = baud,
        .rx_buffer_size = 256,
        .tx_buffer_size = 256,
    };

    ret = rs485_init(&rs485_cfg, &g_rs485);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize RS485: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize Modbus Master
    modbus_config_t modbus_cfg = {
        .rs485 = g_rs485,
        .response_timeout = timeout,
    };

    ret = modbus_init(&modbus_cfg, &g_modbus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Modbus: %s", esp_err_to_name(ret));
        rs485_deinit(g_rs485);
        g_rs485 = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "RS485/Modbus communication initialized");

    // Initialize mightyZAP actuator
    esp_err_t act_ret = mightyzap_init(g_modbus, ACTUATOR_SLAVE_ID, &g_actuator);
    if (act_ret == ESP_OK) {
        ESP_LOGI(TAG, "mightyZAP actuator initialized (ID=%d)", ACTUATOR_SLAVE_ID);
    } else {
        ESP_LOGW(TAG, "mightyZAP init failed (may not be connected)");
        g_actuator = NULL;
    }

    return ESP_OK;
}

/**
 * @brief Initialize WiFi based on configuration
 */
static esp_err_t init_wifi(void)
{
    esp_err_t ret;

    // Initialize WiFi manager
    ret = wifi_manager_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check if we have saved WiFi credentials
    const char *ssid = config_get_wifi_ssid();
    const char *password = config_get_wifi_password();
    bool ap_mode = config_get_wifi_ap_mode();

    if (!ap_mode && strlen(ssid) > 0) {
        // Try to connect to saved network
        ESP_LOGI(TAG, "Connecting to saved network: %s", ssid);
        ret = wifi_manager_connect(ssid, password);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to connect, starting AP mode");
            ap_mode = true;
        }
    } else {
        ap_mode = true;
    }

    if (ap_mode) {
        // Start AP mode for configuration
        const char *ap_ssid = config_get_ap_ssid();
        const char *ap_pass = config_get_ap_password();
        ESP_LOGI(TAG, "Starting AP mode: %s", ap_ssid);
        ret = wifi_manager_start_ap(ap_ssid, ap_pass);
    }

    return ret;
}

/**
 * @brief Test connection to ESP32 Slave
 */
static esp_err_t test_slave_connection(void)
{
    if (g_modbus == NULL) return ESP_ERR_INVALID_STATE;

    uint16_t led_state;
    esp_err_t ret = modbus_read_holding_registers(g_modbus, config_get_modbus_slave_id(),
                                                   REG_LED_STATE, 1, &led_state);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Slave connected, LED state: %u", led_state);
    } else {
        ESP_LOGD(TAG, "Slave not responding");
    }
    return ret;
}

/**
 * @brief Modbus polling task - periodically checks slave status
 */
static void modbus_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Modbus polling task started");

    // Wait for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        // Test slave connection periodically
        test_slave_connection();

        // Poll every 5 seconds
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  ESP32 Master - RS485 + Web Interface");
    ESP_LOGI(TAG, "==========================================");

    // Initialize configuration manager (includes SPIFFS)
    ESP_LOGI(TAG, "Initializing configuration...");
    if (config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize configuration!");
        return;
    }

    // Initialize RS485/Modbus communication
    ESP_LOGI(TAG, "Initializing RS485/Modbus...");
    if (init_communication() != ESP_OK) {
        ESP_LOGW(TAG, "RS485 init failed, web interface will still work");
    }

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    if (init_wifi() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi!");
        return;
    }

    // Wait for WiFi to be ready
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Start web server
    ESP_LOGI(TAG, "Starting web server...");
    web_server_config_t web_cfg = {
        .port = 80,
        .username = config_get_web_username(),
        .password = config_get_web_password(),
        .auth_enabled = config_get_web_auth_enabled(),
    };

    if (web_server_init(&web_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server!");
        return;
    }

    // Print access information
    char ip[16];
    if (wifi_manager_get_ip(ip) == ESP_OK) {
        ESP_LOGI(TAG, "==========================================");
        ESP_LOGI(TAG, "  Web Interface: http://%s", ip);
        ESP_LOGI(TAG, "==========================================");
    }

    // Create Modbus polling task
    if (g_modbus != NULL) {
        BaseType_t ret = xTaskCreate(
            modbus_task,
            "modbus_poll",
            4096,
            NULL,
            5,
            NULL
        );

        if (ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to create Modbus polling task");
        }
    }

    // Initialize health monitor (starts monitoring task)
    ESP_LOGI(TAG, "Starting health monitor...");
    if (health_monitor_init() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start health monitor");
    }

    ESP_LOGI(TAG, "System ready!");
}
