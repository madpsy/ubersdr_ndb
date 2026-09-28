// app.js — NDB decoder UI: stat tiles, per-stream spectrum, beacon table, map.
//
// Data arrives over a WebSocket on this page's own path (status at 1 Hz,
// spectrum at 0.5 Hz); if the socket cannot open, it falls back to polling
// api/status. api/navaids (published NDBs in the covered band) is fetched
// once per coverage change for the map's "unheard" layer and spectrum hover.
// All URLs are relative, so it works both directly and behind /addon/ndb/.

'use strict';

(() => {
const $ = (id) => document.getElementById(id);

const state = {
  status: null,
  spectrum: null,
  navaids: [],          // published NDBs in band (api/navaids)
  navaidsKey: '',
  selected: null,       // channel id
  sort: { key: 'freq', asc: true },
  filter: '',
  identOnly: false,
  heard: [],            // server-side heard log
  heardNow: 0,
};

// ── Helpers ─────────────────────────────────────────────────────────────────

const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const flag = (cc) => (cc && cc.length === 2)
  ? String.fromCodePoint(...[...cc.toUpperCase()].map((c) => 127397 + c.charCodeAt(0))) : '';
const khz = (hz) => (hz / 1e3).toFixed(3);
const compass = (deg) => ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][Math.round(deg / 45) % 8];
const snrColour = (snr) => snr >= 30 ? 'var(--ok)' : snr >= 18 ? 'var(--accent)' : snr >= 12 ? 'var(--warn)' : 'var(--err)';

// What to call a channel, and how sure we are.
//   confirmed  — decoded ident equals the published one on this frequency
//   near       — decoded ident is the published one minus its last letter
//   guess      — nothing decoded that matches; nearest published candidate
function identity(c) {
  if (c.navaid) return { nav: c.navaid, label: c.navaid.ident, kind: c.navaid.exact ? 'confirmed' : 'near' };
  if (c.ident) return { nav: null, label: c.ident, kind: 'decoded' };
  if (c.candidates && c.candidates.length) return { nav: c.candidates[0], label: null, kind: 'guess' };
  return { nav: null, label: null, kind: 'none' };
}

function visibleChannels() {
  if (!state.status) return [];
  const f = state.filter.trim().toLowerCase();
  let rows = state.status.channels.filter((c) => {
    const id = identity(c);
    if (state.identOnly && !c.ident) return false;
    if (!f) return true;
    const hay = [khz(c.freq_hz), c.ident, id.nav?.ident, id.nav?.name, id.nav?.country].join(' ').toLowerCase();
    return hay.includes(f);
  });
  const k = state.sort.key, dir = state.sort.asc ? 1 : -1;
  const val = (c) => {
    const id = identity(c);
    switch (k) {
      case 'ident': return id.label || '~';
      case 'name':  return (id.kind !== 'guess' && id.nav?.name) || '~';
      case 'dist':  return (id.kind !== 'guess' && id.nav?.dist_km) ?? 1e9;
      case 'snr':   return c.snr_db;
      default:      return c.freq_hz;
    }
  };
  rows.sort((a, b) => (val(a) < val(b) ? -1 : val(a) > val(b) ? 1 : 0) * dir);
  return rows;
}

// ── Header + stats ──────────────────────────────────────────────────────────

function renderHeader() {
  const s = state.status;
  const st = $('status');
  const live = s.streams.filter((x) => x.connected).length;
  if (live === s.streams.length && live > 0) { st.className = 'status-ok'; st.textContent = 'Live'; }
  else if (live > 0) { st.className = 'status-warn'; st.textContent = `${live}/${s.streams.length} streams`; }
  else { st.className = 'status-err'; st.textContent = s.streams[0]?.message || 'Disconnected'; }

  const rx = s.receiver || {};
  const where = [rx.name, rx.location].filter(Boolean).join(' — ');
  $('subtitle').textContent = where || 'Non-directional beacons — every carrier in the IQ passband, decoded in parallel';

  const chans = s.channels;
  const idd = chans.filter((c) => c.navaid || c.ident);
  $('st-carriers').textContent = chans.length;
  $('st-ident').innerHTML = `${idd.length}<small> / ${chans.length}</small>`;
  let far = null;
  for (const c of chans) if (c.navaid?.dist_km != null && (!far || c.navaid.dist_km > far.navaid.dist_km)) far = c;
  $('st-far').innerHTML = far ? `${far.navaid.dist_km} km <small>${esc(far.navaid.ident)}</small>` : '–';
  const bands = s.streams.filter((x) => x.sample_rate).map((x) =>
    `${((x.center_hz - x.sample_rate * 0.45) / 1e3).toFixed(0)}–${((x.center_hz + x.sample_rate * 0.45) / 1e3).toFixed(0)}`);
  $('st-band').innerHTML = bands.length ? `${bands.join(', ')} <small>kHz</small>` : '–';
}

// ── Table ───────────────────────────────────────────────────────────────────

function renderTable() {
  const rows = visibleChannels();
  document.querySelectorAll('#beacons th').forEach((th) => {
    th.classList.toggle('sorted', th.dataset.sort === state.sort.key);
    th.classList.toggle('asc', th.dataset.sort === state.sort.key && state.sort.asc);
  });
  if (!rows.length) {
    const why = !state.status?.streams.some((x) => x.sample_rate) ? 'Waiting for the first IQ…'
      : state.status.channels.length ? 'No beacons match the filter.'
      : 'No carriers yet — the detector needs a few seconds of averaging.';
    $('rows').innerHTML = `<tr><td colspan="8" class="empty">${why}</td></tr>`;
    return;
  }
  $('rows').innerHTML = rows.map((c) => {
    const id = identity(c);
    let identCell;
    if (id.kind === 'confirmed') identCell = `${esc(id.label)}<span class="badge ok" title="Decoded ident matches the published beacon">✓</span>`;
    else if (id.kind === 'near') identCell = `${esc(id.label)}<span class="badge near" title="Decoded ${esc(c.ident)} — all but the last letter">≈</span>`;
    else if (id.kind === 'decoded') identCell = `${esc(id.label)}<span class="badge n" title="Not in the beacon database on this frequency">×${c.ident_count}</span>`;
    else identCell = '<span class="none">…</span>';
    if (c.ggmorse) identCell += '<span class="badge g" title="ggmorse is assisting: a second decoder, given to a few unidentified channels that show keying">2nd</span>';

    let station = '', dist = '';
    if (id.nav) {
      const guess = id.kind === 'guess';
      const more = guess && c.candidates.length > 1 ? ` · ${c.candidates.length} on freq` : '';
      station = `<td class="station${guess ? ' guess' : ''}" title="${esc(id.nav.ident)} ${esc(id.nav.name)}${guess ? ' (unconfirmed)' : ''}">
        ${guess ? esc(id.nav.ident) + ' ' : ''}${esc(id.nav.name)}<span class="cc">${flag(id.nav.country)} ${esc(id.nav.country)}${more}</span></td>`;
      dist = id.nav.dist_km != null
        ? `<td class="num dist">${guess ? '<i>' : ''}${id.nav.dist_km} km${guess ? '</i>' : ''}<span class="brg">${compass(id.nav.bearing_deg)}</span></td>`
        : '<td class="num dist">–</td>';
    } else {
      station = '<td class="station guess">—</td>';
      dist = '<td class="num dist">–</td>';
    }

    const pct = Math.max(4, Math.min(100, c.snr_db / 50 * 100));
    const txt = c.text.length > 90 ? c.text.slice(-90) : c.text;
    const hl = c.ident ? esc(txt).split(esc(c.ident)).join(`<b>${esc(c.ident)}</b>`) : esc(txt);
    const stale = c.last_seen_s > 30;
    // A carrier with no tone keying after a minute is most likely a spur
    // (e.g. a switching-supply comb on whole kHz), or an NDB too weak to read.
    const unkeyed = !c.ident && !c.keying && c.age_s > 60 && c.contrast_db < 15;
    const copy = hl ? `<span>${hl}</span>`
      : unkeyed ? `<span class="nokey" title="Tone on/off contrast ${c.contrast_db.toFixed(0)} dB — no keying found">no keying</span>` : '';
    return `<tr data-id="${c.id}" class="${c.id === state.selected ? 'sel' : ''}${stale || unkeyed ? ' stale' : ''}">
      <td class="freq">${khz(c.freq_hz)}</td>
      <td class="ident">${identCell}</td>
      ${station}${dist}
      <td class="num"><span class="snr">${c.snr_db.toFixed(0)} dB<span class="snr-bar"><i style="width:${pct}%;background:${snrColour(c.snr_db)}"></i></span></span></td>
      <td class="num">${c.keying && c.pitch_hz ? c.pitch_hz + ' Hz' : '–'}</td>
      <td class="num">${c.keying && c.speed_wpm ? c.speed_wpm : '–'}</td>
      <td class="copy">${copy}</td>
    </tr>`;
  }).join('');
}

$('rows').addEventListener('click', (e) => {
  const tr = e.target.closest('tr[data-id]');
  if (tr) select(Number(tr.dataset.id), 'table');
});
document.querySelectorAll('#beacons th[data-sort]').forEach((th) => th.addEventListener('click', () => {
  const k = th.dataset.sort;
  state.sort = { key: k, asc: state.sort.key === k ? !state.sort.asc : k !== 'snr' };
  renderTable();
}));
$('search').addEventListener('input', (e) => { state.filter = e.target.value; renderAll(); });
$('ident-only').addEventListener('change', (e) => { state.identOnly = e.target.checked; renderAll(); });

function select(id, from) {
  state.selected = state.selected === id ? null : id;
  renderTable();
  drawSpectra();
  renderMap();
  if (state.selected == null) return;
  if (from !== 'table') document.querySelector(`tr[data-id="${id}"]`)?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  if (from !== 'map') focusOnMap(id);
}

// ── Spectrum ────────────────────────────────────────────────────────────────

const tip = document.createElement('div');
tip.id = 'tip';
document.body.appendChild(tip);
const specViews = new Map();   // stream index -> { wrap, canvas, label, geom }

function ensureSpecViews() {
  const streams = state.spectrum?.streams || [];
  for (const s of streams) {
    if (specViews.has(s.index)) continue;
    const wrap = document.createElement('div');
    wrap.className = 'spec';
    const canvas = document.createElement('canvas');
    const label = document.createElement('div');
    label.className = 'label';
    wrap.append(canvas, label);
    $('spectra').appendChild(wrap);
    const view = { wrap, canvas, label, geom: null, index: s.index };
    canvas.addEventListener('mousemove', (e) => hover(view, e));
    canvas.addEventListener('mouseleave', () => { tip.style.display = 'none'; });
    canvas.addEventListener('click', (e) => clickSpec(view, e));
    specViews.set(s.index, view);
  }
}

function drawSpectra() {
  if (!state.spectrum) return;
  ensureSpecViews();
  for (const s of state.spectrum.streams) drawSpectrum(specViews.get(s.index), s);
}

function drawSpectrum(view, s) {
  const { canvas, wrap } = view;
  const dpr = window.devicePixelRatio || 1;
  const W = wrap.clientWidth, H = wrap.clientHeight;
  if (canvas.width !== Math.round(W * dpr) || canvas.height !== Math.round(H * dpr)) {
    canvas.width = Math.round(W * dpr);
    canvas.height = Math.round(H * dpr);
  }
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  view.label.textContent = `${(s.center_hz / 1e3).toFixed(1)} kHz · ${state.status?.streams[s.index]?.mode || ''}`;
  if (!s.db.length || !s.span_hz) { view.geom = null; return; }

  const valid = s.db.filter((v) => v > -190).sort((a, b) => a - b);
  if (!valid.length) return;
  const lo = valid[Math.floor(valid.length * 0.03)] - 4;
  const hi = Math.max(valid[valid.length - 1] + 4, lo + 30);
  const top = 26, bot = H - 18;
  const f0 = s.center_hz - s.span_hz / 2;
  const x = (hz) => (hz - f0) / s.span_hz * W;
  const y = (v) => bot - (Math.max(v, lo) - lo) / (hi - lo) * (bot - top);
  view.geom = { f0, span: s.span_hz, W };

  // Grid: every 5 kHz.
  g.lineWidth = 1;
  g.font = '10px Inter, system-ui, sans-serif';
  for (let f = Math.ceil(f0 / 5000) * 5000; f <= f0 + s.span_hz; f += 5000) {
    const xx = Math.round(x(f)) + 0.5;
    g.strokeStyle = '#ffffff0d';
    g.beginPath(); g.moveTo(xx, top - 8); g.lineTo(xx, bot); g.stroke();
    g.fillStyle = '#6b6b7a';
    g.fillText((f / 1e3).toFixed(0), xx + 3, H - 5);
  }

  // Filled spectrum trace.
  const n = s.db.length;
  const grad = g.createLinearGradient(0, top, 0, bot);
  grad.addColorStop(0, '#00d4ff55');
  grad.addColorStop(1, '#7c3aed08');
  g.beginPath();
  g.moveTo(0, bot);
  s.db.forEach((v, i) => g.lineTo(i / n * W, y(v)));
  g.lineTo(W, bot);
  g.closePath();
  g.fillStyle = grad;
  g.fill();
  g.beginPath();
  s.db.forEach((v, i) => (i ? g.lineTo(i / n * W, y(v)) : g.moveTo(0, y(v))));
  g.strokeStyle = '#00d4ff';
  g.stroke();

  // Noise floor estimate.
  g.setLineDash([3, 3]);
  g.strokeStyle = '#7c3aed99';
  g.beginPath();
  s.floor.forEach((v, i) => (i ? g.lineTo(i / n * W, y(v)) : g.moveTo(0, y(v))));
  g.stroke();
  g.setLineDash([]);

  // Beacon markers, labels thinned so they don't overprint.
  const chans = (state.status?.channels || []).filter((c) => c.stream === s.index);
  let lastRight = -1e9;
  g.textAlign = 'center';
  g.font = '600 11px ui-monospace, Menlo, monospace';
  for (const c of [...chans].sort((a, b) => a.freq_hz - b.freq_hz)) {
    const xx = Math.round(x(c.freq_hz)) + 0.5;
    const id = identity(c);
    const sel = c.id === state.selected;
    const col = sel ? '#00d4ff' : id.kind === 'confirmed' || id.kind === 'near' ? '#22c55e'
      : id.kind === 'decoded' ? '#e2e2e8' : '#6b6b7a';
    g.strokeStyle = col;
    g.globalAlpha = sel ? 0.9 : 0.35;
    g.beginPath(); g.moveTo(xx, top - 6); g.lineTo(xx, bot); g.stroke();
    g.globalAlpha = 1;
    const text = id.label && id.kind !== 'guess' ? id.label : '';
    if (!text) continue;
    const w = g.measureText(text).width;
    if (xx - w / 2 < lastRight + 4 && !sel) continue;
    g.fillStyle = col;
    g.fillText(text, Math.min(Math.max(xx, w / 2 + 2), W - w / 2 - 2), 14);
    lastRight = xx + w / 2;
  }
  g.textAlign = 'start';
}

function hzAt(view, e) {
  const r = view.canvas.getBoundingClientRect();
  return view.geom ? view.geom.f0 + (e.clientX - r.left) / r.width * view.geom.span : null;
}

function nearestChannel(view, hz) {
  if (!view.geom) return null;
  const hzPerPx = view.geom.span / view.geom.W;
  let best = null;
  for (const c of state.status?.channels || []) {
    if (c.stream !== view.index) continue;
    const d = Math.abs(c.freq_hz - hz);
    if (d < 6 * hzPerPx && (!best || d < Math.abs(best.freq_hz - hz))) best = c;
  }
  return best;
}

function hover(view, e) {
  const hz = hzAt(view, e);
  if (hz == null) return;
  const ch = nearestChannel(view, hz);
  const at = ch ? ch.freq_hz : hz;
  const pubs = state.navaids.filter((n) => Math.abs(n.freq_hz - at) <= 500)
    .sort((a, b) => (a.dist_km ?? 0) - (b.dist_km ?? 0)).slice(0, 5);
  let html = `<div><b>${khz(at)}</b> kHz</div>`;
  if (ch) {
    const id = identity(ch);
    html += `<div>${ch.snr_db.toFixed(0)} dB SNR${ch.ident ? ` · copying <b>${esc(ch.ident)}</b>` : ''}</div>`;
    if (id.nav && id.kind !== 'guess') html += `<div>${esc(id.nav.name)} ${flag(id.nav.country)}</div>`;
  }
  if (pubs.length) {
    html += '<div class="m" style="margin-top:4px">Published here:</div>' + pubs.map((n) =>
      `<div><b>${esc(n.ident)}</b> ${esc(n.name)} ${flag(n.country)}${n.dist_km != null ? ` <span class="m">${n.dist_km} km</span>` : ''}</div>`).join('');
  }
  tip.innerHTML = html;
  tip.style.display = 'block';
  const tx = Math.min(e.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
  tip.style.left = `${tx}px`;
  tip.style.top = `${e.clientY + 14}px`;
}

function clickSpec(view, e) {
  const hz = hzAt(view, e);
  const ch = hz != null && nearestChannel(view, hz);
  if (ch) select(ch.id, 'spectrum');
}

// ── Map ─────────────────────────────────────────────────────────────────────
//
// Markers are kept and updated in place (keyed), not rebuilt each second, so
// a hover tooltip stays open while the data behind it refreshes. The ident
// label is part of each marker's icon, which leaves the tooltip free for the
// detail shown on hover.

let map = null, ringsFor = '', unheardSig = '';
// Autofit: keep the view framed on the receiver and the confirmed beacons,
// re-fitting when that set changes. A move the user makes (or asks for, via
// search or selecting a beacon) turns it off; `fitting` marks our own moves
// so they don't count as the user's.
let fitSig = '', fitting = false;
const layers = {};                 // name -> L.LayerGroup
const stores = { heard: new Map(), earlier: new Map(), unheard: new Map() };  // key -> { m, iconKey }
let rxMarker = null;

const RING_KM = [50, 100, 250, 500, 1000, 1500, 2000, 3000];
const TIP = { direction: 'top', offset: [0, -10], className: 'map-tip', opacity: 1 };

// NDB chart symbol: a dot inside a ring of dots, with its ident beside it.
function ndbIcon(colour, { size = 18, solid = true, sel = false, label = '', labelClass = '' } = {}) {
  const r = size / 2, dots = [];
  for (let i = 0; i < 12; i++) {
    const a = i / 12 * Math.PI * 2;
    dots.push(`<circle cx="${(r + Math.cos(a) * (r - 1.5)).toFixed(2)}" cy="${(r + Math.sin(a) * (r - 1.5)).toFixed(2)}" r="1.1" fill="${colour}"/>`);
  }
  const halo = sel ? `<circle cx="${r}" cy="${r}" r="${r + 3}" fill="none" stroke="${colour}" stroke-width="1.5" opacity="0.7"/>` : '';
  const svg = `<svg width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">${halo}${dots.join('')}
    <circle cx="${r}" cy="${r}" r="${solid ? 3.2 : 2.6}" fill="${solid ? colour : '#0d0d0f'}" stroke="${colour}" stroke-width="1.4"/></svg>`;
  const lab = label ? `<span class="ndb-label ${labelClass}">${esc(label)}</span>` : '';
  return L.divIcon({ className: 'ndb-icon', html: svg + lab, iconSize: [size, size], iconAnchor: [r, r] });
}

function initMap() {
  if (map || typeof L === 'undefined') return;
  map = L.map('map', { worldCopyJump: true, zoomControl: true }).setView([54, -2], 5);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom: 12,
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
  }).addTo(map);
  for (const name of ['rings', 'unheard', 'earlier', 'paths', 'heard']) layers[name] = L.layerGroup().addTo(map);
  const bind = (box, layer) => $(box).addEventListener('change', (e) => {
    if (e.target.checked) layers[layer].addTo(map); else map.removeLayer(layers[layer]);
  });
  bind('show-rings', 'rings');

  const af = $('autofit');
  try { const v = localStorage.getItem('ndb.autofit'); if (v !== null) af.checked = v === '1'; } catch (e) { /* storage unavailable */ }
  af.addEventListener('change', () => {
    try { localStorage.setItem('ndb.autofit', af.checked ? '1' : '0'); } catch (e) { /* storage unavailable */ }
    fitSig = '';          // re-fit straight away when turned back on
    renderMap();
  });
  const userMoved = () => { if (!fitting) setAutofit(false); };
  map.on('dragstart', userMoved);
  map.on('zoomstart', userMoved);
  const sel = $('earlier-window');
  try { const v = localStorage.getItem('ndb.earlierWindow'); if (v !== null) sel.value = v; } catch (e) { /* storage unavailable */ }
  sel.addEventListener('change', () => {
    try { localStorage.setItem('ndb.earlierWindow', sel.value); } catch (e) { /* storage unavailable */ }
    renderMap();
  });
  bind('show-earlier', 'earlier');
  bind('show-unheard', 'unheard');
}

