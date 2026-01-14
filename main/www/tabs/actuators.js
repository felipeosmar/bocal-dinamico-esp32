// Actuators Module

let selectedActuatorId = null;
let actuatorsData = [];
let actuatorsInterval = null;

// ============================================================================
// Data & Rendering
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
                <div class="name">${act.name || 'Actuator #' + act.id}</div>
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

// ============================================================================
// Actions
// ============================================================================

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
// Modal
// ============================================================================

function openActuator(id) {
    selectedActuatorId = id;
    const act = actuatorsData.find(a => a.id === id);
    if (!act) return;

    document.getElementById('modal-act-id').textContent = '#' + id;
    document.getElementById('modal-act-name').value = act.name || '';

    if (act.connected) {
        document.getElementById('modal-pos').value = act.position;
        document.getElementById('modal-pos-val').value = act.position;
    }

    document.getElementById('actuator-modal').classList.add('show');
    syncSliders();
}

function closeActuatorModal() {
    document.getElementById('actuator-modal').classList.remove('show');
    selectedActuatorId = null;
}

async function saveActuatorName() {
    if (!selectedActuatorId) return;

    const name = document.getElementById('modal-act-name').value.trim();

    try {
        const r = await api('actuator/set-name', 'POST', {
            id: selectedActuatorId,
            name: name
        });

        if (r.success) {
            toast('Name saved', 'success');
            refreshActuators();
        } else {
            toast(r.message || 'Failed to save name', 'error');
        }
    } catch (e) {}
}

function syncSliders() {
    const pairs = [
        ['modal-pos', 'modal-pos-val'],
        ['modal-spd', 'modal-spd-val'],
        ['modal-cur', 'modal-cur-val']
    ];

    pairs.forEach(([slider, input]) => {
        const s = document.getElementById(slider);
        const i = document.getElementById(input);
        if (s && i) {
            s.oninput = () => i.value = s.value;
            i.oninput = () => s.value = i.value;
        }
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

async function sendActuatorCommand() {
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
// Module Init
// ============================================================================

function initActuators() {
    // Setup modal close on outside click
    const modal = document.getElementById('actuator-modal');
    if (modal) {
        modal.addEventListener('click', (e) => {
            if (e.target.id === 'actuator-modal') closeActuatorModal();
        });
    }

    // Start refresh interval (reduced frequency to avoid RS485 congestion)
    if (actuatorsInterval) clearInterval(actuatorsInterval);
    actuatorsInterval = setInterval(refreshActuators, 3000);

    // Initial load
    refreshActuators();
}

// Register module
registerModule('actuators', initActuators);
