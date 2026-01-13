# Bocal Dinâmico - ESP32 RS485 Modbus Controller

ESP32-based industrial control system for mightyZAP actuators via RS485/Modbus RTU with a web-based management interface. Designed for 24/7 operation with robust error handling, health monitoring, and remote configuration.

## Features

- **RS485/Modbus RTU Master**: Control mightyZAP actuators (FC_MODBUS model) via Modbus protocol
- **Web Interface**: Modern responsive UI for configuration and real-time control
- **Dual WiFi Modes**: Station (STA) mode for existing networks or Access Point (AP) mode for direct connection
- **Persistent Configuration**: JSON-based config stored in dedicated partition, preserved across firmware updates
- **Real-time Monitoring**: View actuator position, current, voltage, and system health
- **Actuator Scanning**: Automatically discover connected devices on RS485 bus
- **Diagnostics**: RS485 statistics, FreeRTOS task monitoring, memory usage
- **File Management**: Upload/download files via web interface
- **Secure Access**: Optional HTTP Basic Authentication
- **Industrial-Grade**: Watchdog timers, stack protection, coredump support, brownout detection

## Hardware Requirements

### Essential Components

- **ESP32 Development Board** (ESP32-WROOM-32, ESP32-DevKitC, or similar)
- **RS485 Transceiver Module** (MAX485, MAX3485, or equivalent)
- **mightyZAP Actuators** (FC_MODBUS model recommended)
- **Power Supply**: 5V for ESP32, appropriate voltage for actuators (typically 12V)

### Wiring

**RS485 Connection (ESP32 ↔ MAX485)**:
```
ESP32 GPIO 17  →  MAX485 DI  (Transmit Data)
ESP32 GPIO 5   →  MAX485 RO  (Receive Data)
ESP32 GPIO 18  →  MAX485 DE/RE (Direction Control)
ESP32 GND      →  MAX485 GND
ESP32 5V       →  MAX485 VCC
```

**RS485 Bus (MAX485 ↔ Actuators)**:
```
MAX485 A  →  Actuator A (or D+)
MAX485 B  →  Actuator B (or D-)
```

**Important Notes**:
- Use twisted pair cable for RS485 bus (Cat5/Cat6 works well)
- Add 120Ω termination resistor at both ends of RS485 bus for long cables (>3m)
- Keep RS485 cable away from high-voltage AC lines to reduce noise
- Power actuators separately (do not power from ESP32)

## Software Prerequisites

### ESP-IDF Installation

This project requires **ESP-IDF v5.0 or later**.

**Linux/macOS**:
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# Clone ESP-IDF
mkdir -p ~/esp
cd ~/esp
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git

# Install ESP-IDF
cd esp-idf
./install.sh esp32

# Set up environment (add to ~/.bashrc for persistence)
. ~/esp/esp-idf/export.sh
```

**Windows**:
Download and run the [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/).

Verify installation:
```bash
idf.py --version
```

## Quick Start

### 1. Clone Repository

```bash
git clone <repository-url>
cd bocal-dinamico-esp32
```

### 2. Configure Project (Optional)

The project comes with sensible defaults in `sdkconfig.defaults`. To customize:

```bash
idf.py menuconfig
```

### 3. Build Firmware

```bash
# Activate ESP-IDF environment (if not already active)
. ~/esp/esp-idf/export.sh

# Build project
idf.py build
```

### 4. Flash to ESP32

**First-time flash (factory reset)**:
```bash
# Connect ESP32 via USB
# Default port is /dev/ttyUSB0 (Linux) or COM3 (Windows)

idf.py -p /dev/ttyUSB0 flash
```

**Or use the flash helper script**:
```bash
./flash.sh all
```

### 5. Monitor Serial Output

```bash
idf.py -p /dev/ttyUSB0 monitor