function drawRings(rx) {
  const key = `${rx.lat},${rx.lon}`;
  if (ringsFor === key) return;
  ringsFor = key;
  layers.rings.clearLayers();
  for (const km of RING_KM) {
    L.circle([rx.lat, rx.lon], {
      radius: km * 1000, color: '#00d4ff', weight: 1, opacity: 0.28, fill: false, dashArray: '3 6', interactive: false,
    }).addTo(layers.rings);
    // Label at the ring's southern edge, where it rarely collides with markers north of a European receiver.
    L.tooltip({ permanent: true, direction: 'center', className: 'ring-label', interactive: false })
      .setLatLng([rx.lat - km / 111.2, rx.lon]).setContent(km >= 1000 ? `${km / 1000}k km` : `${km} km`).addTo(layers.rings);
  }
}

// Great circle from the receiver, so long paths bend the way the signal goes.
function greatCircle(a, b, n = 48) {
  const r = Math.PI / 180;
  const [la1, lo1, la2, lo2] = [a[0] * r, a[1] * r, b[0] * r, b[1] * r];
  const d = 2 * Math.asin(Math.sqrt(Math.sin((la2 - la1) / 2) ** 2 +
    Math.cos(la1) * Math.cos(la2) * Math.sin((lo2 - lo1) / 2) ** 2));
  if (d < 1e-6) return [a, b];
  const pts = [];
  for (let i = 0; i <= n; i++) {
    const f = i / n, A = Math.sin((1 - f) * d) / Math.sin(d), B = Math.sin(f * d) / Math.sin(d);
    const x = A * Math.cos(la1) * Math.cos(lo1) + B * Math.cos(la2) * Math.cos(lo2);
    const y = A * Math.cos(la1) * Math.sin(lo1) + B * Math.cos(la2) * Math.sin(lo2);
    const z = A * Math.sin(la1) + B * Math.sin(la2);
    pts.push([Math.atan2(z, Math.hypot(x, y)) / r, Math.atan2(y, x) / r]);
  }
  return pts;
}

