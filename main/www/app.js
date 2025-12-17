// ESP32 Control Panel - Multi-Actuator Support

let selectedActuatorId = null;
let actuatorsData = [];

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
// Tab Navigation
// ============================================================================

document.querySelectorAll('.nav-btn').forEach(btn => {
    btn.addEventListener('click', () => {
        document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
        document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
        btn.classList.add('active');
        document.getElementById('tab-' + btn.dataset.tab).classList.add('active');

        if (btn.dataset.tab === 'actuators') refreshActuators();
        else if (btn.dataset.tab === 'system') refreshSystem();
        else if (btn.dataset.tab === 'config') refreshConfig();
    });
});

// ============================================================================
// System Status
// ============================================================================

async function refreshSystem() {
    try {
        const d = await api('status');
        document.getElementById('sys-ip').textContent = d.wifi_ip || '--';
        document.getElementById('sys-ssid').textContent = d.wifi_ssid || '--';
        document.getElementById('sys-rssi').textContent = d.wifi_rssi ? `${d.wifi_rssi} dBm` : '--';
        document.getElementById('sys-uptime').textContent = formatUptime(d.uptime_ms);
        document.getElementById('sys-heap').textContent = formatBytes(d.heap_free);

        const wifiBadge = document.getElementById('wifi-badge');
        const modbusBadge = document.getElementById('modbus-badge');

        wifiBadge.className = 'badge ' + (d.wifi_status >= 3 ? 'on' : 'off');
        modbusBadge.className = 'badge ' + (d.modbus_ready ? 'on' : 'off');

        // Update LED status
        refreshLED();
    } catch (e) {}
}

async function restartDevice() {
    if (!confirm('Restart device?')) return;
    toast('Restarting...', 'info');
    try { await api('restart', 'POST'); } catch (e) {}
    setTimeout(() => location.reload(), 5000);
}

// ============================================================================
// LED Control
// ============================================================================

async function refreshLED() {
    try {
        const d = await api('led/status');
        if (d.error) return;

        const ind = document.getElementById('led-ind');
        ind.className = 'led-indicator' + (d.led_on ? ' on' : '') + (d.blink_mode ? ' blink' : '');

        document.getElementById('btn-led-on').classList.toggle('active', d.led_on);
        document.getElementById('btn-led-off').classList.toggle('active', !d.led_on);
        document.getElementById('btn-blink-on').classList.toggle('active', d.blink_mode);
        document.getElementById('btn-blink-off').classList.toggle('active', !d.blink_mode);
    } catch (e) {}
}

async function setLED(on) {
    try {
        const r = await api('led/control', 'POST', { led_on: on });
        if (r.success) { toast('LED ' + (on ? 'ON' : 'OFF'), 'success'); refreshLED(); }
        else toast(r.message || 'Failed', 'error');
    } catch (e) {}
}

async function setBlink(on) {
    try {
        const r = await api('led/control', 'POST', { blink_mode: on });
        if (r.success) { toast('Blink ' + (on ? 'enabled' : 'disabled'), 'success'); refreshLED(); }
        else toast(r.message || 'Failed', 'error');
    } catch (e) {}
}

// ============================================================================
// Actuators - Multi-device support
// ============================================================================

async function refreshActuators() {
    try {
        const d = await api('actuator/status');
        actuatorsData = d.actuators || [];
        renderActuators();
    } catch (e) {}
}

function renderActuators() {
    const container = document.getElementById('actuators-container');

    if (actuatorsData.length === 0) {
        container.innerHTML = `
            <div class="empty-state">
                <p>No actuators</p>
                <button onclick="scanActuators()" class="btn">Scan for devices</button>
            </div>`;
        return;
    }

    container.innerHTML = actuatorsData.map(act => `
        <div class="actuator-card" onclick="openActuator(${act.id})">
            <div class="id">${act.id}</div>
            <div class="info">
                <div class="name">Actuator #${act.id}</div>
                <div class="stats">
                    ${act.connected ?
                        `Pos: ${act.position} | ${act.current}mA | ${act.voltage.toFixed(1)}V` :
                        'Disconnected'}
                </div>
            </div>
            <div class="status-dot ${act.connected ? 'on' : ''}"></div>
            <button class="remove" onclick="event.stopPropagation(); removeActuator(${act.id})" title="Remove">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
                    <path d="M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z"/>
                </svg>
            </button>
        </div>
    `).join('');
}

async function scanActuators() {
    toast('Scanning...', 'info');
    try {
        const r = await api('actuator/scan');
        if (r.count > 0) {
            toast(`Found ${r.count} actuator(s)`, 'success');
            refreshActuators();
        } else {
            toast('No actuators found', 'error');
        }
    } catch (e) {}
}

function addActuatorPrompt() {
    const id = prompt('Enter actuator ID (1-247):');
    if (id && !isNaN(id) && id >= 1 && id <= 247) {
        addActuator(parseInt(id));
    }
}

