// ESP32 Control Panel - Core Module
// Handles: Utilities, API, Toast, Navigation, Lazy Loading

// ============================================================================
// Module Registry
// ============================================================================

const modules = {
    actuators: { loaded: false, init: null },
    system: { loaded: false, init: null },
    tasks: { loaded: false, init: null },
    setup: { loaded: false, init: null },
    config: { loaded: false, init: null },
    files: { loaded: false, init: null }
};

// ============================================================================
// Utilities
// ============================================================================

function toast(msg, type = 'info') {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.className = 'show ' + type;
    setTimeout(() => t.className = '', 2500);
}

function formatUptime(ms) {
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    if (h > 0) return `${h}h ${m % 60}m`;
    if (m > 0) return `${m}m ${s % 60}s`;
    return `${s}s`;
}

function formatBytes(b) {
    if (b < 1024) return b + ' B';
    return (b / 1024).toFixed(1) + ' KB';
}

// ============================================================================
// API Helper
// ============================================================================

// Authentication state
let authCredentials = null; // base64-encoded "user:pass"

function getAuthHeaders() {
    const headers = { 'Content-Type': 'application/json' };
    if (authCredentials) {
        headers['Authorization'] = 'Basic ' + authCredentials;
    }
    return headers;
}

function promptLogin() {
    const user = prompt('Username:');
    if (!user) return false;
    const pass = prompt('Password:');
    if (pass === null) return false;
    authCredentials = btoa(user + ':' + pass);
    return true;
}

async function api(endpoint, method = 'GET', data = null) {
    try {
        const opts = { method, headers: getAuthHeaders() };
        if (data) opts.body = JSON.stringify(data);
        let res = await fetch('/api/' + endpoint, opts);

        // Handle 401 - prompt for credentials and retry once
        if (res.status === 401) {
            if (promptLogin()) {
                opts.headers = getAuthHeaders();
                res = await fetch('/api/' + endpoint, opts);
                if (res.status === 401) {
                    authCredentials = null;
                    toast('Authentication failed', 'error');
                    throw new Error('Authentication failed');
                }
            } else {
                throw new Error('Authentication required');
            }
        }

        // Any successful response means connection is working
        connectionState.connected = true;
        connectionState.lastSuccessfulPing = Date.now();

        return await res.json();
    } catch (e) {
        // Network error - trigger reconnection if we were connected
        if (e.message !== 'Authentication failed' && e.message !== 'Authentication required') {
            if (connectionState.connected) {
                toast('Communication error', 'error');
                startReconnection();
            }
        }

        throw e;
    }
}

// ============================================================================
// Connection State Management
// ============================================================================

const connectionState = {
    connected: true,
    reconnecting: false,
    retryCount: 0,
    lastSuccessfulPing: null
};

let reconnectionTimer = null;

function updateConnectionBanner() {
    const banner = document.getElementById('connection-banner');
    if (!banner) return;

    if (connectionState.connected) {
        banner.className = 'connection-banner hidden';
        banner.textContent = '';
    } else if (connectionState.reconnecting) {
        banner.className = 'connection-banner show reconnecting';
        banner.textContent = `Connection lost. Reconnecting... (attempt ${connectionState.retryCount})`;
    } else {
        banner.className = 'connection-banner show disconnected';
        banner.textContent = 'Connection lost';
    }
}

function startReconnection() {
    if (connectionState.reconnecting) return;

    connectionState.connected = false;
    connectionState.reconnecting = true;
    connectionState.retryCount = 0;

    updateConnectionBanner();

    function scheduleNextAttempt() {
        if (connectionState.connected) return;

        connectionState.retryCount++;
        updateConnectionBanner();

        checkConnection().then(isConnected => {
            if (isConnected) {
                connectionState.connected = true;
                connectionState.reconnecting = false;
                connectionState.retryCount = 0;
                connectionState.lastSuccessfulPing = Date.now();
                updateConnectionBanner();

                // Show success briefly
                const banner = document.getElementById('connection-banner');
                if (banner) {
                    banner.className = 'connection-banner show connected';
                    banner.textContent = 'Connection restored';
                    setTimeout(() => updateConnectionBanner(), 2000);
                }
            } else {
                // Exponential backoff: 1s, 1.5s, 2.25s, 3.375s, ... max 30s
                const delay = Math.min(1000 * Math.pow(1.5, connectionState.retryCount - 1), 30000);
                reconnectionTimer = setTimeout(scheduleNextAttempt, delay);
            }
        });
    }

    scheduleNextAttempt();
}