function maidenhead(lat, lon) {
  let x = lon + 180, y = lat + 90;
  const A = 'ABCDEFGHIJKLMNOPQR', a = 'abcdefghijklmnopqrstuvwx';
  const f1 = A[Math.floor(x / 20)] + A[Math.floor(y / 10)];
  x %= 20; y %= 10;
  const f2 = `${Math.floor(x / 2)}${Math.floor(y)}`;
  x %= 2; y %= 1;
  return f1 + f2 + a[Math.floor(x * 12)] + a[Math.floor(y * 24)];
}

// ── Tooltip content ──

const row = (k, v) => v == null || v === '' ? '' : `<tr><td>${k}</td><td>${v}</td></tr>`;
const where = (n) => n.dist_km != null ? `${n.dist_km} km ${compass(n.bearing_deg)} <span class="m">(${n.bearing_deg}°)</span>` : '';
const power = (p) => p ? p.charAt(0) + p.slice(1).toLowerCase() : '';

function tipBeacon(n, c, kind) {
  const status = {
    confirmed: '<span class="ok">✓ decoded ident matches</span>',
    near: `<span class="warn">≈ decoded ${esc(c.ident)} — all but the last letter</span>`,
    guess: '<span class="warn">unconfirmed — nearest published beacon on this frequency</span>',
  }[kind];
  const offset = Math.round(c.freq_hz - n.freq_hz) || 0;   // || 0 turns -0 into 0
  const others = kind === 'guess' && c.candidates.length > 1
    ? c.candidates.slice(1, 4).map((x) => `${esc(x.ident)} ${esc(x.name)}${x.dist_km != null ? ` <span class="m">${x.dist_km} km</span>` : ''}`).join('<br>')
    : '';
  const copy = c.text.trim() ? esc(c.text.trim().slice(-48)) : '';
  return `<div class="tt-head"><b>${esc(n.ident)}</b> ${esc(n.name)} ${flag(n.country)}</div>
    <div class="tt-sub">${status}</div>
    <table>
      ${row('Published', `${(n.freq_hz / 1e3).toFixed(1)} kHz`)}
      ${row('Measured', `${khz(c.freq_hz)} kHz <span class="m">(${offset > 0 ? '+' : ''}${offset} Hz)</span>`)}
      ${row('Distance', where(n))}
      ${row('SNR', `${c.snr_db.toFixed(0)} dB`)}
      ${row('Tone / speed', c.keying ? `${c.pitch_hz} Hz · ${c.speed_wpm} wpm` : '<span class="m">no keying seen</span>')}
      ${row('Copies', c.ident ? `${esc(c.ident)} ×${c.ident_count}` : '')}
      ${row('Power', power(n.power))}
      ${row('Also here', others)}
    </table>
    ${copy ? `<div class="tt-copy">${copy}</div>` : ''}`;
}