async function addActuator(id) {
    try {
        const r = await api('actuator/add', 'POST', { id });
        if (r.success) {
            toast(`Actuator ${id} added`, 'success');
            refreshActuators();
        } else {
            toast(r.message || 'Failed to add', 'error');
        }
    } catch (e) {}
}

async function removeActuator(id) {
    if (!confirm(`Remove actuator ${id}?`)) return;
    try {
        const r = await api('actuator/remove', 'POST', { id });
        if (r.success) {
            toast(`Actuator ${id} removed`, 'success');
            refreshActuators();
        }
    } catch (e) {}
}

// ============================================================================
// Actuator Modal
// ============================================================================

function openActuator(id) {
    selectedActuatorId = id;
    const act = actuatorsData.find(a => a.id === id);
    if (!act) return;

    document.getElementById('modal-act-id').textContent = '#' + id;

    if (act.connected) {
        document.getElementById('modal-pos').value = act.position;
        document.getElementById('modal-pos-val').value = act.position;
    }

    document.getElementById('actuator-modal').classList.add('show');
    syncSliders();
}

function closeModal() {
    document.getElementById('actuator-modal').classList.remove('show');
    selectedActuatorId = null;
}

// Close modal on outside click
document.getElementById('actuator-modal').addEventListener('click', (e) => {
    if (e.target.id === 'actuator-modal') closeModal();
});

function syncSliders() {
    const pairs = [
        ['modal-pos', 'modal-pos-val'],
        ['modal-spd', 'modal-spd-val'],
        ['modal-cur', 'modal-cur-val']
    ];

    pairs.forEach(([slider, input]) => {
        const s = document.getElementById(slider);
        const i = document.getElementById(input);
        s.oninput = () => i.value = s.value;
        i.oninput = () => s.value = i.value;
    });
}

async function setForce(on) {
    if (!selectedActuatorId) return;
    try {
        const r = await api('actuator/control', 'POST', { id: selectedActuatorId, force: on });
        if (r.success) {
            toast(`Force ${on ? 'enabled' : 'disabled'}`, 'success');
            document.getElementById('btn-force-on').classList.toggle('active', on);
            document.getElementById('btn-force-off').classList.toggle('active', !on);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

function quickPos(pos) {
    document.getElementById('modal-pos').value = pos;
    document.getElementById('modal-pos-val').value = pos;
}

async function sendCommand() {
    if (!selectedActuatorId) return;

    const pos = parseInt(document.getElementById('modal-pos-val').value);
    const spd = parseInt(document.getElementById('modal-spd-val').value);
    const cur = parseInt(document.getElementById('modal-cur-val').value);

    try {
        const r = await api('actuator/control', 'POST', {
            id: selectedActuatorId,
            goal: { position: pos, speed: spd, current: cur }
        });

        if (r.success) {
            toast(`Moving to ${pos}`, 'success');
            setTimeout(refreshActuators, 500);
        } else {
            toast(r.message || 'Command failed', 'error');
        }
    } catch (e) {}
}

// ============================================================================
// WiFi
// ============================================================================

async function scanWiFi() {
    toast('Scanning...', 'info');
    try {
        const nets = await api('wifi/scan');
        const sel = document.getElementById('wifi-select');
        sel.innerHTML = '<option value="">Select network...</option>';
        nets.forEach(n => {
            const opt = document.createElement('option');
            opt.value = n.ssid;
            opt.textContent = `${n.ssid} (${n.rssi} dBm)`;
            sel.appendChild(opt);
        });
        toast(`Found ${nets.length} networks`, 'success');
    } catch (e) {}
}

async function connectWiFi() {
    const ssid = document.getElementById('wifi-select').value;
    const pass = document.getElementById('wifi-pass').value;

    if (!ssid) { toast('Select a network', 'error'); return; }

    toast('Connecting...', 'info');
    try {
        const r = await api('wifi/connect', 'POST', { ssid, password: pass });
        if (r.success) {
            toast('Connected!', 'success');
            setTimeout(refreshSystem, 2000);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

// ============================================================================
// RS485 Config
// ============================================================================

async function refreshConfig() {
    try {
        const d = await api('rs485/config');
        document.getElementById('rs485-baud').value = d.baud_rate;
        document.getElementById('rs485-tx').textContent = 'GPIO' + d.tx_pin;
        document.getElementById('rs485-rx').textContent = 'GPIO' + d.rx_pin;
        document.getElementById('rs485-de').textContent = 'GPIO' + d.de_pin;
    } catch (e) {}
}

async function saveRS485() {
    const baud = parseInt(document.getElementById('rs485-baud').value);

    try {
        const r = await api('rs485/config', 'POST', { baud_rate: baud });
        if (r.success) {
            toast('Saved! Restarting...', 'success');
            setTimeout(() => api('restart', 'POST'), 1000);
            setTimeout(() => location.reload(), 5000);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

// ============================================================================
// Init
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    refreshSystem();
    refreshActuators();
    setInterval(refreshSystem, 10000);
    setInterval(refreshActuators, 5000);
});
