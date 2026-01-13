# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based industrial control system for mightyZAP actuators via RS485/Modbus RTU with a web interface. Designed for 24/7 operation with robust error handling, health monitoring, and remote configuration.

## Build System

This project uses ESP-IDF (Espressif IoT Development Framework) with CMake.

### Common Commands

```bash
# Full build
idf.py build

# Build and flash
idf.py -p /dev/ttyUSB0 flash

# Monitor serial output
idf.py -p /dev/ttyUSB0 monitor

# Build, flash, and monitor in one command
idf.py -p /dev/ttyUSB0 flash monitor

# Clean build
idf.py fullclean

# Configure project (menuconfig)
idf.py menuconfig
```

### Flash Script (Recommended)

The `flash.sh` script provides intelligent flashing options that preserve user configuration:

```bash
# Normal update (preserves userdata partition with config.json)
./flash.sh update

# Flash only firmware (fastest, keeps www and userdata)
./flash.sh app

# Flash only web interface
./flash.sh www

# Factory reset (erases all user config)
./flash.sh all

# Use custom port
PORT=/dev/ttyACM0 ./flash.sh update
```

**Important**: Use `./flash.sh update` for production updates to preserve user configuration stored in the `userdata` partition.

## Architecture

### Partition Layout

The custom partition table (`partitions.csv`) defines:
- `factory` (0x1B0000): Main application firmware
- `www` (0x20000): Web interface files (LittleFS), safe to update with firmware
- `userdata` (0x10000): User configuration (LittleFS), preserved across updates
- `coredump` (0x10000): Post-mortem debugging data

### Component Structure

The codebase is organized into independent modules in `main/`:

**Communication Stack** (bottom-up):
- `rs485/` - Low-level UART driver with DE/RE pin control for MAX485 transceiver
- `modbus/` - Modbus RTU master implementation (function codes 0x03, 0x04, 0x06)
- `mightyzap/` - High-level actuator control API wrapping Modbus registers

**Configuration & Storage**:
- `config/` - JSON-based configuration manager using LittleFS on `userdata` partition
  - `config.json` stores WiFi, RS485, Modbus, and web auth settings
  - Auto-formats partition on first boot, preserved across firmware updates

**Networking**:
- `wifi/` - WiFi manager supporting both STA (station) and AP (access point) modes
- `webserver/` - HTTP server with Basic Auth serving static files from `www` partition

**System Services**:
- `health/` - FreeRTOS task monitoring watchdog timeouts and stack usage

### Global Handles

Three critical handles are defined in `main.c` and accessed by web server:
```c
extern rs485_handle_t g_rs485;        // RS485 driver handle
extern modbus_handle_t g_modbus;      // Modbus master handle
extern mightyzap_handle_t g_actuator; // mightyZAP actuator handle
```

These must be initialized before web server starts to enable actuator control via REST API.

## REST API Endpoints

The web server (`main/webserver/web_server.c`) exposes:

**Actuator Control**:
- `GET /api/actuator/status` - Read position, current, voltage
- `POST /api/actuator/control` - Set goal position, speed, force
- `POST /api/actuator/scan` - Scan RS485 bus for actuators (ID 1-20)
- `POST /api/actuator/add` - Add actuator by ID
- `POST /api/actuator/remove` - Remove actuator

**RS485/Modbus Diagnostics**:
- `GET /api/rs485/config` - Get UART config (baud, pins)
- `PUT /api/rs485/config` - Update RS485 settings (requires restart)
- `GET /api/rs485/diag` - Read stats (TX/RX counts, errors)
- `POST /api/rs485/test` - Send test command to verify communication
- `POST /api/rs485/reset_stats` - Clear diagnostic counters

**System**:
- `GET /api/status` - Uptime, free heap, WiFi status
- `GET /api/tasks` - FreeRTOS task list with stack usage
- `POST /api/restart` - Reboot ESP32

**WiFi**:
- `GET /api/wifi/scan` - Scan for networks
- `POST /api/wifi/connect` - Connect to SSID (saves to config.json)
- `GET /api/wifi/status` - Connection state, IP address

**File System**:
- `GET /api/files/list` - List files in `userdata` partition
- `GET /api/files/download` - Download file
- `POST /api/files/upload` - Upload file
- `DELETE /api/files/delete` - Delete file

## mightyZAP Actuator Integration

The `mightyzap` component provides a high-level API for IR-Robot's FC_MODBUS model actuators.

### Key Registers (Modbus Holding Registers)

**Non-Volatile (EEPROM)**:
- `0x0000-0x0001`: Model number, firmware version (read-only)
- `0x0002`: Servo ID (1-247, change requires restart)
- `0x0003`: Baud rate (16-128, formula: `2000000/(reg+1)`)
- `0x0005-0x0006`: Stroke limits (short/long, 0-4095)
- `0x000D`: Speed limit (0-1023)
- `0x000E`: Current limit (0-1600, 100mA units)

