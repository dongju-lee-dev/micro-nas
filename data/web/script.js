let authToken = sessionStorage.getItem('authToken') || "";
let currentPath = "";
let statusInterval = null;
let historyStack = [];
let forwardStack = [];
let selectedFiles = new Set();

const loginView = document.getElementById('login-view');
const mainView = document.getElementById('main-view');
const passwordInput = document.getElementById('password');

// Check for existing session on load
window.onload = () => {
    if (authToken) {
        showMainView();
    } else {
        loginView.classList.remove('hidden');
    }
    setupDragAndDrop();
};

// Utility for API calls
async function apiPost(endpoint, data = {}, isRaw = false) {
    const options = {
        method: 'POST',
        headers: {
            'X-Auth-Token': authToken
        }
    };

    if (isRaw) {
        options.body = data;
    } else {
        options.headers['Content-Type'] = 'application/json';
        options.body = JSON.stringify(data);
    }

    try {
        const response = await fetch(endpoint, options);
        
        // Handle unauthorized or invalid token
        if (response.status === 401) {
            handleLogout();
            throw new Error("Unauthorized");
        }

        if (endpoint.includes('/api/download')) return response;
        
        const json = await response.json();
        
        // Some firmware might return 200 OK but with an "Unauthorized" message in JSON
        if (json.status === 'error' && (json.message === 'Unauthorized' || json.message === 'Auth failed')) {
            handleLogout();
        }
        
        return json;
    } catch (e) {
        if (e.message !== "Unauthorized") {
            console.error("API Call failed", e);
        }
        throw e;
    }
}

// Authentication
const performLogin = async () => {
    const password = passwordInput.value;
    try {
        const res = await fetch('/api/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ password })
        });
        const data = await res.json();
        if (data.status === 'success') {
            authToken = data.token;
            sessionStorage.setItem('authToken', authToken);
            showMainView();
        } else {
            alert('Login failed: ' + data.message);
        }
    } catch (e) {
        alert('Error: ' + e.message);
    }
};

const handleLogout = () => {
    authToken = "";
    sessionStorage.removeItem('authToken');
    if (statusInterval) clearInterval(statusInterval);
    loginView.classList.remove('hidden');
    mainView.classList.add('hidden');
};

document.getElementById('login-btn').onclick = performLogin;

passwordInput.onkeydown = (e) => {
    if (e.key === 'Enter') performLogin();
};

document.getElementById('logout-btn').onclick = handleLogout;

function showMainView() {
    loginView.classList.add('hidden');
    mainView.classList.remove('hidden');
    updateStatus();
    statusInterval = setInterval(updateStatus, 5000);
}

// Status Updates
async function updateStatus() {
    try {
        const data = await apiPost('/api/status');
        if (data.status === 'success') {
            document.getElementById('cpu-cores').innerText = data.cpu.cores;
            const ramUsage = Math.round(((data.memory.total_heap - data.memory.free_heap) / data.memory.total_heap) * 100);
            document.getElementById('ram-usage').innerText = ramUsage;
            document.getElementById('ram-free').innerText = Math.round(data.memory.free_heap / 1024);
            document.getElementById('uptime').innerText = data.uptime;
            document.getElementById('wifi-ssid').innerText = data.wifi.ssid;
            document.getElementById('wifi-rssi').innerText = data.wifi.rssi;
            document.getElementById('mount-status').innerText = data.storage_mounted ? "Mounted" : "Unmounted";

            renderStorageList(data.storage_details);
        }
    } catch (e) {
        console.error("Status update failed", e);
    }
}

