const peerSelect = document.querySelector('#peer');
const manualHost = document.querySelector('#manualHost');
const statusEl = document.querySelector('#status');
const interfacesEl = document.querySelector('#interfaces');
const dropzone = document.querySelector('#dropzone');
const fileInput = document.querySelector('#fileInput');
const logEl = document.querySelector('#log');

document.querySelector('#refresh').addEventListener('click', refresh);
fileInput.addEventListener('change', () => sendFiles([...fileInput.files]));

dropzone.addEventListener('dragover', (event) => {
  event.preventDefault();
  dropzone.classList.add('dragover');
});

dropzone.addEventListener('dragleave', () => dropzone.classList.remove('dragover'));
dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('dragover');
  sendFiles([...event.dataTransfer.files]);
});

refresh();

async function refresh() {
  statusEl.textContent = 'Scanning for peers...';
  const [peers, interfaces] = await Promise.all([
    fetch('/api/discover').then((res) => res.json()),
    fetch('/api/interfaces').then((res) => res.json())
  ]);

  peerSelect.innerHTML = '';
  for (const peer of peers) {
    const option = document.createElement('option');
    option.value = JSON.stringify({ host: peer.host, port: peer.port });
    option.textContent = `${peer.name} (${peer.host}:${peer.port})`;
    peerSelect.append(option);
  }

  statusEl.textContent = peers.length ? `${peers.length} peer${peers.length === 1 ? '' : 's'} available` : 'No peers found';
  interfacesEl.innerHTML = interfaces.map((entry) => (
    `<div class="iface"><span>${entry.name}</span><span>${entry.address}</span><span>${entry.linkLocal ? 'link-local' : 'network'}</span></div>`
  )).join('');
}

async function sendFiles(files) {
  const endpoint = manualHost.value.trim()
    ? parseEndpoint(manualHost.value.trim())
    : peerSelect.value
      ? JSON.parse(peerSelect.value)
      : null;
  if (!endpoint?.host) {
    log('Choose a peer or enter a host first.');
    return;
  }

  for (const file of files) {
    const relativePath = file.webkitRelativePath || file.name;
    log(`Sending ${relativePath} (${formatBytes(file.size)})`);
    const response = await fetch(`/api/send-upload?host=${encodeURIComponent(endpoint.host)}&port=${encodeURIComponent(endpoint.port ?? 44777)}&path=${encodeURIComponent(relativePath)}&size=${file.size}`, {
      method: 'PUT',
      body: file
    });
    const result = await response.json();
    if (!response.ok || result.error) {
      log(`Failed: ${result.error ?? response.statusText}`);
      return;
    }
    log(`Done ${relativePath}`);
  }
}

function parseEndpoint(value) {
  if (value.startsWith('[')) {
    const end = value.indexOf(']');
    if (end !== -1) {
      return {
        host: value.slice(1, end),
        port: value[end + 1] === ':' ? value.slice(end + 2) : '44777'
      };
    }
  }

  const colonCount = (value.match(/:/g) ?? []).length;
  if (colonCount === 1) {
    const [host, port] = value.split(':');
    return { host, port };
  }

  return { host: value, port: '44777' };
}

function log(message) {
  logEl.textContent += `[${new Date().toLocaleTimeString()}] ${message}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function formatBytes(bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}
