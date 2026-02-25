// Actuators Module - Synchronized & Independent Control

// Configuration - loaded dynamically from /api/actuator/roles
let SYNC_IDS = [1, 2];  // Default IDs for synchronized group
let IND_ID = 3;          // Default ID for independent actuator
let rolesLoaded = false;

let actuatorsData = [];
let statusInterval = null;
let commandInProgress = false;

// ============================================================================
// Command Lock - Prevent RS485 bus collisions
// ============================================================================

let commandLockTimeout = null;

function acquireCommandLock(duration = 5000) {
    if (commandInProgress) {
        console.log('[Actuators] Command lock busy');
        return false;
    }
    
    commandInProgress = true;
    stopPolling();
    
    if (commandLockTimeout) clearTimeout(commandLockTimeout);
    commandLockTimeout = setTimeout(() => {
        releaseCommandLock();
        console.log('[Actuators] Command lock auto-released');
    }, duration);
    
    return true;
}

function releaseCommandLock(delayBeforePolling = 500) {
    if (commandLockTimeout) {
        clearTimeout(commandLockTimeout);
        commandLockTimeout = null;
    }
    
    commandInProgress = false;
    
    setTimeout(() => {
        if (!commandInProgress) startPolling();
    }, delayBeforePolling);
}

function stopPolling() {
    if (statusInterval) {
        clearInterval(statusInterval);
        statusInterval = null;
    }
}

function startPolling() {
    if (!statusInterval && !commandInProgress) {
        statusInterval = setInterval(refreshStatus, 3000);
    }
}

// ============================================================================
// Status Refresh
// ============================================================================

async function refreshStatus() {
    if (commandInProgress) return;
    
    try {
        const d = await api('actuator/status');
        actuatorsData = d.actuators || [];
        updateStatusDisplay();
    } catch (e) {
        console.error('[Actuators] Status refresh failed:', e);
    }
}

function updateStatusDisplay() {
    // Update Sync Group status
    const act1 = actuatorsData.find(a => a.id === SYNC_IDS[0]);
    const act2 = actuatorsData.find(a => a.id === SYNC_IDS[1]);
    
    if (act1) {
        document.getElementById('sync-pos-1').textContent = act1.connected ? act1.position : '--';
        document.getElementById('sync-status-1').className = 'status-dot ' + (act1.connected ? 'on' : '');
    }
    if (act2) {
        document.getElementById('sync-pos-2').textContent = act2.connected ? act2.position : '--';
        document.getElementById('sync-status-2').className = 'status-dot ' + (act2.connected ? 'on' : '');
    }
    
    // Calculate desync
    if (act1?.connected && act2?.connected) {
        const desync = Math.abs(act1.position - act2.position);
        const desyncEl = document.getElementById('sync-desync');
        desyncEl.textContent = desync;
        desyncEl.className = 'value ' + (desync > 100 ? 'warn' : '');
    } else {
        document.getElementById('sync-desync').textContent = '--';
    }
    
    // Update Independent actuator status
    const act3 = actuatorsData.find(a => a.id === IND_ID);
    if (act3) {
        document.getElementById('ind-pos-display').textContent = act3.connected ? act3.position : '--';
        document.getElementById('ind-cur-display').textContent = act3.connected ? act3.current + 'mA' : '--';
        document.getElementById('ind-status-display').textContent = act3.connected ? 
            (act3.moving ? 'Moving' : 'Idle') : 'Offline';
        document.getElementById('ind-status-dot').className = 'status-dot ' + (act3.connected ? 'on' : '');
    }
}

// ============================================================================
// Scan
// ============================================================================

async function scanActuators() {
    if (!acquireCommandLock(15000)) {
        toast('Operation in progress', 'info');
        return;
    }
    
    toast('Scanning RS485 bus...', 'info');
    
    try {
        const r = await api('actuator/scan');
        if (r.count > 0) {
            toast(`Found ${r.count} actuator(s)`, 'success');
        } else {
            toast('No actuators found', 'error');
        }
    } catch (e) {
        toast('Scan failed', 'error');
    } finally {
        releaseCommandLock(1000);
        refreshStatus();
    }
}

// ============================================================================
// Synchronized Group Controls
// ============================================================================

