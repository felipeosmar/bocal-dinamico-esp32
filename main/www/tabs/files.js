// Files Module

let fmPartition = 'www';
let fmPath = '/';
let fmEditingFile = null;

// ============================================================================
// Partition & Navigation
// ============================================================================

function fmSelectPartition(partition) {
    fmPartition = partition;
    fmPath = '/';
    document.querySelectorAll('.fm-tab').forEach(t => {
        t.classList.toggle('active', t.dataset.partition === partition);
    });
    fmUpdatePath();
    fmRefresh();
}

function fmUpdatePath() {
    document.getElementById('fm-current-path').textContent = `/${fmPartition}${fmPath}`;
}

function fmNavigateTo(name) {
    fmPath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    fmUpdatePath();
    fmRefresh();
}

function fmNavigateUp() {
    const parts = fmPath.split('/').filter(p => p);
    parts.pop();
    fmPath = parts.length ? '/' + parts.join('/') : '/';
    fmUpdatePath();
    fmRefresh();
}

// ============================================================================
// File List
// ============================================================================

async function fmRefresh() {
    const list = document.getElementById('fm-file-list');
    list.innerHTML = '<div class="fm-empty">Loading...</div>';

    try {
        const res = await fetch(`/api/files/list?partition=${fmPartition}&dir=${encodeURIComponent(fmPath)}`);
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        fmRenderFiles(data.files || []);
        fmUpdateStorageInfo();
    } catch (e) {
        list.innerHTML = `<div class="fm-empty">Error: ${e.message}</div>`;
    }
}

function fmRenderFiles(files) {
    const list = document.getElementById('fm-file-list');

    if (files.length === 0 && fmPath === '/') {
        list.innerHTML = '<div class="fm-empty">No files</div>';
        return;
    }

    let html = '';

    // Parent directory
    if (fmPath !== '/') {
        html += `
            <div class="fm-file-item dir" onclick="fmNavigateUp()">
                <span class="fm-file-icon">📁</span>
                <div class="fm-file-info">
                    <div class="fm-file-name">..</div>
                    <div class="fm-file-size">Parent</div>
                </div>
            </div>`;
    }

    // Sort: dirs first
    files.sort((a, b) => {
        if (a.isDir && !b.isDir) return -1;
        if (!a.isDir && b.isDir) return 1;
        return a.name.localeCompare(b.name);
    });

    files.forEach(f => {
        const icon = f.isDir ? '📁' : fmGetIcon(f.name);
        const size = f.isDir ? '' : formatBytes(f.size);
        const canEdit = !f.isDir && fmIsText(f.name);
        const eName = fmEscape(f.name);

        html += `
            <div class="fm-file-item ${f.isDir ? 'dir' : ''}" ${f.isDir ? `onclick="fmNavigateTo('${eName}')"` : ''}>
                <span class="fm-file-icon">${icon}</span>
                <div class="fm-file-info">
                    <div class="fm-file-name">${eName}</div>
                    <div class="fm-file-size">${size}</div>
                </div>
                <div class="fm-file-actions" onclick="event.stopPropagation()">
                    ${!f.isDir ? `<button onclick="fmDownload('${eName}')">DL</button>` : ''}
                    ${canEdit ? `<button onclick="fmEdit('${eName}')">Edit</button>` : ''}
                    <button class="del" onclick="fmDelete('${eName}', ${f.isDir})">Del</button>
                </div>
            </div>`;
    });

    list.innerHTML = html || '<div class="fm-empty">No files</div>';
}

// ============================================================================
// File Operations
// ============================================================================

function fmDownload(name) {
    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    window.open(`/api/files/download?partition=${fmPartition}&file=${encodeURIComponent(filepath)}`, '_blank');
}

async function fmEdit(name) {
    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;

    try {
        const res = await fetch(`/api/files/read?partition=${fmPartition}&file=${encodeURIComponent(filepath)}`);
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        fmEditingFile = filepath;
        document.getElementById('fm-editor-title').textContent = name;
        document.getElementById('fm-editor-content').value = data.content;
        document.getElementById('fm-editor-modal').classList.add('show');
    } catch (e) {
        toast(e.message, 'error');
    }
}

function fmCloseEditor() {
    document.getElementById('fm-editor-modal').classList.remove('show');
    fmEditingFile = null;
}

