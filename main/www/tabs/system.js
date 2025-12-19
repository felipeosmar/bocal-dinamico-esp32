// System Module

// ============================================================================
// Status
// ============================================================================

async function refreshSystem() {
    try {
        const d = await api('status');
        document.getElementById('sys-ip').textContent = d.wifi_ip || '--';
        document.getElementById('sys-ssid').textContent = d.wifi_ssid || '--';
        document.getElementById('sys-rssi').textContent = d.wifi_rssi ? `${d.wifi_rssi} dBm` : '--';
        document.getElementById('sys-uptime').textContent = formatUptime(d.uptime_ms);
        document.getElementById('sys-heap').textContent = formatBytes(d.heap_free);
    } catch (e) {}
}

async function restartDevice() {
    if (!confirm('Restart device?')) return;
    toast('Restarting...', 'info');
    try { await api('restart', 'POST'); } catch (e) {}
    setTimeout(() => location.reload(), 5000);
}

// ============================================================================
// Module Init
// ============================================================================

function initSystem() {
    refreshSystem();
}

// Register module
registerModule('system', initSystem);
