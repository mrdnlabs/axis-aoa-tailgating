'use strict';

const API_BASE    = `/local/antitailgate/admin`;
const INGEST_BASE = `/local/antitailgate/ingest`;
const POLL_MS     = 2000;

let alarmPassConfigured   = false;
let alarmHeaderConfigured = false;
let clearAlarmPassword    = false;
let lastAlarmTotal        = null;   /* null until first poll (no false alarm on load) */

/* ------------------------------------------------------------------ */
/* API helpers                                                         */
/* ------------------------------------------------------------------ */

async function apiFetch(path, opts = {}) {
  const headers = {
    'Content-Type': 'application/json',
    'X-Requested-With': 'antitailgate',
    ...(opts.headers || {}),
  };
  const resp = await fetch(API_BASE + path, { credentials: 'same-origin', ...opts, headers });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) throw new Error(data.message || data.result || `HTTP ${resp.status}`);
  return data;
}

async function ingestFetch(path, opts = {}) {
  const headers = { 'Content-Type': 'application/json', ...(opts.headers || {}) };
  const resp = await fetch(INGEST_BASE + path, { credentials: 'same-origin', ...opts, headers });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) throw new Error(data.message || `HTTP ${resp.status}`);
  return data;
}

/* ------------------------------------------------------------------ */
/* Toast                                                               */
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
/* Formatting + safe DOM                                               */
/* ------------------------------------------------------------------ */

function fmtTime(iso) {
  if (!iso) return '—';
  const d = new Date(iso);
  return isNaN(d) ? iso : d.toLocaleTimeString([], { hour12: false });
}
const TYPE_LABEL = { badge_read: 'Badge Read', line_crossing: 'Line Crossing' };
const OUTCOME_LABEL = {
  token_created: 'Token Created', authorized: 'Authorized',
  alarm: 'ALARM', expired: 'Expired',
};
const ALARM_LABEL = {
  pending: 'Pending', not_configured: 'No action configured',
  skipped_cooldown: 'Skipped by cooldown', dispatch_failed: 'Dispatch failed',
  request_failed: 'Request failed', request_succeeded: 'Request succeeded',
};

function cell(text, className) {
  const td = document.createElement('td');
  td.textContent = text;
  if (className) td.className = className;
  return td;
}

/* ------------------------------------------------------------------ */
/* Dashboard polling                                                   */
/* ------------------------------------------------------------------ */

const pollIndicator = document.getElementById('poll-indicator');

async function pollStatus() {
  pollIndicator.classList.add('active');
  try { renderStatus(await apiFetch('/status')); }
  catch (e) { console.error('Poll failed:', e); }
  setTimeout(() => pollIndicator.classList.remove('active'), 300);
}