function formatSize(bytes) {
    if (bytes === 0) return "0 B";
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function renderStorageList(details) {
    const list = document.getElementById('storage-list');
    list.innerHTML = "";
    details.forEach(card => {
        if (card.total === 0) return; // Skip zero capacity storages

        const div = document.createElement('div');
        div.className = `storage-card ${currentPath.startsWith(card.path) ? 'active' : ''}`;
        
        const usedPercent = card.total > 0 ? (card.used / card.total) * 100 : 0;
        const totalStr = formatSize(card.total);
        const usedStr = formatSize(card.used);

        div.innerHTML = `
            <div class="storage-info">
                <strong>${card.path}</strong><br>
                ${usedStr} / ${totalStr} (${Math.round(usedPercent)}%)
            </div>
            <div class="progress-bar">
                <div class="progress-fill" style="width: ${usedPercent}%"></div>
            </div>
        `;
        div.onclick = () => {
            if (currentPath !== card.path) {
                navigateTo(card.path);
            }
        };
        list.appendChild(div);
    });
}

// File Explorer
let selectedItem = null;
const contextMenu = document.getElementById('context-menu');
const pathInput = document.getElementById('current-path');

async function listDirectory(path, isNavigating = false) {
    const oldPath = currentPath;
    currentPath = path;
    pathInput.value = path;
    selectedFiles.clear();
    updateMultiDeleteUI();
    document.getElementById('select-all').checked = false;
    
    const tbody = document.getElementById('file-list');
    const toolbar = document.querySelector('.toolbar');
    
    tbody.innerHTML = '<tr><td colspan="2" style="text-align:center;">Loading...</td></tr>';
    toolbar.style.opacity = '0.5';
    toolbar.style.pointerEvents = 'none';

    try {
        const data = await apiPost('/api/list', { path });
        toolbar.style.opacity = '1';
        toolbar.style.pointerEvents = 'auto';

        if (data.status === 'success') {
            tbody.innerHTML = "";
            if (data.items.length === 0) {
                tbody.innerHTML = '<tr><td colspan="2" style="text-align:center;">No files found in this directory.</td></tr>';
            } else {
                data.items.sort((a,b) => b.is_dir - a.is_dir || a.name.localeCompare(b.name)).forEach(item => {
                    if (item.name === "." || item.name === "..") return;
                    
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td><input type="checkbox" class="file-checkbox" data-name="${item.name}"></td>
                        <td>${item.is_dir ? '📁' : '📄'} ${item.name}</td>
                    `;
                    
                    tr.onclick = (e) => {
                        if (e.target.type === 'checkbox') return;
                        e.stopPropagation();
                        showMenu(e.pageX, e.pageY, item);
                    };

                    tr.ondblclick = (e) => {
                        e.stopPropagation();
                        contextMenu.classList.add('hidden');
                        if (item.is_dir) {
                            const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + item.name;
                            navigateTo(fullPath);
                        }
                    };

                    const checkbox = tr.querySelector('.file-checkbox');
                    checkbox.onchange = () => {
                        if (checkbox.checked) selectedFiles.add(item.name);
                        else selectedFiles.delete(item.name);
                        updateMultiDeleteUI();
                    };

                    tbody.appendChild(tr);
                });
            }
            updateNavButtons();
        } else {
            tbody.innerHTML = '<tr><td colspan="2" style="text-align:center; color:red;">Failed to load: ' + data.message + '</td></tr>';
            currentPath = oldPath;
            // DO NOT update pathInput.value here to preserve user input
        }
    } catch (e) {
        toolbar.style.opacity = '1';
        toolbar.style.pointerEvents = 'auto';
        tbody.innerHTML = '<tr><td colspan="2" style="text-align:center; color:red;">Error connecting to API.</td></tr>';
        currentPath = oldPath;
        // DO NOT update pathInput.value here to preserve user input
    }
}

function updateMultiDeleteUI() {
    const btn = document.getElementById('delete-selected-btn');
    if (selectedFiles.size > 0) {
        btn.innerText = `Delete Selected (${selectedFiles.size})`;
        btn.classList.remove('hidden');
    } else {
        btn.classList.add('hidden');
    }
}

document.getElementById('select-all').onchange = (e) => {
    const checkboxes = document.querySelectorAll('.file-checkbox');
    checkboxes.forEach(cb => {
        cb.checked = e.target.checked;
        const name = cb.getAttribute('data-name');
        if (e.target.checked) selectedFiles.add(name);
        else selectedFiles.delete(name);
    });
    updateMultiDeleteUI();
};

document.getElementById('delete-selected-btn').onclick = async () => {
    if (confirm(`Delete ${selectedFiles.size} items? This will recursively delete folders.`)) {
        const files = Array.from(selectedFiles);
        for (let name of files) {
            const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + name;
            await apiPost('/api/delete', { path: fullPath });
        }
        listDirectory(currentPath);
    }
};

pathInput.onkeydown = (e) => {
    if (e.key === 'Enter') {
        const newPath = pathInput.value.trim();
        if (newPath && newPath !== currentPath) {
            navigateTo(newPath);
        }
    }
};

document.getElementById('refresh-btn').onclick = () => {
    if (currentPath) listDirectory(currentPath);
};

function navigateTo(path) {
    if (currentPath) {
        historyStack.push(currentPath);
    }
    forwardStack = []; // Clear forward history on new navigation
    listDirectory(path);
}

function updateNavButtons() {
    document.getElementById('back-btn').disabled = historyStack.length === 0;
    document.getElementById('forward-btn').disabled = forwardStack.length === 0;
}

document.getElementById('back-btn').onclick = () => {
    if (historyStack.length > 0) {
        forwardStack.push(currentPath);
        const prevPath = historyStack.pop();
        listDirectory(prevPath, true);
    }
};

document.getElementById('forward-btn').onclick = () => {
    if (forwardStack.length > 0) {
        historyStack.push(currentPath);
        const nextPath = forwardStack.pop();
        listDirectory(nextPath, true);
    }
};

function showMenu(x, y, item) {
    selectedItem = item;
    contextMenu.style.left = (x + 10) + 'px';
    contextMenu.style.top = y + 'px';
    contextMenu.classList.remove('hidden');

    // Toggle menu items based on type
    document.getElementById('menu-open').style.display = item.is_dir ? 'block' : 'none';
    document.getElementById('menu-download').style.display = item.is_dir ? 'none' : 'block';
}

// Hide menu on click elsewhere
window.addEventListener('click', () => {
    contextMenu.classList.add('hidden');
});

// Menu Action Handlers
document.getElementById('menu-open').onclick = () => {
    if (selectedItem.is_dir) {
        const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + selectedItem.name;
        navigateTo(fullPath);
    } else {
        alert("Cannot open file in browser.");
    }
};

document.getElementById('menu-rename').onclick = async () => {
    const name = selectedItem.name;
    const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + name;
    const newName = prompt("New name:", name);
    if (newName && newName !== name) {
        const newPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + newName;
        const res = await apiPost('/api/rename', { path: fullPath, new: newPath });
        if (res.status === 'success') listDirectory(currentPath);
        else alert("Rename failed");
    }
};

document.getElementById('menu-delete').onclick = async () => {
    if (confirm(`Delete ${selectedItem.name}? This will recursively delete folders.`)) {
        const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + selectedItem.name;
        const res = await apiPost('/api/delete', { path: fullPath });
        if (res.status === 'success') listDirectory(currentPath);
        else alert("Delete failed");
    }
};

document.getElementById('menu-download').onclick = async () => {
    const name = selectedItem.name;
    const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + name;
    const progressContainer = document.getElementById('download-progress-container');
    const progressFill = document.getElementById('download-progress-fill');
    const progressText = document.getElementById('download-progress-text');

    if (statusInterval) clearInterval(statusInterval);

    progressContainer.classList.remove('hidden');
    progressFill.style.width = '0%';
    progressText.innerText = '0%';

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/download', true);
    xhr.setRequestHeader('X-Auth-Token', authToken);
    xhr.setRequestHeader('Content-Type', 'application/json');
    xhr.responseType = 'blob';

    xhr.onprogress = (event) => {
        if (event.lengthComputable) {
            const percent = Math.round((event.loaded / event.total) * 100);
            progressFill.style.width = percent + '%';
            progressText.innerText = percent + '%';
        }
    };

    const resumeStatus = () => {
        progressContainer.classList.add('hidden');
        statusInterval = setInterval(updateStatus, 5000);
        updateStatus();
    };

    xhr.onload = () => {
        resumeStatus();
        if (xhr.status === 200) {
            const blob = xhr.response;
            const url = window.URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = name;
            document.body.appendChild(a);
            a.click();
            window.URL.revokeObjectURL(url);
        } else {
            alert("Download failed: " + xhr.status);
        }
    };

    xhr.onerror = () => {
        resumeStatus();
        alert("Download failed: Network error");
    };

    xhr.send(JSON.stringify({ path: fullPath }));
};

document.getElementById('menu-info').onclick = async () => {
    const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + selectedItem.name;
    try {
        const data = await apiPost('/api/info', { path: fullPath });
        if (data.status === 'success') {
            const modal = document.getElementById('modal-overlay');
            const content = document.getElementById('modal-content');
            
            const date = new Date(data.mtime * 1000).toLocaleString();
            const size = data.is_dir ? "-" : formatSize(data.size);
            
            content.innerHTML = `
                <strong>Name:</strong> ${data.name}<br>
                <strong>Type:</strong> ${data.is_dir ? "Directory" : "File"}<br>
                <strong>Size:</strong> ${size}<br>
                <strong>Modified:</strong> ${date}<br>
                <strong>Path:</strong> ${fullPath}
            `;
            modal.classList.remove('hidden');
        } else {
            alert("Failed to get properties");
        }
    } catch (e) {
        alert("Error fetching properties");
    }
};

document.getElementById('modal-close').onclick = () => {
    document.getElementById('modal-overlay').classList.add('hidden');
};

document.getElementById('modal-overlay').onclick = (e) => {
    if (e.target === document.getElementById('modal-overlay')) {
        document.getElementById('modal-overlay').classList.add('hidden');
    }
};

document.getElementById('mkdir-btn').onclick = async () => {
    const name = prompt("Folder name:");
    if (name) {
        const res = await apiPost('/api/new', { path: currentPath + '/' + name });
        if (res.status === 'success') listDirectory(currentPath);
        else alert("Failed to create folder");
    }
};

document.getElementById('touch-btn').onclick = async () => {
    const name = prompt("File name:");
    if (name) {
        const res = await apiPost('/api/touch', { path: currentPath + '/' + name });
        if (res.status === 'success') listDirectory(currentPath);
        else alert("Failed to create file");
    }
};

document.getElementById('upload-btn').onclick = () => document.getElementById('upload-input').click();

async function uploadSingleFile(file) {
    return new Promise((resolve, reject) => {
        const path = currentPath + (currentPath.endsWith('/') ? '' : '/') + file.name;
        const progressContainer = document.getElementById('upload-progress-container');
        const progressFill = document.getElementById('upload-progress-fill');
        const progressText = document.getElementById('upload-progress-text');

        progressContainer.classList.remove('hidden');
        progressFill.style.width = '0%';
        progressText.innerText = `0% (${file.name})`;

        const xhr = new XMLHttpRequest();
        xhr.open('POST', `/api/upload?path=${encodeURIComponent(path)}`, true);
        xhr.setRequestHeader('X-Auth-Token', authToken);

        xhr.upload.onprogress = (event) => {
            if (event.lengthComputable) {
                const percent = Math.round((event.loaded / event.total) * 100);
                progressFill.style.width = percent + '%';
                progressText.innerText = `${percent}% (${file.name})`;
            }
        };

        xhr.onload = () => {
            if (xhr.status === 200) resolve();
            else reject(new Error(xhr.statusText));
        };

        xhr.onerror = () => reject(new Error("Network error"));
        xhr.send(file);
    });
}

async function handleFileUploads(files) {
    if (files.length === 0) return;
    if (statusInterval) clearInterval(statusInterval);

    for (let file of files) {
        try {
            await uploadSingleFile(file);
        } catch (e) {
            alert(`Failed to upload ${file.name}: ${e.message}`);
        }
    }

    document.getElementById('upload-progress-container').classList.add('hidden');
    statusInterval = setInterval(updateStatus, 5000);
    updateStatus();
    listDirectory(currentPath);
}

document.getElementById('upload-input').onchange = (e) => {
    handleFileUploads(e.target.files);
};

function setupDragAndDrop() {
    const explorer = document.getElementById('explorer');
    const overlay = document.getElementById('drop-overlay');

    window.addEventListener('dragover', (e) => {
        e.preventDefault();
        overlay.classList.remove('hidden');
    });

    overlay.addEventListener('dragleave', (e) => {
        overlay.classList.add('hidden');
    });

    window.addEventListener('drop', (e) => {
        e.preventDefault();
        overlay.classList.add('hidden');
        if (e.dataTransfer.files.length > 0) {
            handleFileUploads(e.dataTransfer.files);
        }
    });
}
