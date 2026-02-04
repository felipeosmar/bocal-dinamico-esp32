// Config Module

let configActuators = [];
let selectedConfigActuator = null;
let originalConfig = null;

// ============================================================================
// Actuator Configuration
// ============================================================================

async function refreshConfigActuators() {
    try {
        const d = await api('actuator/status');
        configActuators = d.actuators || [];
        renderConfigActuatorList();
    } catch (e) {
        console.error('Failed to refresh actuators:', e);
    }
}

function renderConfigActuatorList() {
    const container = document.getElementById('config-actuator-list');
    
    if (configActuators.length === 0) {
        container.innerHTML = `
            <div class="empty-state small">
                <p>No actuators</p>
            </div>`;
        return;
    }
    
    container.innerHTML = configActuators.map(act => `
        <div class="config-actuator-item ${selectedConfigActuator === act.id ? 'selected' : ''} ${act.connected ? '' : 'disconnected'}" 
             onclick="selectConfigActuator(${act.id})">
            <span class="id">#${act.id}</span>
            <span class="name">${act.name || 'Actuator'}</span>
            <span class="status-dot ${act.connected ? 'on' : ''}"></span>
        </div>
    `).join('');
}

async function selectConfigActuator(id) {
    selectedConfigActuator = id;
    renderConfigActuatorList();
    
    document.getElementById('config-placeholder').style.display = 'none';
    document.getElementById('config-panel').style.display = 'block';
    document.getElementById('config-act-id').textContent = '#' + id;
    
    await loadActuatorConfig(id);
}

async function loadActuatorConfig(id) {
    try {
        toast('Loading config...', 'info');
        const d = await api(`actuator/config?id=${id}`);
        
        if (!d.success) {
            toast(d.error || 'Failed to load config', 'error');
            return;
        }
        
        // Store original for comparison
        originalConfig = JSON.parse(JSON.stringify(d.config));
        
        // Populate info fields
        if (d.info) {
            document.getElementById('cfg-model').textContent = d.info.model;
            document.getElementById('cfg-firmware').textContent = 'v' + (d.info.firmware / 10).toFixed(1);
            document.getElementById('cfg-voltage').textContent = 
                (d.info.voltage_min / 10).toFixed(1) + 'V - ' + (d.info.voltage_max / 10).toFixed(1) + 'V';
        }
        
        // Populate config fields
        const cfg = d.config;
        document.getElementById('cfg-slave-id').value = cfg.slave_id;
        document.getElementById('cfg-baud-rate').value = cfg.baud_rate;
        
        setSliderValue('cfg-short-stroke', cfg.short_stroke_limit);
        setSliderValue('cfg-long-stroke', cfg.long_stroke_limit);
        setSliderValue('cfg-speed-limit', cfg.speed_limit);
        setSliderValue('cfg-current-limit', cfg.current_limit);
        setSliderValue('cfg-start-compliance', cfg.start_compliance);
        setSliderValue('cfg-end-compliance', cfg.end_compliance);
        
        document.getElementById('cfg-alarm-led').value = cfg.alarm_led;
        document.getElementById('cfg-alarm-shutdown').value = cfg.alarm_shutdown;
        
        toast('Config loaded', 'success');
    } catch (e) {
        toast('Failed to load config', 'error');
        console.error(e);
    }
}

function setSliderValue(id, value) {
    document.getElementById(id).value = value;
    document.getElementById(id + '-val').value = value;
}

function getSliderValue(id) {
    return parseInt(document.getElementById(id + '-val').value);
}