function renderStatus(data) {
  const tc = data.token_count ?? 0;
  document.getElementById('token-count').textContent = tc;

  const stats = data.stats || { badge_reads: 0, authorized: 0, alarms: 0 };
  document.getElementById('stat-badge-reads').textContent = stats.badge_reads ?? 0;
  document.getElementById('stat-authorized').textContent  = stats.authorized ?? 0;
  document.getElementById('stat-alarms').textContent      = stats.alarms ?? 0;

  const dot = document.getElementById('status-dot');
  const txt = document.getElementById('status-text');
  const alarmTotal = stats.alarms ?? 0;
  const newAlarm = (lastAlarmTotal !== null && alarmTotal > lastAlarmTotal);
  lastAlarmTotal = alarmTotal;

  if (newAlarm) {
    dot.className = 'dot alarm';
    txt.textContent = 'ALARM — tailgating detected!';
  } else if (tc > 0) {
    dot.className = 'dot';
    txt.textContent = `${tc} token${tc !== 1 ? 's' : ''} queued, awaiting entry`;
  } else {
    dot.className = 'dot idle';
    txt.textContent = 'Idle — no pending tokens';
  }

  const events = data.events ?? [];
  const tbody = document.getElementById('event-table-body');
  tbody.replaceChildren();
  if (events.length === 0) {
    const tr = document.createElement('tr');
    const td = cell('No events yet', 'empty-row'); td.colSpan = 5;
    tr.appendChild(td); tbody.appendChild(tr);
  } else {
    [...events].reverse().forEach(ev => {
      const tr = document.createElement('tr');
      tr.appendChild(cell(fmtTime(ev.timestamp)));
      tr.appendChild(cell(TYPE_LABEL[ev.type] || ev.type));
      tr.appendChild(cell(ev.source || ''));
      tr.appendChild(cell(ev.badge_id || '—'));
      const oc = cell(OUTCOME_LABEL[ev.outcome] || ev.outcome);
      const span = document.createElement('span');
      span.className = 'badge-outcome ' + (ev.outcome || '').replace(/[^a-z_]/gi, '');
      span.textContent = OUTCOME_LABEL[ev.outcome] || ev.outcome;
      oc.textContent = ''; oc.appendChild(span);
      tr.appendChild(oc);
      tbody.appendChild(tr);
    });
  }

  const alarms = data.alarms ?? [];
  const atbody = document.getElementById('alarm-table-body');
  atbody.replaceChildren();
  if (alarms.length === 0) {
    const tr = document.createElement('tr');
    const td = cell('No alarms', 'empty-row'); td.colSpan = 2;
    tr.appendChild(td); atbody.appendChild(tr);
  } else {
    [...alarms].reverse().forEach(al => {
      const tr = document.createElement('tr');
      tr.appendChild(cell(fmtTime(al.timestamp)));
      tr.appendChild(cell(ALARM_LABEL[al.action_status] || al.action_status || '—'));
      atbody.appendChild(tr);
    });
  }
}

/* ------------------------------------------------------------------ */
/* Dashboard actions                                                   */
/* ------------------------------------------------------------------ */

async function badgeRead() {
  try {
    const d = await ingestFetch('/badge-read', { method: 'POST', body: '{}' });
    showToast(`Token created. Active: ${d.token_count}`, 'success');
    pollStatus();
  } catch { showToast('Badge read failed', 'error'); }
}

async function thresholdCrossing() {
  try {
    const d = await apiFetch('/threshold-crossing', { method: 'POST' });
    showToast(d.outcome === 'authorized' ? 'Authorized entry' : 'ALARM: tailgating detected',
              d.outcome === 'authorized' ? 'success' : 'error');
    pollStatus();
  } catch { showToast('Crossing request failed', 'error'); }
}

async function clearHistory() {
  if (!confirm('Clear event history AND drop all active tokens?')) return;
  try {
    await apiFetch('/clear-history', { method: 'POST' });
    lastAlarmTotal = null;
    showToast('History and tokens cleared', 'success');
    pollStatus();
  } catch { showToast('Clear failed', 'error'); }
}

/* ------------------------------------------------------------------ */
/* Settings: AOA scenario discovery                                    */
/* ------------------------------------------------------------------ */

async function loadAoaScenarios(currentId) {
  const sel  = document.getElementById('cfg-aoa-id');
  const hint = document.getElementById('cfg-aoa-hint');
  sel.replaceChildren();

  function addOption(value, label, selected) {
    const opt = document.createElement('option');
    opt.value = value; opt.textContent = label; opt.selected = !!selected;
    sel.appendChild(opt);
  }

  try {
    const r = await fetch('/local/objectanalytics/control.cgi', {
      method: 'POST', credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ apiVersion: '1.0', method: 'getConfiguration' }),
    });
    const data = await r.json();
    const scenarios = (data.data?.scenarios ?? []).filter(s =>
      (s.type === 'fence' || s.type === 'crosslinecounting') &&
      (s.objectClassifications?.length > 0) &&
      s.objectClassifications.every(oc => oc.type === 'human'));

    if (scenarios.length === 0) {
      /* Keep whatever is configured so a save doesn't wipe it. */
      addOption(currentId || '1', currentId
        ? `Scenario ${currentId} (none detected — keeping)` : 'No scenarios found', true);
      hint.textContent = 'No human line-crossing scenario detected. Create one in AXIS Object Analytics (Human only), or keep the current ID.';
      return;
    }
    let matched = false;
    for (const s of scenarios) {
      const sel_ = String(s.id) === String(currentId);
      if (sel_) matched = true;
      addOption(String(s.id), `${s.id} — ${s.name} (${s.type})`, sel_);
    }
    if (!matched && currentId) {
      addOption(String(currentId), `Scenario ${currentId} (configured, not in list)`, true);
      hint.textContent = 'The saved scenario is not currently a human line-crossing scenario.';
    } else {
      hint.textContent = `${scenarios.length} human line-crossing scenario(s) found.`;
    }
  } catch {
    addOption(currentId || '1', `Scenario ${currentId || '1'} (AOA unavailable)`, true);
    hint.textContent = 'Could not query AXIS Object Analytics; keeping the configured scenario.';
  }
}