async function checkConnection() {
    try {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 5000);

        const res = await fetch('/api/status', {
            method: 'GET',
            signal: controller.signal
        });

        clearTimeout(timeoutId);
        return res.ok;
    } catch (e) {
        return false;
    }
}

// ============================================================================
// Status Badge Updates
// ============================================================================

async function updateStatusBadges() {
    try {
        const d = await api('status');
        const wifiBadge = document.getElementById('wifi-badge');
        const modbusBadge = document.getElementById('modbus-badge');
        wifiBadge.className = 'badge ' + (d.wifi_status >= 3 ? 'on' : 'off');
        modbusBadge.className = 'badge ' + (d.modbus_ready ? 'on' : 'off');
    } catch (e) {}
}

// ============================================================================
// Lazy Loading System
// ============================================================================

async function loadModule(name) {
    if (modules[name].loaded) {
        if (modules[name].init) modules[name].init();
        return;
    }

    const container = document.getElementById('tab-' + name);

    try {
        // Load HTML
        const htmlRes = await fetch(`tabs/${name}.html`);
        if (!htmlRes.ok) throw new Error('HTML not found');
        const html = await htmlRes.text();
        container.innerHTML = html;

        // Load JS
        const script = document.createElement('script');
        script.src = `tabs/${name}.js`;
        script.onload = () => {
            modules[name].loaded = true;
            // Call init function if registered
            if (modules[name].init) modules[name].init();
        };
        script.onerror = () => {
            console.error(`Failed to load ${name}.js`);
            toast(`Failed to load ${name} module`, 'error');
        };
        document.body.appendChild(script);
    } catch (e) {
        console.error(`Failed to load module ${name}:`, e);
        container.innerHTML = `<div class="tab-error">Failed to load module</div>`;
    }
}

// Register module init function (called by each module)
function registerModule(name, initFn) {
    modules[name].init = initFn;
    modules[name].loaded = true;
}

// ============================================================================
// Tab Navigation
// ============================================================================

let currentTab = 'actuators';

function switchTab(tabName) {
    // Update nav buttons
    document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
    document.querySelector(`.nav-btn[data-tab="${tabName}"]`).classList.add('active');

    // Update tab visibility
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.getElementById('tab-' + tabName).classList.add('active');

    currentTab = tabName;

    // Load module if needed
    loadModule(tabName);
}

// Setup navigation listeners
document.querySelectorAll('.nav-btn').forEach(btn => {
    btn.addEventListener('click', () => switchTab(btn.dataset.tab));
});

// ============================================================================
// Log Viewer
// ============================================================================

const logState = {
    expanded: false,
    lastSequence: 0,
    pollInterval: null,
    entries: [],
    errorCount: 0,
    warnCount: 0
};

const LOG_LEVELS = ['', 'E', 'W', 'I', 'D', 'V'];
const LOG_LEVEL_NAMES = ['None', 'Error', 'Warn', 'Info', 'Debug', 'Verbose'];

function toggleLogViewer() {
    const viewer = document.getElementById('log-viewer');
    logState.expanded = !logState.expanded;

    if (logState.expanded) {
        viewer.classList.remove('collapsed');
        viewer.classList.add('expanded');
        document.body.classList.add('log-expanded');
        startLogPolling();
    } else {
        viewer.classList.remove('expanded');
        viewer.classList.add('collapsed');
        document.body.classList.remove('log-expanded');
        stopLogPolling();
    }
}

