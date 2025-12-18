// ESP32 Control Panel - Multi-Actuator Support

let selectedActuatorId = null;
let actuatorsData = [];
let ledSlaveId = 2;  // Default slave ID for LED-Modbus

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
        else if (btn.dataset.tab === 'ledmodbus') ledModbusRefresh();
        else if (btn.dataset.tab === 'system') refreshSystem();
        else if (btn.dataset.tab === 'config') refreshConfig();
        else if (btn.dataset.tab === 'files') fmRefresh();
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
    } catch (e) {}
}

async function restartDevice() {
    if (!confirm('Restart device?')) return;
    toast('Restarting...', 'info');
    try { await api('restart', 'POST'); } catch (e) {}
    setTimeout(() => location.reload(), 5000);
}

// ============================================================================
// LED-Modbus Control (Full Protocol Support)
// ============================================================================

async function ledModbusRefresh() {
    ledSlaveId = parseInt(document.getElementById('led-slave-id').value) || 2;

    try {
        const d = await api(`ledmodbus/status?id=${ledSlaveId}`);

        if (d.connected) {
            document.getElementById('led-conn-status').textContent = 'Connected';
            document.getElementById('led-conn-status').style.color = '#4caf50';
            document.getElementById('led-fw-version').textContent = d.fw_version || '--';

            // Update LED indicator
            const ind = document.getElementById('led-ind');
            ind.className = 'led-indicator' + (d.led_on ? ' on' : '') + (d.blink_mode ? ' blink' : '');

            // Update buttons
            document.getElementById('btn-led-on').classList.toggle('active', d.led_on);
            document.getElementById('btn-led-off').classList.toggle('active', !d.led_on);
            document.getElementById('btn-blink-on').classList.toggle('active', d.blink_mode);
            document.getElementById('btn-blink-off').classList.toggle('active', !d.blink_mode);

            // Update period
            document.getElementById('led-period-slider').value = d.blink_period;
            document.getElementById('led-period-val').value = d.blink_period;

            // Update new slave ID field
            document.getElementById('led-new-slave-id').value = ledSlaveId;
        } else {
            document.getElementById('led-conn-status').textContent = d.error || 'Disconnected';
            document.getElementById('led-conn-status').style.color = '#f44336';
            document.getElementById('led-fw-version').textContent = '--';
        }
    } catch (e) {
        document.getElementById('led-conn-status').textContent = 'Error';
        document.getElementById('led-conn-status').style.color = '#f44336';
    }
}

