---
markmap:
  colorFreezeLevel: 2
  maxWidth: 340
---

# Configuração e Partições Flash

## Partições Flash (`partitions.csv`)
### `nvs` — 24 KB @ 0x9000
- Tipo: data/nvs
- Credenciais WiFi e BT (NVS do ESP-IDF)
- Gerenciado pelo WiFi manager
### `phy_init` — 4 KB @ 0xF000
- Calibração do rádio WiFi/BT
- Escrito na fábrica / primeiro boot
### `factory` — 1,6 MB @ 0x10000
- Firmware da aplicação (binário ELF)
- Atualizado com `./flash.sh update` ou `./flash.sh app`
### `coredump` — 64 KB @ 0x1A0000
- Dump de pânico em formato ELF
- Analisar com `idf.py coredump-info`
### `www` — 256 KB @ 0x1B0000
- Interface web (LittleFS)
- Atualizado com `./flash.sh www`
- Contém: index.html, core.js, style.css, tabs/
### `userdata` — 64 KB @ 0x1F0000
- **config.json** (LittleFS)
- Preservado em atualizações de firmware (`./flash.sh update`)
- Apagado apenas com `./flash.sh all`

## config.json — Seções
### `wifi`
- `ssid` / `password` — rede do cliente
- `ap_mode` — true = modo AP exclusivo
- `ap_ssid` / `ap_password` — rede criada no modo AP
### `rs485` — Barramento 1
- `baud` — taxa (padrão 57600)
- `tx_pin` — GPIO TX (padrão 17)
- `rx_pin` — GPIO RX (padrão 5)
- `de_pin` — GPIO DE/RE (padrão 18)
### `rs485_sync` — Barramento 2
- Mesma estrutura do `rs485`
- Pinos independentes para UART2
### `modbus`
- `timeout` — timeout de resposta em ms (padrão 100)
### `web`
- `username` / `password` — credenciais Basic Auth
- `auth_enabled` — habilita/desabilita auth (padrão false)
### `actuator`
- `scan_max_id` — ID máximo na varredura (padrão 20)
### `baumer`
- `enabled` — habilita sensor
- `slave_id` — Modbus ID do Baumer (padrão 10)
### `control`
- `running` — auto-inicia loop no boot
- `interval_ms` — período do loop (padrão 500)
- `measurement_index` — índice do `values[]` para gap
- `equations[]` — lista de equações lineares
### `roles`
- `lens_a` — `{id, baud}` para lente A
- `lens_b` — `{id, baud}` para lente B
- `nozzle` — `{id, baud}` para bico

## Config Manager API (`config_manager.h`)
### Leitura (thread-safe)
- `config_get_wifi_ssid()` → `const char*`
- `config_get_rs485_baud()` → `uint32_t`
- `config_get_rs485_tx_pin()` → `uint8_t`
- `config_get_modbus_timeout()` → `uint32_t`
- `config_get_baumer_enabled()` → `bool`
### Escrita (HTTP task only)
- `config_set_wifi_ssid(ssid)`
- `config_set_rs485_baud(baud)`
- `config_save()` → grava em `/userdata/config.json`
### Inicialização
- `config_init()` → monta LittleFS, carrega JSON
- Auto-format na primeira execução
- Defaults definidos em `config_manager.c`

## Script de Flash (`flash.sh`)
- `./flash.sh update` — firmware + www (preserva userdata)
- `./flash.sh app` — só firmware (mais rápido)
- `./flash.sh www` — só interface web
- `./flash.sh all` — apaga tudo (factory reset)
- `PORT=/dev/ttyACM0 ./flash.sh update` — porta customizada