/* ------------------------------------------------------------------ */
/* Settings: I/O port discovery (device-native 0-based values)         */
/* ------------------------------------------------------------------ */

async function loadIoPorts(currentPort) {
  const sel  = document.getElementById('cfg-io-port');
  const hint = document.getElementById('cfg-io-hint');
  sel.replaceChildren();

  function addOption(value, label, selected) {
    const opt = document.createElement('option');
    opt.value = value; opt.textContent = label; opt.selected = !!selected;
    sel.appendChild(opt);
  }
  addOption('none', 'None (disabled)', currentPort === 'none' || !currentPort);

  try {
    const r = await fetch('/axis-cgi/param.cgi?action=list&group=root.IOPort',
                          { credentials: 'same-origin' });
    if (!r.ok) throw new Error('VAPIX unavailable');
    const text = await r.text();
    const re = /root\.IOPort\.I(\d+)\.Direction=(\w+)/g;
    let m, found = 0;
    while ((m = re.exec(text)) !== null) {
      const idx = parseInt(m[1], 10);        /* device-native 0-based index */
      if (m[2] === 'output') continue;
      const nameMatch = text.match(new RegExp(`I${idx}\\.Input\\.Name=(.+)`));
      const name = nameMatch ? nameMatch[1].trim() : `Port ${idx + 1}`;
      addOption(String(idx), `${name} (input)`, String(idx) === String(currentPort));
      found++;
    }
    hint.textContent = found
      ? `${found} input port(s) detected. Wire one to the door-controller access-granted relay.`
      : 'No input ports detected on this device.';
  } catch {
    for (let i = 0; i < 4; i++)
      addOption(String(i), `Port ${i + 1}`, String(i) === String(currentPort));
    hint.textContent = 'Could not auto-detect ports; choose manually if your device has I/O.';
  }
  if (currentPort && currentPort !== 'none') sel.value = String(currentPort);
}

/* ------------------------------------------------------------------ */
/* Settings: alarm action UI                                           */
/* ------------------------------------------------------------------ */

function onActionTypeChange() {
  const type = document.getElementById('cfg-alarm-type').value;
  const show = (id, on) => { document.getElementById(id).style.display = on ? '' : 'none'; };
  const remote = (type === 'virtual_input' || type === 'a9210_output');

  show('alarm-remote-fields', remote);
  show('alarm-port-fields', remote);
  show('alarm-tls-fields', remote || type === 'custom_http');
  show('alarm-http-fields', type === 'custom_http');
  show('alarm-test-row', type !== 'none');

  if (remote) {
    document.getElementById('alarm-port-label').textContent =
      type === 'a9210_output' ? 'Output Port' : 'Virtual Input Port';
  }
  onTlsChange();
}

function onTlsChange() {
  const tls = document.getElementById('cfg-alarm-tls').value;
  document.getElementById('alarm-pin-wrap').style.display = (tls === 'pin') ? '' : 'none';
}

async function testAlarmAction() {
  try {
    await saveConfig(true);                 /* test uses saved config */
    const d = await apiFetch('/test-alarm-action', { method: 'POST' });
    showToast(`Test: ${d.result || d.status}`, d.status === 'ok' ? 'success' : 'error');
  } catch (e) { showToast(`Test failed: ${e.message}`, 'error'); }
}