function tipEarlier(e) {
  return `<div class="tt-head"><b>${esc(e.ident)}</b> ${esc(e.name)} ${flag(e.country)}</div>
    <div class="tt-sub"><span class="earlier">heard earlier — not live now</span></div>
    <table>
      ${row('Frequency', `${(e.freq_hz / 1e3).toFixed(1)} kHz`)}
      ${row('Distance', where(e))}
      ${row('Last heard', `${ago(e.last_s)} <span class="m">${utcDate(e.last_s)}</span>`)}
      ${row('First heard', `${ago(e.first_s)}`)}
      ${row('Best SNR', `${e.best_snr_db.toFixed(0)} dB`)}
      ${row('Best copies', `×${e.best_copies}`)}
    </table>`;
}

function tipUnheard(n) {
  return `<div class="tt-head"><b>${esc(n.ident)}</b> ${esc(n.name)} ${flag(n.country)}</div>
    <div class="tt-sub"><span class="m">published · not heard</span></div>
    <table>
      ${row('Frequency', `${(n.freq_hz / 1e3).toFixed(1)} kHz`)}
      ${row('Distance', where(n))}
      ${row('Power', power(n.power))}
      ${row('Position', `${n.lat.toFixed(3)}, ${n.lon.toFixed(3)}`)}
    </table>`;
}