async function fmSaveFile() {
    if (!fmEditingFile) return;

    const content = document.getElementById('fm-editor-content').value;
    const formData = new FormData();
    formData.append('file', fmEditingFile);
    formData.append('content', content);

    try {
        const res = await fetch(`/api/files/write?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('File saved', 'success');
        fmCloseEditor();
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

async function fmDelete(name, isDir) {
    if (!confirm(`Delete ${isDir ? 'folder' : 'file'} "${name}"?`)) return;

    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    const formData = new FormData();
    formData.append('file', filepath);

    try {
        const res = await fetch(`/api/files/delete?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('Deleted', 'success');
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

async function fmCreateFolder() {
    const name = prompt('Folder name:');
    if (!name) return;

    if (!/^[a-zA-Z0-9_\-\.]+$/.test(name)) {
        toast('Invalid name', 'error');
        return;
    }

    const dirpath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    const formData = new FormData();
    formData.append('dir', dirpath);

    try {
        const res = await fetch(`/api/files/mkdir?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('Folder created', 'success');
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

async function fmCreateFile() {
    const name = prompt('File name (with extension):');
    if (!name) return;

    if (!/^[a-zA-Z0-9_\-\.]+$/.test(name)) {
        toast('Invalid name', 'error');
        return;
    }

    const filepath = fmPath === '/' ? `/${name}` : `${fmPath}/${name}`;
    const formData = new FormData();
    formData.append('file', filepath);
    formData.append('content', '');

    try {
        const res = await fetch(`/api/files/write?partition=${fmPartition}`, {
            method: 'POST',
            body: formData
        });
        const data = await res.json();

        if (!res.ok) throw new Error(data.error || 'Failed');

        toast('File created', 'success');
        fmRefresh();
    } catch (e) {
        toast(e.message, 'error');
    }
}

// ============================================================================
// Upload
// ============================================================================

function fmHandleFileSelect(e) {
    for (let f of e.target.files) fmUpload(f);
    e.target.value = '';
}

async function fmUpload(file) {
    const progress = document.getElementById('fm-progress');
    const fill = document.getElementById('fm-progress-fill');

    progress.classList.add('show');
    fill.style.width = '0%';
    fill.textContent = '0%';

    const formData = new FormData();
    formData.append('file', file);

    const xhr = new XMLHttpRequest();

    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const pct = Math.round((e.loaded / e.total) * 100);
            fill.style.width = pct + '%';
            fill.textContent = pct + '%';
        }
    });

    xhr.addEventListener('load', () => {
        progress.classList.remove('show');
        if (xhr.status === 200) {
            toast(`Uploaded: ${file.name}`, 'success');
            fmRefresh();
        } else {
            toast('Upload failed', 'error');
        }
    });

    xhr.addEventListener('error', () => {
        progress.classList.remove('show');
        toast('Upload error', 'error');
    });

    xhr.open('POST', `/api/files/upload?partition=${fmPartition}&dir=${encodeURIComponent(fmPath)}`);
    xhr.send(formData);
}

async function fmUpdateStorageInfo() {
    try {
        const res = await fetch('/api/files/info');
        const data = await res.json();

        if (data.www) {
            document.getElementById('www-size').textContent = `${formatBytes(data.www.used)}/${formatBytes(data.www.total)}`;
        }
        if (data.userdata) {
            document.getElementById('userdata-size').textContent = `${formatBytes(data.userdata.used)}/${formatBytes(data.userdata.total)}`;
        }
    } catch (e) {}
}

// ============================================================================
// Utilities
// ============================================================================

function fmGetIcon(name) {
    const ext = name.split('.').pop().toLowerCase();
    const icons = {
        html: '📄', htm: '📄', css: '🎨', js: '📜', json: '📋',
        txt: '📝', md: '📝', png: '🖼️', jpg: '🖼️', ico: '🖼️', bin: '💾'
    };
    return icons[ext] || '📄';
}

function fmIsText(name) {
    const ext = name.split('.').pop().toLowerCase();
    return ['html', 'htm', 'css', 'js', 'json', 'txt', 'md', 'xml', 'csv', 'cfg', 'conf', 'ini', 'log'].includes(ext);
}

function fmEscape(s) {
    const d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
}

// ============================================================================
// Module Init
// ============================================================================

function initFiles() {
    // Setup drag & drop
    const zone = document.getElementById('fm-upload-zone');
    if (zone) {
        zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('dragging'); });
        zone.addEventListener('dragleave', () => zone.classList.remove('dragging'));
        zone.addEventListener('drop', (e) => {
            e.preventDefault();
            zone.classList.remove('dragging');
            for (let f of e.dataTransfer.files) fmUpload(f);
        });
        zone.addEventListener('click', () => document.getElementById('fm-file-input').click());
    }

    // Setup modal close on outside click
    const modal = document.getElementById('fm-editor-modal');
    if (modal) {
        modal.addEventListener('click', (e) => {
            if (e.target === modal) fmCloseEditor();
        });
    }

    // Initial load
    fmRefresh();
}

// Register module
registerModule('files', initFiles);
