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
        return await res.json();
    } catch (e) {
        console.error('API Error:', e);
        toast('Communication error', 'error');
        throw e;
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
