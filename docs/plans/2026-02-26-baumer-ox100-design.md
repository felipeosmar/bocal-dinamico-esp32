# Design: Integrating Baumer OX100 Profilometer

**Date:** 2026-02-26
**Status:** Approved

## Context

The ESP32 system controls mightyZAP actuators that regulate a dynamic nozzle ("bocal dinamico") for material injection. A Baumer OX100 profilometer (smart profile sensor) is connected on the same RS485 bus (Modbus RTU, slave ID 8) and measures the gap between two metal sheets. The gap measurement drives closed-loop control of actuator positions to regulate material injection volume.

## Requirements

- Read 4 measured values (Float32) from Baumer OX100 via Modbus RTU FC04
- Measured Value 1 = gap between sheets (primary control value)
- Display all 4 values on the web interface
- Automatic control mode: operator enables/disables via web interface
- Control loop interval configurable from ~100ms to 60000ms
- Configurable linear equation per actuator: `position = a * gap + b`
- Coefficients `a` and `b` configurable per actuator via web interface
- Configuration persisted in config.json, survives reboots

## Approach

**Approach A (selected):** Dedicated `baumer/` component + FreeRTOS control loop task.

Follows the existing project pattern (like `mightyzap/`): modular components with clear separation of responsibilities. A FreeRTOS task handles the real-time control loop independently from the web server.

Rejected alternatives:
- **B: Direct integration in web server** -- mixes responsibilities, timer callback limitations
- **C: Generic sensor abstraction** -- over-engineering for a single sensor (YAGNI)

## Component: `baumer/`

### Files
- `main/baumer/baumer_ox100.h`
- `main/baumer/baumer_ox100.c`
- `main/baumer/CMakeLists.txt`

### Data Structures

```c
typedef struct {
    uint16_t status;        // Bit 2 = valid values, Bit 3 = alarm
    uint8_t  quality;       // 0=OK, 1=Weak signal, 2=No signal
    uint8_t  output;        // Binary outputs
    float    values[4];     // Measured values 1-4
} baumer_measurement_t;

typedef struct baumer_ctx *baumer_handle_t;
```

### API

```c
esp_err_t baumer_init(modbus_handle_t modbus, uint8_t slave_id, baumer_handle_t *handle);
esp_err_t baumer_read_measurements(baumer_handle_t handle, baumer_measurement_t *out);
esp_err_t baumer_set_laser(baumer_handle_t handle, bool on);
esp_err_t baumer_get_cached(baumer_handle_t handle, baumer_measurement_t *out);
void      baumer_deinit(baumer_handle_t handle);
```

### Modbus Communication

- FC04 (Read Input Registers), address 200, quantity 11 registers
- Returns: status (reg 200), quality (reg 201), output (reg 202), values 1-4 (regs 203-210)
- Float32 Little Endian: low word at address N, high word at address N+1
- Laser control: FC06 (Write Single Register), address 410, value 0=OFF / 1=ON
- Internal mutex protects cached measurement for thread-safe web server access

### Float32 Little Endian Conversion

```c
uint32_t raw = ((uint32_t)reg[1] << 16) | (uint32_t)reg[0];
float value;
memcpy(&value, &raw, sizeof(float));
```

## Component: `control_loop/`

### Files
- `main/control_loop/control_loop.h`
- `main/control_loop/control_loop.c`
- `main/control_loop/CMakeLists.txt`

### Data Structures

```c
typedef struct {
    uint8_t actuator_id;
    float coeff_a;
    float coeff_b;
    bool enabled;
} control_equation_t;

typedef struct {
    bool running;
    uint32_t interval_ms;           // 100ms to 60000ms
    uint8_t measurement_index;      // 0-3, default 0
    control_equation_t equations[10];
    uint8_t equation_count;
} control_config_t;

typedef struct {
    bool running;
    float last_gap_value;
    uint8_t last_quality;
    uint32_t loop_count;
    uint32_t error_count;
    int64_t last_run_timestamp;
    uint16_t computed_positions[10];
} control_status_t;
```

### API

```c
esp_err_t control_loop_init(baumer_handle_t baumer, control_config_t *config);
esp_err_t control_loop_start(void);
esp_err_t control_loop_stop(void);
esp_err_t control_loop_set_interval(uint32_t interval_ms);
esp_err_t control_loop_set_equation(uint8_t actuator_id, float a, float b, bool enabled);
esp_err_t control_loop_get_config(control_config_t *out);
esp_err_t control_loop_get_status(control_status_t *out);
```

### Task Logic

```
task_control_loop:
  loop:
    wait(interval_ms)
    if (!running) continue

    baumer_read_measurements()
    if (quality == No signal) -> skip, increment error_count
    if (status bit 2 == 0) -> skip, invalid values

    gap = values[measurement_index]

    for each equation where enabled:
      position = clamp(a * gap + b, 0, 4095)
      mightyzap_set_position(actuator_id, position)

    update control_status_t (internal mutex)
```

