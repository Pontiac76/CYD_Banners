function showTab(name) {
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
  document.querySelectorAll('.tab')[['dashboard','content','devices','diagnostics','settings'].indexOf(name)].classList.add('active');
  document.getElementById('tab-' + name).classList.add('active');
  if (name === 'devices') loadDevices();
  if (name === 'content') loadContentDirs();
  if (name === 'diagnostics') loadSize();
}

async function loadDevices() {
  var b = document.getElementById('device-body');
  try {
    var rowTemplate = await fetchText('/cyd-banners/static/fragments/device_row.html');
    var r = await fetch('/cyd-banners/api/devices');
    var devs = await r.json();
    var sizes = [];
    for (var i = 0; i < devs.length; i++) {
      try {
        var sr = await fetch('/cyd-banners/api/device_size?mac=' + encodeURIComponent(devs[i].mac));
        sizes[i] = await sr.json();
      } catch(e) {
        sizes[i] = {content_size: 0, sd_size_gb: devs[i].sd_size_gb, sd_size_bytes: devs[i].sd_size_gb * 1024 * 1024 * 1024, used_pct: 0};
      }
    }
    b.innerHTML = devs.map(function(d, i) {
      var sz = sizes[i] || {content_size: 0, sd_size_gb: d.sd_size_gb, sd_size_bytes: d.sd_size_gb * 1024 * 1024 * 1024, used_pct: 0};
      var usedPct = sz.used_pct || 0;
      var barColor = usedPct > 90 ? '#b00020' : usedPct > 70 ? '#d0a000' : '#4a8aff';
      return renderTemplate(rowTemplate, {
        ALIAS: escapeHtml(d.alias),
        MAC: escapeHtml(d.mac),
        MAC_JS: escapeJs(d.mac),
        MAC_URL: encodeURIComponent(d.mac),
        LAST_SEEN: escapeHtml(last_seen_age_text(d.last_seen)),
        LAST_IP: escapeHtml(d.last_ip),
        CALL_COUNT: String(d.call_count),
        UPDATE_STATUS: deviceUpdateStatusHtml(d),
        SD_OPTIONS: sdOptionsHtml(d.sd_size_gb),
        PLAYLIST: escapeHtml(d.playlist),
        CONTENT_SIZE: format_bytes(sz.content_size),
        SD_SIZE_LABEL: sdSizeLabel(sz.sd_size_gb),
        USED_PCT: usedPct.toFixed(1),
        USED_PCT_RAW: String(usedPct),
        BAR_COLOR: barColor
      });
    }).join('') || '<tr><td colspan="9" style="text-align:center">No devices</td></tr>';
  } catch(e) {
    b.innerHTML = '<tr><td colspan="9" style="text-align:center;color:red">Error loading devices: ' + escapeHtml(String(e)) + '</td></tr>';
  }
}

function deviceUpdateStatusHtml(d) {
  var parts = [];
  if (d.update) parts.push('<div>' + escapeHtml(d.update) + '</div>');
  var counts = [];
  if ((d.priority_count || 0) > 0) counts.push('P ' + d.priority_count);
  if ((d.background_count || 0) > 0) counts.push('B ' + d.background_count);
  if (counts.length) parts.push('<small>' + escapeHtml(counts.join(' / ')) + '</small>');
  return parts.join('') || '<span class="muted">unknown</span>';
}

async function loadContentDirs() {
  const r = await fetch('/cyd-banners/api/directories');
  const dirs = await r.json();
  const c = document.getElementById('content-list');
  c.innerHTML = dirs.map(function(d) {
    var name = escapeHtml(d.name);
    return '<div class="panel" style="display:flex;align-items:center;gap:1rem">' +
      '<strong>' + name + '</strong>' +
      '<span>' + d.image_count + ' images</span>' +
      '<span>' + format_bytes(d.total_size) + '</span>' +
      '<form method="post" action="/cyd-banners/api/regenerate_dir?dir=' + escapeHtml(d.name) + '" style="display:inline">' +
      '<button type="submit">Regenerate images</button>' +
      '</form>' +
      '</div>';
  }).join('') || '<p>No directories</p>';
}