function tipReceiver(rx) {
  const s = state.status;
  const chans = s.channels;
  const idd = chans.filter((c) => c.navaid || c.ident);
  let far = null;
  for (const c of chans) if (c.navaid?.dist_km != null && (!far || c.navaid.dist_km > far.navaid.dist_km)) far = c;
  const streams = s.streams.map((x) => {
    const span = x.sample_rate ? `${((x.center_hz - x.sample_rate * 0.45) / 1e3).toFixed(0)}–${((x.center_hz + x.sample_rate * 0.45) / 1e3).toFixed(0)} kHz` : `${(x.center_hz / 1e3).toFixed(0)} kHz`;
    return `${span} <span class="m">${esc(x.mode)}${x.connected ? '' : ' · ' + esc(x.message)}</span>`;
  }).join('<br>');
  return `<div class="tt-head"><b>${esc(rx.callsign || rx.name || 'Receiver')}</b>${rx.callsign && rx.name ? ' ' + esc(rx.name) : ''}</div>
    <div class="tt-sub"><span class="accent">this UberSDR instance</span></div>
    <table>
      ${row('Location', esc(rx.location))}
      ${row('Position', `${rx.lat.toFixed(4)}, ${rx.lon.toFixed(4)} <span class="m">${maidenhead(rx.lat, rx.lon)}</span>`)}
      ${row('Listening', streams)}
      ${row('Carriers', `${chans.length} <span class="m">· ${idd.length} identified</span>`)}
      ${row('Furthest now', far ? `${esc(far.navaid.ident)} ${esc(far.navaid.name)} · ${far.navaid.dist_km} km` : '')}
      ${row('Heard log', `${state.heard.length} beacon${state.heard.length === 1 ? '' : 's'}`)}
    </table>`;
}

// Open a marker's tooltip below it when there isn't room above for a full
// one (they run to ~240 px), so it isn't clipped by the map's top edge.
const TIP_ROOM_PX = 260;
function placeTip(m) {
  const t = m.getTooltip();
  if (!t || !map) return;
  const y = map.latLngToContainerPoint(m.getLatLng()).y;
  const below = y < TIP_ROOM_PX && map.getSize().y - y > y;
  t.options.direction = below ? 'bottom' : 'top';
  t.options.offset = below ? [0, 10] : [0, -10];
  if (t.isOpen()) t.update();
}

