---
markmap:
  colorFreezeLevel: 2
  maxWidth: 320
---

# Malha Fechada de Controle

## Sensor Baumer OX100
### Conexão
- Protocolo: Modbus RTU (Barramento 1)
- Slave ID: configurável (padrão 10)
### Saídas
- `values[0]` — medição primária (gap)
- `values[1..3]` — medições secundárias
- `quality` — 0=OK, 1=sinal fraco, 2=sem sinal
- `status` — bits de status do sensor
- `output` — saídas binárias do sensor
### API
- `baumer_read_measurements(handle, &meas)`
- `baumer_set_laser(handle, enable)`

## Equações de Controle
### Modelo
- Uma equação por atuador
- Função linear: `pos = coeff_a × gap + coeff_b`
- `coeff_a` — ganho (unidades de posição por unidade de gap)
- `coeff_b` — offset de posição
### Configuração
- Persistida em `config.json → control.equations[]`
- `actuator_id` — ID do atuador alvo
- `bus` — 1 = primário (individual), 2 = sync (broadcast)
- `enabled` — ativa/desativa equação individualmente
### Exemplo
- Atuador 1: `pos = 100 × gap + 500`
- Gap medido = 12,3 → Position = 100 × 12,3 + 500 = **1730**

## Tarefa `control_loop_task`
### Ciclo de execução (500 ms)
- Verifica `control.running`
- Lê Baumer: `baumer_read_measurements`
- Extrai `values[measurement_index]`
- Para cada equação habilitada
  - Calcula posição alvo
  - Obtém handle do atuador por ID
  - Chama `mightyzap_set_position`
### Tratamento de erros
- Falha de leitura Baumer → loga e continua
- Falha Modbus → retry automático (3×)
- Sinal fraco (`quality != 0`) → loga aviso

## Estados do Loop
- **Stopped** — tarefa não executa ações de controle
- **Running** — ciclo ativo a cada `interval_ms`
- **Monitoring** — medição contínua sem atuação (após `POST /stop`)

## Configuração Persistida
```
control:
  running: false          ← auto-start no boot
  interval_ms: 500        ← frequência do loop
  measurement_index: 0    ← qual values[] usar
  equations:
    - actuator_id: 1
      coeff_a: 100.0
      coeff_b: 0.0
      enabled: true
      bus: 1
```

## API REST de Controle
- `POST /api/control/start` — inicia malha
- `POST /api/control/stop` — para atuação
- `GET /api/control/status` — gap atual, posições calculadas, estado
- `GET /api/control/equations` — equações ativas
- `POST /api/control/equations` — atualiza equações

## Diagrama de Fluxo
- Sensor Baumer → gap (mm)
- gap → equação linear → posição alvo (0–4095)
- posição alvo → Modbus FC06 (reg 0x0034)
- Atuador move para posição
- Sensor mede novo gap
- Loop fecha
