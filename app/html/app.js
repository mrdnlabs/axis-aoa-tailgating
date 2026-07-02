'use strict';

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

const API_BASE     = `/local/antitailgate/admin`;
const INGEST_BASE  = `/local/antitailgate/ingest`;
const POLL_MS     = 2000;
let alarmPassConfigured = false;
let clearAlarmPassword = false;

/* ------------------------------------------------------------------ */
/* Tab navigation                                                      */
/* ------------------------------------------------------------------ */

document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
    if (btn.dataset.tab === 'settings') { loadConfig(); populateBadgeUrl(); }
  });
});

/* ------------------------------------------------------------------ */
/* Toast notifications                                                 */
/* ------------------------------------------------------------------ */

let toastTimer = null;

function showToast(msg, type = 'info') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'show ' + type;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = ''; }, 3000);
}

/* ------------------------------------------------------------------ */
/* API helpers                                                         */
/* ------------------------------------------------------------------ */

async function apiFetch(path, opts = {}) {
  /* X-Requested-With is the CSRF gate on admin mutating endpoints; cross-
   * origin simple forms cannot set custom request headers. */
  const headers = {
    'Content-Type':     'application/json',
    'X-Requested-With': 'antitailgate',
    ...(opts.headers || {}),
  };
  const resp = await fetch(API_BASE + path, {
    credentials: 'same-origin',
    ...opts,
    headers,
  });
  const data = await resp.json();
  if (!resp.ok) {
    throw new Error(data.message || `HTTP ${resp.status}`);
  }
  return data;
}

async function ingestFetch(path, opts = {}) {
  const headers = {
    'Content-Type': 'application/json',
    ...(opts.headers || {}),
  };
  const resp = await fetch(INGEST_BASE + path, {
    credentials: 'same-origin',
    ...opts,
    headers,
  });
  const data = await resp.json();
  if (!resp.ok) {
    throw new Error(data.message || `HTTP ${resp.status}`);
  }
  return data;
}

/* ------------------------------------------------------------------ */
/* Dashboard: polling status                                          */
/* ------------------------------------------------------------------ */

const pollIndicator = document.getElementById('poll-indicator');

let lastAlarmCount = 0;

async function pollStatus() {
  pollIndicator.classList.add('active');
  try {
    const data = await apiFetch('/status');
    renderStatus(data);
  } catch (e) {
    console.error('Poll failed:', e);
  }
  setTimeout(() => pollIndicator.classList.remove('active'), 300);
}

