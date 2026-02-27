// System Module (merged System + Tasks)
let systemRefreshInterval = null;

async function refreshSystem() {
    try {
        const [status, tasks] = await Promise.all([
            api('status'),
            fetch('/api/tasks').then(r => r.json())
        ]);

        // System overview - from /api/status
        document.getElementById('sys-ip').textContent = status.wifi_ip || '--';
        document.getElementById('sys-ssid').textContent = status.wifi_ssid || '--';
        document.getElementById('sys-rssi').textContent = status.wifi_rssi ? `${status.wifi_rssi} dBm` : '--';

        // System overview - from /api/tasks
        document.getElementById('sys-uptime').textContent = formatUptime(tasks.uptime_s * 1000);
        document.getElementById('sys-heap').textContent = formatBytes(tasks.heap_free);
        document.getElementById('sys-heap-min').textContent = formatBytes(tasks.heap_min);
        document.getElementById('sys-task-count').textContent = tasks.task_count;

        // Sort tasks by CPU usage
        const sorted = tasks.tasks.sort((a, b) => b.cpu_percent - a.cpu_percent);

        // Task list table
        const tbody = document.getElementById('tasks-tbody');
        tbody.innerHTML = sorted.map(task => `
            <tr>
                <td><strong>${task.name}</strong></td>
                <td><span class="state-${task.state}">${task.state}</span></td>
                <td>${task.priority}</td>
                <td>${task.cpu_percent}%</td>
                <td>${formatBytes(task.stack_hwm * 4)}</td>
            </tr>
        `).join('');

        // CPU bars - only Core 0 and Core 1
        const idleTasks = sorted.filter(t => t.name === 'IDLE0' || t.name === 'IDLE1');
        const cpuBars = document.getElementById('cpu-bars');
        cpuBars.innerHTML = idleTasks.map(task => {
            const coreNum = task.name === 'IDLE0' ? '0' : '1';
            const usagePercent = 100 - task.cpu_percent;
            return `
            <div class="cpu-bar">
                <span class="cpu-bar-label">Core ${coreNum}</span>
                <div class="cpu-bar-track">
                    <div class="cpu-bar-fill" style="width: ${Math.min(usagePercent, 100)}%"></div>
                </div>
                <span class="cpu-bar-value">${usagePercent}%</span>
            </div>`;
        }).join('');

    } catch (e) {
        console.error('Failed to refresh system:', e);
    }
}

async function restartDevice() {
    if (!confirm('Restart device?')) return;
    toast('Restarting...', 'info');
    try { await api('restart', 'POST'); } catch (e) {}
    setTimeout(() => location.reload(), 5000);
}

function initSystem() {
    refreshSystem();

    const autoRefresh = document.getElementById('auto-refresh');
    if (autoRefresh) {
        autoRefresh.addEventListener('change', function() {
            if (this.checked) {
                systemRefreshInterval = setInterval(refreshSystem, 2000);
            } else {
                clearInterval(systemRefreshInterval);
                systemRefreshInterval = null;
            }
        });

        if (autoRefresh.checked) {
            systemRefreshInterval = setInterval(refreshSystem, 2000);
        }
    }
}

function cleanupSystem() {
    if (systemRefreshInterval) {
        clearInterval(systemRefreshInterval);
        systemRefreshInterval = null;
    }
}

// Register module
registerModule('system', initSystem, cleanupSystem);