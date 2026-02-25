// Setup Wizard Module

let scanResults = [];
let roleMap = {}; // { actuatorIndex: role } where role = 'lens_a', 'lens_b', 'nozzle', 'skip'

// ============================================================================
// Step Navigation
// ============================================================================

function goToStep(step) {
    // Hide all steps
    for (let i = 1; i <= 4; i++) {
        document.getElementById('setup-step-' + i).classList.remove('active');
        document.getElementById('step-ind-' + i).classList.remove('active');
    }
    // Show target step
    document.getElementById('setup-step-' + step).classList.add('active');
    document.getElementById('step-ind-' + step).classList.add('active');

    if (step === 2) buildRoleAssignments();
    if (step === 3) buildStandardizePreview();
    if (step === 4) buildSaveSummary();
}

// ============================================================================
// Step 1: Smart Scan
// ============================================================================

async function startSmartScan() {
    const btn = document.getElementById('btn-smart-scan');
    btn.disabled = true;
    btn.textContent = 'Scanning...';

    const progress = document.getElementById('scan-progress');
    progress.classList.remove('hidden');

    const fill = document.getElementById('scan-progress-fill');
    const text = document.getElementById('scan-progress-text');

    // Animate progress (scan takes ~15-30s)
    fill.style.width = '10%';
    text.textContent = 'Scanning all baud rates and IDs 1-20...';

    let pct = 10;
    const progressTimer = setInterval(() => {
        pct = Math.min(pct + 5, 90);
        fill.style.width = pct + '%';
    }, 1000);

    try {
        const r = await api('actuator/smart-scan', 'POST');
        clearInterval(progressTimer);
        fill.style.width = '100%';

        scanResults = r.actuators || [];

        const resultsDiv = document.getElementById('scan-results');
        const noResults = document.getElementById('scan-no-results');
        const tbody = document.getElementById('scan-results-body');

        resultsDiv.classList.remove('hidden');

        if (scanResults.length === 0) {
            noResults.classList.remove('hidden');
            tbody.innerHTML = '';
            document.getElementById('btn-step1-next').disabled = true;
            text.textContent = 'Scan complete - no actuators found';
        } else {
            noResults.classList.add('hidden');
            tbody.innerHTML = scanResults.map((a, i) =>
                `<tr>
                    <td>${a.id}</td>
                    <td>${a.baud_rate}</td>
                    <td>${a.model}</td>
                    <td>${a.protocol}</td>
                </tr>`
            ).join('');

            document.getElementById('btn-step1-next').disabled = false;
            text.textContent = `Scan complete - found ${scanResults.length} actuator(s)`;

            // Auto-assign roles if exactly 3 actuators
            roleMap = {};
            if (scanResults.length >= 1) roleMap[0] = 'lens_a';
            if (scanResults.length >= 2) roleMap[1] = 'lens_b';
            if (scanResults.length >= 3) roleMap[2] = 'nozzle';
            for (let i = 3; i < scanResults.length; i++) roleMap[i] = 'skip';
        }

        toast(`Found ${scanResults.length} actuator(s)`, scanResults.length > 0 ? 'success' : 'info');
    } catch (e) {
        clearInterval(progressTimer);
        fill.style.width = '0%';
        text.textContent = 'Scan failed';
        toast('Smart scan failed', 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = 'Start Smart Scan';
    }
}

// ============================================================================
// Step 2: Role Assignment
// ============================================================================

function buildRoleAssignments() {
    const container = document.getElementById('role-assignments');
    if (scanResults.length === 0) {
        container.innerHTML = '<p>No actuators found. Go back and scan first.</p>';
        return;
    }

    container.innerHTML = scanResults.map((a, i) => {
        const sel = roleMap[i] || 'skip';
        return `<div class="role-row">
            <div class="role-info">
                <strong>ID ${a.id}</strong> @ ${a.baud_rate} baud (model: ${a.model})
            </div>
            <div class="role-controls">
                <select id="role-select-${i}" onchange="updateRole(${i}, this.value)">
                    <option value="skip" ${sel === 'skip' ? 'selected' : ''}>Skip</option>
                    <option value="lens_a" ${sel === 'lens_a' ? 'selected' : ''}>Lens Focus A (sync)</option>
                    <option value="lens_b" ${sel === 'lens_b' ? 'selected' : ''}>Lens Focus B (sync)</option>
                    <option value="nozzle" ${sel === 'nozzle' ? 'selected' : ''}>Nozzle (independent)</option>
                </select>
                <button onclick="jogActuator(${a.id}, ${a.baud_rate})" class="btn btn-small">Jog</button>
            </div>
        </div>`;
    }).join('');

    validateRoles();
}

function updateRole(index, role) {
    roleMap[index] = role;
    validateRoles();
}

function validateRoles() {
    const roles = Object.values(roleMap);
    const lensACount = roles.filter(r => r === 'lens_a').length;
    const lensBCount = roles.filter(r => r === 'lens_b').length;
    const nozzleCount = roles.filter(r => r === 'nozzle').length;

    const validationDiv = document.getElementById('role-validation');
    const validationText = document.getElementById('role-validation-text');
    const nextBtn = document.getElementById('btn-step2-next');

    const errors = [];
    if (lensACount !== 1) errors.push(`Lens Focus A: need exactly 1, have ${lensACount}`);
    if (lensBCount !== 1) errors.push(`Lens Focus B: need exactly 1, have ${lensBCount}`);
    if (nozzleCount !== 1) errors.push(`Nozzle: need exactly 1, have ${nozzleCount}`);

    if (errors.length > 0) {
        validationDiv.classList.remove('hidden');
        validationText.textContent = errors.join(' | ');
        nextBtn.disabled = true;
    } else {
        validationDiv.classList.add('hidden');
        nextBtn.disabled = false;
    }
}

async function jogActuator(id, baudRate) {
    toast('Jogging actuator ' + id + '...', 'info');
    try {
        const r = await api('actuator/jog', 'POST', { id, baud_rate: baudRate });
        if (r.success) {
            toast('Jog complete', 'success');
        } else {
            toast(r.message || 'Jog failed', 'error');
        }
    } catch (e) {
        toast('Jog failed', 'error');
    }
}

// ============================================================================
// Step 3: Standardize
// ============================================================================

function getRoleMapping() {
    // Build role -> actuator mapping from roleMap
    const mapping = {};
    for (const [idx, role] of Object.entries(roleMap)) {
        if (role !== 'skip') {
            mapping[role] = scanResults[parseInt(idx)];
        }
    }
    return mapping;
}

function buildStandardizePreview() {
    const mapping = getRoleMapping();
    const targetBaud = parseInt(document.getElementById('std-target-baud').value);
    const tbody = document.getElementById('std-changes-body');

    const roleNames = {
        lens_a: 'Lens Focus A',
        lens_b: 'Lens Focus B',
        nozzle: 'Nozzle'
    };
    const newIds = { lens_a: 1, lens_b: 2, nozzle: 3 };

    let html = '';
    let needsChanges = false;

    for (const [role, act] of Object.entries(mapping)) {
        const newId = newIds[role];
        const baudChanged = act.baud_rate !== targetBaud;
        const idChanged = act.id !== newId;

        if (baudChanged || idChanged) needsChanges = true;

        html += `<tr>
            <td>${roleNames[role]}</td>
            <td>${act.id}</td>
            <td>${idChanged ? '<strong>' + newId + '</strong>' : newId}</td>
            <td>${act.baud_rate}</td>
            <td>${baudChanged ? '<strong>' + targetBaud + '</strong>' : targetBaud}</td>
        </tr>`;
    }

    tbody.innerHTML = html;

    const stdBtn = document.getElementById('btn-standardize');
    if (!needsChanges) {
        stdBtn.textContent = 'No Changes Needed — Skip';
        stdBtn.onclick = () => {
            document.getElementById('btn-step3-next').disabled = false;
            goToStep(4);
        };
    } else {
        stdBtn.textContent = 'Apply Standardization';
        stdBtn.onclick = doStandardize;
    }

    // Enable skip to step 4 always
    document.getElementById('btn-step3-next').disabled = false;
}

async function doStandardize() {
    const mapping = getRoleMapping();
    const targetBaud = parseInt(document.getElementById('std-target-baud').value);
    const newIds = { lens_a: 1, lens_b: 2, nozzle: 3 };

    const actuators = Object.entries(mapping).map(([role, act]) => ({
        id: act.id,
        baud_rate: act.baud_rate,
        new_id: newIds[role]
    }));

    const btn = document.getElementById('btn-standardize');
    btn.disabled = true;
    btn.textContent = 'Applying...';

    try {
        const r = await api('actuator/standardize', 'POST', {
            target_baud: targetBaud,
            actuators: actuators
        });

        const resultDiv = document.getElementById('std-result');
        resultDiv.classList.remove('hidden');

        if (r.success) {
            resultDiv.className = 'info-box success';
            resultDiv.textContent = 'Standardization complete! All actuators reconfigured.';
            document.getElementById('btn-step3-next').disabled = false;

            // Update scan results to reflect new IDs/baud
            for (const [role, act] of Object.entries(mapping)) {
                act.id = newIds[role];
                act.baud_rate = targetBaud;
            }

            toast('Standardization complete', 'success');
        } else {
            resultDiv.className = 'info-box warn';
            resultDiv.textContent = r.message || 'Some actuators failed to standardize';
            toast('Standardization partially failed', 'error');
        }
    } catch (e) {
        toast('Standardization failed', 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = 'Apply Standardization';
    }
}

// Update preview when baud changes
(function() {
    var el = document.getElementById('std-target-baud');
    if (el) el.addEventListener('change', buildStandardizePreview);
})();

// ============================================================================
// Step 4: Save & Apply
// ============================================================================

function buildSaveSummary() {
    const mapping = getRoleMapping();
    const content = document.getElementById('save-summary-content');

    const roleNames = {
        lens_a: 'Lens Focus A (sync)',
        lens_b: 'Lens Focus B (sync)',
        nozzle: 'Nozzle (independent)'
    };

    let html = '<ul>';
    for (const [role, act] of Object.entries(mapping)) {
        html += `<li><strong>${roleNames[role]}</strong>: ID ${act.id} @ ${act.baud_rate} baud</li>`;
    }
    html += '</ul>';
    content.innerHTML = html;
}

async function saveAndApply() {
    const mapping = getRoleMapping();
    const btn = document.getElementById('btn-save-roles');
    btn.disabled = true;
    btn.textContent = 'Saving...';

    const payload = {};
    for (const [role, act] of Object.entries(mapping)) {
        payload[role] = { id: act.id, baud: act.baud_rate };
    }

    try {
        const r = await api('actuator/roles', 'POST', payload);

        const resultDiv = document.getElementById('save-result');
        resultDiv.classList.remove('hidden');

        if (r.success) {
            resultDiv.className = 'info-box success';
            resultDiv.innerHTML = '✅ Configuration saved successfully! The actuator tab will now use these roles.';
            toast('Roles saved', 'success');
        } else {
            resultDiv.className = 'info-box warn';
            resultDiv.textContent = r.message || 'Failed to save';
            toast('Save failed', 'error');
        }
    } catch (e) {
        toast('Save failed', 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = 'Save Configuration';
    }
}

// ============================================================================
// Module Init
// ============================================================================

function initSetup() {
    // Nothing to poll - wizard is user-driven
}

registerModule('setup', initSetup);