// Create or update a keyed marker in place.
function upsert(store, layer, key, ll, iconKey, makeIcon, tip, onClick, z = 0) {
  let e = store.get(key);
  if (!e) {
    const m = L.marker(ll, { icon: makeIcon(), zIndexOffset: z });
    m.on('mouseover', () => placeTip(m));
    m.bindTooltip(tip, TIP).addTo(layer);
    if (onClick) m.on('click', onClick);
    e = { m, iconKey };
    store.set(key, e);
  } else {
    const cur = e.m.getLatLng();
    if (cur.lat !== ll[0] || cur.lng !== ll[1]) e.m.setLatLng(ll);
    if (e.iconKey !== iconKey) { e.m.setIcon(makeIcon()); e.m.setZIndexOffset(z); e.iconKey = iconKey; }
    e.m.setTooltipContent(tip);
  }
  e.seen = true;
  return e.m;
}

function sweep(store, layer) {
  for (const [k, e] of store) {
    if (!e.seen) { layer.removeLayer(e.m); store.delete(k); } else e.seen = false;
  }
}

function renderMap() {
  initMap();
  if (!map || !state.status) return;
  const rx = state.status.receiver;
  const haveRx = rx && rx.lat != null;
  if (haveRx) {
    drawRings(rx);
    if (!rxMarker) {
      rxMarker = L.marker([rx.lat, rx.lon], {
        icon: L.divIcon({ className: '', html: '<div class="rx-icon"><div class="pulse"></div><div class="core"></div></div>', iconSize: [22, 22], iconAnchor: [11, 11] }),
        zIndexOffset: 1000,
      });
      rxMarker.on('mouseover', () => placeTip(rxMarker));
      rxMarker.bindTooltip('', TIP).addTo(map);
    }
    rxMarker.setTooltipContent(tipReceiver(rx));
  }

  // Live beacons (confirmed, near, and — if shown — unconfirmed guesses).
  layers.paths.clearLayers();
  const pts = haveRx ? [[rx.lat, rx.lon]] : [];
  const liveIdents = new Set();
  const guessKeys = new Set();     // published beacons already drawn as a guess
  for (const c of visibleChannels()) {
    const id = identity(c);
    if (!id.nav) continue;
    const guess = id.kind === 'guess';
    if (guess && !$('show-unheard').checked) continue;
    const sel = c.id === state.selected;
    const ll = [id.nav.lat, id.nav.lon];
    if (!guess) liveIdents.add(id.nav.ident);
    else guessKeys.add(`${id.nav.ident}@${id.nav.freq_hz}`);
    const col = sel ? '#00d4ff' : guess ? '#f59e0b' : '#22c55e';
    if (haveRx) {
      L.polyline(greatCircle([rx.lat, rx.lon], ll), {
        color: col, weight: sel ? 2.5 : 1.5, opacity: guess ? 0.35 : 0.65, dashArray: guess ? '4 5' : null, interactive: false,
      }).addTo(layers.paths);
    }
    const label = guess ? id.nav.ident + '?' : id.nav.ident;
    const labelClass = `${guess ? 'guess' : ''}${sel ? ' sel' : ''}`;
    upsert(stores.heard, layers.heard, `c${c.id}:${id.nav.ident}`, ll, `${col}|${guess}|${sel}|${label}`,
      () => ndbIcon(col, { solid: !guess, sel, label, labelClass }),
      tipBeacon(id.nav, c, id.kind), () => select(c.id, 'map'), sel ? 900 : guess ? 0 : 500);
    if (!guess) pts.push(ll);
  }
  sweep(stores.heard, layers.heard);

  // Heard before, not live now — within the chosen window. NDB reception is
  // diurnal (night skywave reaches far beyond daytime groundwave), so the
  // default 24 h shows one full day/night cycle; "all" is the whole log.
  const windowS = Number($('earlier-window').value);
  const now = state.heardNow || Date.now() / 1000;
  for (const e of state.heard) {
    if (!e.confirmed || e.lat == null || liveIdents.has(e.ident)) continue;
    if (windowS > 0 && now - e.last_s > windowS) continue;
    upsert(stores.earlier, layers.earlier, e.key, [e.lat, e.lon], 'earlier',
      () => ndbIcon('#3b82f6', { size: 14, solid: false, label: e.ident, labelClass: 'earlier' }), tipEarlier(e));
  }
  sweep(stores.earlier, layers.earlier);

  // Published in band, never heard. Only rebuilt when the set changes.
  const heardIdents = new Set([...liveIdents, ...state.heard.map((e) => e.ident)]);
  const unheard = state.navaids.filter((n) => !heardIdents.has(n.ident) && !guessKeys.has(`${n.ident}@${n.freq_hz}`));
  const sig = unheard.length + ':' + [...heardIdents, ...guessKeys].sort().join(',');
  if (sig !== unheardSig) {
    unheardSig = sig;
    for (const n of unheard) {
      upsert(stores.unheard, layers.unheard, `${n.ident}@${n.freq_hz}@${n.lat}`, [n.lat, n.lon], 'u',
        () => L.divIcon({ className: 'unheard-dot', iconSize: [6, 6], iconAnchor: [3, 3] }), tipUnheard(n), null, -1000);
    }
    sweep(stores.unheard, layers.unheard);
  }

  if ($('autofit').checked) {
    const sig = pts.map((p) => p.join(',')).sort().join(';');
    if (sig !== fitSig && pts.length) {
      fitSig = sig;
      fitting = true;
      if (pts.length >= 2) map.fitBounds(L.latLngBounds(pts).pad(0.3), { maxZoom: 8 });
      else map.setView(pts[0], 6);
      // Leaflet fires its move/zoom events around the change; clear the flag
      // once they have run.
      setTimeout(() => { fitting = false; }, 400);
    }
  }
}

function setAutofit(on) {
  const af = $('autofit');
  if (af.checked === on) return;
  af.checked = on;
  try { localStorage.setItem('ndb.autofit', on ? '1' : '0'); } catch (e) { /* storage unavailable */ }
  if (on) { fitSig = ''; renderMap(); }
}

function focusOnMap(id) {
  if (!map) return;
  for (const [k, e] of stores.heard) {
    if (!k.startsWith(`c${id}:`)) continue;
    const ll = e.m.getLatLng();
    if (!map.getBounds().pad(-0.1).contains(ll)) { setAutofit(false); map.panTo(ll); }
    e.m.openTooltip();
    return;
  }
}

