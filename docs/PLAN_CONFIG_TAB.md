# Planejamento: Tab de Configuração de Atuadores

## Visão Geral

Nova aba na interface web para configurar parâmetros persistentes (EEPROM) dos atuadores mightyZAP.

---

## 1. Layout da Interface

```
┌─────────────────────────────────────────────────────────────────────┐
│  [Actuators] [Config] [System] [Tasks] [Files]                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────┐  ┌─────────────────────────────────────────┐  │
│  │ ACTUATORS       │  │ CONFIGURATION - Actuator #3             │  │
│  │                 │  │                                         │  │
│  │ ┌─────────────┐ │  │ ─── Device Info (Read-Only) ───        │  │
│  │ │ #3  ●       │ │  │ Model:     17L-27F-27 (FC_MODBUS)      │  │
│  │ │ mightyZAP   │◄┼──│ Firmware:  v2.1                        │  │
│  │ │ Connected   │ │  │ Voltage:   70-130 (7.0V - 13.0V)       │  │
│  │ └─────────────┘ │  │                                         │  │
│  │                 │  │ ─── Communication ───                   │  │
│  │ ┌─────────────┐ │  │ Slave ID:     [ 3    ] (1-247)         │  │
│  │ │ #5  ○       │ │  │ Baud Rate:    [57600 ▼]                │  │
│  │ │ Disconnected│ │  │                                         │  │
│  │ └─────────────┘ │  │ ─── Stroke Limits ───                   │  │
│  │                 │  │ Short Limit:  [====●====] 0    (0-4095) │  │
│  │ [+ Add] [Scan] │  │ Long Limit:   [========●] 4095 (0-4095) │  │
│  │                 │  │                                         │  │
│  └─────────────────┘  │ ─── Performance Limits ───              │  │
│                       │ Speed Limit:   [=====●===] 400 (0-1023) │  │
│                       │ Current Limit: [====●====] 800 (0-1600) │  │
│                       │                                         │  │
│                       │ ─── Compliance (Positioning) ───        │  │
│                       │ Start Margin:  [●========] 7   (0-255)  │  │
│                       │ End Margin:    [●========] 2   (0-255)  │  │
│                       │                                         │  │
│                       │ ─── Alarms ───                          │  │
│                       │ Alarm LED:      [Overload ▼]            │  │
│                       │ Alarm Shutdown: [Overload ▼]            │  │
│                       │                                         │  │
│                       │ ─── Actions ───                         │  │
│                       │ [Save Config] [Reload] [Factory Reset]  │  │
│                       │ [Restart Actuator]                      │  │
│                       │                                         │  │
│                       │ ⚠️ Changes saved to EEPROM persist      │  │
│                       │    after power cycle                    │  │
│                       └─────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Parâmetros de Configuração

### 2.1 Somente Leitura (Info)

| Parâmetro | Registrador | Descrição |
|-----------|-------------|-----------|
| Model Number | 0x0000 | Modelo do atuador |
| Firmware Version | 0x0001 | Versão do firmware |
| Lowest Voltage | 0x0007 | Tensão mínima permitida |
| Highest Voltage | 0x0008 | Tensão máxima permitida |

### 2.2 Configuráveis (EEPROM)

| Parâmetro | Registrador | Range | Default | Descrição |
|-----------|-------------|-------|---------|-----------|
| Slave ID | 0x0002 | 1-247 | 1 | ID Modbus do atuador |
| Baud Rate | 0x0003 | 16/32/64/128 | 32 | Taxa de comunicação |
| Short Stroke Limit | 0x0005 | 0-4095 | 0 | Limite mínimo de curso |
| Long Stroke Limit | 0x0006 | 0-4095 | 4095 | Limite máximo de curso |
| Alarm LED | 0x0009 | bitmask | 32 | Alarmes que acendem LED |
| Alarm Shutdown | 0x000A | bitmask | 32 | Alarmes que desligam motor |
| Start Compliance | 0x000B | 0-255 | 7 | Margem inicial de posicionamento |
| End Compliance | 0x000C | 0-255 | 2 | Margem final de posicionamento |
| Speed Limit | 0x000D | 0-1023 | 1023 | Velocidade máxima |
| Current Limit | 0x000E | 0-1600 | 800 | Corrente máxima (mA) |

### 2.3 Baud Rates

| Valor | Baud Rate |
|-------|-----------|
| 16 (0x10) | 9600 |
| 32 (0x20) | 19200 |
| 64 (0x40) | 57600 |
| 128 (0x80) | 115200 |

### 2.4 Alarm Bits

| Bit | Valor | Descrição |
|-----|-------|-----------|
| 0 | 1 | Input Voltage Error |
| 1 | 2 | Reserved |
| 2 | 4 | Motor Position Error |
| 3 | 8 | Reserved |
| 4 | 16 | Overload Error |
| 5 | 32 | Overheating Error |

---

## 3. API Endpoints

### 3.1 Novos Endpoints

```
GET  /api/actuator/config?id=3     # Ler configuração completa
POST /api/actuator/config          # Salvar configuração
POST /api/actuator/restart         # Reiniciar atuador
POST /api/actuator/factory-reset   # Reset de fábrica
```

### 3.2 Estrutura de Dados

**GET /api/actuator/config?id=3**
```json
{
  "success": true,
  "id": 3,
  "info": {
    "model": 12345,
    "model_name": "17L-27F-27",
    "firmware": 21,
    "voltage_min": 70,
    "voltage_max": 130
  },
  "config": {
    "slave_id": 3,
    "baud_rate": 64,
    "short_stroke_limit": 0,
    "long_stroke_limit": 4095,
    "speed_limit": 400,
    "current_limit": 800,
    "start_compliance": 7,
    "end_compliance": 2,
    "alarm_led": 32,
    "alarm_shutdown": 32
  }
}
```

**POST /api/actuator/config**
```json
{
  "id": 3,
  "config": {
    "speed_limit": 500,
    "current_limit": 1000
  }
}
```

---

## 4. Arquivos a Criar/Modificar

### 4.1 Novos Arquivos

| Arquivo | Descrição |
|---------|-----------|
| `main/www/tabs/config.html` | Layout da aba (já existe, será atualizado) |
| `main/www/tabs/config.js` | Lógica da aba (já existe, será atualizado) |

### 4.2 Arquivos a Modificar

| Arquivo | Modificação |
|---------|-------------|
| `main/mightyzap/mightyzap.h` | Adicionar structs de config |
| `main/mightyzap/mightyzap.c` | Funções get/set config |
| `main/webserver/web_server.c` | Endpoints de config |

---

## 5. Implementação Backend (C)

### 5.1 Nova Struct

```c
typedef struct {
    // Read-only info
    uint16_t model;
    uint16_t firmware;
    uint16_t voltage_min;
    uint16_t voltage_max;
    
    // Configurable (EEPROM)
    uint8_t slave_id;
    uint8_t baud_rate;
    uint16_t short_stroke_limit;
    uint16_t long_stroke_limit;
    uint16_t speed_limit;
    uint16_t current_limit;
    uint8_t start_compliance;
    uint8_t end_compliance;
    uint8_t alarm_led;
    uint8_t alarm_shutdown;
} mightyzap_config_t;
```

### 5.2 Novas Funções

```c
esp_err_t mightyzap_get_config(mightyzap_handle_t handle, mightyzap_config_t *config);
esp_err_t mightyzap_set_config(mightyzap_handle_t handle, const mightyzap_config_t *config);
esp_err_t mightyzap_get_info(mightyzap_handle_t handle, mightyzap_config_t *info);
```

---

## 6. Fluxo de Usuário

1. Usuário acessa aba "Config"
2. Lista de atuadores é exibida à esquerda
3. Ao clicar em um atuador:
   - Sistema lê todos os registradores de config
   - Exibe valores nos campos
4. Usuário modifica valores
5. Ao clicar "Save Config":
   - Sistema valida valores
   - Escreve apenas registradores modificados
   - Mostra confirmação
6. Ao clicar "Factory Reset":
   - Confirmação obrigatória
   - Envia comando de reset
   - Aguarda reinício

---

## 7. Considerações de Segurança

1. **Mudança de ID**: Avisar que perderá comunicação
2. **Mudança de Baud**: Avisar que precisa reconfigurar ESP32
3. **Factory Reset**: Dupla confirmação
4. **Validação**: Todos os campos validados no frontend E backend

---

## 8. Estimativa de Tempo

| Tarefa | Horas |
|--------|-------|
| Backend: structs e funções mightyzap | 2h |
| Backend: endpoints web_server | 2h |
| Frontend: HTML layout | 1h |
| Frontend: JavaScript logic | 3h |
| Testes e ajustes | 2h |
| **Total** | **10h** |

---

## 9. Ordem de Implementação

1. [ ] Backend: Adicionar struct `mightyzap_config_t`
2. [ ] Backend: Implementar `mightyzap_get_config()`
3. [ ] Backend: Implementar `mightyzap_set_config()`
4. [ ] Backend: Endpoints `/api/actuator/config`
5. [ ] Frontend: Atualizar `config.html` com layout
6. [ ] Frontend: Atualizar `config.js` com lógica
7. [ ] Testes end-to-end
8. [ ] Documentação

---

## 10. Mockup Visual (ASCII)

```
┌────────────────────────────────────────────────────────────┐
│ ⚙️ Actuator Configuration                                  │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  Select Actuator:  [#3 - mightyZAP ▼]     [🔄 Refresh]    │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ 📋 Device Information                                │ │
│  ├──────────────────────────────────────────────────────┤ │
│  │ Model:     17L-27F-27 (FC_MODBUS)                    │ │
│  │ Firmware:  v2.1                                      │ │
│  │ Voltage:   7.0V - 13.0V                              │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ 🔌 Communication                                     │ │
│  ├──────────────────────────────────────────────────────┤ │
│  │ Slave ID        [  3  ]     ⚠️ Changing loses conn   │ │
│  │ Baud Rate       [57600 ▼]   ⚠️ Must match ESP32      │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ 📏 Stroke Limits                                     │ │
│  ├──────────────────────────────────────────────────────┤ │
│  │ Short Limit     [●━━━━━━━━━━━━━] 0                   │ │
│  │ Long Limit      [━━━━━━━━━━━━━●] 4095                │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ ⚡ Performance                                        │ │
│  ├──────────────────────────────────────────────────────┤ │
│  │ Speed Limit     [━━━━●━━━━━━━━━] 400                 │ │
│  │ Current Limit   [━━━━━━●━━━━━━━] 800 mA              │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ 🎯 Compliance Margins                                │ │
│  ├──────────────────────────────────────────────────────┤ │
│  │ Start Margin    [●━━━━━━━━━━━━━] 7                   │ │
│  │ End Margin      [●━━━━━━━━━━━━━] 2                   │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ 🚨 Alarms                                            │ │
│  ├──────────────────────────────────────────────────────┤ │
│  │ LED Alarm       [Overload + Overheat ▼]              │ │
│  │ Shutdown Alarm  [Overload ▼]                         │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐ │
│  │                                                      │ │
│  │  [💾 Save Config]  [🔄 Reload]  [🔧 Factory Reset]   │ │
│  │                                                      │ │
│  │               [🔌 Restart Actuator]                  │ │
│  │                                                      │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  ⚠️ Configuration changes are saved to actuator EEPROM   │
│     and persist after power cycle.                        │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

---

**Aprovado por:** ________________  
**Data:** ________________
