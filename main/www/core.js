// ESP32 Control Panel - Core Module
// Handles: Utilities, API, Toast, Navigation, Lazy Loading

// ============================================================================
// Module Registry
// ============================================================================

const modules = {
    actuators: { loaded: false, init: null },
    system: { loaded: false, init: null },
    tasks: { loaded: false, init: null },
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

async function api(endpoint, method = 'GET', data = null) {
    try {
        const opts = { method, headers: { 'Content-Type': 'application/json' } };
        if (data) opts.body = JSON.stringify(data);
        const res = await fetch('/api/' + endpoint, opts);

        // Any successful response means connection is working
        connectionState.connected = true;
        connectionState.lastSuccessfulPing = Date.now();

        return await res.json();
    } catch (e) {
        // Network error - trigger reconnection if we were connected
        if (connectionState.connected) {
            toast('Communication error', 'error');
            startReconnection();
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
        banner.className = 'connection-banner warning';
        banner.textContent = `Connection lost. Reconnecting... (attempt ${connectionState.retryCount})`;
    } else {
        banner.className = 'connection-banner error';
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
                    banner.className = 'connection-banner success';
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
// Initialization
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    // Update status badges
    updateStatusBadges();
    setInterval(updateStatusBadges, 10000);

    // Load initial tab (actuators)
    loadModule('actuators');
});