// ── Live copy ───────────────────────────────────────────────────────────────

// Chunks from one channel a few seconds apart are one line, so the feed
// reads as copy ("EDN ED EDN") rather than a column of letters. Beacons are
// decoded in parallel, so their chunks interleave: a chunk joins its own
// channel's latest line, which then moves to the top.
const live = [];          // { id, freq_hz, t0, t1, text }, oldest first
const LIVE_MAX = 150;
const LIVE_JOIN_MS = 15000;

function channelLabel(id, freq) {
  const c = state.status?.channels.find((x) => x.id === id);
  const idn = c ? identity(c) : null;
  const label = idn && idn.kind !== 'guess' ? idn.label : null;
  return { label, freq: c ? c.freq_hz : freq };
}

function addDecodes(items, backfill) {
  for (const it of items) {
    let i = live.length - 1;
    while (i >= 0 && live[i].id !== it.id) i--;
    const line = i >= 0 && it.t - live[i].t1 < LIVE_JOIN_MS ? live.splice(i, 1)[0] : null;
    if (line) {
      line.text += it.text;
      line.t1 = it.t;
      line.fresh = !backfill;
      live.push(line);
    } else {
      live.push({ id: it.id, freq_hz: it.freq_hz, t0: it.t, t1: it.t, text: it.text, fresh: !backfill });
    }
  }
  while (live.length > LIVE_MAX) live.shift();
  renderLive();
}

function renderLive() {
  const el = $('live');
  const shown = live.filter((l) => l.text.trim());
  if (!shown.length) { el.innerHTML = '<div class="empty">Nothing decoded yet.</div>'; return; }
  const stick = el.scrollTop < 8;
  el.innerHTML = shown.slice().reverse().map((l) => {
    const { label, freq } = channelLabel(l.id, l.freq_hz);
    const t = new Date(l.t1).toISOString().slice(11, 19);
    let txt = esc(l.text.trim());
    if (label) txt = txt.split(esc(label)).join(`<mark>${esc(label)}</mark>`);
    const fresh = l.fresh ? ' fresh' : '';
    l.fresh = false;
    return `<div class="live-line${fresh}" data-id="${l.id}"><span class="t">${t}</span>
      <span class="who">${label ? `<b>${esc(label)}</b>` : ''}<span>${(freq / 1e3).toFixed(1)}</span></span>
      <span class="txt">${txt}</span></div>`;
  }).join('');
  if (stick) el.scrollTop = 0;
}

$('live').addEventListener('click', (e) => {
  const line = e.target.closest('.live-line');
  if (line && state.status?.channels.some((c) => c.id === Number(line.dataset.id))) select(Number(line.dataset.id), 'live');
});

// ── Heard log ───────────────────────────────────────────────────────────────

function ago(s) {
  const d = Math.max(0, (state.heardNow || Date.now() / 1000) - s);
  if (d < 90) return 'just now';
  if (d < 5400) return `${Math.round(d / 60)} min ago`;
  if (d < 172800) return `${Math.round(d / 3600)} h ago`;
  return `${Math.round(d / 86400)} d ago`;
}
const utcDate = (s) => new Date(s * 1000).toISOString().replace('T', ' ').slice(0, 16) + ' UTC';

function renderHeard() {
  const rows = [...state.heard].sort((a, b) => b.last_s - a.last_s);
  $('heard-hint').textContent = rows.length ? `${rows.length} beacon${rows.length === 1 ? '' : 's'}` : '';
  if (!rows.length) {
    $('heard-rows').innerHTML = '<tr><td colspan="7" class="empty">No beacons identified yet.</td></tr>';
    return;
  }
  $('heard-rows').innerHTML = rows.map((e) => `<tr>
    <td class="ident">${esc(e.ident)}${e.confirmed ? '<span class="badge ok">✓</span>' : ''}</td>
    <td class="station">${e.name ? `${esc(e.name)}<span class="cc">${flag(e.country)} ${esc(e.country)}</span>` : '<span class="guess">not in database</span>'}</td>
    <td class="freq">${(e.freq_hz / 1e3).toFixed(1)}</td>
    <td class="num dist">${e.dist_km != null ? `${e.dist_km} km<span class="brg">${compass(e.bearing_deg)}</span>` : '–'}</td>
    <td class="num">${e.best_snr_db.toFixed(0)} dB</td>
    <td class="num when" title="${utcDate(e.first_s)}">${ago(e.first_s)}</td>
    <td class="num when" title="${utcDate(e.last_s)}">${ago(e.last_s)}</td>
  </tr>`).join('');
}

// ── Search ──────────────────────────────────────────────────────────────────
//
// Server-side over the whole navaid list (api/search), so it finds beacons
// far outside the band or the map radius too. Debounced: every keystroke
// would otherwise spend the proxy's per-minute request allowance.

let searchTimer = null, searchSeq = 0, searchHits = [];

function openSearch() {
  $('search-modal').hidden = false;
  const q = $('search-q');
  q.focus();
  q.select();
}
function closeSearch() { $('search-modal').hidden = true; }