function startLogPolling() {
    if (logState.pollInterval) return;
    fetchLogs();
    logState.pollInterval = setInterval(fetchLogs, 1500);
}

function stopLogPolling() {
    if (logState.pollInterval) {
        clearInterval(logState.pollInterval);
        logState.pollInterval = null;
    }
}

async function fetchLogs() {
    try {
        const data = await api(`logs?since=${logState.lastSequence}`);

        if (data.logs && data.logs.length > 0) {
            // Add new entries
            for (const log of data.logs) {
                logState.entries.push(log);
                if (log.lvl === 1) logState.errorCount++;
                if (log.lvl === 2) logState.warnCount++;
            }

            // Keep only last 200 entries in memory
            if (logState.entries.length > 200) {
                const removed = logState.entries.splice(0, logState.entries.length - 200);
                // Adjust counts for removed entries
                for (const log of removed) {
                    if (log.lvl === 1) logState.errorCount--;
                    if (log.lvl === 2) logState.warnCount--;
                }
            }

            logState.lastSequence = data.sequence;
            renderLogs();
        }

        updateLogBadge();
    } catch (e) {
        // Silently ignore - connection handling is done elsewhere
    }
}

function renderLogs() {
    const container = document.getElementById('log-entries');
    const levelFilter = parseInt(document.getElementById('log-level-filter').value);
    const tagFilter = document.getElementById('log-tag-filter').value.toLowerCase();
    const autoScroll = document.getElementById('log-autoscroll').checked;

    // Filter entries
    const filtered = logState.entries.filter(log => {
        if (levelFilter > 0 && log.lvl > levelFilter) return false;
        if (tagFilter && !log.tag.toLowerCase().includes(tagFilter)) return false;
        return true;
    });

    if (filtered.length === 0) {
        container.innerHTML = '<div class="log-empty">No logs to display</div>';
        return;
    }

    // Build HTML
    let html = '';
    for (const log of filtered) {
        const ts = formatLogTimestamp(log.ts);
        const lvl = LOG_LEVELS[log.lvl] || '?';
        html += `<div class="log-entry level-${log.lvl}">
            <span class="log-ts">${ts}</span>
            <span class="log-lvl">${lvl}</span>
            <span class="log-tag">${escapeHtml(log.tag)}</span>
            <span class="log-msg">${escapeHtml(log.msg)}</span>
        </div>`;
    }

    container.innerHTML = html;

    // Auto-scroll to bottom
    if (autoScroll) {
        container.scrollTop = container.scrollHeight;
    }
}

function formatLogTimestamp(ms) {
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const sec = s % 60;
    const msec = ms % 1000;
    return `${m}:${sec.toString().padStart(2, '0')}.${msec.toString().padStart(3, '0')}`;
}

function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}

function updateLogBadge() {
    const badge = document.getElementById('log-count');
    const count = logState.entries.length;
    badge.textContent = count;

    badge.classList.remove('has-errors', 'has-warnings');
    if (logState.errorCount > 0) {
        badge.classList.add('has-errors');
    } else if (logState.warnCount > 0) {
        badge.classList.add('has-warnings');
    }
}

async function clearLogs() {
    try {
        await api('logs/clear', 'POST');
        logState.entries = [];
        logState.lastSequence = 0;
        logState.errorCount = 0;
        logState.warnCount = 0;
        renderLogs();
        updateLogBadge();
        toast('Logs cleared', 'success');
    } catch (e) {
        toast('Failed to clear logs', 'error');
    }
}

function updateLogFilter() {
    renderLogs();
}

// ============================================================================
// Initialization
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    // Update status badges
    updateStatusBadges();
    setInterval(updateStatusBadges, 10000);

    // Load initial tab (actuators)
    loadModule('actuators');

    // Setup log filter listeners
    document.getElementById('log-level-filter').addEventListener('change', updateLogFilter);
    document.getElementById('log-tag-filter').addEventListener('input', updateLogFilter);
});