# Exit monitor: Ctrl+]
```

**Or combine flash and monitor**:
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## First-Time Configuration

### Initial Setup

1. **Power on ESP32**: After flashing, the device will start in AP mode

2. **Connect to WiFi AP**:
   - SSID: `Bocal-Dinamico`
   - Password: `12345678`

3. **Access Web Interface**:
   - Open browser: http://192.168.4.1
   - Default credentials:
     - Username: `admin`
     - Password: `admin`

### Configure WiFi (Station Mode)

1. Go to **Config** tab in web interface
2. Enter your WiFi network SSID and password
3. Click **Save & Restart**
4. Device will reboot and connect to your network
5. Check serial monitor for assigned IP address:
   ```
   Web Interface: http://192.168.1.xxx
   ```

### Configure RS485/Modbus

1. Go to **Config** tab
2. Adjust RS485 settings if using non-default pins:
   - **Baud Rate**: Match actuator baud rate (default: 57600)
   - **TX Pin**: GPIO for transmit (default: 17)
   - **RX Pin**: GPIO for receive (default: 5)
   - **DE Pin**: GPIO for direction control (default: 18)
3. Set **Modbus Timeout** (default: 100ms)
4. Click **Save & Restart**

### Scan for Actuators

1. Go to **Actuators** tab
2. Click **Scan Bus** (scans IDs 1-20 by default)
3. Found actuators will appear in the list
4. Click **Add** to enable control

## Usage

### Web Interface Overview

The web interface provides five main tabs:

**System Tab**:
- View uptime, memory usage, WiFi status
- Monitor FreeRTOS tasks and stack usage
- Restart system

**Actuators Tab**:
- Real-time position, current, voltage display
- Control goal position, speed, force
- Enable/disable force mode
- Scan for new actuators

**Config Tab**:
- WiFi settings (STA/AP mode)
- RS485/Modbus configuration
- Web authentication settings
- Save changes (requires restart)

**Files Tab**:
- Browse files on `userdata` partition
- Upload/download configuration files
- View `config.json`

**Tasks Tab**:
- Real-time FreeRTOS task monitoring
- CPU usage and stack high water marks

### REST API

The system exposes a REST API for programmatic control. See [CLAUDE.md](CLAUDE.md) for complete API documentation.

Example using curl:
```bash
# Get actuator status
curl http://192.168.1.xxx/api/actuator/status

# Set goal position
curl -X POST http://192.168.1.xxx/api/actuator/control \
  -H "Content-Type: application/json" \
  -d '{"goal_position": 2048, "goal_speed": 512}'

# Scan RS485 bus
curl -X POST http://192.168.1.xxx/api/actuator/scan
```

## Updating Firmware

### Recommended Method (Preserves Configuration)

```bash
# Build new firmware
idf.py build

# Flash firmware and web interface, keep user config
./flash.sh update
```

### Alternative Methods

```bash
# Flash only firmware (fastest)
./flash.sh app

# Flash only web interface
./flash.sh www