function syncSliders(prefix) {
    const pairs = [
        [prefix + '-pos', prefix + '-pos-val'],
        [prefix + '-spd', prefix + '-spd-val'],
        [prefix + '-cur', prefix + '-cur-val']
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

function syncQuickPos(pos) {
    document.getElementById('sync-pos').value = pos;
    document.getElementById('sync-pos-val').value = pos;
}

function setButtonState(btnId, state) {
    const btn = document.getElementById(btnId);
    if (!btn) return;
    
    btn.disabled = (state === 'sending');
    
    switch(state) {
        case 'sending':
            btn.innerHTML = '<span class="spinner"></span> Sending...';
            btn.classList.add('loading');
            break;
        case 'success':
            btn.innerHTML = '✓ Done!';
            btn.classList.remove('loading');
            setTimeout(() => setButtonState(btnId, 'idle'), 1500);
            break;
        case 'error':
            btn.innerHTML = '✗ Failed';
            btn.classList.remove('loading');
            setTimeout(() => setButtonState(btnId, 'idle'), 1500);
            break;
        default:
            btn.innerHTML = btnId === 'btn-sync-command' ? 
                'Send Synchronized Command' : 'Send Command';
            btn.classList.remove('loading');
    }
}

async function sendSyncCommand() {
    if (!acquireCommandLock(15000)) {
        toast('Operation in progress', 'info');
        return;
    }

    const pos = parseInt(document.getElementById('sync-pos-val').value);
    let spd = parseInt(document.getElementById('sync-spd-val').value);
    const cur = parseInt(document.getElementById('sync-cur-val').value);

    // Clamp speed
    if (spd > 400) {
        spd = 400;
        document.getElementById('sync-spd').value = spd;
        document.getElementById('sync-spd-val').value = spd;
    }
    
    setButtonState('btn-sync-command', 'sending');

    try {
        const r = await api('actuator/sync-move', 'POST', {
            ids: SYNC_IDS,
            position: pos,
            speed: spd,
            current: cur,
            wait: false,
            timeout: 10000
        });

        if (!r.success) {
            toast(r.message || 'Sync move failed', 'error');
            setButtonState('btn-sync-command', 'error');
            releaseCommandLock(500);
            refreshStatus();
            return;
        }

        // Poll sync-status until movement completes
        const pollSync = async () => {
            try {
                const s = await api('actuator/sync-status');
                if (!s.initialized) {
                    toast('Sync group lost', 'error');
                    setButtonState('btn-sync-command', 'error');
                    releaseCommandLock(500);
                    return;
                }
                if (s.in_position || s.all_stopped) {
                    toast(`Sync move complete (desync: ${s.desync})`, 'success');
                    setButtonState('btn-sync-command', 'success');
                    releaseCommandLock(500);
                    refreshStatus();
                    return;
                }
                // Still moving — poll again
                setTimeout(pollSync, 500);
            } catch (e) {
                toast('Status poll failed', 'error');
                setButtonState('btn-sync-command', 'error');
                releaseCommandLock(500);
            }
        };
        // Start polling after a short delay for movement to begin
        setTimeout(pollSync, 300);
    } catch (e) {
        toast('Command failed', 'error');
        setButtonState('btn-sync-command', 'error');
        releaseCommandLock(500);
        refreshStatus();
    }
}

// ============================================================================
// Independent Actuator Controls
// ============================================================================

function indQuickPos(pos) {
    document.getElementById('ind-pos').value = pos;
    document.getElementById('ind-pos-val').value = pos;
}

async function setIndForce(on) {
    if (!acquireCommandLock(3000)) {
        toast('Operation in progress', 'info');
        return;
    }
    
    try {
        const r = await api('actuator/control', 'POST', { id: IND_ID, force: on });
        if (r.success) {
            toast(`Force ${on ? 'enabled' : 'disabled'}`, 'success');
            document.getElementById('btn-ind-force-on').classList.toggle('active', on);
            document.getElementById('btn-ind-force-off').classList.toggle('active', !on);
        } else {
            toast(r.message || 'Failed', 'error');
        }
    } catch (e) {
        toast('Command failed', 'error');
    } finally {
        releaseCommandLock(500);
    }
}

async function sendIndCommand() {
    if (!acquireCommandLock(8000)) {
        toast('Operation in progress', 'info');
        return;
    }

    const pos = parseInt(document.getElementById('ind-pos-val').value);
    let spd = parseInt(document.getElementById('ind-spd-val').value);
    const cur = parseInt(document.getElementById('ind-cur-val').value);

    if (spd > 400) {
        spd = 400;
        document.getElementById('ind-spd').value = spd;
        document.getElementById('ind-spd-val').value = spd;
    }
    
    setButtonState('btn-ind-command', 'sending');

    try {
        const r = await api('actuator/control', 'POST', {
            id: IND_ID,
            goal: { position: pos, speed: spd, current: cur }
        });

        if (r.success) {
            toast(`Moving to ${pos}`, 'success');
            setButtonState('btn-ind-command', 'success');
        } else {
            toast(r.message || 'Command failed', 'error');
            setButtonState('btn-ind-command', 'error');
        }
    } catch (e) {
        toast('Command failed', 'error');
        setButtonState('btn-ind-command', 'error');
    } finally {
        // Wait for movement before releasing
        setTimeout(() => {
            releaseCommandLock(500);
            refreshStatus();
        }, 2000);
    }
}

// ============================================================================
// Module Init
// ============================================================================

async function loadRoles() {
    try {
        const r = await api('actuator/roles');
        if (r.success) {
            SYNC_IDS = [r.lens_a.id, r.lens_b.id];
            IND_ID = r.nozzle.id;
            rolesLoaded = true;

            // Update UI labels
            const syncSub = document.querySelector('.control-card .card-subtitle');
            if (syncSub && syncSub.textContent.includes('Lens Focus')) {
                syncSub.textContent = `Lens Focus (IDs: ${SYNC_IDS[0]}, ${SYNC_IDS[1]})`;
            }
            const indSub = document.querySelectorAll('.control-card .card-subtitle');
            if (indSub.length > 1 && indSub[1].textContent.includes('Actuator #')) {
                indSub[1].textContent = `Actuator #${IND_ID}`;
            }

            console.log('[Actuators] Roles loaded: sync=' + SYNC_IDS + ' ind=' + IND_ID);
        }
    } catch (e) {
        console.warn('[Actuators] Failed to load roles, using defaults:', e);
    }
}

function initActuators() {
    // Setup slider sync
    syncSliders('sync');
    syncSliders('ind');
    
    // Load roles then start
    loadRoles().then(() => {
        refreshStatus().then(() => startPolling());
    });
}

function cleanupActuators() {
    stopPolling();
}

// Register module
registerModule('actuators', initActuators);
