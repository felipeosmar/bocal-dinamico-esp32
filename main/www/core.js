// ESP32 Control Panel - Core Module
// Handles: Utilities, API, Toast, Navigation, Lazy Loading

(function() {
    'use strict';

    // ========================================================================
    // Module Registry
    // ========================================================================

    const modules = {
        actuators: { loaded: false, init: null, destroy: null },
        system: { loaded: false, init: null, destroy: null },
        setup: { loaded: false, init: null, destroy: null },
        profiler: { loaded: false, init: null, destroy: null },
        config: { loaded: false, init: null, destroy: null },
        files: { loaded: false, init: null, destroy: null }
    };

    // ========================================================================
    // Utilities
    // ========================================================================

    function toast(msg, type) {
        type = type || 'info';
        var t = document.getElementById('toast');
        t.textContent = msg;
        t.className = 'show ' + type;
        setTimeout(function() { t.className = ''; }, 2500);
    }

    function formatUptime(ms) {
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        var h = Math.floor(m / 60);
        if (h > 0) return h + 'h ' + (m % 60) + 'm';
        if (m > 0) return m + 'm ' + (s % 60) + 's';
        return s + 's';
    }

    function formatBytes(b) {
        if (b < 1024) return b + ' B';
        return (b / 1024).toFixed(1) + ' KB';
    }

    function escapeHtml(str) {
        var div = document.createElement('div');
        div.textContent = str;
        return div.innerHTML;
    }

    // ========================================================================
    // Authentication
    // ========================================================================

    var authCredentials = null;

    function getAuthHeaders() {
        var headers = { 'Content-Type': 'application/json' };
        if (authCredentials) {
            headers['Authorization'] = 'Basic ' + authCredentials;
        }
        return headers;
    }

    function promptLogin() {
        return new Promise(function(resolve) {
            var modal = document.getElementById('login-modal');
            var form = document.getElementById('login-form');
            var userInput = document.getElementById('login-user');
            var passInput = document.getElementById('login-pass');
            var cancelBtn = document.getElementById('login-cancel');

            modal.classList.add('show');
            userInput.value = '';
            passInput.value = '';
            userInput.focus();

            function cleanup() {
                modal.classList.remove('show');
                form.removeEventListener('submit', onSubmit);
                cancelBtn.removeEventListener('click', onCancel);
            }

            function onSubmit(e) {
                e.preventDefault();
                var user = userInput.value.trim();
                var pass = passInput.value;
                if (!user) { cleanup(); resolve(false); return; }
                authCredentials = btoa(user + ':' + pass);
                cleanup();
                resolve(true);
            }

            function onCancel() {
                cleanup();
                resolve(false);
            }

            form.addEventListener('submit', onSubmit);
            cancelBtn.addEventListener('click', onCancel);
        });
    }

    // ========================================================================
    // API Helper
    // ========================================================================

    async function api(endpoint, method, data) {
        method = method || 'GET';
        data = data || null;
        try {
            var opts = { method: method, headers: getAuthHeaders() };
            if (data) opts.body = JSON.stringify(data);
            var res = await fetch('/api/' + endpoint, opts);

            // Handle 401 - show login modal and retry once
            if (res.status === 401) {
                var loggedIn = await promptLogin();
                if (loggedIn) {
                    opts.headers = getAuthHeaders();
                    res = await fetch('/api/' + endpoint, opts);
                    if (res.status === 401) {
                        authCredentials = null;
                        toast('Authentication failed', 'error');
                        throw new Error('Authentication failed');
                    }
                } else {
                    throw new Error('Authentication required');
                }
            }

            // Any successful response means connection is working
            connectionState.connected = true;
            connectionState.lastSuccessfulPing = Date.now();

            return await res.json();
        } catch (e) {
            // Network error - trigger reconnection if we were connected
            if (e.message !== 'Authentication failed' && e.message !== 'Authentication required') {
                if (connectionState.connected) {
                    toast('Communication error', 'error');
                    startReconnection();
                }
            }
            throw e;
        }
    }

    // ========================================================================
    // Connection State Management
    // ========================================================================

    var connectionState = {
        connected: true,
        reconnecting: false,
        retryCount: 0,
        lastSuccessfulPing: null
    };

    var reconnectionTimer = null;

    function updateConnectionBanner() {
        var banner = document.getElementById('connection-banner');
        if (!banner) return;

        if (connectionState.connected) {
            banner.className = 'connection-banner hidden';
            banner.textContent = '';
        } else if (connectionState.reconnecting) {
            banner.className = 'connection-banner show reconnecting';
            banner.textContent = 'Connection lost. Reconnecting... (attempt ' + connectionState.retryCount + ')';
        } else {
            banner.className = 'connection-banner show disconnected';
            banner.textContent = 'Connection lost';
        }
    }

    function startReconnection() {
        if (connectionState.reconnecting) return;

        connectionState.connected = false;
        connectionState.reconnecting = true;
        connectionState.retryCount = 0;

        updateConnectionBanner();

        function scheduleNextAttempt() {
            if (connectionState.connected) return;

            connectionState.retryCount++;
            updateConnectionBanner();

            checkConnection().then(function(isConnected) {
                if (isConnected) {
                    connectionState.connected = true;
                    connectionState.reconnecting = false;
                    connectionState.retryCount = 0;
                    connectionState.lastSuccessfulPing = Date.now();
                    updateConnectionBanner();

                    // Show success briefly
                    var banner = document.getElementById('connection-banner');
                    if (banner) {
                        banner.className = 'connection-banner show connected';
                        banner.textContent = 'Connection restored';
                        setTimeout(function() { updateConnectionBanner(); }, 2000);
                    }
                } else {
                    // Exponential backoff: 1s, 1.5s, 2.25s, ... max 30s
                    var delay = Math.min(1000 * Math.pow(1.5, connectionState.retryCount - 1), 30000);
                    reconnectionTimer = setTimeout(scheduleNextAttempt, delay);
                }
            });
        }

        scheduleNextAttempt();
    }

    async function checkConnection() {
        try {
            var controller = new AbortController();
            var timeoutId = setTimeout(function() { controller.abort(); }, 5000);

            var res = await fetch('/api/status', {
                method: 'GET',
                headers: getAuthHeaders(),
                signal: controller.signal
            });

            clearTimeout(timeoutId);
            return res.ok;
        } catch (e) {
            return false;
        }
    }

    // ========================================================================
    // Status Badge Updates
    // ========================================================================

    async function updateStatusBadges() {
        try {
            var d = await api('status');
            var wifiBadge = document.getElementById('wifi-badge');
            var modbusBadge = document.getElementById('modbus-badge');
            wifiBadge.className = 'badge ' + (d.wifi_status >= 3 ? 'on' : 'off');
            modbusBadge.className = 'badge ' + (d.modbus_ready ? 'on' : 'off');
        } catch (e) {}
    }

    // ========================================================================
    // Lazy Loading System
    // ========================================================================

    async function loadModule(name) {
        if (modules[name].loaded) {
            if (modules[name].init) modules[name].init();
            return;
        }

        var container = document.getElementById('tab-' + name);

        try {
            // Load HTML
            var htmlRes = await fetch('tabs/' + name + '.html');
            if (!htmlRes.ok) throw new Error('HTML not found');
            var html = await htmlRes.text();
            container.innerHTML = html;

            // Load JS
            var script = document.createElement('script');
            script.src = 'tabs/' + name + '.js';
            script.onload = function() {
                modules[name].loaded = true;
                if (modules[name].init) modules[name].init();
            };
            script.onerror = function() {
                console.error('Failed to load ' + name + '.js');
                toast('Failed to load ' + name + ' module', 'error');
            };
            document.body.appendChild(script);
        } catch (e) {
            console.error('Failed to load module ' + name + ':', e);
            container.innerHTML = '<div class="tab-error">Failed to load module</div>';
        }
    }

    // Register module init and optional destroy functions
    function registerModule(name, initFn, destroyFn) {
        modules[name].init = initFn;
        modules[name].destroy = destroyFn || null;
        modules[name].loaded = true;
    }

    // ========================================================================
    // Tab Navigation
    // ========================================================================

    var currentTab = 'actuators';

    function switchTab(tabName) {
        // Destroy previous module (clean up intervals, listeners, etc.)
        if (modules[currentTab] && modules[currentTab].destroy) {
            modules[currentTab].destroy();
        }

        // Update nav buttons
        document.querySelectorAll('.nav-btn').forEach(function(b) {
            b.classList.remove('active');
        });
        var targetBtn = document.querySelector('.nav-btn[data-tab="' + tabName + '"]');
        if (targetBtn) targetBtn.classList.add('active');

        // Update tab visibility
        document.querySelectorAll('.tab').forEach(function(t) {
            t.classList.remove('active');
        });
        document.getElementById('tab-' + tabName).classList.add('active');

        currentTab = tabName;

        // Load module if needed
        loadModule(tabName);
    }

    // Setup navigation listeners
    document.querySelectorAll('.nav-btn').forEach(function(btn) {
        btn.addEventListener('click', function() { switchTab(btn.dataset.tab); });
    });

    // ========================================================================
    // Log Viewer
    // ========================================================================

    var logState = {
        expanded: false,
        lastSequence: 0,
        pollInterval: null,
        entries: [],
        errorCount: 0,
        warnCount: 0,
        lastRenderedIndex: 0,
        lastLevelFilter: '0',
        lastTagFilter: ''
    };

    var LOG_LEVELS = ['', 'E', 'W', 'I', 'D', 'V'];

    function toggleLogViewer() {
        var viewer = document.getElementById('log-viewer');
        logState.expanded = !logState.expanded;

        if (logState.expanded) {
            viewer.classList.remove('collapsed');
            viewer.classList.add('expanded');
            document.body.classList.add('log-expanded');
            startLogPolling();
        } else {
            viewer.classList.remove('expanded');
            viewer.classList.add('collapsed');
            document.body.classList.remove('log-expanded');
            stopLogPolling();
        }
    }

    function startLogPolling() {
        if (logState.pollInterval) return;
        fetchLogs();
        logState.pollInterval = setInterval(fetchLogs, 1500);
    }

    function stopLogPolling() {
        if (logState.pollInterval) {
            clearInterval(logState.pollInterval);
            logState.pollInterval = null;
        }
    }

    async function fetchLogs() {
        try {
            var data = await api('logs?since=' + logState.lastSequence);

            if (data.logs && data.logs.length > 0) {
                for (var i = 0; i < data.logs.length; i++) {
                    var log = data.logs[i];
                    logState.entries.push(log);
                    if (log.lvl === 1) logState.errorCount++;
                    if (log.lvl === 2) logState.warnCount++;
                }

                // Keep only last 200 entries in memory
                if (logState.entries.length > 200) {
                    var removed = logState.entries.splice(0, logState.entries.length - 200);
                    for (var j = 0; j < removed.length; j++) {
                        if (removed[j].lvl === 1) logState.errorCount--;
                        if (removed[j].lvl === 2) logState.warnCount--;
                    }
                    logState.lastRenderedIndex = 0;
                }

                logState.lastSequence = data.sequence;
                renderLogs();
            }

            updateLogBadge();
        } catch (e) {
            // Silently ignore - connection handling is done elsewhere
        }
    }

    function renderLogEntry(log) {
        var ts = formatLogTimestamp(log.ts);
        var lvl = LOG_LEVELS[log.lvl] || '?';
        return '<div class="log-entry level-' + log.lvl + '">' +
            '<span class="log-ts">' + ts + '</span>' +
            '<span class="log-lvl">' + lvl + '</span>' +
            '<span class="log-tag">' + escapeHtml(log.tag) + '</span>' +
            '<span class="log-msg">' + escapeHtml(log.msg) + '</span>' +
            '</div>';
    }

    function logMatchesFilter(log, levelFilter, tagFilter) {
        if (levelFilter > 0 && log.lvl > levelFilter) return false;
        if (tagFilter && !log.tag.toLowerCase().includes(tagFilter)) return false;
        return true;
    }

    function renderLogs() {
        var container = document.getElementById('log-entries');
        var levelFilter = parseInt(document.getElementById('log-level-filter').value);
        var tagFilter = document.getElementById('log-tag-filter').value.toLowerCase();
        var autoScroll = document.getElementById('log-autoscroll').checked;

        // Check if filters changed - if so, do full rebuild
        var filtersChanged = (levelFilter.toString() !== logState.lastLevelFilter ||
                             tagFilter !== logState.lastTagFilter);
        logState.lastLevelFilter = levelFilter.toString();
        logState.lastTagFilter = tagFilter;

        if (filtersChanged) {
            logState.lastRenderedIndex = 0;
            container.innerHTML = '';
        }

        // Incremental append: only render entries from lastRenderedIndex onward
        var html = '';
        var hasNew = false;
        for (var i = logState.lastRenderedIndex; i < logState.entries.length; i++) {
            var log = logState.entries[i];
            if (logMatchesFilter(log, levelFilter, tagFilter)) {
                html += renderLogEntry(log);
                hasNew = true;
            }
        }
        logState.lastRenderedIndex = logState.entries.length;

        if (hasNew) {
            var emptyEl = container.querySelector('.log-empty');
            if (emptyEl) emptyEl.remove();
            container.insertAdjacentHTML('beforeend', html);
        }

        if (container.children.length === 0) {
            container.innerHTML = '<div class="log-empty">No logs to display</div>';
            return;
        }

        if (autoScroll) {
            container.scrollTop = container.scrollHeight;
        }
    }

    function formatLogTimestamp(ms) {
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        var sec = s % 60;
        var msec = ms % 1000;
        return m + ':' + sec.toString().padStart(2, '0') + '.' + msec.toString().padStart(3, '0');
    }

    function updateLogBadge() {
        var badge = document.getElementById('log-count');
        var count = logState.entries.length;
        badge.textContent = count;

        badge.classList.remove('has-errors', 'has-warnings');
        if (logState.errorCount > 0) {
            badge.classList.add('has-errors');
        } else if (logState.warnCount > 0) {
            badge.classList.add('has-warnings');
        }
    }

    async function clearLogs() {
        try {
            await api('logs/clear', 'POST');
            logState.entries = [];
            logState.lastSequence = 0;
            logState.errorCount = 0;
            logState.warnCount = 0;
            logState.lastRenderedIndex = 0;
            renderLogs();
            updateLogBadge();
            toast('Logs cleared', 'success');
        } catch (e) {
            toast('Failed to clear logs', 'error');
        }
    }

    function updateLogFilter() {
        renderLogs();
    }

    // ========================================================================
    // Initialization
    // ========================================================================

    document.addEventListener('DOMContentLoaded', function() {
        updateStatusBadges();
        setInterval(updateStatusBadges, 10000);

        // Load initial tab
        loadModule('actuators');

        // Setup log filter listeners
        document.getElementById('log-level-filter').addEventListener('change', updateLogFilter);
        document.getElementById('log-tag-filter').addEventListener('input', updateLogFilter);
    });

    // ========================================================================
    // Public API - expose to global scope for tab modules
    // ========================================================================

    window.api = api;
    window.toast = toast;
    window.formatUptime = formatUptime;
    window.formatBytes = formatBytes;
    window.escapeHtml = escapeHtml;
    window.registerModule = registerModule;
    window.switchTab = switchTab;
    window.toggleLogViewer = toggleLogViewer;
    window.clearLogs = clearLogs;
    window.updateLogFilter = updateLogFilter;
    window.getAuthHeaders = getAuthHeaders;

})();
