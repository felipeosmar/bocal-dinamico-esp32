// Setup Wizard Module - Dual Bus

// Each entry: { id, baud_rate, model, bus }
var bus1Actuators = [];
var bus2Actuators = [];
var baumerStatus = null;
var roleMap = {}; // { "bus:index": role }

// ============================================================================
// Step Navigation
// ============================================================================

function goToStep(step) {
    for (var i = 1; i <= 4; i++) {
        document.getElementById('setup-step-' + i).classList.remove('active');
        document.getElementById('step-ind-' + i).classList.remove('active');
    }
    document.getElementById('setup-step-' + step).classList.add('active');
    document.getElementById('step-ind-' + step).classList.add('active');

    if (step === 2) buildRoleAssignments();
    if (step === 3) buildStandardizePreview();
    if (step === 4) buildSaveSummary();
}

// ============================================================================
// Step 1: Dual Bus Scan
// ============================================================================

async function startBusScan() {
    var btn = document.getElementById('btn-smart-scan');
    btn.disabled = true;
    btn.textContent = 'Scanning...';

    var progress = document.getElementById('scan-progress');
    progress.classList.remove('hidden');

    var fill = document.getElementById('scan-progress-fill');
    var text = document.getElementById('scan-progress-text');

    fill.style.width = '10%';
    text.textContent = 'Scanning Bus 1 and Bus 2...';

    var pct = 10;
    var progressTimer = setInterval(function() {
        pct = Math.min(pct + 3, 90);
        fill.style.width = pct + '%';
    }, 1000);

    try {
        var r = await api('setup/scan-buses', 'POST');
        clearInterval(progressTimer);
        fill.style.width = '100%';

        // Parse Bus 1 results
        bus1Actuators = [];
        if (r.bus1 && r.bus1.actuators) {
            for (var i = 0; i < r.bus1.actuators.length; i++) {
                var a = r.bus1.actuators[i];
                a.bus = 1;
                bus1Actuators.push(a);
            }
        }
        baumerStatus = (r.bus1 && r.bus1.baumer) ? r.bus1.baumer : null;

        // Parse Bus 2 results
        bus2Actuators = [];
        if (r.bus2 && r.bus2.actuators) {
            for (var j = 0; j < r.bus2.actuators.length; j++) {
                var b = r.bus2.actuators[j];
                b.bus = 2;
                bus2Actuators.push(b);
            }
        }

        // Display Bus 1
        var bus1Div = document.getElementById('scan-bus1-results');
        var bus1Body = document.getElementById('scan-bus1-body');
        var bus1Empty = document.getElementById('scan-bus1-empty');
        bus1Div.classList.remove('hidden');

        if (bus1Actuators.length === 0) {
            bus1Empty.classList.remove('hidden');
            bus1Body.innerHTML = '';
        } else {
            bus1Empty.classList.add('hidden');
            bus1Body.innerHTML = bus1Actuators.map(function(a) {
                return '<tr><td>' + a.id + '</td><td>' + a.baud_rate + '</td><td>' + a.model + '</td></tr>';
            }).join('');
        }

        // Display Baumer status
        var baumerDiv = document.getElementById('scan-baumer-status');
        if (baumerStatus) {
            if (baumerStatus.detected) {
                baumerDiv.className = 'info-box success';
                baumerDiv.textContent = 'Baumer OX100 detected (ID=' + baumerStatus.id + ')';
            } else {
                baumerDiv.className = 'info-box warn';
                baumerDiv.textContent = 'Baumer OX100 not detected (ID=' + baumerStatus.id + ')';
            }
        }

        // Display Bus 2
        var bus2Div = document.getElementById('scan-bus2-results');
        var bus2Body = document.getElementById('scan-bus2-body');
        var bus2Empty = document.getElementById('scan-bus2-empty');
        bus2Div.classList.remove('hidden');

        if (bus2Actuators.length === 0) {
            bus2Empty.classList.remove('hidden');
            bus2Body.innerHTML = '';
        } else {
            bus2Empty.classList.add('hidden');
            bus2Body.innerHTML = bus2Actuators.map(function(a) {
                return '<tr><td>' + a.id + '</td><td>' + a.baud_rate + '</td><td>' + a.model + '</td></tr>';
            }).join('');
        }

        var totalFound = bus1Actuators.length + bus2Actuators.length;
        text.textContent = 'Scan complete - Bus 1: ' + bus1Actuators.length +
            ' actuator(s)' + (baumerStatus && baumerStatus.detected ? ' + Baumer' : '') +
            ', Bus 2: ' + bus2Actuators.length + ' actuator(s)';

        // Enable next step if we found anything
        document.getElementById('btn-step1-next').disabled = (totalFound === 0);

        // Auto-assign roles
        roleMap = {};
        // Bus 2 actuators -> lens_a, lens_b
        if (bus2Actuators.length >= 1) roleMap['2:0'] = 'lens_a';
        if (bus2Actuators.length >= 2) roleMap['2:1'] = 'lens_b';
        for (var k = 2; k < bus2Actuators.length; k++) roleMap['2:' + k] = 'skip';
        // Bus 1 actuators -> nozzle, then skip
        if (bus1Actuators.length >= 1) roleMap['1:0'] = 'nozzle';
        for (var m = 1; m < bus1Actuators.length; m++) roleMap['1:' + m] = 'skip';

        toast('Found ' + totalFound + ' actuator(s)', totalFound > 0 ? 'success' : 'info');
    } catch (e) {
        clearInterval(progressTimer);
        fill.style.width = '0%';
        text.textContent = 'Scan failed';
        toast('Bus scan failed', 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = 'Scan Both Buses';
    }
}

// ============================================================================
// Step 2: Role Assignment
// ============================================================================

function getActuatorByKey(key) {
    var parts = key.split(':');
    var bus = parseInt(parts[0]);
    var idx = parseInt(parts[1]);
    return bus === 2 ? bus2Actuators[idx] : bus1Actuators[idx];
}

function buildRoleAssignments() {
    // Bus 2 section (sync actuators)
    var bus2Container = document.getElementById('role-bus2-assignments');
    if (bus2Actuators.length === 0) {
        bus2Container.innerHTML = '<p>No actuators found on Bus 2.</p>';
    } else {
        bus2Container.innerHTML = bus2Actuators.map(function(a, i) {
            var key = '2:' + i;
            var sel = roleMap[key] || 'skip';
            return '<div class="role-row">' +
                '<div class="role-info"><strong>ID ' + a.id + '</strong> @ ' + a.baud_rate + ' baud (model: ' + a.model + ')</div>' +
                '<div class="role-controls">' +
                '<select id="role-select-' + key + '" onchange="updateRole(\'' + key + '\', this.value)">' +
                '<option value="skip"' + (sel === 'skip' ? ' selected' : '') + '>Skip</option>' +
                '<option value="lens_a"' + (sel === 'lens_a' ? ' selected' : '') + '>Lens Focus A (sync)</option>' +
                '<option value="lens_b"' + (sel === 'lens_b' ? ' selected' : '') + '>Lens Focus B (sync)</option>' +
                '</select>' +
                '<button onclick="jogActuator(' + a.id + ',' + a.baud_rate + ',2)" class="btn btn-small">Jog</button>' +
                '</div></div>';
        }).join('');
    }

    // Bus 1 section (independent actuators)
    var bus1Container = document.getElementById('role-bus1-assignments');
    if (bus1Actuators.length === 0) {
        bus1Container.innerHTML = '<p>No actuators found on Bus 1.</p>';
    } else {
        bus1Container.innerHTML = bus1Actuators.map(function(a, i) {
            var key = '1:' + i;
            var sel = roleMap[key] || 'skip';
            return '<div class="role-row">' +
                '<div class="role-info"><strong>ID ' + a.id + '</strong> @ ' + a.baud_rate + ' baud (model: ' + a.model + ')</div>' +
                '<div class="role-controls">' +
                '<select id="role-select-' + key + '" onchange="updateRole(\'' + key + '\', this.value)">' +
                '<option value="skip"' + (sel === 'skip' ? ' selected' : '') + '>Skip</option>' +
                '<option value="nozzle"' + (sel === 'nozzle' ? ' selected' : '') + '>Nozzle (independent)</option>' +
                '</select>' +
                '<button onclick="jogActuator(' + a.id + ',' + a.baud_rate + ',1)" class="btn btn-small">Jog</button>' +
                '</div></div>';
        }).join('');
    }

    validateRoles();
}

function updateRole(key, role) {
    roleMap[key] = role;
    validateRoles();
}

function validateRoles() {
    var roles = [];
    for (var k in roleMap) roles.push(roleMap[k]);
    var lensACount = roles.filter(function(r) { return r === 'lens_a'; }).length;
    var lensBCount = roles.filter(function(r) { return r === 'lens_b'; }).length;
    var nozzleCount = roles.filter(function(r) { return r === 'nozzle'; }).length;

    var validationDiv = document.getElementById('role-validation');
    var validationText = document.getElementById('role-validation-text');
    var nextBtn = document.getElementById('btn-step2-next');

    var errors = [];
    if (lensACount !== 1) errors.push('Lens A: need 1, have ' + lensACount);
    if (lensBCount !== 1) errors.push('Lens B: need 1, have ' + lensBCount);
    if (nozzleCount !== 1) errors.push('Nozzle: need 1, have ' + nozzleCount);

    if (errors.length > 0) {
        validationDiv.classList.remove('hidden');
        validationText.textContent = errors.join(' | ');
        nextBtn.disabled = true;
    } else {
        validationDiv.classList.add('hidden');
        nextBtn.disabled = false;
    }
}

async function jogActuator(id, baudRate, bus) {
    toast('Jogging actuator ' + id + ' on Bus ' + bus + '...', 'info');
    try {
        var r = await api('actuator/jog', 'POST', { id: id, baud_rate: baudRate, bus: bus });
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
    var mapping = {};
    for (var key in roleMap) {
        var role = roleMap[key];
        if (role !== 'skip') {
            var act = getActuatorByKey(key);
            if (act) mapping[role] = act;
        }
    }
    return mapping;
}

function buildStandardizePreview() {
    var mapping = getRoleMapping();
    var targetBaud = parseInt(document.getElementById('std-target-baud').value);
    var tbody = document.getElementById('std-changes-body');

    var roleNames = {
        lens_a: 'Lens Focus A',
        lens_b: 'Lens Focus B',
        nozzle: 'Nozzle'
    };
    var newIds = { lens_a: 1, lens_b: 2, nozzle: 3 };

    var html = '';
    var needsChanges = false;

    for (var role in mapping) {
        var act = mapping[role];
        var newId = newIds[role];
        var baudChanged = act.baud_rate !== targetBaud;
        var idChanged = act.id !== newId;

        if (baudChanged || idChanged) needsChanges = true;

        html += '<tr>' +
            '<td>' + roleNames[role] + '</td>' +
            '<td>Bus ' + act.bus + '</td>' +
            '<td>' + act.id + '</td>' +
            '<td>' + (idChanged ? '<strong>' + newId + '</strong>' : newId) + '</td>' +
            '<td>' + act.baud_rate + '</td>' +
            '<td>' + (baudChanged ? '<strong>' + targetBaud + '</strong>' : targetBaud) + '</td>' +
            '</tr>';
    }

    tbody.innerHTML = html;

    var stdBtn = document.getElementById('btn-standardize');
    if (!needsChanges) {
        stdBtn.textContent = 'No Changes Needed - Skip';
        stdBtn.onclick = function() {
            document.getElementById('btn-step3-next').disabled = false;
            goToStep(4);
        };
    } else {
        stdBtn.textContent = 'Apply Standardization';
        stdBtn.onclick = doStandardize;
    }

    document.getElementById('btn-step3-next').disabled = false;
}

async function doStandardize() {
    var mapping = getRoleMapping();
    var targetBaud = parseInt(document.getElementById('std-target-baud').value);
    var newIds = { lens_a: 1, lens_b: 2, nozzle: 3 };

    var actuators = [];
    for (var role in mapping) {
        var act = mapping[role];
        actuators.push({
            id: act.id,
            baud_rate: act.baud_rate,
            new_id: newIds[role],
            bus: act.bus
        });
    }

    var btn = document.getElementById('btn-standardize');
    btn.disabled = true;
    btn.textContent = 'Applying...';

    try {
        var r = await api('actuator/standardize', 'POST', {
            target_baud: targetBaud,
            actuators: actuators
        });

        var resultDiv = document.getElementById('std-result');
        resultDiv.classList.remove('hidden');

        if (r.success) {
            resultDiv.className = 'info-box success';
            resultDiv.textContent = 'Standardization complete! All actuators reconfigured.';
            document.getElementById('btn-step3-next').disabled = false;

            // Update local data to reflect new IDs/baud
            for (var role2 in mapping) {
                mapping[role2].id = newIds[role2];
                mapping[role2].baud_rate = targetBaud;
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
    var mapping = getRoleMapping();
    var content = document.getElementById('save-summary-content');

    var roleNames = {
        lens_a: 'Lens Focus A (sync, Bus 2)',
        lens_b: 'Lens Focus B (sync, Bus 2)',
        nozzle: 'Nozzle (independent, Bus 1)'
    };

    var html = '<ul>';
    for (var role in mapping) {
        var act = mapping[role];
        html += '<li><strong>' + roleNames[role] + '</strong>: ID ' + act.id + ' @ ' + act.baud_rate + ' baud</li>';
    }
    if (baumerStatus && baumerStatus.detected) {
        html += '<li><strong>Baumer OX100 (Bus 1)</strong>: ID ' + baumerStatus.id + '</li>';
    }
    html += '</ul>';
    content.innerHTML = html;
}

async function saveAndApply() {
    var mapping = getRoleMapping();
    var btn = document.getElementById('btn-save-roles');
    btn.disabled = true;
    btn.textContent = 'Saving...';

    var payload = {};
    for (var role in mapping) {
        payload[role] = { id: mapping[role].id, baud: mapping[role].baud_rate };
    }

    try {
        var r = await api('actuator/roles', 'POST', payload);

        var resultDiv = document.getElementById('save-result');
        resultDiv.classList.remove('hidden');

        if (r.success) {
            resultDiv.className = 'info-box success';
            resultDiv.innerHTML = 'Configuration saved successfully! The actuator tab will now use these roles.';
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
