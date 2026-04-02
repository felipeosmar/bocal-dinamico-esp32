---
markmap:
  colorFreezeLevel: 2
  maxWidth: 320
---

# Sequência de Inicialização (app_main)

## 1. Log Buffer
- Captura logs antes do WiFi subir
- Buffer circular em memória RAM
- Acessível via `GET /api/logs`

## 2. Configuração (`config_init`)
- Monta partição **userdata** (LittleFS)
- Lê `/userdata/config.json`
- Formata partição automaticamente no primeiro boot
- Fornece valores padrão se chave ausente

## 3. RS485 — Barramento 1
- Inicializa **UART1**
- Pinos: TX=17, RX=5, DE/RE=18 (padrão)
- Modo half-duplex
- Cria `g_rs485` (handle global)

## 4. Modbus — Barramento 1
- Wraps `g_rs485`
- Timeout: 100 ms (configurável)
- Cria `g_modbus` (handle global)

## 5. RS485 + Modbus — Barramento 2 (Sync)
- Inicializa **UART2** com pinos do config `rs485_sync`
- Cria `g_rs485_sync` e `g_modbus_sync`

## 6. Mutexes de Barramento
- `g_bus_mutex` → protege Barramento 1
- `g_bus_sync_mutex` → protege Barramento 2
- Timeout de aquisição: 5000 ms (controle), 30000 ms (scan)

## 7. Actuator Task
- Cria fila `s_actuator_queue` (10 comandos)
- Inicia tarefa FreeRTOS `actuator_worker` (prio 4, core 1)
- Cria `g_actuator` (handle de cada atuador registrado)

## 8. Sync Controller
- Lê grupo de sincronização do config
- Inicia `sync_ctrl_task` (prio 3) se sincronização habilitada

## 9. Sensor Baumer (condicional)
- Verifica `baumer.enabled` no config
- Inicializa Baumer OX100 via Modbus (barramento 1)
- `slave_id` configurável (padrão 10)

## 10. Control Loop (condicional)
- Inicia se Baumer habilitado
- Carrega equações do config
- Inicia `control_loop_task` (prio 3)
- Auto-inicia se `control.running = true`

## 11. WiFi Manager
- Modo STA: conecta ao SSID configurado
- Modo AP: cria rede `Bocal-Dinamico`
- Fallback para AP se STA falhar

## 12. Web Server
- Monta partição **www** (LittleFS)
- Registra todas as rotas REST
- Inicia httpd (ESP-IDF HTTP server)
- Autenticação Basic Auth (se habilitada)

## 13. Health Monitor
- Inicia `health_monitor` (prio 2)
- Loga a cada 10 s: estado das tarefas, HWM de stack, heap
