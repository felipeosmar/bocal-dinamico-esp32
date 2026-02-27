# Wiring - Bocal Dinamico ESP32

## ESP32 Pinout

Dois barramentos RS485 independentes, cada um com seu transceiver MAX485.

### Barramento Primario (UART1) - Sensores e Leitura

Usado para comunicacao geral: leitura de status dos atuadores mightyZAP e sensor Baumer OX100.

| ESP32 GPIO | MAX485 Pino | Funcao        |
|------------|-------------|---------------|
| GPIO 17    | DI          | TX (transmit) |
| GPIO 16    | RO          | RX (receive)  |
| GPIO 4     | DE + RE     | Direction     |

### Barramento Secundario (UART2) - Sincronizacao

Dedicado para comandos de movimento sincronizado dos atuadores.

| ESP32 GPIO | MAX485 Pino | Funcao        |
|------------|-------------|---------------|
| GPIO 18    | DI          | TX (transmit) |
| GPIO 19    | RO          | RX (receive)  |
| GPIO 21    | DE + RE     | Direction     |

### Configuracao Padrao

| Parametro | Valor  |
|-----------|--------|
| Baud rate | 57600  |
| Data bits | 8      |
| Parity    | None   |
| Stop bits | 1      |

### Dispositivos no Barramento

| Dispositivo       | Slave ID | Barramento   |
|-------------------|----------|--------------|
| mightyZAP (atuador) | 1     | Primario + Secundario |
| Baumer OX100 (sensor) | 8   | Primario     |

## Diagrama de Ligacao

```
                         Barramento Primario (UART1)
                        ┌─────────────────────────────────────────┐
                        │                                         │
  ESP32                 │  MAX485 #1            RS485 Bus         │
 ┌──────────┐          ┌┴──────────┐         ┌──────────┐        │
 │ GPIO 17 ─┼── TX ──> │ DI     A ├────┬───>│ mightyZAP │        │
 │ GPIO 16 <┼── RX ──  │ RO     B ├────┤    │  ID=1     │        │
 │ GPIO  4 ─┼── DIR ─> │ DE+RE    │    │    └──────────┘        │
 └──────────┘          └───────────┘    │    ┌──────────┐        │
                                        ├───>│ Baumer   │        │
                                        │    │ OX100    │        │
                                        │    │  ID=8    │        │
                                        │    └──────────┘        │
                                        │                        │
                         Barramento Secundario (UART2)            │
                        ┌─────────────────────────────────────────┘
                        │
  ESP32                 │  MAX485 #2            RS485 Bus
 ┌──────────┐          ┌┴──────────┐         ┌──────────┐
 │ GPIO 18 ─┼── TX ──> │ DI     A ├────────>│ mightyZAP │
 │ GPIO 19 <┼── RX ──  │ RO     B ├────────>│  ID=1     │
 │ GPIO 21 ─┼── DIR ─> │ DE+RE    │         │ (sync)    │
 └──────────┘          └───────────┘         └──────────┘
```

## Ligacao do MAX485

```
  MAX485
 ┌────────┐
 │ RO   1├──> ESP32 RX
 │ RE   2├──┐
 │ DE   3├──┴── ESP32 DE (GPIO)
 │ DI   4├──> ESP32 TX
 │ GND  5├──> GND
 │ A    6├──> RS485 A (+)
 │ B    7├──> RS485 B (-)
 │ VCC  8├──> 3.3V
 └────────┘
```

**RE e DE ligados juntos** no mesmo GPIO. Quando HIGH = transmitindo, quando LOW = recebendo.

## Notas

- UART0 e reservado para o console serial (GPIO 1 TX, GPIO 3 RX)
- Todos os pinos sao configuraveis via `config.json` na particao `userdata`
- Terminacao de 120 ohm entre A e B recomendada nas extremidades do barramento
- Alimentacao dos atuadores mightyZAP: 12V DC (separada do ESP32)