async function loadSize() {
  const r = await fetch('/cyd-banners/api/size');
  const s = await r.json();
  const c = document.getElementById('size-chart');
  var html = '<p>Total: ' + format_bytes(s.total) + '</p>';
  Object.entries(s.by_dir).forEach(function(entry) {
    var n = entry[0], v = entry[1];
    var pct = s.total > 0 ? (v/s.total*100).toFixed(1) : 0;
    html += '<div style="margin:.5rem 0"><strong>' + escapeHtml(n) + '</strong> ' + format_bytes(v) + ' (' + pct + '%)' +
      '<div class="bar"><div class="bar-fill" style="width:' + pct + '%"></div><div class="bar-text">' + pct + '%</div></div></div>';
  });
  c.innerHTML = html;
}

async function updateDevice(mac, field, value) {
  const fd = new FormData();
  fd.set('mac', mac); fd.set(field, value);
  const resp = await fetch('/cyd-banners/api/device', {method:'POST', body:fd});
  if (resp.ok) loadDevices();
}

async function showDevicePlaylist(mac) {
  var panel = document.getElementById('device-playlist-panel');
  var title = document.getElementById('device-playlist-title');
  var out = document.getElementById('device-playlist-output');
  panel.style.display = 'block';
  title.textContent = 'Rendered playlist for ' + mac;
  out.textContent = 'Loading...';
  try {
    var r = await fetch('/cyd-banners/api/device_playlist?mac=' + encodeURIComponent(mac));
    var data = await r.json();
    if (!r.ok) throw new Error(data.error || ('HTTP ' + r.status));
    title.textContent = 'Rendered playlist for ' + data.mac + ' (' + data.count + ' items)';
    out.textContent = data.playlist.join('\n') || '(empty playlist)';
  } catch(e) {
    out.textContent = 'Error loading rendered playlist: ' + String(e);
  }
}

function last_seen_age_text(value) {
  if (!value || value === 'unknown') return value || 'never';
  try {
    var seen = new Date(value.replace('Z', '+00:00'));
    var diff = (Date.now() - seen.getTime()) / 1000;
    var days = Math.floor(diff / 86400);
    var hours = Math.floor((diff % 86400) / 3600);
    var mins = Math.floor((diff % 3600) / 60);
    return days + 'd:' + String(hours).padStart(2,'0') + 'h:' + String(mins).padStart(2,'0') + 'm ago';
  } catch(e) { return 'unknown'; }
}

function format_bytes(b) {
  var units = ['B','KB','MB','GB','TB'];
  var i = 0;
  while (b >= 1024 && i < units.length - 1) { b /= 1024; i++; }
  return b.toFixed(1) + ' ' + units[i];
}

var textCache = {};
async function fetchText(url) {
  if (!textCache[url]) {
    var r = await fetch(url);
    if (!r.ok) throw new Error('Failed to fetch ' + url + ': ' + r.status);
    textCache[url] = await r.text();
  }
  return textCache[url];
}

function renderTemplate(template, values) {
  Object.keys(values).forEach(function(key) {
    template = template.split('%%' + key + '%%').join(values[key]);
  });
  return template;
}

function sdOptionsHtml(current) {
  return [4,8,16,32,64,128,256,512,1024].map(function(s) {
    var label = sdSizeLabel(s);
    var sel = current == s ? ' selected' : '';
    return '<option value="' + s + '"' + sel + '>' + label + '</option>';
  }).join('');
}

function sdSizeLabel(gb) {
  return gb >= 1024 ? (gb/1024) + ' TB' : gb + ' GB';
}

function escapeJs(s) {
  return String(s || '').replace(/\\/g, '\\\\').replace(/'/g, "\\'").replace(/\n/g, '\\n').replace(/\r/g, '');
}

function escapeHtml(s) {
  var d = document.createElement('div');
  d.appendChild(document.createTextNode(String(s || '')));
  return d.innerHTML;
}
