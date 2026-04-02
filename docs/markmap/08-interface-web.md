---
markmap:
  colorFreezeLevel: 2
  maxWidth: 320
---

# Interface Web

## Arquitetura Frontend
- SPA (Single Page Application)
- Carregamento dinâmico de módulos por aba
- Framework próprio em `core.js` (sem dependências externas)
- Armazenada na partição **www** (LittleFS, 256 KB)

## `core.js` — Framework Central
### Registro de módulos
- Lazy loading: aba só carrega ao ser ativada
- Cache de módulos já carregados
### Autenticação
- Basic Auth com modal de login
- Retry automático em 401
- Credenciais em memória de sessão
### API Client
- `apiFetch(url, options)` — wrapper sobre fetch
- Retry em 401 com re-auth
- Toast de erro automático em falha
### Estado de conexão
- Polling periódico ao ESP32
- Banner de aviso se offline
### Notificações
- Sistema de toast (info, erro, sucesso)

## `index.html` — Shell da SPA
- Container de abas (tab bar)
- Viewport para conteúdo dinâmico
- Carrega core.js e style.css

## Aba: Atuadores (`tabs/actuators`)
### Status em tempo real (polling 3 s)
- Posição atual, corrente, tensão
- Indicador de movimento (Moving)
- Alerta de dessincronização
### Controle individual
- Slider de posição (0–4095)
- Campo de velocidade (0–1023)
- Campo de corrente (0–1600 mA)
- Botão de acionamento assíncrono
### Controle sincronizado
- Slider de posição para broadcast
- Monitoramento de desync do grupo
- Botão de parada de emergência
### Gerenciamento
- Botão de varredura do barramento
- Adicionar / remover atuador
- Renomear atuador

## Aba: Sistema (`tabs/system`)
### Recursos do ESP32
- Heap livre e mínimo histórico
- Uptime em hh:mm:ss
- Info WiFi (SSID, IP, RSSI)
### Tarefas FreeRTOS
- Nome, estado, prioridade
- Uso de CPU (%)
- HWM de stack (bytes livres)
### Controles
- Auto-refresh (toggle)
- Botão de restart

## Aba: Setup (`tabs/setup`)
### Wizard de configuração inicial
- Smart scan: identifica dispositivos no barramento
- Atribuição de papéis (lens_a, lens_b, nozzle)
- Controles de jog para teste de movimento
- Padronização: define posições home
- Configuração de baud rate

## Aba: Config (`tabs/config`)
### Editor de configuração
- Edita config.json diretamente
- Salvar / recarregar do dispositivo
- Seções: WiFi, RS485, Modbus, Baumer, Controle, Papéis

## Aba: Arquivos (`tabs/files`)
### Gerenciador de arquivos LittleFS
- Navegação por diretórios
- Upload de arquivos (multipart)
- Download de arquivos
- Criar diretórios
- Deletar arquivos
- Editor de texto inline (para config.json)

## Aba: Profiler (`tabs/profiler`)
### Métricas de desempenho
- Uso de memória ao longo do tempo
- Traces de tarefas em tempo real
- Histórico de heap

## Fluxo de Comunicação Frontend → Backend
- Usuário interaje com UI
- `core.js apiFetch` monta requisição HTTP
- Basic Auth header adicionado (se habilitado)
- ESP32 recebe na tarefa `httpd`
- Handler valida auth → processa → responde JSON
- Frontend atualiza UI com resposta
