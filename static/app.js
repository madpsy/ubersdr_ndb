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

let map = null, rxMarker = null, fitted = false, ringsFor = '';
const layers = {};                // name -> L.LayerGroup
const heardMarkers = new Map();   // channel id -> marker

const RING_KM = [50, 100, 250, 500, 1000, 1500, 2000, 3000];

// NDB chart symbol: a dot inside a ring of dots. `fill` for live beacons,
// hollow for guesses and history.
function ndbIcon(colour, { size = 18, solid = true, sel = false } = {}) {
  const r = size / 2, dots = [];
  for (let i = 0; i < 12; i++) {
    const a = i / 12 * Math.PI * 2;
    dots.push(`<circle cx="${(r + Math.cos(a) * (r - 1.5)).toFixed(2)}" cy="${(r + Math.sin(a) * (r - 1.5)).toFixed(2)}" r="1.1" fill="${colour}"/>`);
  }
  const halo = sel ? `<circle cx="${r}" cy="${r}" r="${r + 3}" fill="none" stroke="${colour}" stroke-width="1.5" opacity="0.7"/>` : '';
  const svg = `<svg width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">${halo}${dots.join('')}
    <circle cx="${r}" cy="${r}" r="${solid ? 3.2 : 2.6}" fill="${solid ? colour : '#0d0d0f'}" stroke="${colour}" stroke-width="1.4"/></svg>`;
  return L.divIcon({ className: 'ndb-icon', html: svg, iconSize: [size, size], iconAnchor: [r, r] });
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
    const lat = rx.lat - km / 111.2;
    L.tooltip({ permanent: true, direction: 'center', className: 'ring-label', interactive: false })
      .setLatLng([lat, rx.lon]).setContent(km >= 1000 ? `${km / 1000}k km` : `${km} km`).addTo(layers.rings);
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

function navPopup(n, c, extra = '') {
  let h = `<b>${esc(n.ident)}</b> ${esc(n.name)} ${flag(n.country)}<br>${khz(n.freq_hz)} kHz`;
  if (n.dist_km != null) h += ` · ${n.dist_km} km ${compass(n.bearing_deg)} (${n.bearing_deg}°)`;
  if (n.power) h += `<br><span style="color:var(--muted)">power: ${esc(n.power.toLowerCase())}</span>`;
  if (c) h += `<br>${c.snr_db.toFixed(0)} dB SNR${c.ident ? ` · copying <b>${esc(c.ident)}</b>` : ' · no copy yet'}`;
  return h + extra;
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
      }).bindTooltip(`<b>${esc(rx.callsign || rx.name || 'Receiver')}</b>${rx.location ? '<br>' + esc(rx.location) : ''}`, { direction: 'top', offset: [0, -10] })
        .addTo(map);
    }
  }

  layers.heard.clearLayers();
  layers.paths.clearLayers();
  heardMarkers.clear();
  const pts = haveRx ? [[rx.lat, rx.lon]] : [];
  const liveKeys = new Set();
  for (const c of visibleChannels()) {
    const id = identity(c);
    if (!id.nav) continue;
    const guess = id.kind === 'guess';
    if (guess && !$('show-unheard').checked) continue;
    const sel = c.id === state.selected;
    const ll = [id.nav.lat, id.nav.lon];
    liveKeys.add(`${id.nav.ident}@${id.nav.freq_hz}`);
    const col = sel ? '#00d4ff' : guess ? '#f59e0b' : '#22c55e';
    if (haveRx) {
      L.polyline(greatCircle([rx.lat, rx.lon], ll), {
        color: col, weight: sel ? 2.5 : 1.5, opacity: guess ? 0.35 : 0.65, dashArray: guess ? '4 5' : null, interactive: false,
      }).addTo(layers.paths);
    }
    const m = L.marker(ll, { icon: ndbIcon(col, { solid: !guess, sel }), zIndexOffset: sel ? 900 : guess ? 0 : 500 })
      .bindPopup(navPopup(id.nav, c, guess ? '<br><span style="color:var(--warn)">unconfirmed — nearest published beacon on this frequency</span>' : ''))
      .bindTooltip(esc(guess ? id.nav.ident + '?' : id.nav.ident), {
        permanent: true, direction: 'right', offset: [9, 0], className: `ndb-label${guess ? ' guess' : ''}${sel ? ' sel' : ''}`,
      })
      .on('click', () => select(c.id, 'map'))
      .addTo(layers.heard);
    heardMarkers.set(c.id, m);
    if (!guess) pts.push(ll);
  }

  // Heard before, not live now.
  layers.earlier.clearLayers();
  const earlierKeys = new Set();
  for (const e of state.heard) {
    if (!e.confirmed || e.lat == null) continue;
    earlierKeys.add(`${e.ident}`);
    if ([...liveKeys].some((lk) => lk.startsWith(e.ident + '@'))) continue;
    L.marker([e.lat, e.lon], { icon: ndbIcon('#3b82f6', { size: 14, solid: false }) })
      .bindPopup(navPopup({ ...e, power: '' }, null, `<br><span style="color:var(--muted)">last heard ${ago(e.last_s)} · best ${e.best_snr_db.toFixed(0)} dB</span>`))
      .bindTooltip(esc(e.ident), { direction: 'right', offset: [7, 0] })
      .addTo(layers.earlier);
  }

  layers.unheard.clearLayers();
  for (const n of state.navaids) {
    if (liveKeys.has(`${n.ident}@${n.freq_hz}`) || earlierKeys.has(n.ident)) continue;
    L.circleMarker([n.lat, n.lon], { radius: 2.5, color: '#6b6b7a', weight: 1, fillOpacity: 0.5 })
      .bindPopup(navPopup(n, null, '<br><span style="color:var(--muted)">not heard</span>'))
      .addTo(layers.unheard);
  }

  if (!fitted && pts.length >= 2) {
    map.fitBounds(L.latLngBounds(pts).pad(0.3), { maxZoom: 8 });
    fitted = true;
  } else if (!fitted && haveRx) {
    map.setView([rx.lat, rx.lon], 6);
  }
}

function focusOnMap(id) {
  const m = heardMarkers.get(id);
  if (!m || !map) return;
  const ll = m.getLatLng();
  if (!map.getBounds().pad(-0.1).contains(ll)) map.panTo(ll);
  m.openPopup();
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

// ── Data ────────────────────────────────────────────────────────────────────

async function refreshNavaids() {
  const s = state.status;
  if (!s) return;
  const key = s.streams.map((x) => `${x.center_hz}/${x.sample_rate || 0}`).join(',') + (s.receiver?.lat ?? '');
  if (key === state.navaidsKey || !s.streams.some((x) => x.sample_rate)) return;
  state.navaidsKey = key;
  try {
    const r = await fetch('api/navaids?max_km=2500');
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
