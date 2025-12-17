# Bocal Dinamico ESP32

Projeto ESP-IDF para controle de atuadores mightyZAP e ESP32 Slave via RS485 Modbus RTU, com interface web para configuração e controle.

## Funcionalidades

- **RS485 Modbus RTU** - Comunicação com atuadores e dispositivos escravos
- **Interface Web** - Configuração e controle via navegador
- **WiFi AP/STA** - Modo Access Point para configuração ou conexão à rede existente
- **Configuração Persistente** - Parâmetros salvos em SPIFFS (JSON)
- **FreeRTOS** - Arquitetura multitarefa

## Estrutura do Projeto

```
bocal-dinamico-esp32/
├── CMakeLists.txt
├── partitions.csv              # Tabela de partições (inclui SPIFFS)
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  # Aplicação principal
│   ├── rs485/
│   │   ├── rs485_driver.h
│   │   └── rs485_driver.c      # Driver RS485 half-duplex
│   ├── modbus/
│   │   ├── modbus_rtu.h
│   │   └── modbus_rtu.c        # Protocolo Modbus RTU Master
│   ├── mightyzap/
│   │   ├── mightyzap.h
│   │   └── mightyzap.c         # Driver específico mightyZAP
│   ├── wifi/
│   │   ├── wifi_manager.h
│   │   └── wifi_manager.c      # Gerenciador WiFi (AP/STA)
│   ├── webserver/
│   │   ├── web_server.h
│   │   └── web_server.c        # Servidor HTTP + REST API
│   ├── config/
│   │   ├── config_manager.h
│   │   └── config_manager.c    # Configurações JSON em SPIFFS
│   └── www/
│       ├── index.html          # Interface web principal
│       ├── style.css           # Estilos CSS
│       └── app.js              # JavaScript (API calls)
└── docs/                       # Manual do mightyZAP
```

## Arquitetura

```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32 Master                          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐     ┌──────────────┐     ┌─────────────┐  │
│  │  Web Server  │◄───►│ Config Mgr   │◄───►│   SPIFFS    │  │
│  │  (HTTP API)  │     │   (JSON)     │     │  (storage)  │  │
│  └──────┬───────┘     └──────────────┘     └─────────────┘  │
│         │                                                    │
│         │ REST API                                           │
│         ▼                                                    │
│  ┌──────────────┐     ┌──────────────┐                      │
│  │ WiFi Manager │     │ Modbus Task  │                      │
│  │  (AP/STA)    │     │  (polling)   │                      │
│  └──────────────┘     └──────┬───────┘                      │
│                              │                               │
│                       ┌──────▼───────┐                      │
│                       │  Modbus RTU  │                      │
│                       │   (Master)   │                      │
│                       └──────┬───────┘                      │
│                              │                               │
│                       ┌──────▼───────┐                      │
│                       │    RS485     │                      │
│                       │ (UART + DE)  │                      │
│                       └──────┬───────┘                      │
└──────────────────────────────┼──────────────────────────────┘
                               │
                         ┌─────▼─────┐
                         │  MAX485   │
                         └─────┬─────┘
                               │ RS485 Bus
              ┌────────────────┼────────────────┐
              │                │                │
        ┌─────▼─────┐    ┌─────▼─────┐    ┌─────▼─────┐
        │ ESP32     │    │ mightyZAP │    │  Outros   │
        │ Slave     │    │ (ID=1)    │    │  Slaves   │
        │ (ID=2)    │    │           │    │           │
        └───────────┘    └───────────┘    └───────────┘
```

## Hardware

### Pinagem (GPIOs Seguros)

Usamos GPIOs que não interferem no boot do ESP32:

| ESP32   | MAX485    | Função              |
|---------|-----------|---------------------|
| GPIO17  | DI (4)    | TX (dados enviados) |
| GPIO5   | RO (1)    | RX (dados recebidos)|
| GPIO18  | DE+RE (2,3) | Controle de direção |
| GND     | GND (5)   | Terra               |
| 3.3V    | VCC (8)   | Alimentação         |

**Nota:** Evitamos GPIO0, GPIO2, GPIO12, GPIO15 (strapping pins) e GPIO16 que pode causar problemas em algumas placas.

### Diagrama de Conexão

```
┌─────────────┐    ┌─────────────┐
│   ESP32     │    │   MAX485    │
│             │    │             │
│  GPIO17 ────┼───►│ DI      B ──┼───► RS485 B-
│  GPIO5  ◄───┼────│ RO      A ──┼───► RS485 A+
│  GPIO18 ────┼───►│ DE/RE      │
│  GND    ────┼────│ GND    VCC │◄── 3.3V
│  3.3V   ────┼───►│ VCC        │
└─────────────┘    └─────────────┘
```

## Interface Web

### Abas Disponíveis

| Aba | Funcionalidade |
|-----|----------------|
| **Status** | Informações do sistema (IP, heap, uptime, status Modbus) |
| **LED Control** | Controle do LED remoto no ESP32 Slave (ID=2) |
| **WiFi** | Escanear redes e conectar |
| **RS485** | Configuração de baud rate e slave ID |

### API REST

| Método | Endpoint | Descrição |
|--------|----------|-----------|
| GET | `/api/status` | Status do sistema |
| GET | `/api/wifi/scan` | Escanear redes WiFi |
| GET | `/api/wifi/status` | Status da conexão WiFi |
| POST | `/api/wifi/connect` | Conectar a uma rede |
| GET | `/api/led/status` | Status do LED remoto |
| POST | `/api/led/control` | Controlar LED (on/off/blink) |
| GET | `/api/rs485/config` | Obter configuração RS485 |
| POST | `/api/rs485/config` | Salvar configuração RS485 |
| POST | `/api/restart` | Reiniciar o dispositivo |