async function runSearch() {
  const q = $('search-q').value.trim();
  const seq = ++searchSeq;
  if (!q) {
    searchHits = [];
    $('search-meta').textContent = 'Searches every published NDB in the OurAirports list, nearest first.';
    $('search-results').innerHTML = '';
    return;
  }
  try {
    const r = await (await fetch('api/search?q=' + encodeURIComponent(q))).json();
    if (seq !== searchSeq) return;   // a newer query is already on its way
    searchHits = r.results;
    const more = r.total > r.results.length ? ` (showing ${r.results.length})` : '';
    $('search-meta').textContent = r.total ? `${r.total} match${r.total === 1 ? '' : 'es'}${more} — click one to show it on the map` : 'No published NDB matches.';
    $('search-results').innerHTML = r.results.length ? `<table><thead><tr>
        <th>Ident</th><th>Name</th><th>kHz</th><th class="num">Distance</th><th>Power</th><th>Status</th>
      </tr></thead><tbody>${r.results.map((n, i) => {
        let st;
        if (n.live) st = `<span class="st live" title="Being received now">live · ${n.live.snr_db.toFixed(0)} dB</span>`;
        else if (n.heard) st = `<span class="st heard" title="Last heard ${utcDate(n.heard.last_s)}">heard ${ago(n.heard.last_s)}</span>`;
        else if (n.in_band) st = '<span class="st band" title="Inside a stream being decoded, not heard yet">in band</span>';
        else st = '<span class="st out" title="Outside every stream being decoded">not covered</span>';
        return `<tr data-i="${i}">
          <td class="ident">${esc(n.ident)}</td>
          <td class="station">${esc(n.name)}<span class="cc">${flag(n.country)} ${esc(n.country)}</span></td>
          <td class="freq">${(n.freq_hz / 1e3).toFixed(1)}</td>
          <td class="num dist">${n.dist_km != null ? `${n.dist_km} km<span class="brg">${compass(n.bearing_deg)}</span>` : '–'}</td>
          <td>${esc(power(n.power))}</td>
          <td>${st}</td></tr>`;
      }).join('')}</tbody></table>` : '';
  } catch (e) {
    if (seq === searchSeq) $('search-meta').textContent = 'Search failed — no response from the addon.';
  }
}

// Show a search hit on the map: select its live channel if there is one,
// otherwise fly there and drop a temporary marker with its tooltip.
let searchPin = null;
function showHit(n) {
  closeSearch();
  const ch = state.status?.channels.find((c) => c.navaid && c.navaid.ident === n.ident && Math.abs(c.navaid.freq_hz - n.freq_hz) < 1);
  if (ch) { select(ch.id, 'search'); return; }
  if (!map) return;
  if (searchPin) map.removeLayer(searchPin);
  searchPin = L.marker([n.lat, n.lon], { icon: ndbIcon('#e2e2e8', { solid: false, label: n.ident, labelClass: 'found' }), zIndexOffset: 1200 })
    .bindTooltip(tipUnheard(n), TIP).addTo(map);
  searchPin.on('mouseover', () => placeTip(searchPin));
  setAutofit(false);   // the user asked to look somewhere specific
  map.flyTo([n.lat, n.lon], Math.max(map.getZoom(), 7), { duration: 0.8 });
  map.once('moveend', () => { placeTip(searchPin); searchPin.openTooltip(); });
  $('map-card').scrollIntoView({ behavior: 'smooth', block: 'center' });
}

$('search-open').addEventListener('click', openSearch);
$('search-modal').addEventListener('click', (e) => { if (e.target.closest('[data-close]')) closeSearch(); });
$('search-q').addEventListener('input', () => { clearTimeout(searchTimer); searchTimer = setTimeout(runSearch, 300); });
$('search-q').addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && searchHits.length) showHit(searchHits[0]);
});
$('search-results').addEventListener('click', (e) => {
  const tr = e.target.closest('tr[data-i]');
  if (tr) showHit(searchHits[Number(tr.dataset.i)]);
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !$('search-modal').hidden) closeSearch();
  else if (e.key === '/' && $('search-modal').hidden && !/INPUT|TEXTAREA|SELECT/.test(document.activeElement?.tagName)) {
    e.preventDefault();
    openSearch();
  }
});

// ── Data ────────────────────────────────────────────────────────────────────

async function refreshNavaids() {
  const s = state.status;
  if (!s) return;
  const key = s.streams.map((x) => `${x.center_hz}/${x.sample_rate || 0}`).join(',') + (s.receiver?.lat ?? '');
  if (key === state.navaidsKey || !s.streams.some((x) => x.sample_rate)) return;
  state.navaidsKey = key;
  try {
    const r = await fetch('api/navaids');   // radius: NDB_MAP_RADIUS_KM, server side
    state.navaids = (await r.json()).navaids || [];
    renderMap();
  } catch (e) { state.navaidsKey = ''; }
}

function renderAll() {
  if (!state.status) return;
  renderHeader();
  renderTable();
  drawSpectra();
  renderMap();
}

function onMessage(m) {
  if (m.type === 'status') {
    state.status = m;
    if (state.selected != null && !m.channels.some((c) => c.id === state.selected)) state.selected = null;
    renderAll();
    refreshNavaids();
  } else if (m.type === 'spectrum') {
    state.spectrum = m;
    drawSpectra();
  } else if (m.type === 'decodes') {
    if (m.backfill) live.length = 0;
    addDecodes(m.items, m.backfill);
  } else if (m.type === 'heard') {
    state.heard = m.entries;
    state.heardNow = m.now;
    renderHeard();
    renderMap();
  }
}

let pollTimer = null;
async function poll() {
  try {
    onMessage(await (await fetch('api/status')).json());
    onMessage(await (await fetch('api/spectrum')).json());
    onMessage(await (await fetch('api/heard')).json());
    if (!live.length) onMessage(await (await fetch('api/decodes')).json());
  } catch (e) {
    $('status').className = 'status-err';
    $('status').textContent = 'No response';
  }
}
function startPolling() { if (!pollTimer) { poll(); pollTimer = setInterval(poll, 5000); } }

function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + location.pathname.replace(/[^/]*$/, '');
  let ws, opened = false;
  try { ws = new WebSocket(url); } catch (e) { startPolling(); return; }
  ws.onopen = () => { opened = true; if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } };
  ws.onmessage = (ev) => { try { onMessage(JSON.parse(ev.data)); } catch (e) { /* ignore a bad frame */ } };
  ws.onclose = () => {
    if (!opened) startPolling();
    else { $('status').className = 'status-err'; $('status').textContent = 'Reconnecting…'; }
    setTimeout(connect, 5000);
  };
}

window.addEventListener('resize', () => { drawSpectra(); map?.invalidateSize(); });
setInterval(() => { state.navaidsKey = ''; refreshNavaids(); }, 10 * 60 * 1000);
initMap();
connect();
})();
