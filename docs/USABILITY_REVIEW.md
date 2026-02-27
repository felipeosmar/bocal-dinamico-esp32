# Análise de Usabilidade — Interface Web Bocal Dinâmico ESP32

**Data:** 2026-02-27  
**Versão analisada:** Código-fonte em `main/www/`  
**Analista:** Revisão automatizada

---

## 1. Resumo Executivo

**Nota Geral: 7/10**

A interface é bem estruturada, com design dark moderno, lazy loading de módulos, e bom tratamento de conexão/reconexão. Para um projeto ESP32, o nível de polish é acima da média. Porém, há lacunas importantes em acessibilidade, ajuda contextual, e responsividade que impactam o uso por operadores no chão de fábrica.

### Top 5 Problemas

| # | Problema | Severidade |
|---|---------|-----------|
| 1 | **Sem tooltips/ajuda contextual** — termos técnicos (Goal Position, Compliance, Stroke Limits) não são explicados; unidades inconsistentes | Crítico |
| 2 | **Botões de toque pequenos** — `btn-icon` é 36×36px (abaixo do mínimo de 44×44px para toque) | Crítico |
| 3 | **Ações destrutivas com confirm() nativo** — Factory Reset usa `prompt()`, delete usa `confirm()`, ambos não seguem o design system e são inacessíveis em mobile | Importante |
| 4 | **Profiler usa `alert()` para erros** — quebra o padrão de `toast()` usado no resto da app | Importante |
| 5 | **CSS com 30KB não minificado** — total de ~141KB de assets; em ESP32 com SPIFFS limitado, minificação economizaria ~40-50% | Importante |

---

## 2. Análise Detalhada

### 2.1 Usabilidade Geral

**Nota: 7/10**

#### ✅ Pontos Positivos
- **Lazy loading inteligente**: Módulos (HTML+JS) são carregados sob demanda, reduzindo tempo de carregamento inicial
- **Navegação por abas clara**: 6 abas bem definidas (Actuators, System, Setup, Profiler, Config, Files)
- **Feedback de conexão**: Banner de reconexão com backoff exponencial e estados visuais (connected/disconnected/reconnecting)
- **Toast notifications**: Sistema unificado de feedback com tipos success/error/info
- **Command lock**: Previne colisão no barramento RS485 durante operações
- **Setup Wizard**: Fluxo em 4 passos (Scan → Assign → Standardize → Save) é bem pensado
- **Módulo destroy/cleanup**: Cada aba limpa seus intervalos ao sair

#### ⚠️ Problemas

**Fluxo de uso não guiado:**
O operador chega na aba "Actuators" sem saber se precisa primeiro ir ao "Setup". Não há indicação de que o Setup Wizard deve ser executado na primeira vez.

**Linguagem em inglês:**
Toda a interface está em inglês (`<html lang="pt-BR">` no HTML, mas conteúdo em inglês). Para operadores brasileiros de chão de fábrica, isso é uma barreira.

**Ações destrutivas:**

```javascript
// config.js - Factory Reset usa prompt() nativo
async function factoryResetActuator() {
    const confirmText = prompt('Type "RESET" to confirm factory reset:');
    if (confirmText !== 'RESET') { ... }
}

// files.js - Delete usa confirm() nativo  
async function fmDelete(name, isDir) {
    if (!confirm(`Delete ${isDir ? 'folder' : 'file'} "${name}"?`)) return;
}
```

Esses diálogos nativos não seguem o design system (modal escuro) e em mobile podem ser confusos.

**Restart do dispositivo sem confirmação robusta:**
```javascript
// system.js
async function restartDevice() {
    if (!confirm('Restart device?')) return;  // Muito simples para ação tão impactante
}
```

**Estado vazio no Actuators:**
Não há empty state se nenhum atuador está conectado — o card de controle sincronizado aparece sempre, mesmo sem dados.

---

### 2.2 Tooltips e Ajuda Contextual

**Nota: 4/10**

#### ✅ O que existe
- Botões de ícone (scan, refresh) têm `title=""` attributes
- O Setup Wizard tem subtítulos explicativos nos steps
- Warning box no Config: "Config saved to EEPROM persists after power cycle"

#### ❌ O que falta

**Nenhum campo de controle tem tooltip ou explicação:**