### Exemplos de API

```bash
# Status do sistema
curl http://192.168.4.1/api/status

# Ligar LED
curl -X POST http://192.168.4.1/api/led/control \
  -H "Content-Type: application/json" \
  -d '{"led_on": true}'

# Ativar modo blink
curl -X POST http://192.168.4.1/api/led/control \
  -H "Content-Type: application/json" \
  -d '{"blink_mode": true, "blink_period": 500}'

# Conectar ao WiFi
curl -X POST http://192.168.4.1/api/wifi/connect \
  -H "Content-Type: application/json" \
  -d '{"ssid": "MinhaRede", "password": "senha123"}'
```

## Compilar e Gravar

### Pré-requisitos

- ESP-IDF v5.x instalado
- Variáveis de ambiente configuradas

### Comandos

```bash
# Ativar ambiente ESP-IDF
source /home/felipe/esp/esp-idf/export.sh

# Entrar no diretório do projeto
cd /home/felipe/work/bocal-dinamico-esp32

# Limpar build anterior (recomendado após mudanças em partições)
idf.py fullclean

# Configurar target (apenas na primeira vez)
idf.py set-target esp32

# Compilar
idf.py build

# Gravar e monitorar
idf.py -p /dev/ttyUSB0 flash monitor
```

Para sair do monitor: `Ctrl+]`

## Primeiro Acesso

1. **Grave o firmware** no ESP32
2. O ESP32 inicia em modo **Access Point**
3. Conecte ao WiFi: **ESP32-Master** (senha: **12345678**)
4. Acesse no navegador: **http://192.168.4.1**
5. Configure o WiFi na aba "WiFi" para conectar à sua rede local

Após conectar a uma rede WiFi, o ESP32 receberá um IP do roteador. Verifique o IP no monitor serial ou no roteador.

## Configuração Persistente

As configurações são salvas em `/spiffs/config.json`:

```json
{
  "wifi": {
    "ssid": "MinhaRede",
    "password": "senha123",
    "ap_mode": false,
    "ap_ssid": "ESP32-Master",
    "ap_password": "12345678"
  },
  "rs485": {
    "baud": 19200,
    "tx_pin": 17,
    "rx_pin": 5,
    "de_pin": 18
  },
  "modbus": {
    "slave_id": 2,
    "timeout": 500
  },
  "web": {
    "username": "admin",
    "password": "admin",
    "auth_enabled": false
  }
}
```

## ESP32 Slave (LED Controller)

O projeto inclui suporte para comunicação com um ESP32 Slave (projeto separado em `bocal-dinamico-esp32-slave/`) que controla o LED built-in.

### Registradores do Slave (ID=2)

| Endereço | Nome | Acesso | Valores | Descrição |
|----------|------|--------|---------|-----------|
| 0x0000 | LED State | RW | 0=OFF, 1=ON | Liga/desliga o LED |
| 0x0001 | Blink Mode | RW | 0=Solid, 1=Blink | Modo contínuo ou pisca |
| 0x0002 | Blink Period | RW | 100-10000 (ms) | Período do pisca-pisca |
| 0x0003 | Device ID | R | 2 | ID do dispositivo |

## Funcionalidades do Driver mightyZAP

### API Principal

```c
// Inicialização
esp_err_t mightyzap_init(modbus_handle_t modbus, uint8_t slave_id, mightyzap_handle_t *handle);

// Controle de movimento
esp_err_t mightyzap_set_position(mightyzap_handle_t handle, uint16_t position);
esp_err_t mightyzap_set_speed(mightyzap_handle_t handle, uint16_t speed);
esp_err_t mightyzap_set_force_enable(mightyzap_handle_t handle, bool enable);

// Leitura de status
esp_err_t mightyzap_get_position(mightyzap_handle_t handle, uint16_t *position);
esp_err_t mightyzap_get_status(mightyzap_handle_t handle, mightyzap_status_t *status);
```

### Registradores Modbus do mightyZAP

| Endereço | Nome | Acesso | Descrição |
|----------|------|--------|-----------|
| 0x0080 | Force On/Off | RW | Habilita torque (0/1) |
| 0x0086 | Goal Position | RW | Posição alvo |
| 0x0087 | Goal Speed | RW | Velocidade alvo |
| 0x0096 | Present Position | R | Posição atual |
| 0x009F | Moving | R | Status de movimento |

## Troubleshooting

### ESP32 reinicia quando MAX485 está conectado

- Use os GPIOs seguros: TX=17, RX=5, DE=18
- Evite GPIO0, GPIO2, GPIO12, GPIO15, GPIO16

### Não consegue acessar a interface web

1. Verifique se está conectado ao WiFi correto
2. Em modo AP, use o IP 192.168.4.1
3. Verifique o monitor serial para ver o IP atribuído

### Slave não responde

1. Verifique se o Slave está ligado e no mesmo barramento
2. Confirme que os fios A/B não estão invertidos
3. Verifique se o baud rate é o mesmo (19200)
4. Aumente o timeout se necessário

### Erro de CRC

1. Use cabos blindados para RS485
2. Adicione resistores de terminação (120Ω) nas extremidades
3. Reduza o comprimento do cabo ou baud rate

## Licença

Este projeto é fornecido como exemplo educacional.

## Referências

- [mightyZAP User Manual](docs/FC_MODBUS_mightyZAP-User-Manual_ENG_23H08_V3.4/)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [Modbus Protocol Specification](https://modbus.org/specs.php)
