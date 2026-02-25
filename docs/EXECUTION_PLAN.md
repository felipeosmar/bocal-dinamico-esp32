# Plano de Execução — Code Review Fixes

**Data:** 2026-02-25
**Referência:** docs/CODE_REVIEW.md

---

## Sprint 1 — 🔴 Críticos (Segurança & Estabilidade)

### 1.1 [C1] Autenticação em todos os endpoints API
- **Arquivos:** `main/webserver/handlers/*.c`, `auth.c`
- **Tarefas:**
  1. Criar macro `REQUIRE_AUTH(req)` em `handlers.h`
  2. Criar `send_unauthorized(req)` helper retornando 401
  3. Adicionar `REQUIRE_AUTH(req)` no início de TODOS os API handlers
  4. Testar com auth habilitado e desabilitado
- **Estimativa:** 1h

### 1.2 [C7] Fix deadlock no config_load()
- **Arquivo:** `main/config/config_manager.c`
- **Tarefas:**
  1. Auditar todos os caminhos de erro em `config_load()`
  2. Garantir `xSemaphoreGive()` antes de todo `return ESP_FAIL`
  3. Aplicar mesmo padrão em `config_save()` se necessário
- **Estimativa:** 30min

### 1.3 [C6] Fix offset errado do www no flash.sh
- **Arquivo:** `flash.sh`
- **Tarefas:**
  1. Corrigir offset de `0x1D0000` para `0x1C0000`
  2. Verificar todas as funções (flash_www, flash_update)
- **Estimativa:** 10min

### 1.4 [C4] Thread safety no log_buffer vprintf hook
- **Arquivo:** `main/logs/log_buffer.c`
- **Tarefas:**
  1. Remover `static` do `line_buffer[256]` (mover pra stack)
  2. Verificar stack dos tasks que chamam ESP_LOG (mínimo 512 bytes extra)
- **Estimativa:** 15min

### 1.5 [C5] Fix dangling pointers no web_server_set_auth
- **Arquivo:** `main/webserver/web_server.c`
- **Tarefas:**
  1. Mudar `g_web_config.username/password` para `char[64]` em vez de `const char*`
  2. Usar `strncpy` no setter
- **Estimativa:** 20min

### 1.6 [C2] Config getters retornando ponteiros inseguros
- **Arquivo:** `main/config/config_manager.c`
- **Tarefas:**
  1. Documentar modelo de threading (single-writer: web server task)
  2. Para strings críticas (WiFi, passwords), mudar getters para copiar em buffer do caller
  3. Alternativa simples: aceitar risco e documentar (single writer + init before server start)
- **Estimativa:** 45min

### 1.7 [C3] Mutex global para acesso ao barramento RS485
- **Arquivos:** `api_setup.c`, `api_actuator.c`
- **Tarefas:**
  1. Criar mutex `g_bus_mutex` em `main.c`
  2. Lock/unlock em smart-scan, standardize, jog (que mudam baud)
  3. Lock/unlock em actuator control/status handlers
  4. Frontend: desabilitar polling de status durante scan
- **Estimativa:** 1.5h

---

## Sprint 2 — 🟠 Importantes (Robustez)

### 2.1 [I5] Check NULL em todas as alocações cJSON
- **Arquivos:** Todos os API handlers
- **Tarefas:**
  1. Grep todos `cJSON_Create*` e `cJSON_Print*`
  2. Adicionar check + resposta de erro 500 em cada um
  3. Criar helper `send_json_response(req, root)` que faz check, print, free
- **Estimativa:** 1.5h

### 2.2 [I2] Desbloquear HTTP server durante sync_move_wait
- **Arquivos:** `api_actuator.c`, `mightyzap.c`
- **Tarefas:**
  1. Mudar sync move para async: iniciar move, retornar imediatamente com `{ "moving": true }`
  2. Criar endpoint `GET /api/actuator/sync-status` pra polling
  3. Frontend: polling de status em vez de bloquear
- **Estimativa:** 2h

### 2.3 [I4] Remover credenciais do config.json versionado
- **Arquivo:** `config.json`, `.gitignore`
- **Tarefas:**
  1. Criar `config.json.example` com placeholders
  2. Adicionar `config.json` ao `.gitignore`
  3. Atualizar README
- **Estimativa:** 15min

### 2.4 [I7] Remover funções duplicadas no tasks.js
- **Arquivo:** `main/www/tabs/tasks.js`
- **Tarefas:**
  1. Remover `formatBytes()` e `formatUptime()` locais
  2. Adaptar para usar versões do `core.js`
- **Estimativa:** 20min

### 2.5 [I3] Thread safety no array de actuators
- **Arquivo:** `main/webserver/handlers/api_actuator.c`
- **Tarefas:**
  1. Verificar se HTTP server é single-threaded (ESP-IDF default = sim)
  2. Se sim: documentar e adicionar assert/comment
  3. Se não: adicionar mutex em add/remove/status
- **Estimativa:** 30min