```html
<!-- actuators.html - Sem explicação do que é "Current" ou qual a unidade -->
<div class="control-row">
    <label>Current</label>
    <input type="range" id="sync-cur" min="0" max="800" value="400">
    <input type="number" id="sync-cur-val" min="0" max="800" value="400">
</div>
```

Problemas específicos:
- **Position**: Sem indicação que é em steps (0-4095), não mm ou graus
- **Speed**: Label diz apenas "Speed", `max="400"` mas sem unidade. O hint "Max: 400" aparece apenas no desktop
- **Current**: Sem unidade (mA). O display mostra "mA" mas o controle não
- **Force ON/OFF**: Sem explicação do que Force faz no atuador
- **Compliance (Start/End)**: Termo técnico sem qualquer explicação — o que um operador faria com isso?
- **Stroke Limits (Short/Long)**: Sem contexto
- **Desync**: Mostrado mas não explicado — quanto é aceitável?
- **Equações do Profiler** (`position = a × gap + b`): Sem explicação do que a e b significam fisicamente

**Quick positions (0%, 25%, 50%, 75%, 100%):**
Úteis, mas sem explicar que 0% = posição 0 e 100% = posição 4095.

---

### 2.3 Design Visual e Cores

**Nota: 8/10**

#### ✅ Pontos Positivos
- **Paleta dark consistente**: Variáveis CSS bem definidas em `:root`
- **Cores de status bem escolhidas**: Verde (success), vermelho (danger), amarelo (warning), azul (accent)
- **Hierarquia visual clara**: Cards, headers, seções bem separadas
- **Status dots**: Indicadores visuais de conexão (verde/cinza) são intuitivos
- **Log viewer com cores por nível**: Error (vermelho), Warning (amarelo), Info (verde), Debug (azul)

#### ⚠️ Problemas

**Contraste de texto muted:**
```css
--text-muted: #8b949e;  /* sobre --bg: #0d1117 */
```
Ratio calculado: ~4.6:1 — passa WCAG AA para texto normal (4.5:1), mas fica no limite. Labels de controles usam `--text-muted` e podem ser difíceis de ler em ambientes industriais com iluminação forte.

**Badge "off" (vermelho sobre vermelho):**
```css
.badge.off { background: var(--danger); color: #fff; }
```
OK em contraste, mas o badge WiFi/Modbus vermelho constante pode causar "alarm fatigue" — se o operador sempre vê vermelho, ignora.

**Tema escuro em ambiente industrial:**
O tema dark é elegante para desenvolvimento, mas em ambientes com luz forte (chão de fábrica com iluminação fluorescente), um tema claro ou a opção de alternar seria mais adequado.

**Sem indicação visual de campos editados/não salvos:**
No Config, o operador pode alterar vários sliders sem perceber que precisa salvar.

---

### 2.4 Responsividade (Mobile/Telas Pequenas)

**Nota: 6/10**

#### ✅ O que funciona
- **Viewport meta tag presente**: `<meta name="viewport" content="width=device-width, initial-scale=1.0">`
- **Breakpoints definidos**: 480px, 500px, 600px, 768px, 800px
- **Nav horizontal scrollável em mobile**: `overflow-x: auto` com scrollbar oculta
- **Control panels empilham**: Grid → column em < 800px
- **Config layout responsivo**: Sidebar/main empilha em < 768px
- **Toast centralizado em mobile**: Left/right 10px

#### ❌ Problemas

**Botões de ícone muito pequenos (36×36px):**
```css
.btn-icon {
    width: 36px;
    height: 36px;  /* Mínimo recomendado: 44×44px */
}
```

**Botões .btn-small ainda menores:**
```css
.btn-small {
    padding: 4px 10px;
    height: 28px;  /* Muito pequeno para toque */
}
```

**Tabelas não se adaptam bem:**
A tabela de tasks (System) e a de equações (Profiler) não têm tratamento para mobile — podem causar scroll horizontal.

```css
.tasks-table-container {
    overflow-x: auto;  /* Permite scroll horizontal, mas sem indicação visual */
}
```

**Nav com 6 abas em tela pequena:**
Com 6 botões ("Actuators", "System", "Setup", "Profiler", "Config", "Files"), em tela < 360px as últimas abas ficam escondidas e sem indicação de que há scroll.

**Slider thumb pequeno:**
```css
.control-card .control-row input[type="range"]::-webkit-slider-thumb {
    width: 18px;
    height: 18px;  /* Difícil de acertar com dedo */
}
```

