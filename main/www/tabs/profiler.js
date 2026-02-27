let profilerTimer = null;

function profilerInit() {
    profilerLoadConfig();
    profilerPoll();
    profilerTimer = setInterval(profilerPoll, 1000);
}

function profilerCleanup() {
    if (profilerTimer) {
        clearInterval(profilerTimer);
        profilerTimer = null;
    }
}

async function profilerPoll() {
    try {
        const [baumer, control] = await Promise.all([
            api('baumer/status'),
            api('control/status')
        ]);

        // Update measured values
        if (baumer.connected && baumer.values) {
            for (let i = 0; i < 4; i++) {
                const el = document.getElementById('pf-val' + (i + 1));
                if (el) el.textContent = baumer.values[i] != null ? baumer.values[i].toFixed(3) : '--';
            }

            const qEl = document.getElementById('pf-quality');
            if (qEl) {
                qEl.textContent = baumer.quality_text || '--';
                qEl.className = 'badge ' + (baumer.quality === 0 ? 'badge-ok' :
                                             baumer.quality === 1 ? 'badge-warn' : 'badge-err');
            }
        }

        // Update control status
        const statusEl = document.getElementById('pf-ctrl-status');
        if (statusEl) {
            statusEl.textContent = control.running ? 'Running' : 'Stopped';
            statusEl.className = 'badge ' + (control.running ? 'badge-ok' : 'badge-off');
        }

        document.getElementById('pf-loop-count').textContent = control.loop_count || 0;
        document.getElementById('pf-error-count').textContent = control.error_count || 0;
        document.getElementById('pf-gap-value').textContent =
            control.last_gap_value != null ? control.last_gap_value.toFixed(3) : '--';
        document.getElementById('pf-interval').value = control.interval_ms || 1000;
        document.getElementById('pf-meas-index').value = control.measurement_index || 0;

        // Update equations table
        profilerUpdateEquationsTable(control.equations || []);

    } catch (e) {
        console.error('Profiler poll error:', e);
    }
}

function profilerUpdateEquationsTable(equations) {
    const tbody = document.getElementById('pf-equations-body');
    if (!tbody) return;

    // Preserve user edits if count hasn't changed
    if (tbody.children.length === equations.length) {
        // Just update computed positions
        for (let i = 0; i < equations.length; i++) {
            const row = tbody.children[i];
            const posCell = row.querySelector('.computed-pos');
            if (posCell) {
                posCell.textContent = equations[i].computed_position != null ?
                    equations[i].computed_position : '--';
            }
        }
        return;
    }

    tbody.innerHTML = '';
    for (const eq of equations) {
        const tr = document.createElement('tr');
        tr.innerHTML =
            '<td>ID ' + eq.actuator_id + '</td>' +
            '<td><input type="number" step="0.001" value="' + eq.a + '" class="input-sm eq-a" data-id="' + eq.actuator_id + '"></td>' +
            '<td><input type="number" step="0.001" value="' + eq.b + '" class="input-sm eq-b" data-id="' + eq.actuator_id + '"></td>' +
            '<td><input type="checkbox" class="eq-enabled" data-id="' + eq.actuator_id + '"' + (eq.enabled ? ' checked' : '') + '></td>' +
            '<td class="computed-pos">' + (eq.computed_position != null ? eq.computed_position : '--') + '</td>' +
            '<td><button class="btn btn-sm" onclick="profilerSaveEquation(' + eq.actuator_id + ')">Save</button></td>';
        tbody.appendChild(tr);
    }
}

async function profilerSaveEquation(actuatorId) {
    const a = parseFloat(document.querySelector('.eq-a[data-id="' + actuatorId + '"]').value);
    const b = parseFloat(document.querySelector('.eq-b[data-id="' + actuatorId + '"]').value);
    const enabled = document.querySelector('.eq-enabled[data-id="' + actuatorId + '"]').checked;

    try {
        await api('control/equation', 'PUT', {
            actuator_id: actuatorId, a: a, b: b, enabled: enabled
        });
    } catch (e) {
        alert('Failed to save equation: ' + e.message);
    }
}

async function profilerStart() {
    try { await api('control/start', 'POST'); } catch (e) { alert('Error: ' + e.message); }
}

async function profilerStop() {
    try { await api('control/stop', 'POST'); } catch (e) { alert('Error: ' + e.message); }
}

async function profilerSetInterval() {
    const ms = parseInt(document.getElementById('pf-interval').value);
    try {
        await api('control/interval', 'PUT', { interval_ms: ms });
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

async function profilerSetMeasIndex() {
    const idx = parseInt(document.getElementById('pf-meas-index').value);
    try {
        await api('control/measurement_index', 'PUT', { measurement_index: idx });
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

async function profilerToggleLaser() {
    try {
        const status = await api('baumer/status');
        // Toggle based on current quality (if reading, laser is on)
        await api('baumer/laser', 'POST', { on: !(status.connected) });
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

async function profilerLoadConfig() {
    try {
        const cfg = await api('baumer/config');
        document.getElementById('pf-slave-id').value = cfg.slave_id || 8;
        document.getElementById('pf-baumer-enabled').checked = cfg.enabled !== false;
    } catch (e) {
        console.error('Failed to load Baumer config:', e);
    }
}

async function profilerSaveBaumerConfig() {
    const slaveId = parseInt(document.getElementById('pf-slave-id').value);
    const enabled = document.getElementById('pf-baumer-enabled').checked;
    try {
        await api('baumer/config', 'PUT', { slave_id: slaveId, enabled: enabled });
        alert('Saved. Restart required for changes to take effect.');
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

registerModule('profiler', profilerInit, profilerCleanup);