/* ------------------------------------------------------------------ */
/* Settings: load / save                                               */
/* ------------------------------------------------------------------ */

async function loadConfig() {
  try {
    const d = await apiFetch('/config');
    document.getElementById('cfg-ttl').value = d.TokenExpirationSeconds ?? 7;
    await loadAoaScenarios(String(d.AoaScenarioId ?? '1'));
    await loadIoPorts(String(d.InputTriggerPort ?? 'none'));
    document.getElementById('ttl-label').textContent = d.TokenExpirationSeconds ?? 7;
    document.getElementById('cfg-alarm-clear-seconds').value = d.AlarmClearSeconds ?? 2;

    document.getElementById('cfg-alarm-type').value     = d.AlarmActionType ?? 'none';
    document.getElementById('cfg-alarm-tls').value      = d.AlarmActionTlsMode ?? 'verify';
    document.getElementById('cfg-alarm-host').value     = d.AlarmActionHost ?? '';
    document.getElementById('cfg-alarm-port').value     = d.AlarmActionPort ?? '1';
    document.getElementById('cfg-alarm-duration').value = d.AlarmActionDuration ?? '5';
    document.getElementById('cfg-alarm-user').value     = d.AlarmActionUser ?? '';
    document.getElementById('cfg-alarm-pass').value     = '';
    document.getElementById('cfg-alarm-pin').value      = '';
    document.getElementById('cfg-alarm-url').value      = d.AlarmActionUrl ?? '';
    document.getElementById('cfg-alarm-method').value   = d.AlarmActionMethod ?? 'GET';
    document.getElementById('cfg-alarm-payload').value  = d.AlarmActionPayload ?? '';
    document.getElementById('cfg-alarm-header').value   = '';
    document.getElementById('cfg-alarm-http-user').value = d.AlarmActionUser ?? '';
    document.getElementById('cfg-alarm-http-pass').value = '';

    alarmPassConfigured   = Boolean(d.AlarmActionPassConfigured);
    alarmHeaderConfigured = Boolean(d.AlarmActionHeaderConfigured);
    clearAlarmPassword = false;
    document.getElementById('cfg-alarm-pin-status').textContent =
      d.AlarmActionPinConfigured ? 'A pinned key is saved; leave blank to keep it.' : '';
    updateSecretStatus();
    onActionTypeChange();
  } catch { showToast('Failed to load config', 'error'); }
}

function updateSecretStatus() {
  const pass = alarmPassConfigured
    ? 'A password is saved. Leave blank to keep it.' : 'No password saved.';
  document.getElementById('cfg-alarm-pass-status').textContent = pass;
  document.getElementById('cfg-alarm-http-pass-status').textContent = pass;
  document.getElementById('cfg-alarm-header-status').textContent = alarmHeaderConfigured
    ? 'A header is saved. Leave blank to keep it.' : 'No header saved.';
}

function clearSavedAlarmPassword() {
  clearAlarmPassword = true;
  alarmPassConfigured = false;
  document.getElementById('cfg-alarm-pass').value = '';
  document.getElementById('cfg-alarm-http-pass').value = '';
  updateSecretStatus();
  showToast('Saved password will be cleared on save', 'info');
}

