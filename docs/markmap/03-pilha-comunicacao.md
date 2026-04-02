---
markmap:
  colorFreezeLevel: 2
  maxWidth: 340
---

# Pilha de Comunicação

## Camada de Aplicação — mightyZAP
### API de Alto Nível
- `mightyzap_set_goal_position(handle, pos)`
- `mightyzap_set_goal_speed(handle, speed)`
- `mightyzap_set_force_enable(handle, on)`
- `mightyzap_get_present_position(handle, &pos)`
- `mightyzap_get_present_current(handle, &cur)`
- `mightyzap_get_present_voltage(handle, &volt)`
### Movimento Sincronizado
- `mightyzap_sync_move_broadcast(group, pos)`
- Envia para ID 0 (broadcast) no Barramento 2
- Sequência: Force ON → Speed → Current → Position
### Registros-chave (RAM)
- `0x0032` Force Enable (0/1)
- `0x0034` Goal Position (0–4095)
- `0x0035` Goal Speed (0–1023)
- `0x0036` Goal Current (0–1600)
- `0x0037` Present Position (read-only)
- `0x0038` Present Current (read-only)
- `0x003A` Voltage × 0,1 V (read-only)
- `0x003B` Moving Status (0/1, read-only)

## Camada de Protocolo — Modbus RTU
### Códigos de Função
- **FC03** Read Holding Registers
- **FC04** Read Input Registers
- **FC06** Write Single Register
### Framing
- Endereço (1 byte) + Função (1 byte) + Dados + CRC16
- Timeout de resposta: 100 ms (padrão)
### Retry
- 3 tentativas por padrão
- Retriáveis: `ACKNOWLEDGE`, `SLAVE_DEVICE_BUSY`, `MOTOR_MOVING`
- Não retriáveis: `ILLEGAL_FUNCTION`, `ILLEGAL_ADDRESS`, `OVERLOAD`
### Estatísticas
- TX count, RX count
- CRC errors, timeouts, exceptions
- Acessível via `GET /api/rs485/diag`

## Camada de Transporte — RS485 Driver
### Configuração UART
- UART1 (Barramento 1) / UART2 (Barramento 2)
- Half-duplex via DE/RE pin (GPIO 18 padrão)
- Baud: 57600 (configurável)
### Temporização
- Inter-character: 3,5 × tempo de char (mín. 5 ms)
- A 57600 baud: 0,6 ms nativo → 5 ms imposto
### Thread Safety
- Mutex por UART: `xSemaphoreTake` antes de TX
- `rs485_transaction(tx, rx, timeout_ms)`

## Fluxo Completo — Mover Atuador #1 para 2000
- `POST /api/actuator/control` → HTTP handler
- `actuator_move_async(handle, 2000)` → fila
- `actuator_worker` (tarefa FreeRTOS) → dequeue
- `xSemaphoreTake(g_bus_mutex)` → trava Barramento 1
- `mightyzap_set_goal_position(handle, 2000)` → mightyZAP API
- `modbus_write_single_register(modbus, 1, 0x0034, 2000)` → Modbus
- `rs485_transaction(frame, response, 100ms)` → RS485
- Atuador recebe e ecoa o frame → confirmação
- `xSemaphoreGive(g_bus_mutex)` → libera
- HTTP 200 `{success: true}` → cliente

## Fluxo Sincronizado — Mover Grupo para 1500
- `POST /api/actuator/sync-move {position: 1500}`
- `mightyzap_sync_ctrl_move(group, 1500)` → fila sync
- `sync_ctrl_task` (tarefa FreeRTOS) → dequeue
- `xSemaphoreTake(g_bus_sync_mutex)` → trava Barramento 2
- Broadcast ID 0: Force ON + Speed + Current + Position
- Todos os atuadores do grupo recebem simultaneamente
- Polling de posições até convergir ou timeout
- `xSemaphoreGive(g_bus_sync_mutex)` → libera
