// alerts.js — what is worth a DXer's attention right now: beacons heard for
// the first time (NEW), and beacons heard now that history says are rarely
// heard — at all, or at this hour (RARE). Both get a badge in the beacon
// table and heard log, an alert on the page, and, if asked for, a desktop
// notification.
//
// "Rarely" comes from the last 30 days of api/stats, hour by hour, leaving
// out the last few hours so an opening in progress doesn't count against
// itself. Reloaded every 30 minutes.
//
// Uses the helpers app.js and stats.js share as window.ndb.

'use strict';

(() => {
const ndb = window.ndb;
const { $, esc, flag, compass, state } = ndb;

const NEW_S = 86400;            // NEW badge: first heard in the last day
const NEW_ALERT_S = 1800;       // ... and alerted if that was in the last half hour
const RARE_PCT = 0.05;          // RARE: heard less than 5% of the time the receiver was up
const RECENT_SLOTS = 3;         // hours left out of the history, as the opening itself
const MIN_UP_MIN = 24 * 60;     // a day of history before anything is called rare
const MIN_HOUR_UP_MIN = 3 * 60; // three days' worth of this hour before it is judged
const LIVE_S = 120;             // heard-log entries this fresh count as heard now
const REALERT_S = 6 * 3600;     // a rare beacon alerts again after this long

const AL = { model: null, alerted: {}, notify: false };

// ── The model ───────────────────────────────────────────────────────────────

async function loadModel() {
  try {
    const d = await ndb.fetchStats(720);
    const n = d.up.length - RECENT_SLOTS;
    if (n <= 0) return;
    const hod = (i) => new Date((d.t0 + i * 3600) * 1000).getUTCHours();
    const upH = new Array(24).fill(0);
    let up = 0;
    for (let i = 0; i < n; i++) { up += d.up[i]; upH[hod(i)] += d.up[i]; }
    const beacons = new Map();
    for (const b of d.beacons) {
      const mH = new Array(24).fill(0);
      let m = 0;
      for (let i = 0; i < n; i++) { m += b.mins[i]; mH[hod(i)] += b.mins[i]; }
      beacons.set(b.key, { m, mH });
    }
    AL.model = { up, upH, beacons, at: d.now };
    ndb.renderTable();
    ndb.renderHeard();
    check();
  } catch (e) { /* tried again on the next round */ }
}

// Why this beacon, heard now, is unusual — or null.
function rarity(key) {
  const M = AL.model;
  if (!M || M.up < MIN_UP_MIN) return null;
  const b = M.beacons.get(key) || { m: 0, mH: new Array(24).fill(0) };
  const avail = b.m / M.up;
  if (avail < RARE_PCT) {
    const text = !b.m ? 'not heard before in the last 30 days'
      : avail < 0.01 ? `heard only ${b.m} min in the last 30 days` : `heard only ${pct(avail)} of the last 30 days`;
    return { kind: 'rare', avail, text };
  }
  const h = new Date().getUTCHours();
  if (M.upH[h] >= MIN_HOUR_UP_MIN && b.mH[h] / M.upH[h] < RARE_PCT) {
    const ah = b.mH[h] / M.upH[h];
    return { kind: 'hour', avail: ah, text: `${ah ? `heard only ${pct(ah)}` : 'never heard'} at ${String(h).padStart(2, '0')}–${String((h + 1) % 24).padStart(2, '0')} UTC (${pct(avail)} overall)` };
  }
  return null;
}
const pct = (v) => (v >= 0.1 ? `${Math.round(v * 100)}%` : `${(v * 100).toFixed(1)}%`);

const heardEntry = (key) => state.heard.find((e) => e.key === key);
const isNew = (e) => e && e.confirmed && (state.heardNow || Date.now() / 1000) - e.first_s < NEW_S;
const isLive = (e) => e && (state.heardNow || Date.now() / 1000) - e.last_s < LIVE_S;

// Badges for a beacon by heard-log key, for app.js's tables. RARE only
// while it is being heard: that is when it is news.
function badgesFor(key) {
  const e = heardEntry(key);
  if (!e) return '';
  let html = '';
  if (isNew(e)) html += `<span class="badge new" title="First heard ${new Date(e.first_s * 1000).toISOString().slice(0, 16).replace('T', ' ')} UTC">NEW</span>`;
  else if (isLive(e)) {
    const r = rarity(key);
    if (r) html += `<span class="badge rare" title="Unusual: ${esc(r.text)}">RARE</span>`;
  }
  return html;
}

// ── Alerts ──────────────────────────────────────────────────────────────────

function loadAlerted() {
  try { AL.alerted = JSON.parse(localStorage.getItem('ndb.alerted') || '{}'); } catch (e) { AL.alerted = {}; }
}
function saveAlerted() {
  const now = Date.now() / 1000;
  for (const [k, t] of Object.entries(AL.alerted)) if (now - t > 7 * 86400) delete AL.alerted[k];
  try { localStorage.setItem('ndb.alerted', JSON.stringify(AL.alerted)); } catch (e) { /* storage unavailable */ }
}

function check() {
  const now = state.heardNow || Date.now() / 1000;
  let changed = false;
  for (const e of state.heard) {
    if (!e.confirmed) continue;
    if (now - e.first_s < NEW_ALERT_S) {
      const k = `new|${e.key}`;
      if (!AL.alerted[k]) { AL.alerted[k] = now; changed = true; raise(e, 'new', 'New catch', 'first time heard here'); }
      continue;
    }
    if (!isLive(e) || isNew(e)) continue;
    const r = rarity(e.key);
    if (!r) continue;
    const k = `rare|${e.key}`;
    if (AL.alerted[k] && now - AL.alerted[k] < REALERT_S) continue;
    AL.alerted[k] = now;
    changed = true;
    raise(e, 'rare', r.kind === 'rare' ? 'Rare catch' : 'Unusual at this hour', r.text);
  }
  if (changed) saveAlerted();
}

// On the page: newest at the top, five at most, each gone after a minute.
// Clicking one dismisses it (and, for a beacon, shows it on the map).
function toast(kind, title, main, why, onClick) {
  const t = document.createElement('div');
  t.className = `toast ${kind}`;
  t.innerHTML = `<div class="toast-title">${esc(title)}</div>${main ? `<div class="toast-main">${main}</div>` : ''}
    <div class="toast-why">${esc(why)}</div><button class="toast-x" title="Dismiss">✕</button>`;
  t.addEventListener('click', (ev) => {
    t.remove();
    if (!ev.target.closest('.toast-x') && onClick) onClick();
  });
  const box = $('toasts');
  box.prepend(t);
  while (box.children.length > 5) box.lastChild.remove();
  setTimeout(() => t.remove(), 60 * 1000);
}

function raise(e, kind, title, why) {
  const where = e.dist_km != null ? `${e.dist_km} km ${compass(e.bearing_deg)}` : '';
  const line = `${e.ident} ${e.name} · ${(e.freq_hz / 1e3).toFixed(1)} kHz${where ? ` · ${where}` : ''}`;
  toast(kind, title,
    `<b>${esc(e.ident)}</b> ${esc(e.name)} ${flag(e.country)} <span class="m">${(e.freq_hz / 1e3).toFixed(1)} kHz${where ? ` · ${esc(where)}` : ''}</span>`,
    why, e.lat == null ? null : () => {
      if (!ndb.map) return;
      $('map').scrollIntoView({ behavior: 'smooth', block: 'center' });
      ndb.map.setView([e.lat, e.lon], Math.max(ndb.map.getZoom(), 6));
    });
  if (AL.notify && 'Notification' in window && Notification.permission === 'granted') {
    try { new Notification(`${title}: ${e.ident}`, { body: `${line}\n${why}`, tag: `ndb-${kind}-${e.key}` }); } catch (err) { /* not allowed here */ }
  }
}

// ── Desktop notifications ───────────────────────────────────────────────────

function notifyButton() {
  const b = $('notify-btn');
  const supported = 'Notification' in window;
  b.classList.toggle('on', AL.notify);
  b.title = !supported ? 'This browser cannot show desktop notifications'
    : AL.notify ? 'Desktop notifications on for new and rare catches — click to turn off'
      : 'Desktop notifications for new and rare catches (alerts always show on this page)';
  b.disabled = !supported;
}
$('notify-btn').addEventListener('click', async () => {
  if (AL.notify) AL.notify = false;
  else {
    const p = Notification.permission === 'default' ? await Notification.requestPermission() : Notification.permission;
    AL.notify = p === 'granted';
    if (!AL.notify) {
      toast('info', 'Notifications blocked', '', 'Allow notifications for this site in the browser settings to get them.');
    }
  }
  try { localStorage.setItem('ndb.notify', AL.notify ? '1' : '0'); } catch (e) { /* storage unavailable */ }
  notifyButton();
});
try { AL.notify = localStorage.getItem('ndb.notify') === '1' && 'Notification' in window && Notification.permission === 'granted'; } catch (e) { /* storage unavailable */ }
notifyButton();

loadAlerted();
Object.assign(ndb, { badgesFor, rarity, onHeard: check });
loadModel();
setInterval(loadModel, 30 * 60 * 1000);
})();
