// Tasks Monitor JavaScript
let tasksRefreshInterval = null;

async function refreshTasks() {
    try {
        const response = await fetch('/api/tasks');
        const data = await response.json();
        
        // Update stats
        document.getElementById('heap-free').textContent = formatBytes(data.heap_free);
        document.getElementById('heap-min').textContent = formatBytes(data.heap_min);
        document.getElementById('uptime').textContent = formatUptime(data.uptime_s);
        document.getElementById('task-count').textContent = data.task_count;
        
        // Sort tasks by CPU usage
        const tasks = data.tasks.sort((a, b) => b.cpu_percent - a.cpu_percent);
        
        // Update table
        const tbody = document.getElementById('tasks-tbody');
        tbody.innerHTML = tasks.map(task => `
            <tr>
                <td><strong>${task.name}</strong></td>
                <td><span class="state-${task.state}">${task.state}</span></td>
                <td>${task.priority}</td>
                <td>${task.cpu_percent}%</td>
                <td>${task.stack_hwm * 4} bytes</td>
            </tr>
        `).join('');
        
        // Update CPU bars (top 6 tasks)
        const cpuBars = document.getElementById('cpu-bars');
        cpuBars.innerHTML = tasks.slice(0, 6).map(task => `
            <div class="cpu-bar">
                <span class="cpu-bar-label">${task.name}</span>
                <div class="cpu-bar-track">
                    <div class="cpu-bar-fill" style="width: ${Math.min(task.cpu_percent, 100)}%"></div>
                </div>
                <span class="cpu-bar-value">${task.cpu_percent}%</span>
            </div>
        `).join('');
        
    } catch (error) {
        console.error('Failed to fetch tasks:', error);
    }
}

function formatBytes(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}

function formatUptime(seconds) {
    if (seconds < 60) return seconds + 's';
    if (seconds < 3600) return Math.floor(seconds / 60) + 'm ' + (seconds % 60) + 's';
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    return h + 'h ' + m + 'm';
}

function initTasksTab() {
    refreshTasks();
    
    const autoRefresh = document.getElementById('auto-refresh');
    if (autoRefresh) {
        autoRefresh.addEventListener('change', function() {
            if (this.checked) {
                tasksRefreshInterval = setInterval(refreshTasks, 2000);
            } else {
                clearInterval(tasksRefreshInterval);
            }
        });
        
        // Start auto-refresh
        if (autoRefresh.checked) {
            tasksRefreshInterval = setInterval(refreshTasks, 2000);
        }
    }
}

// Initialize when tab is loaded
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initTasksTab);
} else {
    initTasksTab();
}