# Full flash with factory reset (erases config)
./flash.sh all
```

**Custom Serial Port**:
```bash
PORT=/dev/ttyACM0 ./flash.sh update
```

## Troubleshooting

### ESP32 Not Detected

- **Linux**: Check user permissions for serial port
  ```bash
  sudo usermod -a -G dialout $USER
  # Logout and login for changes to take effect
  ```
- **Windows**: Install [CP210x drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) or [CH340 drivers](http://www.wch-ic.com/downloads/CH341SER_EXE.html)
- Try different USB cable (some are charge-only)

### Cannot Connect to AP Mode

- Verify SSID `Bocal-Dinamico` is visible
- Try moving closer to ESP32 (AP range is limited)
- Check serial monitor for errors
- Factory reset: `./flash.sh all`

### Actuators Not Responding

1. **Check wiring**: Verify A/B connections and polarity
2. **Check baud rate**: Must match actuator configuration (default: 57600)
3. **Check slave ID**: Default is ID=1, verify actuator ID
4. **RS485 Diagnostics**:
   - Go to Config tab → RS485 Diagnostics
   - Check TX/RX counts (should increment on communication)
   - Check CRC errors (should be zero or very low)
5. **Test communication**:
   ```bash
   curl -X POST http://192.168.1.xxx/api/rs485/test
   ```
6. **Add termination**: If cable is >3m, add 120Ω resistor at both ends

### High CRC Error Rate

- Check cable quality (use twisted pair)
- Reduce cable length if possible
- Add termination resistors (120Ω)
- Move cable away from power lines
- Lower baud rate (try 9600 or 19200)

### System Crashes/Reboots

- Check serial monitor for panic messages
- Extract coredump:
  ```bash
  idf.py coredump-info
  ```
- Check power supply stability (brownout detection at 2.90V)
- Monitor task stack usage (System tab → Tasks)

### Web Interface Not Loading

- Clear browser cache
- Check WiFi connection
- Verify IP address in serial monitor
- Try different browser
- Reflash web interface:
  ```bash
  ./flash.sh www
  ```

## Configuration File

User configuration is stored in `/userdata/config.json` on the ESP32. Example:

```json
{
    "wifi": {
        "ssid": "YourNetwork",
        "password": "YourPassword",
        "ap_mode": false,
        "ap_ssid": "Bocal-Dinamico",
        "ap_password": "12345678"
    },
    "rs485": {
        "baud": 57600,
        "tx_pin": 17,
        "rx_pin": 5,
        "de_pin": 18
    },
    "modbus": {
        "slave_id": 1,
        "timeout": 100
    },
    "web": {
        "username": "admin",
        "password": "admin",
        "auth_enabled": true
    },
    "actuator": {
        "scan_max_id": 20
    }
}
```

This file can be edited via web interface (Files tab) or by modifying `config.json` in the project root before first flash.

## Project Structure

```
bocal-dinamico-esp32/
├── main/
│   ├── main.c              # Application entry point
│   ├── config/             # Configuration manager
│   ├── rs485/              # RS485 UART driver
│   ├── modbus/             # Modbus RTU master
│   ├── mightyzap/          # mightyZAP actuator API
│   ├── wifi/               # WiFi manager
│   ├── webserver/          # HTTP server
│   ├── health/             # System health monitor
│   └── www/                # Web interface files
├── flash.sh                # Flash helper script
├── config.json             # Default configuration
├── partitions.csv          # Custom partition table
├── sdkconfig.defaults      # ESP-IDF configuration
├── CMakeLists.txt          # Build configuration
├── CLAUDE.md               # Technical documentation
└── README.md               # This file
```

For detailed architecture and development guide, see [CLAUDE.md](CLAUDE.md).

## Advanced Configuration

### Changing Actuator Slave ID

1. Connect single actuator to RS485 bus
2. Access via web interface
3. Use Modbus register write to change ID:
   - Register: 0x0002 (ID)
   - Value: New ID (1-247)
4. Power cycle actuator for change to take effect

### Custom Baud Rates

mightyZAP baud rate formula: `Baud = 2000000 / (register + 1)`

Common values:
- 9600: Register = 207
- 19200: Register = 103
- 57600: Register = 33 (default)
- 115200: Register = 16
- 1000000: Register = 1

### Coredump Analysis

If system crashes, coredump is saved to flash:

```bash
# Extract and analyze coredump
idf.py coredump-info

# Generate detailed report
idf.py coredump-debug
```

## Performance Tips

- **Reduce Modbus Timeout**: If bus is reliable, reduce timeout from 100ms to 50ms for faster response
- **Increase Baud Rate**: Use 115200 for faster communication (if actuators support it)
- **Disable Authentication**: For local networks, disable web auth to reduce overhead
- **Monitor Stack Usage**: Check Tasks tab regularly, increase stack size if high water mark is low

## Contributing

Contributions are welcome! Please ensure:
- Code follows existing style
- Changes are tested on hardware
- Update documentation (CLAUDE.md) for architectural changes
- Update README.md for user-facing changes

## License

[Specify your license here]

## Support

For issues and questions:
- Check [Troubleshooting](#troubleshooting) section
- Review [CLAUDE.md](CLAUDE.md) for technical details
- Check serial monitor output for error messages

## Credits

- ESP-IDF by Espressif Systems
- LittleFS port for ESP-IDF by joltwallet
- mightyZAP actuators by IR-Robot Co.
