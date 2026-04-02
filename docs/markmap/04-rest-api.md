---
markmap:
  colorFreezeLevel: 2
  maxWidth: 360
---

# REST API

## Atuadores `/api/actuator`
### Leitura
- `GET /status` — posição, corrente, tensão de todos
- `GET /config?id=N` — config EEPROM (modelo, limites, baud)
- `GET /scan` — varre barramento (IDs 1–20)
- `GET /roles` — papéis: lens_a, lens_b, nozzle
- `GET /sync-status` — dessincronização e posições do grupo
### Controle
- `POST /control` — `{id, position, speed, current}` (async queue)
- `POST /sync-move` — mover grupo sincronizado
- `POST /sync-abort` — parar grupo (emergência)
- `POST /jog` — movimento de teste
- `POST /standardize` — definir posição home
### Configuração
- `POST /config` — escreve EEPROM (slave_id, baud, limites)
- `POST /roles` — atribui papéis a IDs
- `POST /add` — adiciona atuador ao tracking
- `POST /remove` — remove atuador do tracking
- `POST /set-name` — renomeia atuador
- `POST /restart` — reinicia atuador
- `POST /factory-reset` — reset de fábrica
- `POST /smart-scan` — identifica topologia do barramento

## Controle de Malha Fechada `/api/control`
- `GET /status` — estado, último gap, posições calculadas
- `POST /start` — inicia loop de controle
- `POST /stop` — para loop (monitoramento continua)
- `GET /equations` — equações lineares configuradas
- `POST /equations` — define `pos = a × gap + b` por atuador

## Sensor Baumer `/api/baumer`
- `GET /status` — medições atuais (4 valores, qualidade, status)
- `POST /laser` — habilita/desabilita laser

## RS485 `/api/rs485`
- `GET /config` — pinos e baud de ambos os barramentos
- `PUT /config` — atualiza pinos/baud (requer restart)
- `GET /diag` — estatísticas Modbus (TX/RX, erros, timeouts)
- `POST /test` — envia comando de teste
- `POST /reset_stats` — zera contadores

## WiFi `/api/wifi`
- `GET /scan` — redes disponíveis
- `POST /connect` — `{ssid, password}` → salva config
- `GET /status` — estado, IP, SSID, RSSI

## Sistema `/api`
- `GET /status` — heap livre, WiFi, uptime
- `GET /tasks` — lista tarefas FreeRTOS com CPU%, HWM de stack
- `GET /logs?since=N` — entradas do buffer de log (base em sequência)
- `POST /logs/clear` — limpa buffer de log
- `POST /restart` — reinicia o ESP32

## Arquivos `/api/files`
- `GET /list?dir=path` — listagem de diretório
- `GET /info?file=path` — metadados do arquivo
- `GET /download?file=path` — download
- `GET /read?file=path` — lê texto
- `POST /write` — escreve texto
- `POST /delete` — deleta arquivo
- `POST /mkdir` — cria diretório
- `POST /upload` — upload multipart

## Setup Wizard `/api/setup`
- `POST /scan-buses` — varre ambos os barramentos

## Arquivos Estáticos
- `GET /` → index.html
- `GET /style.css` → estilos
- `GET /core.js` → framework SPA
- `GET /tabs/*.html` → conteúdo das abas
- `GET /tabs/*.js` → módulos das abas

## Segurança
- Basic Auth (opcional, `web.auth_enabled`)
- Credenciais em config.json (LittleFS)
- Sem HTTPS (usar proxy reverso se público)
- HTTP na porta 80
