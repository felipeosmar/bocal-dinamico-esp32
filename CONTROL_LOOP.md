# Control Loop — Perfilômetro → Atuadores

## Visão Geral

O control loop lê continuamente o Baumer OX100 (perfilômetro) e calcula posições para os atuadores mightyZAP usando equações lineares configuráveis.

```
Baumer OX100 (leitura a cada N ms)
    │
    ├── Equação bus=2 → posição da LENTE → 2 atuadores sincronizados (broadcast)
    │
    └── Equação bus=1 → posição do BICO → 1 atuador individual
```

## Configuração JSON

No `config.json`, a seção de controle fica assim:

```json
{
  "control": {
    "running": false,
    "interval_ms": 500,
    "measurement_index": 0,
    "equations": [
      {
        "actuator_id": 1,
        "a": 200.0,
        "b": 500.0,
        "enabled": true,
        "bus": 1
      },
      {
        "actuator_id": 0,
        "a": 150.0,
        "b": 300.0,
        "enabled": true,
        "bus": 2
      }
    ]
  }
}
```

## Campos

### `running`
- `true` = control loop ativo (movimenta atuadores automaticamente)
- `false` = parado (monitoramento do Baumer continua, mas não move nada)

### `interval_ms`
Intervalo do loop de controle em milissegundos.
- **Mínimo:** 100ms
- **Máximo:** 60000ms (1 minuto)
- **Recomendado:** 200-500ms

Valores menores = resposta mais rápida, mas mais tráfego no barramento RS485.

### `measurement_index`
Seleciona **qual dos 4 valores** retornados pelo Baumer OX100 será usado como entrada das equações.

| Índice | Descrição |
|--------|-----------|
| 0 | Valor medido 1 (measurement tool 1) |
| 1 | Valor medido 2 (measurement tool 2) |
| 2 | Valor medido 3 (measurement tool 3) |
| 3 | Valor medido 4 (measurement tool 4) |

O significado de cada valor depende de como os measurement tools foram configurados na interface web do Baumer (gap, altura, largura, edge, etc.).

### `equations`
Array de equações lineares. Cada equação mapeia o valor do Baumer para uma posição de atuador.

#### Fórmula

```
posição = a × valor_baumer + b
```

O resultado é limitado automaticamente entre 0 e 4095 (range do mightyZAP).

#### Campos da equação

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `actuator_id` | int | ID Modbus do atuador. Para `bus: 2` pode ser qualquer valor (broadcast ignora ID). |
| `a` | float | Coeficiente multiplicador (inclinação da reta). |
| `b` | float | Offset (posição base quando o valor do Baumer é zero). |
| `enabled` | bool | `true` para ativar, `false` para desativar sem remover. |
| `bus` | int | **1** = atuador do bico (individual, bus principal) / **2** = par da lente (broadcast, bus sync). |

#### Diferença entre bus 1 e bus 2

| | Bus 1 | Bus 2 |
|---|---|---|
| **Uso** | Atuador do bico (deposição) | Par de atuadores da lente |
| **Barramento** | UART1 (compartilhado com Baumer) | UART2 (dedicado para sync) |
| **Envio** | Comando individual por `actuator_id` | Broadcast (ID 0) — todos no bus recebem |
| **Atuadores** | 1 | 2 (movem juntos, mesma posição) |

### Exemplo de cálculo

Se o Baumer lê um gap de **5.0 mm** e a equação é `a=200, b=500`:

```
posição = 200 × 5.0 + 500 = 1500
```

O atuador vai para a posição 1500 (de 0 a 4095).

## API REST

| Endpoint | Método | Descrição |
|----------|--------|-----------|
| `/api/control/status` | GET | Status atual + equações + posições computadas |
| `/api/control/start` | POST | Iniciar controle automático |
| `/api/control/stop` | POST | Parar controle (monitoramento continua) |
| `/api/control/interval` | PUT | Alterar intervalo: `{"interval_ms": 500}` |
| `/api/control/equation` | PUT | Configurar equação (ver exemplo abaixo) |
| `/api/control/measurement_index` | PUT | Selecionar valor: `{"measurement_index": 0}` |

### Exemplo: configurar equação via API

```bash
# Equação do bico (bus 1, atuador ID 1)
curl -X PUT http://<ESP32_IP>/api/control/equation \
  -d '{"actuator_id": 1, "a": 200.0, "b": 500.0, "enabled": true, "bus": 1}'

# Equação da lente (bus 2, broadcast)
curl -X PUT http://<ESP32_IP>/api/control/equation \
  -d '{"actuator_id": 0, "a": 150.0, "b": 300.0, "enabled": true, "bus": 2}'

# Iniciar controle
curl -X POST http://<ESP32_IP>/api/control/start

# Ver status
curl http://<ESP32_IP>/api/control/status
```

## Calibração dos Coeficientes

Para determinar `a` e `b`, faça duas medições:

1. Coloque o objeto em uma posição conhecida → anote o valor do Baumer (`v1`) e a posição desejada do atuador (`p1`)
2. Coloque em outra posição → anote `v2` e `p2`

Calcule:
```
a = (p2 - p1) / (v2 - v1)
b = p1 - a × v1
```

## Proteções

- **Sinal fraco/ausente:** se o Baumer reporta `quality = NO_SIGNAL` ou dados inválidos, os atuadores NÃO são movidos
- **Erros consecutivos:** após 5 falhas seguidas na leitura do Baumer, o control loop para automaticamente
- **Dedup:** se a posição calculada é igual à anterior, o comando não é reenviado (evita tráfego desnecessário)
- **NaN/Inf:** valores float inválidos do Baumer são descartados
- **Clamp:** posições são limitadas a 0-4095 independente do resultado da equação
