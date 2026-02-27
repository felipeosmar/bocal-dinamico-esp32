/**
 * @file app_globals.h
 * @brief Centralized extern declarations for global handles and mutexes
 *
 * All global handles are defined in main.c. Include this header
 * instead of declaring individual externs in each file.
 */

#ifndef APP_GLOBALS_H
#define APP_GLOBALS_H

#include "rs485_driver.h"
#include "modbus_rtu.h"
#include "mightyzap.h"
#include "freertos/semphr.h"

// Primary RS485/Modbus bus
extern rs485_handle_t g_rs485;
extern modbus_handle_t g_modbus;
extern mightyzap_handle_t g_actuator;

// Secondary RS485/Modbus bus (sync actuators on UART2)
extern rs485_handle_t g_rs485_sync;
extern modbus_handle_t g_modbus_sync;

// Bus access mutexes — take before any bus operation from API handlers
extern SemaphoreHandle_t g_bus_mutex;
extern SemaphoreHandle_t g_bus_sync_mutex;

#endif // APP_GLOBALS_H