async function ledSetState(on) {
    try {
        const r = await api('ledmodbus/control', 'POST', { slave_id: ledSlaveId, led_on: on });
        if (r.success) {
            toast('LED ' + (on ? 'ON' : 'OFF'), 'success');
            ledModbusRefresh();
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

async function ledSetBlink(on) {
    try {
        const r = await api('ledmodbus/control', 'POST', { slave_id: ledSlaveId, blink_mode: on });
        if (r.success) {
            toast('Blink ' + (on ? 'enabled' : 'disabled'), 'success');
            ledModbusRefresh();
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

async function ledSetPeriod() {
    const period = parseInt(document.getElementById('led-period-val').value);
    if (period < 100 || period > 10000) {
        toast('Period must be 100-10000ms', 'error');
        return;
    }

    try {
        const r = await api('ledmodbus/control', 'POST', { slave_id: ledSlaveId, blink_period: period });
        if (r.success) {
            toast(`Period set to ${period}ms`, 'success');
            ledModbusRefresh();
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

async function ledChangeSlaveId() {
    const newId = parseInt(document.getElementById('led-new-slave-id').value);
    if (newId < 1 || newId > 247) {
        toast('ID must be 1-247', 'error');
        return;
    }

    if (!confirm(`Change slave ID from ${ledSlaveId} to ${newId}?\n\nThe device will reboot and you'll need to connect using the new ID.`)) {
        return;
    }

    try {
        const r = await api('ledmodbus/config', 'POST', { slave_id: ledSlaveId, new_slave_id: newId });
        if (r.success) {
            toast(r.message || 'ID changed', 'success');
            // Update the slave ID input to the new value
            document.getElementById('led-slave-id').value = newId;
            ledSlaveId = newId;
            // Wait a bit for device to reboot then refresh
            setTimeout(ledModbusRefresh, 2000);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

async function ledSaveConfig() {
    try {
        const r = await api('ledmodbus/config', 'POST', { slave_id: ledSlaveId, save_config: true });
        if (r.success) {
            toast(r.message || 'Config saved', 'success');
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

async function ledRebootSlave() {
    if (!confirm(`Reboot slave device (ID: ${ledSlaveId})?`)) return;

    try {
        const r = await api('ledmodbus/config', 'POST', { slave_id: ledSlaveId, reboot: true });
        if (r.success) {
            toast(r.message || 'Rebooting...', 'success');
            // Wait for device to reboot then refresh
            setTimeout(ledModbusRefresh, 2000);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

// Sync period slider and input
document.addEventListener('DOMContentLoaded', () => {
    const slider = document.getElementById('led-period-slider');
    const input = document.getElementById('led-period-val');
    if (slider && input) {
        slider.oninput = () => input.value = slider.value;
        input.oninput = () => slider.value = input.value;
    }
});

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
// File Manager
// ============================================================================

let fmPartition = 'www';
let fmPath = '/';
let fmEditingFile = null;

function fmSelectPartition(partition) {
    fmPartition = partition;
    fmPath = '/';
    document.querySelectorAll('.fm-tab').forEach(t => {
        t.classList.toggle('active', t.dataset.partition === partition);
    });
    fmUpdatePath();
    fmRefresh();
}

function fmUpdatePath() {
    document.getElementById('fm-current-path').textContent = `/${fmPartition}${fmPath}`;
}

async function fmRefresh() {
    const list = document.getElementById('fm-file-list');
    list.innerHTML = '<div class="fm-empty">Loading...</div>';

    try {
        const res = await fetch(`/api/files/list?partition=${fmPartition}&dir=${encodeURIComponent(fmPath)}`);
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        fmRenderFiles(data.files || []);
        fmUpdateStorageInfo();
    } catch (e) {
        list.innerHTML = `<div class="fm-empty">Error: ${e.message}</div>`;
    }
}

function fmRenderFiles(files) {
    const list = document.getElementById('fm-file-list');

    if (files.length === 0 && fmPath === '/') {
        list.innerHTML = '<div class="fm-empty">No files</div>';
        return;
    }

    let html = '';

    // Parent directory
    if (fmPath !== '/') {
        html += `
            <div class="fm-file-item dir" onclick="fmNavigateUp()">
                <span class="fm-file-icon">📁</span>
                <div class="fm-file-info">
                    <div class="fm-file-name">..</div>
                    <div class="fm-file-size">Parent</div>
                </div>
            </div>`;
    }

    // Sort: dirs first
    files.sort((a, b) => {
        if (a.isDir && !b.isDir) return -1;
        if (!a.isDir && b.isDir) return 1;
        return a.name.localeCompare(b.name);
    });

    files.forEach(f => {
        const icon = f.isDir ? '📁' : fmGetIcon(f.name);
        const size = f.isDir ? '' : formatBytes(f.size);
        const canEdit = !f.isDir && fmIsText(f.name);
        const eName = fmEscape(f.name);

        html += `
            <div class="fm-file-item ${f.isDir ? 'dir' : ''}" ${f.isDir ? `onclick="fmNavigateTo('${eName}')"` : ''}>
                <span class="fm-file-icon">${icon}</span>
                <div class="fm-file-info">
                    <div class="fm-file-name">${eName}</div>
                    <div class="fm-file-size">${size}</div>
                </div>
                <div class="fm-file-actions" onclick="event.stopPropagation()">
                    ${!f.isDir ? `<button onclick="fmDownload('${eName}')">DL</button>` : ''}
                    ${canEdit ? `<button onclick="fmEdit('${eName}')">Edit</button>` : ''}
                    <button class="del" onclick="fmDelete('${eName}', ${f.isDir})">Del</button>
                </div>
            </div>`;
    });

    list.innerHTML = html || '<div class="fm-empty">No files</div>';
}

function fmNavigateTo(name) {
    fmPath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    fmUpdatePath();
    fmRefresh();
}

function fmNavigateUp() {
    const parts = fmPath.split('/').filter(p => p);
    parts.pop();
    fmPath = parts.length ? '/' + parts.join('/') : '/';
    fmUpdatePath();
    fmRefresh();
}

function fmDownload(name) {
    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    window.open(`/api/files/download?partition=${fmPartition}&file=${encodeURIComponent(filepath)}`, '_blank');
}

async function fmEdit(name) {
    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;

    try {
        const res = await fetch(`/api/files/read?partition=${fmPartition}&file=${encodeURIComponent(filepath)}`);
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        fmEditingFile = filepath;
        document.getElementById('fm-editor-title').textContent = name;
        document.getElementById('fm-editor-content').value = data.content;
        document.getElementById('fm-editor-modal').classList.add('show');
    } catch (e) {
        toast(e.message, 'error');
    }
}

function fmCloseEditor() {
    document.getElementById('fm-editor-modal').classList.remove('show');
    fmEditingFile = null;
}

async function fmSaveFile() {
    if (!fmEditingFile) return;

    const content = document.getElementById('fm-editor-content').value;
    const formData = new FormData();
    formData.append('file', fmEditingFile);
    formData.append('content', content);

    try {
        const res = await fetch(`/api/files/write?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('File saved', 'success');
        fmCloseEditor();
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

async function fmDelete(name, isDir) {
    if (!confirm(`Delete ${isDir ? 'folder' : 'file'} "${name}"?`)) return;

    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    const formData = new FormData();
    formData.append('file', filepath);

    try {
        const res = await fetch(`/api/files/delete?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('Deleted', 'success');
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

async function fmCreateFolder() {
    const name = prompt('Folder name:');
    if (!name) return;

    if (!/^[a-zA-Z0-9_\-\.]+$/.test(name)) {
        toast('Invalid name', 'error');
        return;
    }

    const dirpath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    const formData = new FormData();
    formData.append('dir', dirpath);

    try {
        const res = await fetch(`/api/files/mkdir?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('Folder created', 'success');
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

async function fmCreateFile() {
    const name = prompt('File name (with extension):');
    if (!name) return;

    if (!/^[a-zA-Z0-9_\-\.]+$/.test(name)) {
        toast('Invalid name', 'error');
        return;
    }

    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    const formData = new FormData();
    formData.append('file', filepath);
    formData.append('content', '');

    try {
        const res = await fetch(`/api/files/write?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('File created', 'success');
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

function fmHandleFileSelect(e) {
    for (let f of e.target.files) fmUpload(f);
    e.target.value = '';
}

async function fmUpload(file) {
    const progress = document.getElementById('fm-progress');
    const fill = document.getElementById('fm-progress-fill');

    progress.classList.add('show');
    fill.style.width = '0%';
    fill.textContent = '0%';

    const formData = new FormData();
    formData.append('file', file);

    const xhr = new XMLHttpRequest();

    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const pct = Math.round((e.loaded / e.total) * 100);
            fill.style.width = pct + '%';
            fill.textContent = pct + '%';
        }
    });

    xhr.addEventListener('load', () => {
        progress.classList.remove('show');
        if (xhr.status === 200) {
            toast(`Uploaded: ${file.name}`, 'success');
            fmRefresh();
        } else {
            toast('Upload failed', 'error');
        }
    });

    xhr.addEventListener('error', () => {
        progress.classList.remove('show');
        toast('Upload error', 'error');
    });

    xhr.open('POST', `/api/files/upload?partition=${fmPartition}&dir=${encodeURIComponent(fmPath)}`);
    xhr.send(formData);
}

async function fmUpdateStorageInfo() {
    try {
        const res = await fetch('/api/files/info');
        const data = await res.json();

        if (data.www) {
            document.getElementById('www-size').textContent = `${formatBytes(data.www.used)}/${formatBytes(data.www.total)}`;
        }
        if (data.userdata) {
            document.getElementById('userdata-size').textContent = `${formatBytes(data.userdata.used)}/${formatBytes(data.userdata.total)}`;
        }
    } catch (e) {}
}

function fmGetIcon(name) {
    const ext = name.split('.').pop().toLowerCase();
    const icons = {
        html: '📄', htm: '📄', css: '🎨', js: '📜', json: '📋',
        txt: '📝', md: '📝', png: '🖼️', jpg: '🖼️', ico: '🖼️', bin: '💾'
    };
    return icons[ext] || '📄';
}

function fmIsText(name) {
    const ext = name.split('.').pop().toLowerCase();
    return ['html', 'htm', 'css', 'js', 'json', 'txt', 'md', 'xml', 'csv', 'cfg', 'conf', 'ini', 'log'].includes(ext);
}

function fmEscape(s) {
    const d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
}

// Drag & drop
document.addEventListener('DOMContentLoaded', () => {
    const zone = document.getElementById('fm-upload-zone');
    if (zone) {
        zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('dragging'); });
        zone.addEventListener('dragleave', () => zone.classList.remove('dragging'));
        zone.addEventListener('drop', (e) => {
            e.preventDefault();
            zone.classList.remove('dragging');
            for (let f of e.dataTransfer.files) fmUpload(f);
        });
        zone.addEventListener('click', () => document.getElementById('fm-file-input').click());
    }
});

// Close modal on outside click
document.addEventListener('DOMContentLoaded', () => {
    const modal = document.getElementById('fm-editor-modal');
    if (modal) {
        modal.addEventListener('click', (e) => {
            if (e.target === modal) fmCloseEditor();
        });
    }
});

// ============================================================================
// Init
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    refreshSystem();
    refreshActuators();
    setInterval(refreshSystem, 10000);
    setInterval(refreshActuators, 5000);
});