/* Build the POST body, including secrets ONLY when the operator typed one. */
async function saveConfig(silent) {
  const type = document.getElementById('cfg-alarm-type').value;
  const isHttp = (type === 'custom_http');
  const pass = isHttp
    ? document.getElementById('cfg-alarm-http-pass').value
    : document.getElementById('cfg-alarm-pass').value;
  const user = isHttp
    ? document.getElementById('cfg-alarm-http-user').value
    : document.getElementById('cfg-alarm-user').value;
  const header = document.getElementById('cfg-alarm-header').value;

  const body = {
    TokenExpirationSeconds: parseInt(document.getElementById('cfg-ttl').value, 10),
    AoaScenarioId:          String(document.getElementById('cfg-aoa-id').value || '1'),
    InputTriggerPort:       String(document.getElementById('cfg-io-port').value),
    AlarmClearSeconds:      parseInt(document.getElementById('cfg-alarm-clear-seconds').value, 10),
    AlarmActionType:        type,
    AlarmActionTlsMode:     document.getElementById('cfg-alarm-tls').value,
    AlarmActionPin:         document.getElementById('cfg-alarm-pin').value,
    AlarmActionHost:        document.getElementById('cfg-alarm-host').value,
    AlarmActionPort:        String(document.getElementById('cfg-alarm-port').value),
    AlarmActionDuration:    String(document.getElementById('cfg-alarm-duration').value),
    AlarmActionUser:        user,
    AlarmActionUrl:         document.getElementById('cfg-alarm-url').value,
    AlarmActionMethod:      document.getElementById('cfg-alarm-method').value,
    AlarmActionPayload:     document.getElementById('cfg-alarm-payload').value,
  };
  if (pass) body.AlarmActionPass = pass;
  else if (clearAlarmPassword) body.AlarmActionClearPass = true;
  if (header) body.AlarmActionHeader = header;

  try {
    await apiFetch('/config', { method: 'POST', body: JSON.stringify(body) });
    if (!silent) showToast('Settings saved', 'success');
    document.getElementById('ttl-label').textContent = body.TokenExpirationSeconds;
    if (pass) alarmPassConfigured = true;
    else if (clearAlarmPassword) alarmPassConfigured = false;
    if (header) alarmHeaderConfigured = true;
    clearAlarmPassword = false;
    document.getElementById('cfg-alarm-pass').value = '';
    document.getElementById('cfg-alarm-http-pass').value = '';
    document.getElementById('cfg-alarm-header').value = '';
    updateSecretStatus();
  } catch (e) {
    if (!silent) showToast(`Save failed: ${e.message}`, 'error');
    throw e;
  }
}

async function resetDefaults() {
  if (!confirm('Reset all settings to factory defaults?')) return;
  try {
    await apiFetch('/reset-defaults', { method: 'POST' });
    showToast('Reset to defaults', 'success');
    loadConfig();
  } catch { showToast('Reset failed', 'error'); }
}

/* ------------------------------------------------------------------ */
/* Badge URL + clipboard                                               */
/* ------------------------------------------------------------------ */

function populateBadgeUrl() {
  document.getElementById('badge-read-url').value =
    `${window.location.origin}${INGEST_BASE}/badge-read`;
}

function copyBadgeUrl() {
  const input = document.getElementById('badge-read-url');
  const btn = document.getElementById('copy-url-btn');
  const done = () => {
    btn.textContent = 'Copied!'; btn.classList.add('copied');
    setTimeout(() => { btn.textContent = 'Copy'; btn.classList.remove('copied'); }, 1800);
  };
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(input.value).then(done).catch(() => { input.select(); document.execCommand('copy'); done(); });
  } else { input.select(); document.execCommand('copy'); done(); }
}

/* ------------------------------------------------------------------ */
/* Wiring (no inline handlers — CSP forbids them)                      */
/* ------------------------------------------------------------------ */

const ACTIONS = {
  'badge-read': badgeRead,
  'threshold-crossing': thresholdCrossing,
  'clear-history': clearHistory,
  'test-alarm': testAlarmAction,
  'clear-pass': clearSavedAlarmPassword,
  'save-config': () => saveConfig(false),
  'reset-defaults': resetDefaults,
  'copy-url': copyBadgeUrl,
  'alarm-type-change': onActionTypeChange,
  'alarm-tls-change': onTlsChange,
};

document.addEventListener('click', (e) => {
  const btn = e.target.closest('[data-action]');
  if (!btn) return;
  const fn = ACTIONS[btn.dataset.action];
  if (fn && btn.tagName === 'BUTTON') { e.preventDefault(); fn(); }
});
document.addEventListener('change', (e) => {
  const el = e.target.closest('[data-action]');
  if (!el || el.tagName !== 'SELECT') return;
  const fn = ACTIONS[el.dataset.action];
  if (fn) fn();
});

document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
    if (btn.dataset.tab === 'settings') { loadConfig(); populateBadgeUrl(); }
  });
});

(function init() {
  pollStatus();
  setInterval(pollStatus, POLL_MS);
})();
