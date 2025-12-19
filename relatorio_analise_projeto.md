# Relatório de Análise do Projeto ESP32 - Bocal Dinâmico

## Visão Geral do Projeto

Este é um projeto **ESP-IDF 5.5.0** para controle de atuadores mightyZAP via **RS485 Modbus RTU**, com interface web para configuração e controle. O sistema utiliza **FreeRTOS** para gerenciamento de tarefas e **LittleFS** para armazenamento persistente.

---

## Padrões Utilizados no Projeto

### ✅ Pontos Positivos Identificados

| Categoria | Padrão | Implementação |
|-----------|--------|---------------|
| **Arquitetura** | Modular com separação de responsabilidades | Módulos independentes: [rs485](file:///home/felipe/work/bocal-dinamico-esp32/main/rs485/rs485_driver.c#20-100), [modbus](file:///home/felipe/work/bocal-dinamico-esp32/main/modbus/modbus_rtu.c#67-88), [wifi](file:///home/felipe/work/bocal-dinamico-esp32/main/main.c#105-147), [config](file:///home/felipe/work/bocal-dinamico-esp32/sdkconfig), `webserver`, [mightyzap](file:///home/felipe/work/bocal-dinamico-esp32/main/mightyzap/mightyzap.c#18-38) |
| **Thread Safety** | Mutex em transações RS485 | `xSemaphoreCreateMutex()` em [rs485_driver.c](file:///home/felipe/work/bocal-dinamico-esp32/main/rs485/rs485_driver.c) |
| **Configuração** | Persistência em JSON | LittleFS com cJSON para config.json |
| **APIs** | Opaque pointers (handles) | `rs485_handle_t`, `modbus_handle_t`, `mightyzap_handle_t` |
| **Documentação** | Doxygen-style comments | Headers bem documentados |
| **Hardware** | GPIOs seguros (sem strapping pins) | TX=17, RX=5, DE=18 |
| **Comunicação** | Modbus RTU com CRC16 | Tabela de lookup para performance |

### ⚠️ Problemas Identificados

| Severidade | Problema | Localização |
|------------|----------|-------------|
| 🔴 Alta | Task Modbus sem handle (impossível parar) | [main.c:239-250](file:///home/felipe/work/bocal-dinamico-esp32/main/main.c#L239-L250) |
| 🔴 Alta | Autenticação HTTP retorna sempre `true` | [web_server.c:66](file:///home/felipe/work/bocal-dinamico-esp32/main/webserver/web_server.c#L66) |
| 🟡 Média | Config manager sem mutex (race conditions) | [config_manager.c:37](file:///home/felipe/work/bocal-dinamico-esp32/main/config/config_manager.c#L37) |
| 🟡 Média | Alocação dinâmica excessiva (cJSON) | Todos os handlers da web API |
| 🟡 Média | Sem watchdog nas tasks de comunicação | [modbus_task](file:///home/felipe/work/bocal-dinamico-esp32/main/main.c#167-185) |
| 🟢 Baixa | Variáveis globais compartilhadas (extern) | `g_modbus`, `g_actuator` |

---

## Recomendações para Ambiente Industrial

### 1. 🔧 Estabilidade e Prevenção de Travamentos

#### 1.1 Watchdog Task-Level

O ESP32 já tem Task WDT habilitado (5s), mas apenas monitora tasks IDLE:

```diff
// sdkconfig.defaults - ADICIONAR
+CONFIG_ESP_TASK_WDT_PANIC=y          // Reset em caso de timeout
+CONFIG_ESP_TASK_WDT_TIMEOUT_S=10     // Aumentar para 10s
```

**Implementar watchdog manual nas tasks:**

```c
// Em modbus_task() - exemplo
#include "esp_task_wdt.h"

static void modbus_task(void *pvParameters)
{
    // Registrar task no watchdog
    esp_task_wdt_add(NULL);
    
    while (1) {
        esp_task_wdt_reset();  // Alimentar o watchdog
        
        // ... código existente ...
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

#### 1.2 Stack Overflow Protection

```diff
// sdkconfig.defaults - ADICIONAR
+CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y
+CONFIG_FREERTOS_CHECK_STACKOVERFLOW=y
+CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
```

#### 1.3 Memory Leak Detection

```c
// Adicionar função de diagnóstico periódica
static void memory_monitor_task(void *pvParameters)
{
    while (1) {
        ESP_LOGI("MEMORY", "Free heap: %lu, Min free: %lu",
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size());
        
        // Alertar se heap baixo
        if (esp_get_free_heap_size() < 20000) {
            ESP_LOGW("MEMORY", "Low memory warning!");
        }
        
        vTaskDelay(pdMS_TO_TICKS(60000));  // A cada minuto
    }
}
```

---

### 2. 🔄 Prevenção de Reinicialização Espontânea

#### 2.1 Brownout Detection Ajustado

Atualmente em nível 0 (muito baixo). Para ambiente industrial:

```diff
// sdkconfig.defaults
-CONFIG_ESP_BROWNOUT_DET_LVL_SEL_0=y
+CONFIG_ESP_BROWNOUT_DET_LVL_SEL_4=y    // 2.90V - mais tolerante
```

#### 2.2 Panic Handler Robusto

```diff
// sdkconfig.defaults
+CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y
+CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=3
+CONFIG_ESP_PANIC_HANDLER_IRAM=y
```

#### 2.3 Registro de Crashes (Coredump)

```diff
// sdkconfig.defaults - ADICIONAR
+CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
+CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=64
+CONFIG_ESP_COREDUMP_STACK_SIZE=1024
```

**Adicionar partição de coredump:**

```csv
# partitions.csv - ADICIONAR linha
coredump,  data, coredump,  0x200000, 64K,
```

---

### 3. 🛡️ Robustez em Operação Contínua

#### 3.1 Soft Restart Agendado (Opcional)

Para operação 24/7, considerar restart preventivo semanal:

```c
#include "esp_system.h"
#include "esp_sntp.h"

// Agendar restart às 3:00 AM de domingo
static void check_scheduled_restart(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 3 && 
        timeinfo.tm_min == 0) {
        ESP_LOGW(TAG, "Scheduled weekly restart");
        config_save();  // Salvar config antes
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}
```

#### 3.2 Health Check Periódico

```c
typedef struct {
    bool modbus_ok;
    bool wifi_ok;
    bool filesystem_ok;
    uint32_t uptime_seconds;
    uint32_t error_count;
} system_health_t;

static system_health_t s_health = {0};

static void health_check_task(void *pvParameters)
{
    while (1) {
        s_health.uptime_seconds++;
        
        // Verificar WiFi
        s_health.wifi_ok = wifi_manager_is_connected();
        
        // Verificar Modbus (último poll bem sucedido há menos de 30s)
        // s_health.modbus_ok = ...
        
        // Verificar filesystem
        size_t total, used;
        s_health.filesystem_ok = (esp_littlefs_info("userdata", &total, &used) == ESP_OK);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

---

### 4. 🔌 Comunicação RS485/Modbus Industrial

#### 4.1 Retry com Backoff Exponencial

```c
#define MAX_RETRIES 3
#define BASE_DELAY_MS 100

esp_err_t modbus_read_with_retry(modbus_handle_t handle, uint8_t slave_addr,
                                  uint16_t start_reg, uint16_t num_regs,
                                  uint16_t *values)
{
    esp_err_t ret;
    int delay_ms = BASE_DELAY_MS;
    
    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        ret = modbus_read_holding_registers(handle, slave_addr, 
                                            start_reg, num_regs, values);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        
        ESP_LOGW(TAG, "Modbus retry %d/%d, delay %dms", 
                 retry + 1, MAX_RETRIES, delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        delay_ms *= 2;  // Backoff exponencial
    }
    
    return ret;
}
```

#### 4.2 Proteção contra Timeout Longos

```c
// Adicionar timeout máximo em transações
#define MODBUS_ABSOLUTE_TIMEOUT_MS 2000

static esp_err_t modbus_send_receive_safe(modbus_handle_t handle, ...)
{
    TickType_t start = xTaskGetTickCount();
    
    // ... operação ...
    
    if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(MODBUS_ABSOLUTE_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Modbus transaction exceeded absolute timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
```

#### 4.3 Verificação de Integridade do Barramento

```c
// Detectar barramento com problemas (CRC errors, timeouts frequentes)
typedef struct {
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t error_count;
    uint32_t crc_error_count;
    uint32_t timeout_count;
} modbus_stats_t;

static modbus_stats_t s_modbus_stats = {0};

// Expor via API REST para diagnóstico
```

---

### 5. 💾 Proteção de Dados

#### 5.1 Double-Buffer para Config

```c
// Evitar corrupção em caso de falha durante escrita
esp_err_t config_save_safe(void)
{
    // 1. Salvar em arquivo temporário
    esp_err_t ret = config_save_to_file("/userdata/config.tmp");
    if (ret != ESP_OK) return ret;
    
    // 2. Renomear arquivo antigo para backup
    rename(CONFIG_FILE, "/userdata/config.bak");
    
    // 3. Renomear temp para definitivo
    ret = rename("/userdata/config.tmp", CONFIG_FILE);
    if (ret != 0) {
        // Restaurar backup em caso de falha
        rename("/userdata/config.bak", CONFIG_FILE);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}
```

#### 5.2 Validação de Configuração

```c
static bool config_validate(const config_t *cfg)
{
    // Validar ranges
    if (cfg->rs485_baud != 9600 && cfg->rs485_baud != 19200 &&
        cfg->rs485_baud != 57600 && cfg->rs485_baud != 115200) {
        return false;
    }
    
    if (cfg->modbus_slave_id < 1 || cfg->modbus_slave_id > 247) {
        return false;
    }
    
    if (cfg->modbus_timeout < 50 || cfg->modbus_timeout > 5000) {
        return false;
    }
    
    // Validar GPIOs (não usar strapping pins)
    const uint8_t forbidden_pins[] = {0, 2, 5, 12, 15};
    // ... verificar ...
    
    return true;
}
```

---

### 6. 🌐 Melhoria do Web Server

#### 6.1 Autenticação Funcional

```c
#include "mbedtls/base64.h"

static bool check_auth(httpd_req_t *req)
{
    if (!s_config.auth_enabled) return true;
    
    char auth_header[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, 
                                     sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;
    
    // Decodificar base64
    char decoded[128];
    size_t decoded_len;
    int ret = mbedtls_base64_decode((unsigned char*)decoded, sizeof(decoded),
                                    &decoded_len, 
                                    (unsigned char*)(auth_header + 6),
                                    strlen(auth_header + 6));
    if (ret != 0) return false;
    decoded[decoded_len] = '\0';
    
    // Comparar com credenciais
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", 
             s_config.username, s_config.password);
    
    return strcmp(decoded, expected) == 0;
}
```

#### 6.2 Rate Limiting

```c
#define MAX_REQUESTS_PER_MINUTE 60

typedef struct {
    uint32_t count;
    TickType_t window_start;
} rate_limit_t;

static rate_limit_t s_rate_limit = {0};

static bool check_rate_limit(void)
{
    TickType_t now = xTaskGetTickCount();
    
    if ((now - s_rate_limit.window_start) > pdMS_TO_TICKS(60000)) {
        s_rate_limit.count = 0;
        s_rate_limit.window_start = now;
    }
    
    s_rate_limit.count++;
    return (s_rate_limit.count <= MAX_REQUESTS_PER_MINUTE);
}
```

---

### 7. 📊 Monitoramento e Diagnóstico

#### 7.1 Endpoint de Diagnóstico Completo

```c
// GET /api/diagnostics
static esp_err_t api_diagnostics_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    // Sistema
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block", 
                            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Reset reason
    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddNumberToObject(root, "reset_reason", reason);
    
    // Modbus stats
    cJSON *modbus = cJSON_CreateObject();
    cJSON_AddNumberToObject(modbus, "tx_count", s_modbus_stats.tx_count);
    cJSON_AddNumberToObject(modbus, "error_count", s_modbus_stats.error_count);
    cJSON_AddItemToObject(root, "modbus", modbus);
    
    // Tasks info
    char *task_list = malloc(2048);
    vTaskList(task_list);
    cJSON_AddStringToObject(root, "tasks", task_list);
    free(task_list);
    
    // ... enviar resposta ...
}
```

#### 7.2 Log Persistente (Ringbuffer)

```c
#define LOG_BUFFER_SIZE 4096
static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_pos = 0;
static SemaphoreHandle_t s_log_mutex = NULL;

static int persistent_log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < len && s_log_pos < LOG_BUFFER_SIZE - 1; i++) {
            s_log_buffer[s_log_pos++] = buf[i];
        }
        xSemaphoreGive(s_log_mutex);
    }
    
    return vprintf(fmt, args);  // Também imprimir no console
}

// Instalar: esp_log_set_vprintf(persistent_log_vprintf);
```

---

### 8. ⚡ Otimizações de Performance

#### 8.1 Reduzir Fragmentação de Memória

```c
// Usar buffers estáticos para operações frequentes
static char s_json_buffer[2048];  // Reutilizar para respostas JSON

// Pool de buffers para Modbus
#define MODBUS_BUFFER_POOL_SIZE 4
static uint8_t s_modbus_buffers[MODBUS_BUFFER_POOL_SIZE][256];
static bool s_buffer_in_use[MODBUS_BUFFER_POOL_SIZE] = {0};
```

#### 8.2 Prioridades de Task Otimizadas

```c
// Prioridades recomendadas para ambiente industrial
#define PRIORITY_MODBUS_TASK        (configMAX_PRIORITIES - 2)  // Alta
#define PRIORITY_HEALTH_CHECK_TASK  (tskIDLE_PRIORITY + 2)      // Baixa
#define PRIORITY_WEB_SERVER_TASK    (tskIDLE_PRIORITY + 3)      // Média
```

---

## Resumo de Alterações Prioritárias

### 🔴 Crítico (Implementar Imediatamente)

1. **Habilitar Task WDT com Panic** - Previne travamentos permanentes
2. **Corrigir autenticação HTTP** - Segurança básica está quebrada
3. **Adicionar mutex no config_manager** - Evitar race conditions

### 🟡 Importante (Próxima Iteração)

4. **Implementar retry com backoff** - Comunicação Modbus mais robusta
5. **Adicionar coredump** - Diagnóstico post-mortem
6. **Health check task** - Monitoramento contínuo

### 🟢 Melhoria Contínua

7. **Estatísticas de comunicação** - Diagnóstico de problemas
8. **Log persistente** - Análise de falhas
9. **Rate limiting** - Proteção contra DoS

---

## Configuração sdkconfig.defaults Recomendada

```ini
# ESP32 Target
CONFIG_IDF_TARGET="esp32"

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_CHECK_STACKOVERFLOW=y
CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y

# Watchdog
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_PANIC=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10

# Stack Protection
CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y

# UART
CONFIG_UART_ISR_IN_IRAM=y

# Panic Handler
CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y
CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=3

# Brownout (industrial)
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_ESP_BROWNOUT_DET_LVL_SEL_4=y

# Coredump
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y

# Log level
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_DEFAULT_LEVEL=3

# HTTP Server
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512

# WiFi
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y
```

---

## Conclusão

O projeto possui uma **base sólida** com arquitetura modular e uso adequado de handles opacos. As principais áreas que requerem atenção para operação industrial contínua são:

1. **Gestão de watchdog e recuperação de falhas**
2. **Thread safety completo no config manager**
3. **Mecanismos de retry e recuperação de comunicação**
4. **Monitoramento e diagnóstico proativo**

Com as melhorias sugeridas, o sistema estará preparado para operar 24/7 em ambiente industrial com alta disponibilidade.