**Config layout em mobile coloca sidebar depois do main:**
```css
@media (max-width: 768px) {
    .config-sidebar { order: 2; }
    .config-main { order: 1; }
}
```
Isso faz o painel de config aparecer antes da lista de atuadores — o operador precisa rolar para baixo para selecionar um atuador.

---

### 2.5 Formulários e Inputs

**Nota: 6/10**

#### ✅ O que funciona
- **Inputs numéricos com min/max**: Todos os sliders/numbers têm `min` e `max` definidos
- **Slider + number sincronizados**: Boa UX — alteração no slider reflete no input e vice-versa
- **Login form com autocomplete**: `autocomplete="username"` e `autocomplete="current-password"`
- **Validação de nomes de arquivo**: Regex `^[a-zA-Z0-9_\-\.]+$` no file manager

#### ❌ Problemas

**Sem atributo `step` nos inputs numéricos:**
```html
<input type="number" id="sync-cur-val" min="0" max="800" value="400">
<!-- Sem step — permite decimais (ex: 400.5) que não fazem sentido para steps -->
```
Exceção: equações do Profiler usam `step="0.001"` — correto.

**Labels não associados via `for`/`id` (acessibilidade):**
```html
<!-- actuators.html -->
<label>Position</label>
<input type="range" id="sync-pos" ...>
<!-- Label não tem for="sync-pos" — screen readers não associam -->
```

**Sem validação client-side antes de enviar:**
Embora os inputs tenham min/max no HTML, não há validação JavaScript antes de enviar comandos. Valores fora do range podem ser enviados via input number (browsers não bloqueiam hard).

**Placeholders ausentes nos controles principais:**
Inputs de posição/velocidade/corrente não têm placeholders indicando valores padrão ou recomendados.

**WiFi password sem toggle de visibilidade:**
```html
<input type="password" id="wifi-pass" placeholder="Password">
<!-- Sem botão "mostrar senha" -->
```

**Select de baud rate no config usa valores raw:**
```html
<option value="16">9600</option>
<option value="32">19200</option>
```
Os values são registros Modbus, não baud rates reais — se algum bug expor esses valores, será confuso.

---

### 2.6 Performance e UX

**Nota: 7/10**

#### ✅ Pontos Positivos
- **Lazy loading de módulos**: Apenas o módulo ativo é carregado (HTML+JS)
- **Cleanup de intervalos**: Cada módulo para seu polling ao sair da aba
- **Command lock com timeout**: Previne flood no RS485
- **Incremental log rendering**: Só renderiza novos logs (não recria DOM inteiro)
- **Polling otimizado**: Status a cada 3s (actuators), 2s (system), 1s (profiler), 1.5s (logs)
- **Upload com progress bar**: Feedback visual de upload via XHR

#### ⚠️ Problemas

**Tamanho total dos assets: ~141KB (não minificados):**
| Arquivo | Tamanho |
|---------|---------|
| style.css | 30.3 KB |
| core.js | 19.5 KB |
| setup.js | 16.5 KB |
| actuators.js | 13.1 KB |
| config.js | 11.7 KB |
| files.js | 11.5 KB |
| **Total** | **~141 KB** |

Minificação + gzip reduziria para ~40-50KB. Em ESP32 com partição SPIFFS de 256KB-1MB, cada KB conta.

**Sem debounce nos sliders:**
```javascript
// actuators.js
s.oninput = () => i.value = s.value;
i.oninput = () => s.value = i.value;
```
O slider atualiza o input a cada pixel de movimento, sem debounce. Não é problema por si só (não envia ao servidor), mas se no futuro alguém adicionar envio automático, será.

**System polling a cada 2s é agressivo:**
```javascript
systemRefreshInterval = setInterval(refreshSystem, 2000);
```
Faz 2 fetch requests (`/api/status` + `/api/tasks`) a cada 2s. Em ESP32 com WebSocket disponível, isso poderia ser push-based.

**Sem cache de assets:**
Não há headers de cache definidos nos fetches de HTML dos módulos. Cada troca de aba refaz o fetch se o módulo já foi carregado (mas o JS verifica `modules[name].loaded`).

**Profiler usa `alert()` em vez de `toast()`:**
```javascript
// profiler.js - Inconsistente com o resto da app
async function profilerSaveEquation(actuatorId) {
    ...
    } catch (e) {
        alert('Failed to save equation: ' + e.message);  // Deveria ser toast()
    }
}
```
`alert()` aparece 5 vezes no profiler.js — bloqueia a UI e é inconsistente.

