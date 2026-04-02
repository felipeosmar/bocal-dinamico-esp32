---
markmap:
  colorFreezeLevel: 2
  maxWidth: 300
---

# Bocal Dinâmico ESP32

## Hardware
- **MCU**: ESP32 (dual-core, 240 MHz)
- **Protocolo**: RS485 / Modbus RTU
- **Atuadores**: mightyZAP FC_MODBUS
- **Sensor**: Baumer OX100 (opcional)
- **Transceptor**: MAX485 (half-duplex)

## Software
### Camadas
- Aplicação Web (HTTP/REST)
- Lógica de Controle (control_loop)
- API de Atuadores (mightyzap)
- Protocolo Modbus RTU
- Driver RS485 (UART)

### Sistema Operacional
- FreeRTOS (tick 1000 Hz)
- Multitarefa preemptiva
- Semáforos e mutexes por barramento

## Funcionalidades
### Controle de Atuadores
- Posicionamento individual (0–4095)
- Controle de velocidade (0–1023)
- Controle de corrente (0–1600 mA)
- Movimento sincronizado (broadcast ID 0)

### Malha Fechada
- Leitura do sensor Baumer OX100
- Equações lineares: `pos = a × gap + b`
- Atualização a cada 500 ms (configurável)

### Interface Web
- SPA (Single Page Application)
- Tabs: Atuadores, Sistema, Config, Arquivos
- API REST + Basic Auth (opcional)

### Conectividade
- WiFi STA (cliente) ou AP (ponto de acesso)
- IP dinâmico via DHCP
- HTTP na porta 80

## Armazenamento Flash
- **factory** (1,6 MB): Firmware
- **www** (256 KB): Interface web (LittleFS)
- **userdata** (64 KB): config.json (LittleFS)
- **coredump** (64 KB): Diagnóstico de pânico

## Dois Barramentos RS485
### Barramento 1 — Primário
- UART1
- Controle individual por ID
- IDs 1–247
### Barramento 2 — Sincronizado
- UART2
- Broadcast para ID 0
- Grupo de sincronização
