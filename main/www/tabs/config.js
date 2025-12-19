// Config Module

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
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

// ============================================================================
// RS485
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
// Module Init
// ============================================================================

function initConfig() {
    refreshConfig();
}

// Register module
registerModule('config', initConfig);
