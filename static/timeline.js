// timeline.js — the map's day / night / greyline shading, and its timeline:
// a replay of which beacons were heard, 15 minutes at a time, over the last
// day to month of history (api/stats?step=900), with the greyline moving
// across the map as it goes.
//
// Also the solar arithmetic export.js uses to say whether each reception was
// by day, by night or on the greyline.
//
// Uses app.js's and stats.js's helpers, shared as window.ndb.

'use strict';

(() => {
const ndb = window.ndb;
const { $, esc, flag, compass, state, layers } = ndb;

// ── Sun ─────────────────────────────────────────────────────────────────────

const RAD = Math.PI / 180;

// Subsolar point at unix time t (s). The almanac's low-precision formulae,
// good to ~0.01°, far finer than a 15-minute slot needs.
function subsolar(t) {
  const d = t / 86400 - 10957.5;          // days since J2000.0
  const g = (357.529 + 0.98560028 * d) * RAD;
  const q = 280.459 + 0.98564736 * d;
  const L = (q + 1.915 * Math.sin(g) + 0.020 * Math.sin(2 * g)) * RAD;
  const e = (23.439 - 0.00000036 * d) * RAD;
  const ra = Math.atan2(Math.cos(e) * Math.sin(L), Math.cos(L)) / RAD;
  const dec = Math.asin(Math.sin(e) * Math.sin(L)) / RAD;
  const gmst = ((18.697374558 + 24.06570982441908 * d) % 24 + 24) % 24;
  return { lat: dec, lon: ((ra - gmst * 15) % 360 + 540) % 360 - 180 };
}

// Sun's elevation in degrees at (lat, lon), given the subsolar point.
function sunElev(lat, lon, ss) {
  const s = Math.sin(lat * RAD) * Math.sin(ss.lat * RAD) +
    Math.cos(lat * RAD) * Math.cos(ss.lat * RAD) * Math.cos((lon - ss.lon) * RAD);
  return Math.asin(Math.max(-1, Math.min(1, s))) / RAD;
}

// The greyline: the sun within 6° of the horizon, either side — civil
// twilight and the matching band after sunrise / before sunset, when the
// D layer is forming or decaying.
const GREY_DEG = 6;
const lightOf = (el) => (el >= GREY_DEG ? 'day' : el > -GREY_DEG ? 'greyline' : 'night');
// A path: day or night only when both ends agree; greyline if either end is
// on it; otherwise one end in daylight and the other in darkness.
const pathLight = (a, b) => (a === 'greyline' || b === 'greyline' ? 'greyline' : a === b ? a : 'mixed');

// ── Day / night layer ───────────────────────────────────────────────────────
//
// A canvas over the tiles, one cell per 4 px, shaded by the sun's elevation
// there and scaled up smooth: clear by day, an amber band along the
// greyline, deepening blue into the night. Web Mercator keeps latitude a
// function of y alone and longitude of x alone, so the trig is per row and
// per column, and world copies come for free.

const NIGHT = [24, 36, 110], AMBER = [245, 158, 11];
const DayNight = L.Layer.extend({
  initialize() { this._t = null; },
  setTime(t) { this._t = t; if (this._map) this._draw(); return this; },
  onAdd(map) {
    if (!map.getPane('daynight')) {
      const p = map.createPane('daynight');
      p.style.zIndex = 250;            // above the tiles, below the markers and paths
      p.style.pointerEvents = 'none';
    }
    this._c = L.DomUtil.create('canvas', 'daynight', map.getPane('daynight'));
    map.on('moveend zoomend resize viewreset', this._draw, this);
    map.on('zoomstart', this._hide, this);
    this._draw();
  },
  onRemove(map) {
    map.off('moveend zoomend resize viewreset', this._draw, this);
    map.off('zoomstart', this._hide, this);
    this._c.remove();
  },
  _hide() { this._c.style.visibility = 'hidden'; },
  _draw() {
    const map = this._map, c = this._c, k = 4;
    const size = map.getSize();
    const w = Math.ceil(size.x / k) + 1, h = Math.ceil(size.y / k) + 1;
    c.width = w; c.height = h;
    c.style.width = `${w * k}px`; c.style.height = `${h * k}px`;
    L.DomUtil.setPosition(c, map.containerPointToLayerPoint([0, 0]));
    const ss = subsolar(this._t ?? Date.now() / 1000);
    const sd = Math.sin(ss.lat * RAD), cd = Math.cos(ss.lat * RAD);
    const rowS = new Float64Array(h), rowC = new Float64Array(h), colC = new Float64Array(w);
    for (let y = 0; y < h; y++) {
      const lat = map.containerPointToLatLng([0, y * k + k / 2]).lat * RAD;
      rowS[y] = Math.sin(lat) * sd;
      rowC[y] = Math.cos(lat) * cd;
    }
    for (let x = 0; x < w; x++) colC[x] = Math.cos((map.containerPointToLatLng([x * k + k / 2, 0]).lng - ss.lon) * RAD);
    const ctx = c.getContext('2d'), img = ctx.createImageData(w, h), px = img.data;
    for (let y = 0, o = 0; y < h; y++) {
      for (let x = 0; x < w; x++, o += 4) {
        const el = Math.asin(Math.max(-1, Math.min(1, rowS[y] + rowC[y] * colC[x]))) / RAD;
        const dark = 0.55 * Math.max(0, Math.min(1, (GREY_DEG - el) / 24));
        const grey = 0.2 * Math.max(0, Math.min(1, (GREY_DEG + 0.75 - Math.abs(el)) / 1.5));
        const a = grey + dark * (1 - grey);
        if (a <= 0) continue;
        for (let i = 0; i < 3; i++) px[o + i] = (AMBER[i] * grey + NIGHT[i] * dark * (1 - grey)) / a;
        px[o + 3] = a * 255;
      }
    }
    ctx.putImageData(img, 0, 0);
    c.style.visibility = '';
  },
});

const dayNight = new DayNight();
const legend = L.control({ position: 'bottomleft' });
legend.onAdd = () => {
  const el = L.DomUtil.create('div', 'dn-legend');
  el.innerHTML = '<span><i class="dn-day"></i>Day</span><span><i class="dn-grey"></i>Greyline</span><span><i class="dn-night"></i>Night</span>';
  el.title = `Greyline: the sun within ${GREY_DEG}° of the horizon. Night: more than ${GREY_DEG}° below it.`;
  return el;
};

function showDayNight(on) {
  const map = ndb.map;
  if (!map) return;
  if (on) { dayNight.addTo(map); legend.addTo(map); } else { map.removeLayer(dayNight); legend.remove(); }
}
{
  const box = $('show-daynight');
  try { const v = localStorage.getItem('ndb.daynight'); if (v !== null) box.checked = v === '1'; } catch (e) { /* storage unavailable */ }
  box.addEventListener('change', () => {
    try { localStorage.setItem('ndb.daynight', box.checked ? '1' : '0'); } catch (e) { /* storage unavailable */ }
    showDayNight(box.checked);
  });
  showDayNight(box.checked);
  // Live, the terminator moves ~1° of longitude every 4 minutes.
  setInterval(() => { if (!TL.on) dayNight.setTime(null); }, 60 * 1000);
}

// ── Timeline ────────────────────────────────────────────────────────────────

const SNR_COLOURS = [[30, '#22c55e'], [18, '#00d4ff'], [12, '#f59e0b'], [-1e9, '#ef4444']];
const snrHex = (snr) => SNR_COLOURS.find(([lo]) => snr >= lo)[1];
const TRAIL_SLOTS = 4;            // an hour of fading after a beacon goes quiet

const TL = {
  on: false,
  hours: 24,
  speed: 8,                       // slots per second
  trail: true,
  data: null,
  i: 0,
  playing: false,
  raf: 0,
  last: 0,
  seq: 0,
  group: null,                    // markers
  paths: null,
  markers: new Map(),             // beacon key -> { m, iconKey }
};

const fmtCache = new Map();
function fmt(tz, opts) {
  const k = tz + JSON.stringify(opts);
  if (!fmtCache.has(k)) fmtCache.set(k, new Intl.DateTimeFormat('en-GB', { timeZone: tz, hourCycle: 'h23', ...opts }));
  return fmtCache.get(k);
}
const DAY = { weekday: 'short', day: '2-digit', month: 'short' };
const HM = { hour: '2-digit', minute: '2-digit' };

function tlOpen() {
  const map = ndb.map;
  if (!map || TL.on) return;
  TL.on = state.timeline = true;
  for (const name of ['heard', 'paths', 'earlier']) map.removeLayer(layers[name]);
  TL.paths = TL.paths || L.layerGroup();
  TL.group = TL.group || L.layerGroup();
  TL.paths.addTo(map);
  TL.group.addTo(map);
  $('tl-bar').hidden = false;
  $('tl-open').classList.add('on');
  map.invalidateSize();
  tlLoad(true);
}

function tlClose() {
  const map = ndb.map;
  if (!TL.on) return;
  tlPause();
  TL.on = state.timeline = false;
  map.removeLayer(TL.paths);
  map.removeLayer(TL.group);
  layers.paths.addTo(map);
  layers.heard.addTo(map);
  if ($('show-earlier').checked) layers.earlier.addTo(map);
  $('tl-bar').hidden = true;
  $('tl-open').classList.remove('on');
  dayNight.setTime(null);
  map.invalidateSize();
  ndb.resumeLive();
}

async function tlLoad(play) {
  const seq = ++TL.seq;
  tlPause();
  $('tl-time').textContent = 'Loading…';
  try {
    const d = await ndb.fetchStats(TL.hours, 900);
    if (seq !== TL.seq || !TL.on) return;
    d.slots = d.up.length;
    d.beacons = d.beacons.filter((b) => b.lat != null);
    // Per slot: how many were heard (for the strip under the slider) and
    // the light at the receiver.
    d.count = new Array(d.slots).fill(0);
    for (const b of d.beacons) b.mins.forEach((m, i) => { if (m) d.count[i]++; });
    d.rxLight = d.up.map((_, i) => (d.rx_lat == null ? null
      : lightOf(sunElev(d.rx_lat, d.rx_lon, subsolar(d.t0 + i * d.step + d.step / 2)))));
    TL.data = d;
    for (const [, e] of TL.markers) TL.group.removeLayer(e.m);
    TL.markers.clear();
    const slider = $('tl-slider');
    slider.max = d.slots - 1;
    // Start where there is something to see: the first slot anything was heard.
    const first = d.count.findIndex((n) => n > 0);
    TL.i = play ? Math.max(0, first) : Math.min(TL.i, d.slots - 1);
    if ($('autofit').checked) {
      const pts = d.beacons.map((b) => [b.lat, b.lon]);
      if (d.rx_lat != null) pts.push([d.rx_lat, d.rx_lon]);
      ndb.fitView(pts);
    }
    tlStrip();
    tlFrame();
    if (play && first >= 0) tlPlay();
  } catch (e) {
    if (seq === TL.seq) $('tl-time').textContent = `Could not load history (${e.message})`;
  }
}

function tlTip(b, i, d) {
  const t = d.t0 + i * d.step, m = b.mins[i];
  const ss = subsolar(t + d.step / 2);
  const bl = lightOf(sunElev(b.lat, b.lon, ss));
  const rl = d.rx_lat != null ? lightOf(sunElev(d.rx_lat, d.rx_lon, ss)) : null;
  const heardAgo = (() => {
    for (let j = i - 1; j >= 0; j--) if (b.mins[j]) return (i - j) * d.step / 60;
    return null;
  })();
  const now = m
    ? `<span class="ok">heard ${m} of ${d.up[i]} min · ${b.snrs[i]} dB</span>`
    : heardAgo != null ? `<span class="earlier">last heard ${heardAgo < 60 ? `${heardAgo} min` : `${(heardAgo / 60).toFixed(heardAgo < 600 ? 1 : 0)} h`} before</span>`
      : '<span class="m">not heard yet in this window</span>';
  const r = (k, v) => (v ? `<tr><td>${k}</td><td>${v}</td></tr>` : '');
  return `<div class="tt-head"><b>${esc(b.ident)}</b> ${esc(b.name)} ${flag(b.country)}</div>
    <div class="tt-sub">${now}</div>
    <table>
      ${r('Frequency', `${(b.freq_hz / 1e3).toFixed(1)} kHz`)}
      ${r('Distance', b.dist_km != null ? `${b.dist_km} km ${b.bearing_deg != null ? compass(b.bearing_deg) : ''}` : '')}
      ${r('Sun', `${bl} here${rl ? ` · ${rl} at the receiver` : ''}`)}
      ${r('In window', `heard ${b.minutes} min · best ${b.snr_best.toFixed(0)} dB`)}
    </table>`;
}

function tlFrame() {
  const d = TL.data;
  if (!d) return;
  const i = TL.i, t = d.t0 + i * d.step;
  $('tl-slider').value = i;
  dayNight.setTime(t + d.step / 2);

  TL.paths.clearLayers();
  const rx = d.rx_lat != null ? [d.rx_lat, d.rx_lon] : null;
  let heard = 0;
  for (const b of d.beacons) {
    const m = b.mins[i];
    let age = 0;
    if (!m && TL.trail) for (let j = 1; j <= TRAIL_SLOTS && i - j >= 0; j++) if (b.mins[i - j]) { age = j; break; }
    let iconKey, make, opacity = 1, z = 0;
    if (m) {
      heard++;
      const col = snrHex(b.snrs[i]);
      iconKey = `on|${col}`;
      make = () => ndb.ndbIcon(col, { label: b.ident });
      z = 500 + b.snrs[i];
      if (rx) {
        L.polyline(ndb.greatCircle(rx, [b.lat, b.lon]), {
          color: col, weight: 1.5, opacity: 0.25 + 0.5 * (d.up[i] ? m / d.up[i] : 0), interactive: false,
        }).addTo(TL.paths);
      }
    } else if (age) {
      iconKey = 'trail';
      make = () => ndb.ndbIcon('#3b82f6', { size: 14, solid: false, label: b.ident, labelClass: 'earlier' });
      opacity = 1 - age / (TRAIL_SLOTS + 1);
    } else {
      iconKey = 'idle';
      make = () => L.divIcon({ className: 'tl-idle', iconSize: [7, 7], iconAnchor: [3.5, 3.5] });
      z = -500;
    }
    let e = TL.markers.get(b.key);
    if (!e) {
      const mk = L.marker([b.lat, b.lon], { icon: make(), zIndexOffset: z });
      mk.on('mouseover', () => ndb.placeTip(mk));
      mk.bindTooltip('', ndb.TIP).addTo(TL.group);
      e = { m: mk, iconKey };
      TL.markers.set(b.key, e);
    } else if (e.iconKey !== iconKey) {
      e.m.setIcon(make());
      e.iconKey = iconKey;
    }
    e.m.setZIndexOffset(z);
    e.m.setOpacity(opacity);
    e.m.setTooltipContent(tlTip(b, i, d));
  }

  const tz = d.timezone;
  const end = t + d.step;
  const utc = `${fmt('UTC', DAY).format(new Date(t * 1000))} ${fmt('UTC', HM).format(new Date(t * 1000))}–${fmt('UTC', HM).format(new Date(end * 1000))} UTC`;
  const local = tz && tz !== 'UTC' ? `${fmt(tz, HM).format(new Date(t * 1000))} local` : '';
  const rl = d.rxLight[i];
  $('tl-time').innerHTML = `<b>${utc}</b>${local ? ` <span class="m">· ${local}</span>` : ''}<br>` +
    (d.up[i] ? `${heard} heard${rl ? ` <span class="tl-light ${rl}">${rl} at receiver</span>` : ''}` : '<span class="m">receiver down</span>');
}

// The strip under the slider: one column per slot, its height the beacons
// heard then, on a band coloured by the light at the receiver.
function tlStrip() {
  const d = TL.data, c = $('tl-strip');
  if (!d) return;
  const w = c.clientWidth, h = c.clientHeight, dpr = devicePixelRatio || 1;
  c.width = w * dpr; c.height = h * dpr;
  const ctx = c.getContext('2d');
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, h);
  const N = d.slots, cw = w / N, max = Math.max(1, ...d.count);
  const band = { day: '#2a2a1c', greyline: '#4a3510', night: '#101632' };
  for (let i = 0; i < N; i++) {
    const x = i * cw;
    ctx.fillStyle = d.up[i] ? band[d.rxLight[i]] || '#1c1c22' : '#0d0d0f';
    ctx.fillRect(x, 0, cw + 0.5, h);
    if (d.count[i]) {
      const bh = (d.count[i] / max) * (h - 3);
      ctx.fillStyle = '#3987e5';
      ctx.fillRect(x, h - bh, Math.max(1, cw - (cw > 3 ? 0.5 : 0)), bh);
    }
  }
}

function tlStep(now) {
  if (!TL.playing) return;
  const dt = (now - TL.last) / 1000;
  const n = Math.floor(dt * TL.speed);
  if (n > 0) {
    TL.last += (n / TL.speed) * 1000;
    TL.i = Math.min(TL.data.slots - 1, TL.i + n);
    tlFrame();
    if (TL.i >= TL.data.slots - 1) { tlPause(); return; }
  }
  TL.raf = requestAnimationFrame(tlStep);
}
function tlPlay() {
  if (!TL.data || TL.playing) return;
  if (TL.i >= TL.data.slots - 1) TL.i = 0;
  TL.playing = true;
  TL.last = performance.now();
  $('tl-play').textContent = '❚❚';
  $('tl-play').title = 'Pause (space)';
  TL.raf = requestAnimationFrame(tlStep);
}
function tlPause() {
  TL.playing = false;
  cancelAnimationFrame(TL.raf);
  $('tl-play').textContent = '▶';
  $('tl-play').title = 'Play (space)';
}

$('tl-open').addEventListener('click', () => (TL.on ? tlClose() : tlOpen()));
$('tl-close').addEventListener('click', tlClose);
$('tl-play').addEventListener('click', () => (TL.playing ? tlPause() : tlPlay()));
$('tl-slider').addEventListener('input', (e) => { TL.i = Number(e.target.value); tlFrame(); });
$('tl-slider').addEventListener('pointerdown', tlPause);
$('tl-range').addEventListener('change', (e) => { TL.hours = Number(e.target.value); tlLoad(true); });
$('tl-speed').addEventListener('change', (e) => { TL.speed = Number(e.target.value); TL.last = performance.now(); });
$('tl-trail').addEventListener('change', (e) => { TL.trail = e.target.checked; tlFrame(); });
document.addEventListener('keydown', (e) => {
  if (!TL.on || e.target.closest('input:not([type=range]), select, textarea, button') || document.querySelector('.modal:not([hidden])')) return;
  if (e.key === ' ') { e.preventDefault(); TL.playing ? tlPause() : tlPlay(); }
});
let tlResize = null;
addEventListener('resize', () => { clearTimeout(tlResize); tlResize = setTimeout(() => TL.on && tlStrip(), 150); });

Object.assign(ndb, { subsolar, sunElev, lightOf, pathLight, GREY_DEG });
})();