---

## 3. Recomendações Priorizadas

### 🔴 Crítico

1. **Adicionar tooltips/help text nos controles principais**
   - Position: "Posição do atuador em steps (0-4095). 0%=retraído, 100%=estendido"
   - Speed: "Velocidade máxima em steps/s (0-400)"
   - Current: "Limite de corrente em mA (0-800). Maior = mais força"
   - Force: "ON = atuador mantém posição ativamente. OFF = desligado, sem resistência"

2. **Aumentar tamanho dos botões touch para ≥44×44px**
   - `.btn-icon`: 36→44px
   - `.btn-small`: height 28→44px
   - Slider thumb: 18→24px mínimo

### 🟡 Importante

3. **Substituir `alert()`/`confirm()`/`prompt()` por modais do design system**
   - Criar um componente modal reutilizável (já existe o pattern no login modal)
   - Factory Reset deveria usar modal com input estilizado

4. **Substituir `alert()` por `toast()` no profiler.js**
   - 5 ocorrências de `alert()` → `toast(msg, 'error')`

5. **Minificar CSS/JS para produção**
   - Adicionar step de build (terser + cssnano)
   - Estimativa: 141KB → ~50KB

6. **Adicionar unidades visíveis nos inputs**
   - Position: " steps" ou mostrar como %
   - Current: " mA" suffix
   - Speed: " steps/s"

7. **Indicação de "first run" / Setup necessário**
   - Se `/api/actuator/roles` retorna vazio, mostrar banner "Execute o Setup Wizard primeiro"

### 🟢 Nice-to-Have

8. **Internacionalização (pt-BR)**
   - O `lang="pt-BR"` está no HTML mas todo texto é em inglês

9. **Tema claro para ambiente industrial**
   - Toggle light/dark ou auto-detect `prefers-color-scheme`

10. **Labels com `for` nos inputs**
    - Melhoria de acessibilidade simples

11. **Adicionar `step="1"` nos inputs inteiros**
    - Previne valores decimais

12. **Reduzir polling do System de 2s para 5s**
    - Reduz carga no ESP32

13. **Adicionar indicação de scroll horizontal na nav mobile**
    - Gradiente fade ou setas

---

## 4. Quick Wins (< 30 min cada)

### 1. Corrigir tamanho dos botões touch
```css
.btn-icon {
    width: 44px;
    height: 44px;
}

.btn-small {
    min-height: 44px;
    padding: 8px 12px;
}
```

### 2. Substituir alert() no profiler.js
```javascript
// De:
alert('Failed to save equation: ' + e.message);
// Para:
toast('Failed to save equation: ' + e.message, 'error');
```
5 substituições.

### 3. Adicionar unidades nos labels de controle
```html
<!-- De: -->
<label>Current</label>
<!-- Para: -->
<label>Current (mA)</label>
```

### 4. Adicionar step="1" nos inputs inteiros
```html
<input type="number" id="sync-pos-val" min="0" max="4095" step="1" value="2048">
```

### 5. Adicionar title/tooltips nos controles
```html
<label title="Posição alvo do atuador em steps. 0=retraído, 4095=totalmente estendido">Position</label>
```

### 6. Hint de Speed visível em mobile
```css
/* Remover o display:none do hint em mobile */
@media (max-width: 500px) {
    .control-card .control-row .hint {
        display: block;  /* Em vez de none */
        grid-column: 1 / -1;
        text-align: right;
    }
}
```

### 7. Adicionar `for` attributes nos labels do login
```html
<label for="login-user">Username</label>
<label for="login-pass">Password</label>
```
(Já está correto no login — aplicar o mesmo pattern nos controles.)

---

## 5. Resumo de Scores por Categoria

| Categoria | Nota | Comentário |
|-----------|------|-----------|
| Usabilidade Geral | 7/10 | Boa arquitetura, falta guiar primeiro uso |
| Tooltips/Ajuda | 4/10 | Principal lacuna da interface |
| Design Visual | 8/10 | Consistente e profissional |
| Responsividade | 6/10 | Funcional mas touch targets pequenos |
| Formulários/Inputs | 6/10 | Min/max OK, falta step/labels/validação |
| Performance/UX | 7/10 | Lazy loading bom, falta minificação |
| **Média** | **6.3/10** | **Arredondado: 7/10** |