async function saveActuatorConfig() {
    if (!selectedConfigActuator) return;
    
    const config = {
        slave_id: parseInt(document.getElementById('cfg-slave-id').value),
        baud_rate: parseInt(document.getElementById('cfg-baud-rate').value),
        short_stroke_limit: getSliderValue('cfg-short-stroke'),
        long_stroke_limit: getSliderValue('cfg-long-stroke'),
        speed_limit: getSliderValue('cfg-speed-limit'),
        current_limit: getSliderValue('cfg-current-limit'),
        start_compliance: getSliderValue('cfg-start-compliance'),
        end_compliance: getSliderValue('cfg-end-compliance'),
        alarm_led: parseInt(document.getElementById('cfg-alarm-led').value),
        alarm_shutdown: parseInt(document.getElementById('cfg-alarm-shutdown').value)
    };
    
    // Check for dangerous changes
    if (originalConfig) {
        if (config.slave_id !== originalConfig.slave_id) {
            if (!confirm(`Changing Slave ID from ${originalConfig.slave_id} to ${config.slave_id} will lose connection. Continue?`)) {
                return;
            }
        }
        if (config.baud_rate !== originalConfig.baud_rate) {
            if (!confirm(`Changing Baud Rate requires ESP32 reconfiguration. Continue?`)) {
                return;
            }
        }
    }
    
    // Only send changed fields
    const changedConfig = {};
    for (const key in config) {
        if (!originalConfig || config[key] !== originalConfig[key]) {
            changedConfig[key] = config[key];
        }
    }
    
    if (Object.keys(changedConfig).length === 0) {
        toast('No changes to save', 'info');
        return;
    }
    
    const btn = document.getElementById('btn-save-config');
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner"></span> Saving...';
    
    try {
        const r = await api('actuator/config', 'POST', {
            id: selectedConfigActuator,
            config: changedConfig
        });
        
        if (r.success) {
            toast('Config saved to EEPROM', 'success');
            originalConfig = JSON.parse(JSON.stringify(config));
            
            // If ID changed, we need to refresh the list
            if (changedConfig.slave_id) {
                selectedConfigActuator = null;
                document.getElementById('config-panel').style.display = 'none';
                document.getElementById('config-placeholder').style.display = 'block';
                refreshConfigActuators();
            }
        } else {
            toast(r.message || 'Failed to save', 'error');
        }
    } catch (e) {
        toast('Failed to save config', 'error');
    } finally {
        btn.disabled = false;
        btn.innerHTML = '💾 Save Config';
    }
}

async function reloadActuatorConfig() {
    if (!selectedConfigActuator) return;
    await loadActuatorConfig(selectedConfigActuator);
}

async function restartActuator() {
    if (!selectedConfigActuator) return;
    
    if (!confirm('Restart actuator? It will be unavailable for a few seconds.')) {
        return;
    }
    
    try {
        const r = await api('actuator/restart', 'POST', { id: selectedConfigActuator });
        if (r.success) {
            toast('Actuator restarting...', 'success');
            // Wait and refresh
            setTimeout(() => {
                refreshConfigActuators();
                if (selectedConfigActuator) {
                    loadActuatorConfig(selectedConfigActuator);
                }
            }, 3000);
        } else {
            toast(r.message || 'Restart failed', 'error');
        }
    } catch (e) {
        toast('Restart failed', 'error');
    }
}

async function factoryResetActuator() {
    if (!selectedConfigActuator) return;
    
    const confirmText = prompt('Type "RESET" to confirm factory reset:');
    if (confirmText !== 'RESET') {
        toast('Factory reset cancelled', 'info');
        return;
    }
    
    try {
        const r = await api('actuator/factory-reset', 'POST', { 
            id: selectedConfigActuator,
            confirm: true
        });
        
        if (r.success) {
            toast('Factory reset complete! Actuator will restart with default settings.', 'success');
            selectedConfigActuator = null;
            document.getElementById('config-panel').style.display = 'none';
            document.getElementById('config-placeholder').style.display = 'block';
            setTimeout(refreshConfigActuators, 3000);
        } else {
            toast(r.message || 'Factory reset failed', 'error');
        }
    } catch (e) {
        toast('Factory reset failed', 'error');
    }
}

// ============================================================================
// Slider Sync
// ============================================================================

function setupConfigSliders() {
    const sliders = [
        'cfg-short-stroke',
        'cfg-long-stroke',
        'cfg-speed-limit',
        'cfg-current-limit',
        'cfg-start-compliance',
        'cfg-end-compliance'
    ];
    
    sliders.forEach(id => {
        const slider = document.getElementById(id);
        const input = document.getElementById(id + '-val');
        if (slider && input) {
            slider.oninput = () => input.value = slider.value;
            input.oninput = () => slider.value = input.value;
        }
    });
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
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

// ============================================================================
// RS485
// ============================================================================

async function refreshRS485Config() {
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
    refreshConfigActuators();
    refreshRS485Config();
    setupConfigSliders();
}

// Register module
registerModule('config', initConfig);