- Task always runs (reads profilometer for monitoring), only moves actuators when `running == true`
- After 5 consecutive errors, stops control loop automatically
- Stack size: 4096 bytes

## Configuration and Persistence

### config.json Extension

```json
{
    "baumer": {
        "slave_id": 8,
        "enabled": true
    },
    "control": {
        "running": false,
        "interval_ms": 1000,
        "measurement_index": 0,
        "equations": [
            { "actuator_id": 1, "a": 1.0, "b": 0.0, "enabled": false }
        ]
    }
}
```

### config_manager Extensions

```c
uint8_t  config_get_baumer_slave_id(void);
bool     config_get_baumer_enabled(void);
void     config_set_baumer_slave_id(uint8_t id);
void     config_set_baumer_enabled(bool enabled);

bool     config_get_control_running(void);
uint32_t config_get_control_interval(void);
uint8_t  config_get_control_measurement_index(void);
void     config_set_control_running(bool running);
void     config_set_control_interval(uint32_t ms);
void     config_set_control_measurement_index(uint8_t idx);

int      config_get_control_equations(control_equation_t *out, int max_count);
void     config_set_control_equations(const control_equation_t *eqs, int count);
```

### Boot Sequence

```
app_main():
  config_init()
  rs485_init()
  modbus_init()
  mightyzap_init() for each saved actuator

  if (config_get_baumer_enabled()):
    baumer_init(g_modbus, config_get_baumer_slave_id(), &g_baumer)
    control_loop_init(g_baumer, config)
    if (config_get_control_running()):
      control_loop_start()

  webserver_start()
```

## REST API Endpoints

### Baumer Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/baumer/status` | Read cached measurements + quality |
| GET | `/api/baumer/config` | Get Baumer configuration |
| PUT | `/api/baumer/config` | Set slave_id, enabled (restart required) |
| POST | `/api/baumer/laser` | Toggle laser on/off |

### Control Loop Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/control/status` | Loop status + last values + computed positions |
| POST | `/api/control/start` | Start automatic control |
| POST | `/api/control/stop` | Stop automatic control |
| PUT | `/api/control/interval` | Set loop interval in ms |
| PUT | `/api/control/equation` | Set equation for one actuator |

## Web Interface

### New Tab: "Profiler"

Files: `main/www/tabs/profiler.html`, `main/www/tabs/profiler.js`

Layout:
- **Measured Values panel:** 4 values with units, signal quality indicator, laser toggle
- **Control Loop panel:** running status, interval config, measurement selector, loop/error counters, start/stop buttons
- **Equations table:** one row per connected actuator with editable `a`, `b`, enabled checkbox, computed position display
- **Baumer Config panel:** slave ID, enabled toggle (save requires restart)

Polling: 1 second interval, active only when tab visible.

## Error Handling and Safety

| Scenario | Behavior |
|----------|----------|
| Timeout / no response | Increment error_count, keep last cached value, skip actuator update |
| CRC error | Automatic retry (existing Modbus mechanism) |
| Quality = No signal | Skip actuator update, show red alert |
| Quality = Weak signal | Update actuators normally, show yellow warning |
| Status bit 2 = 0 (invalid) | Skip actuator update, increment error_count |
| 5 consecutive errors | Stop control loop automatically, show alert |

### Actuator Protection
- Position clamped to 0-4095 (12-bit physical range)
- Manual web commands overridden by next control loop cycle when running

### Bus Concurrency
- All operations use existing RS485 mutex
- Control loop sequence (read Baumer + write actuators) is not atomic -- web server can interleave
- Acceptable: no data corruption risk, only slight timing variance

## New Files

| File | Purpose |
|------|---------|
| `main/baumer/baumer_ox100.h` | Baumer component public API |
| `main/baumer/baumer_ox100.c` | Modbus FC04 read, cache, Little Endian conversion |
| `main/baumer/CMakeLists.txt` | Component build |
| `main/control_loop/control_loop.h` | Control loop public API |
| `main/control_loop/control_loop.c` | FreeRTOS task, equations, safety logic |
| `main/control_loop/CMakeLists.txt` | Component build |
| `main/webserver/web_baumer.c` | REST handlers for /api/baumer/* |
| `main/webserver/web_control.c` | REST handlers for /api/control/* |
| `main/www/tabs/profiler.html` | Profiler tab HTML |
| `main/www/tabs/profiler.js` | Profiler tab JavaScript |

## Modified Files

| File | Change |
|------|--------|
| `main/main.c` | Add g_baumer, Baumer + control loop initialization |
| `main/config/config_manager.h` | New config functions |
| `main/config/config_manager.c` | Implement new config functions |
| `main/webserver/web_server.c` | Register new handlers |
| `main/www/index.html` | Add "Profiler" tab to menu |
| `main/CMakeLists.txt` | Add new components to build |

## Memory Impact

- Control loop task stack: ~4096 bytes
- Baumer context struct: ~64 bytes
- Control config/status structs: ~256 bytes
- New code (.text): ~8-12 KB
- Web files (HTML+JS): ~5-8 KB
