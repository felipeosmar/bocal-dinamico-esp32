---
markmap:
  colorFreezeLevel: 2
  maxWidth: 340
---

# Tarefas FreeRTOS

## `actuator_worker`
- **Prioridade**: 4 (mais alta da aplicação)
- **Core**: 1 (fixo)
- **Stack**: configurável
- **Sincronização**: fila `s_actuator_queue` (10 itens)
### O que faz
- Aguarda comandos na fila (`xQueueReceive`)
- Adquire `g_bus_mutex` antes de cada transação
- Executa: set position, speed, current, force enable
- Libera mutex após cada transação
### Comandos aceitos
- `ACTUATOR_CMD_MOVE` — posicionar com speed/current
- `ACTUATOR_CMD_SYNC_MOVE` — delega para `sync_ctrl_task`

## `sync_ctrl_task`
- **Prioridade**: 3
- **Sincronização**: fila interna (`g_sync_queue`)
### O que faz
- Recebe comandos de movimento sincronizado
- Adquire `g_bus_sync_mutex` (Barramento 2)
- Broadcast para ID 0: Force ON → Speed → Current → Position
- Polling de posições individuais até convergência
- Reporta desincronização e resultado (`SYNC_OK / TIMEOUT / DESYNC`)

## `control_loop_task`
- **Prioridade**: 3
- **Intervalo**: 500 ms (configurável em `control.interval_ms`)
### O que faz
- Lê medição do Baumer OX100 (`baumer_read_measurements`)
- Extrai valor de gap (`values[measurement_index]`)
- Para cada equação habilitada: calcula `pos = a × gap + b`
- Chama `mightyzap_set_position` via barramento configurado
- Para se `control.running = false`

## `health_monitor`
- **Prioridade**: 2
- **Intervalo**: 10 s
### O que faz
- Loga estado de todas as tarefas (Run/Ready/Blocked/Suspended)
- Registra HWM de stack (mínimo de stack livre)
- Registra heap atual e heap mínimo histórico
- Monitora watchdog das tarefas idle (ambos os cores)

## `httpd` (ESP-IDF)
- **Prioridade**: gerenciada pelo ESP-IDF
- **Modelo**: tarefa única para todos os handlers
- **Stack**: 8 KB
### Thread Safety implícita
- Handlers HTTP executam na mesma tarefa → sem concorrência entre handlers
- Acesso a `s_actuators[]` (lista de atuadores) é seguro sem mutex
- Setters de config chamados apenas desta tarefa → thread-safe

## Tarefas do Sistema (ESP-IDF)
- `wifi_manager` — gerencia conexão WiFi STA/AP
- `IDLE0 / IDLE1` — ociosidade dos cores (monitoradas pelo watchdog)
- `Tmr Svc` — FreeRTOS Software Timer Service
- `esp_timer` — timers de alta resolução

## Mecanismos de Sincronização
### Mutexes
- `g_bus_mutex` — Barramento 1 (UART1)
- `g_bus_sync_mutex` — Barramento 2 (UART2)
- Mutex por UART no rs485_driver
### Filas
- `s_actuator_queue` — 10 comandos de atuador
- `g_sync_queue` — comandos de sync
### Seção Crítica
- `portMUX_TYPE` em modbus_rtu.c para atualizar estatísticas

## Watchdog
- Timeout: 10 s (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`)
- Monitora tarefas idle de ambos os cores
- Stack overflow: proteção forte habilitada
- Pânico: delay de 3 s antes do reboot (captura de logs)