### 2.6 [I6] Consumir body completo em erros de POST
- **Arquivos:** Todos os POST handlers
- **Tarefas:**
  1. Auditar todos os caminhos de erro pós-recv
  2. Garantir que usam `httpd_resp_send_err()` em vez de `return ESP_FAIL`
- **Estimativa:** 45min

### 2.7 [I1] Stats Modbus — snapshot atômico
- **Arquivo:** `main/modbus/modbus_rtu.c`
- **Tarefas:**
  1. Adicionar `modbus_get_stats_snapshot()` com critical section
  2. Usar no handler de diagnóstico
- **Estimativa:** 30min

---

## Sprint 3 — 🟡 Qualidade + 🔒 Segurança + ⚡ Performance

### 3.1 [N1] Eliminar magic numbers
- **Arquivos:** `mightyzap.h`, JS tabs
- **Tarefas:**
  1. Definir constantes em `mightyzap.h`
  2. Criar endpoint `GET /api/actuator/limits` retornando ranges
  3. Frontend: ler limites do backend em vez de hardcoded
- **Estimativa:** 1h

### 3.2 [N2] Desabilitar hex dump em produção
- **Arquivo:** `main/rs485/rs485_driver.c`
- **Tarefa:** Setar `RS485_DEBUG_HEX_DUMP` para `0`
- **Estimativa:** 2min

### 3.3 [N3+N4] Fix referências SPIFFS → LittleFS
- **Arquivos:** `config_manager.h`, `CMakeLists.txt`
- **Tarefas:**
  1. Atualizar comentários SPIFFS → LittleFS
  2. Trocar `spiffs` por `esp_littlefs` no REQUIRES
- **Estimativa:** 10min

### 3.4 [N5] Padronizar error handling nos handlers
- **Arquivos:** Todos os API handlers
- **Tarefa:** Converter para padrão `goto cleanup` consistente
- **Estimativa:** 1h

### 3.5 [N7] tasks.js usar registerModule
- **Arquivo:** `main/www/tabs/tasks.js`
- **Tarefas:**
  1. Usar `registerModule('tasks', initTasksTab)`
  2. Limpar interval ao sair da tab
- **Estimativa:** 20min

### 3.6 [N8] Frontend tratar 401 e mostrar login
- **Arquivo:** `main/www/core.js`
- **Tarefas:**
  1. Detectar `res.status === 401` na função `api()`
  2. Mostrar modal de login
  3. Salvar credenciais em `Authorization` header
- **Estimativa:** 1h

### 3.7 [N9] Validação de pinos GPIO
- **Arquivo:** `main/config/config_manager.c`
- **Tarefa:** Validar GPIO range nos setters de pinos RS485
- **Estimativa:** 20min

### 3.8 [N12] Cache headers em arquivos estáticos
- **Arquivo:** `main/webserver/handlers/static_files.c`
- **Tarefa:** Adicionar `Cache-Control: max-age=3600` para CSS/JS
- **Estimativa:** 10min

### 3.9 [S3] Proteger partição www contra escrita via API
- **Arquivo:** `main/webserver/handlers/api_files.c`
- **Tarefa:** Bloquear write/delete em arquivos da partição www
- **Estimativa:** 20min

### 3.10 [S4] Rate limiting na autenticação
- **Arquivo:** `main/webserver/handlers/auth.c`
- **Tarefas:**
  1. Contador de tentativas falhadas
  2. Delay progressivo (1s, 2s, 4s...) após 3 falhas
  3. Reset após sucesso
- **Estimativa:** 30min

### 3.11 [P1] Log viewer incremental
- **Arquivo:** `main/www/core.js`
- **Tarefa:** Append novos logs ao DOM em vez de rebuild total
- **Estimativa:** 45min

### 3.12 [P3] Cache de resposta para status polling
- **Arquivo:** `api_actuator.c`
- **Tarefa:** Buffer pre-formatado, rebuild só quando estado muda
- **Estimativa:** 1h

### 3.13 [N10] Aumentar stack do health monitor
- **Arquivo:** `main/health/health_monitor.c`
- **Tarefa:** Aumentar de 2048 para 3072 bytes
- **Estimativa:** 2min

### 3.14 [N11] Fix variable shadowing no sync_move
- **Arquivo:** `main/webserver/handlers/api_actuator.c`
- **Tarefa:** Renomear variável local shadowed
- **Estimativa:** 5min

---

## Resumo

| Sprint | Items | Foco | Estimativa |
|--------|-------|------|-----------|
| **1** | 7 | 🔴 Segurança & bugs críticos | ~4h |
| **2** | 7 | 🟠 Robustez & correções | ~5.5h |
| **3** | 14 | 🟡 Qualidade, segurança, performance | ~6.5h |
| **Total** | 28 | | **~16h** |

---

## Ordem de execução sugerida

**Primeiro:** Sprint 1 completo (segurança e estabilidade)
**Depois:** Sprint 2 (robustez)
**Por último:** Sprint 3 (polish)

Dentro de cada sprint, os items estão ordenados por impacto.
