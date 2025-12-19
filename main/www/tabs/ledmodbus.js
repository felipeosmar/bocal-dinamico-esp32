// LED-Modbus Module

let ledSlaveId = 10;

// ============================================================================
// Status & Connection
// ============================================================================

async function ledModbusRefresh() {
    ledSlaveId = parseInt(document.getElementById('led-slave-id').value) || 10;

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

// ============================================================================
// LED Control
// ============================================================================

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

// ============================================================================
// Slave Configuration
// ============================================================================

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
            document.getElementById('led-slave-id').value = newId;
            ledSlaveId = newId;
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
            setTimeout(ledModbusRefresh, 2000);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {}
}

// ============================================================================
// Module Init
// ============================================================================

function initLedModbus() {
    // Sync period slider and input
    const slider = document.getElementById('led-period-slider');
    const input = document.getElementById('led-period-val');
    if (slider && input) {
        slider.oninput = () => input.value = slider.value;
        input.oninput = () => slider.value = input.value;
    }

    // Initial refresh
    ledModbusRefresh();
}

// Register module
registerModule('ledmodbus', initLedModbus);