function renderStatus(data) {
  /* Token count */
  const tc = data.token_count ?? 0;
  const countEl = document.getElementById('token-count');
  countEl.textContent = tc;
  countEl.className = 'gauge-number' + (tc === 0 ? '' : '');

  /* Status dot */
  const dot  = document.getElementById('status-dot');
  const txt  = document.getElementById('status-text');
  const alarms = data.alarms ?? [];
  const events = data.events ?? [];

  if (alarms.length > lastAlarmCount) {
    dot.className = 'dot alarm';
    txt.textContent = 'ALARM — tailgating detected!';
  } else if (tc > 0) {
    dot.className = 'dot';
    txt.textContent = `${tc} token${tc !== 1 ? 's' : ''} queued, awaiting entry`;
  } else {
    dot.className = 'dot idle';
    txt.textContent = 'Idle — no pending tokens';
  }
  lastAlarmCount = alarms.length;

  /* Stats */
  let badgeReads = 0, authorized = 0;
  events.forEach(ev => {
    if (ev.type === 'badge_read') badgeReads++;
    if (ev.outcome === 'authorized') authorized++;
  });
  document.getElementById('stat-badge-reads').textContent = badgeReads;
  document.getElementById('stat-authorized').textContent  = authorized;
  document.getElementById('stat-alarms').textContent      = alarms.length;

  /* Event table */
  const tbody = document.getElementById('event-table-body');
  if (events.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="empty-row">No events yet</td></tr>';
  } else {
    tbody.innerHTML = [...events].reverse().map(ev => `
      <tr>
        <td>${fmtTime(ev.timestamp)}</td>
        <td>${fmtType(ev.type)}</td>
        <td>${esc(ev.source || '')}</td>
        <td>${esc(ev.badge_id || '—')}</td>
        <td><span class="badge-outcome ${ev.outcome}">${fmtOutcome(ev.outcome)}</span></td>
      </tr>`).join('');
  }

  /* Alarm table */
  const atbody = document.getElementById('alarm-table-body');
  if (alarms.length === 0) {
    atbody.innerHTML = '<tr><td colspan="2" class="empty-row">No alarms</td></tr>';
  } else {
    atbody.innerHTML = [...alarms].reverse().map(al => `
      <tr>
        <td>${fmtTime(al.timestamp)}</td>
        <td>${fmtAlarmStatus(al.action_status)}</td>
      </tr>`).join('');
  }
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

function esc(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
    .replace(/`/g, '&#96;');
}

function fmtTime(iso) {
  if (!iso) return '—';
  const d = new Date(iso);
  if (isNaN(d)) return iso;
  return d.toLocaleTimeString([], { hour12: false });
}

function fmtType(t) {
  return { badge_read: 'Badge Read', line_crossing: 'Line Crossing' }[t] || t;
}

function fmtOutcome(o) {
  return {
    token_created: 'Token Created',
    authorized:    'Authorized',
    alarm:         'ALARM',
    expired:       'Expired',
  }[o] || o;
}

function fmtAlarmStatus(status) {
  return {
    pending: 'Pending',
    not_configured: 'No action configured',
    skipped_cooldown: 'Skipped by cooldown',
    dispatch_failed: 'Dispatch failed',
    request_failed: 'Request failed',
    request_succeeded: 'Request succeeded',
  }[status] || (status || '—');
}

/* ------------------------------------------------------------------ */
/* Dashboard action buttons                                            */
/* ------------------------------------------------------------------ */

async function badgeRead() {
  try {
    const d = await ingestFetch('/badge-read', { method: 'POST' });
    showToast(`Token created. Active: ${d.token_count}`, 'success');
    pollStatus();
  } catch { showToast('Badge read failed', 'error'); }
}

async function thresholdCrossing() {
  try {
    const d = await apiFetch('/threshold-crossing', { method: 'POST' });
    if (d.outcome === 'authorized') {
      showToast('Authorized entry', 'success');
    } else {
      showToast('ALARM: tailgating detected', 'error');
    }
    pollStatus();
  } catch { showToast('Crossing request failed', 'error'); }
}

async function clearHistory() {
  try {
    await apiFetch('/clear-history', { method: 'POST' });
    lastAlarmCount = 0;
    showToast('History cleared', 'success');
    pollStatus();
  } catch { showToast('Clear failed', 'error'); }
}

/* ------------------------------------------------------------------ */
/* Settings: AOA scenario discovery                                    */
/* ------------------------------------------------------------------ */

async function loadAoaScenarios(currentId) {
  const sel  = document.getElementById('cfg-aoa-id');
  const hint = document.getElementById('cfg-aoa-hint');
  try {
    const r = await fetch('/local/objectanalytics/control.cgi', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ apiVersion: '1.0', method: 'getConfiguration' }),
    });
    const data = await r.json();
    const scenarios = (data.data?.scenarios ?? []).filter(s =>
      (s.type === 'fence' || s.type === 'crosslinecounting') &&
      s.objectClassifications.length > 0 &&
      s.objectClassifications.every(oc => oc.type === 'human')
    );

    sel.innerHTML = '';
    if (scenarios.length === 0) {
      sel.innerHTML = '<option value="">No human crossline scenarios found</option>';
      hint.textContent = 'No qualifying scenarios. Create a Line Crossing or Crossline Counting scenario in AXIS Object Analytics with only Human selected.';
      return;
    }

    for (const s of scenarios) {
      const opt = document.createElement('option');
      opt.value       = s.id;
      opt.textContent = `${s.id} — ${s.name} (${s.type})`;
      opt.selected    = (s.id === currentId);
      sel.appendChild(opt);
    }

    if (scenarios.length === 1) {
      sel.value        = scenarios[0].id;
      hint.textContent = 'Auto-selected: only one human crossline scenario found.';
    } else {
      hint.textContent = `${scenarios.length} human crossline scenarios found.`;
    }
  } catch (e) {
    sel.innerHTML    = `<option value="${currentId}">${currentId} (AOA unavailable)</option>`;
    hint.textContent = 'Could not query AXIS Object Analytics. Enter scenario ID manually.';
  }
}

/* ------------------------------------------------------------------ */
/* Settings: I/O port discovery                                        */
/* ------------------------------------------------------------------ */

async function loadIoPorts(currentPort) {
  const sel  = document.getElementById('cfg-io-port');
  const hint = document.getElementById('cfg-io-hint');

  /* Reset to just "None" */
  sel.innerHTML = '<option value="none">None (disabled)</option>';

  try {
    const r = await fetch('/axis-cgi/param.cgi?action=list&group=root.IOPort');
    if (!r.ok) throw new Error('VAPIX unavailable');
    const text = await r.text();

    /* Enumerate ports by scanning for I<n>.Direction entries.
     * Camera returns e.g.: root.IOPort.I0.Direction=input
     * Port numbers in VAPIX are 0-based; the AXEvent "port" value is 1-based. */
    const portRegex = /root\.IOPort\.I(\d+)\.Direction=(\w+)/g;
    let match;
    while ((match = portRegex.exec(text)) !== null) {
      const idx  = parseInt(match[1], 10);
      const dir  = match[2];
      if (dir === 'output') continue;

      /* Get the friendly name if available */
      const nameMatch = text.match(new RegExp(`I${idx}\\.Input\\.Name=(.+)`));
      const name = nameMatch ? nameMatch[1].trim() : `Port ${idx + 1}`;

      const portNum = String(idx + 1); /* AXEvent uses 1-based port numbers */
      const opt = document.createElement('option');
      opt.value = portNum;
      opt.textContent = `${name} (input)`;
      opt.selected = (portNum === currentPort);
      sel.appendChild(opt);
    }

    if (sel.options.length > 1) {
      hint.textContent = `${sel.options.length - 1} I/O input port(s) detected. Select a port wired to door controller access-granted relay.`;
    } else {
      hint.textContent = 'No input ports detected on this device.';
    }
  } catch {
    /* Fallback: offer ports 1-4 manually */
    for (let i = 1; i <= 4; i++) {
      const opt = document.createElement('option');
      opt.value = String(i);
      opt.textContent = `Port ${i}`;
      opt.selected = (String(i) === currentPort);
      sel.appendChild(opt);
    }
    hint.textContent = 'Could not auto-detect ports. Select manually if your camera has I/O ports.';
  }

  /* Ensure current value is selected */
  if (currentPort && currentPort !== 'none') {
    sel.value = currentPort;
  }
}

/* ------------------------------------------------------------------ */
/* Settings: Alarm Action UI                                           */
/* ------------------------------------------------------------------ */

function onActionTypeChange() {
  const type = document.getElementById('cfg-alarm-type').value;
  const remoteFields = document.getElementById('alarm-remote-fields');
  const portFields   = document.getElementById('alarm-port-fields');
  const httpFields   = document.getElementById('alarm-http-fields');
  const testRow      = document.getElementById('alarm-test-row');
  const portLabel    = document.getElementById('alarm-port-label');
  const durLabel     = document.getElementById('alarm-duration-label');
  const portNum      = document.getElementById('cfg-alarm-port');

  remoteFields.style.display = 'none';
  portFields.style.display   = 'none';
  httpFields.style.display   = 'none';
  testRow.style.display      = 'none';

  if (type === 'none') return;

  testRow.style.display = '';

  if (type === 'virtual_input') {
    remoteFields.style.display = '';
    portFields.style.display   = '';
    portLabel.textContent      = 'Virtual Input Port';
    durLabel.textContent       = 'Duration (seconds)';
    portNum.max = 64;
  } else if (type === 'a9210_output') {
    remoteFields.style.display = '';
    portFields.style.display   = '';
    portLabel.textContent      = 'Output Port';
    durLabel.textContent       = 'Duration (milliseconds)';
    portNum.max = 12;
  } else if (type === 'custom_http') {
    httpFields.style.display   = '';
  }
}

async function testAlarmAction() {
  try {
    const d = await apiFetch('/test-alarm-action', { method: 'POST' });
    if (d.status === 'ok') {
      showToast('Test alarm action fired', 'success');
    } else {
      showToast(d.message || 'Test failed', 'error');
    }
  } catch { showToast('Test alarm action failed', 'error'); }
}

/* ------------------------------------------------------------------ */
/* Settings: load config                                               */
/* ------------------------------------------------------------------ */

async function loadConfig() {
  try {
    const d = await apiFetch('/config');
    document.getElementById('cfg-ttl').value = d.TokenExpirationSeconds ?? 7;
    await loadAoaScenarios(String(d.AoaScenarioId ?? '1'));
    await loadIoPorts(String(d.InputTriggerPort ?? 'none'));
    document.getElementById('ttl-label').textContent = d.TokenExpirationSeconds ?? 7;
    document.getElementById('cfg-alarm-clear-seconds').value = d.AlarmClearSeconds ?? 2;

    /* Alarm action fields */
    document.getElementById('cfg-alarm-type').value     = d.AlarmActionType     ?? 'none';
    document.getElementById('cfg-alarm-host').value     = d.AlarmActionHost     ?? '';
    document.getElementById('cfg-alarm-port').value     = d.AlarmActionPort     ?? '1';
    document.getElementById('cfg-alarm-duration').value = d.AlarmActionDuration ?? '5';
    document.getElementById('cfg-alarm-user').value     = d.AlarmActionUser     ?? '';
    document.getElementById('cfg-alarm-pass').value     = '';
    document.getElementById('cfg-alarm-url').value      = d.AlarmActionUrl      ?? '';
    document.getElementById('cfg-alarm-method').value   = d.AlarmActionMethod   ?? 'GET';
    document.getElementById('cfg-alarm-payload').value  = d.AlarmActionPayload  ?? '';
    document.getElementById('cfg-alarm-header').value   = d.AlarmActionHeader   ?? '';
    document.getElementById('cfg-alarm-http-user').value = d.AlarmActionUser    ?? '';
    document.getElementById('cfg-alarm-http-pass').value = '';
    alarmPassConfigured = Boolean(d.AlarmActionPassConfigured);
    clearAlarmPassword = false;
    updatePasswordStatus();
    onActionTypeChange();
  } catch { showToast('Failed to load config', 'error'); }
}

function updatePasswordStatus() {
  const text = alarmPassConfigured
    ? 'A password is saved. Leave blank to keep it, or clear it explicitly.'
    : 'No password is currently saved.';
  document.getElementById('cfg-alarm-pass-status').textContent = text;
  document.getElementById('cfg-alarm-http-pass-status').textContent = text;
}

function clearSavedAlarmPassword() {
  clearAlarmPassword = true;
  alarmPassConfigured = false;
  document.getElementById('cfg-alarm-pass').value = '';
  document.getElementById('cfg-alarm-http-pass').value = '';
  updatePasswordStatus();
  showToast('Saved password will be cleared on save', 'info');
}

/* ------------------------------------------------------------------ */
/* Settings: save config                                               */
/* ------------------------------------------------------------------ */

async function saveConfig() {
  const type = document.getElementById('cfg-alarm-type').value;

  /* Pick user/pass from the right fields depending on action type */
  let user = '', pass = '';
  if (type === 'custom_http') {
    user = document.getElementById('cfg-alarm-http-user').value;
    pass = document.getElementById('cfg-alarm-http-pass').value;
  } else {
    user = document.getElementById('cfg-alarm-user').value;
    pass = document.getElementById('cfg-alarm-pass').value;
  }

  let port = document.getElementById('cfg-alarm-port').value;

  const body = {
    TokenExpirationSeconds: parseInt(document.getElementById('cfg-ttl').value, 10),
    AoaScenarioId:          String(document.getElementById('cfg-aoa-id').value),
    InputTriggerPort:       String(document.getElementById('cfg-io-port').value),
    AlarmClearSeconds:      parseInt(document.getElementById('cfg-alarm-clear-seconds').value, 10),
    AlarmActionType:        type,
    AlarmActionHost:        document.getElementById('cfg-alarm-host').value,
    AlarmActionPort:        String(port),
    AlarmActionDuration:    String(document.getElementById('cfg-alarm-duration').value),
    AlarmActionUser:        user,
    AlarmActionPass:        pass,
    AlarmActionUrl:         document.getElementById('cfg-alarm-url').value,
    AlarmActionMethod:      document.getElementById('cfg-alarm-method').value,
    AlarmActionPayload:     document.getElementById('cfg-alarm-payload').value,
    AlarmActionHeader:      document.getElementById('cfg-alarm-header').value,
  };
  if (pass) {
    body.AlarmActionPass = pass;
  } else if (clearAlarmPassword) {
    body.AlarmActionClearPass = true;
  }
  try {
    await apiFetch('/config', { method: 'POST', body: JSON.stringify(body) });
    showToast('Settings saved', 'success');
    document.getElementById('ttl-label').textContent = body.TokenExpirationSeconds;
    alarmPassConfigured = pass ? true : (clearAlarmPassword ? false : alarmPassConfigured);
    clearAlarmPassword = false;
    document.getElementById('cfg-alarm-pass').value = '';
    document.getElementById('cfg-alarm-http-pass').value = '';
    updatePasswordStatus();
  } catch { showToast('Save failed', 'error'); }
}

/* ------------------------------------------------------------------ */
/* Reset to defaults                                                   */
/* ------------------------------------------------------------------ */

async function resetDefaults() {
  if (!confirm('Reset all settings to factory defaults?')) return;
  try {
    await apiFetch('/reset-defaults', { method: 'POST' });
    showToast('Reset to defaults', 'success');
    loadConfig();
  } catch { showToast('Reset failed', 'error'); }
}

/* ------------------------------------------------------------------ */
/* Badge read endpoint URL + copy button                              */
/* ------------------------------------------------------------------ */

function populateBadgeUrl() {
  const url = `${window.location.origin}${INGEST_BASE}/badge-read`;
  document.getElementById('badge-read-url').value = url;
}

function copyBadgeUrl() {
  const input = document.getElementById('badge-read-url');
  const btn = document.getElementById('copy-url-btn');

  function onCopied() {
    btn.textContent = 'Copied!';
    btn.classList.add('copied');
    setTimeout(() => { btn.textContent = 'Copy'; btn.classList.remove('copied'); }, 1800);
  }

  /* navigator.clipboard requires HTTPS — use execCommand fallback over HTTP */
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(input.value).then(onCopied).catch(() => {
      input.select();
      document.execCommand('copy');
      onCopied();
    });
  } else {
    input.select();
    document.execCommand('copy');
    onCopied();
  }
}

/* ------------------------------------------------------------------ */
/* Start polling                                                       */
/* ------------------------------------------------------------------ */

(async function init() {
  pollStatus();
  setInterval(pollStatus, POLL_MS);
})();