**Volatile (RAM)**:
- `0x0032`: Force enable (0=off, 1=on)
- `0x0034`: Goal position (0-4095, 12-bit ADC)
- `0x0035`: Goal speed (0-1023)
- `0x0036`: Goal current/force (0-1600)
- `0x0037`: Present position (read-only)
- `0x0038`: Present current (read-only)
- `0x003A`: Present voltage (0-255, 0.1V units)
- `0x003B`: Moving status (0=stopped, 1=moving)

### API Usage Example

```c
// Initialize (called in main.c)
mightyzap_handle_t actuator;
mightyzap_init(g_modbus, SLAVE_ID, &actuator);

// Read current position
uint16_t pos;
mightyzap_get_present_position(actuator, &pos);

// Move to position with speed control
mightyzap_set_goal_position(actuator, 2048);  // Middle position
mightyzap_set_goal_speed(actuator, 512);      // Half speed

// Read diagnostics
uint16_t current, voltage;
mightyzap_get_present_current(actuator, &current);
mightyzap_get_present_voltage(actuator, &voltage);
```

## Configuration System

Configuration is stored in `/userdata/config.json` (LittleFS partition). The `config_manager` provides a C API to avoid cJSON dependencies elsewhere:

```c
// Load config (called once in app_main)
config_init();  // Mounts partitions, loads config.json

// Get values (returns safe defaults if not set)
const char *ssid = config_get_wifi_ssid();
uint32_t baud = config_get_rs485_baud();
uint8_t tx_pin = config_get_rs485_tx_pin();

// Modify and save
config_set_wifi_ssid("NewNetwork");
config_set_wifi_password("password");
config_save();  // Writes to /userdata/config.json
```

**Default values** are defined in `sdkconfig.defaults` and `config_manager.c`. The `userdata` partition is auto-formatted on first boot but preserved across firmware updates.

## FreeRTOS Configuration

Industrial-grade settings in `sdkconfig.defaults`:

- **Tick Rate**: 1000 Hz (CONFIG_FREERTOS_HZ) for precise timing
- **Task Watchdog**: 10s timeout (CONFIG_ESP_TASK_WDT_TIMEOUT_S), monitors idle tasks on both cores
- **Stack Protection**: Strong checking enabled (CONFIG_COMPILER_STACK_CHECK_MODE_STRONG)
- **Panic Handler**: 3s delay before reboot to capture logs (CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS)
- **Coredump**: Enabled to flash in ELF format for post-mortem debugging
- **Brownout**: 2.90V threshold (CONFIG_ESP_BROWNOUT_DET_LVL_SEL_4) for industrial power supplies

## Web Interface Development

Static files in `main/www/` are built into a LittleFS image and flashed to the `www` partition:

- `index.html` - Main SPA shell
- `core.js` - Shared utilities and API client
- `style.css` - Global styles
- `tabs/*.html`, `tabs/*.js` - Tab content loaded dynamically

**Build process**: CMake automatically calls `littlefs_create_partition_image()` to pack `main/www/` into `build/www.bin`.

**Updating web interface**:
```bash
# Edit files in main/www/
vim main/www/tabs/actuators.js

# Rebuild (regenerates www.bin)
idf.py build

# Flash only web interface (fast, preserves config)
./flash.sh www
```

## Development Notes

### Adding a New Modbus Register

1. Define register in `main/mightyzap/mightyzap.h` enum
2. Implement getter/setter in `main/mightyzap/mightyzap.c` using `modbus_read_holding_registers()` or `modbus_write_single_register()`
3. Expose via REST API in `main/webserver/web_server.c`
4. Update web UI in `main/www/tabs/actuators.js`

### RS485 Communication Troubleshooting

Check diagnostics via API or serial logs:
```c
// In code
rs485_print_stats(g_rs485);

// Via REST
GET /api/rs485/diag
```

Common issues:
- **No response**: Check DE/RE pin wiring, verify baud rate matches actuator
- **CRC errors**: Electrical noise, improper termination, or wrong UART config
- **Timeouts**: Increase `modbus.timeout` in config.json (default 100ms)

### Health Monitoring

The `health_monitor` task runs every 10 seconds and logs:
- FreeRTOS task states (Run, Ready, Blocked, Suspended, Deleted)
- Stack high water marks (minimum free stack)
- CPU usage per task

Enable detailed logging:
```c
esp_log_level_set("HEALTH", ESP_LOG_INFO);
```

### Debugging Coredumps

If the system panics, coredump is saved to flash:
```bash
# Read and analyze coredump
idf.py coredump-info

# Or use standalone tool
espcoredump.py info_corefile -t elf build/bocal-dinamico.elf build/coredump.bin
```

## Pin Configuration

Default pins (configurable in `config.json`):
- RS485 TX: GPIO 17 (to MAX485 DI)
- RS485 RX: GPIO 5 (from MAX485 RO)
- RS485 DE/RE: GPIO 18 (direction control, active high for TX)

UART_NUM_1 is used for RS485 (UART_NUM_0 is reserved for console).
