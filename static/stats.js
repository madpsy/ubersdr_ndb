// stats.js — the reception stats modal: what has been heard over time, by
// hour of day and by distance, from the server's history (api/stats). Built
// for propagation: when does each distance open up, which beacons are only
// heard at night, which are hardly heard at all.
//
// The server sends hour-by-hour data per beacon and the browser does all the
// binning, so "hour of day" can be UTC or the receiver's own time zone (an
// IANA name from UberSDR's /api/description, DST handled by Intl).
//
// Uses app.js's helpers, shared as window.ndb.

'use strict';

(() => {
const { $, esc, flag, compass, tip, state } = window.ndb;

const SX = {
  hours: 168,
  tz: 'UTC',          // 'UTC' or the receiver's IANA zone
  data: null,         // last api/stats response, decoded (see decode())
  includeUnheard: false,
  sort: { key: 'avail', asc: true },   // least heard first
  open: null,         // key of the beacon whose detail row is open
  seq: 0,
};

// Encoding of the per-hour strings: one character per hour, value 0-63.
const SX_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const sxDecode = (str) => Array.from(str, (c) => SX_CHARS.indexOf(c));

// Distance bands for the propagation views. Only bands with a heard beacon show.
const SX_BANDS = [
  { lo: 0, hi: 100, label: '< 100 km' },
  { lo: 100, hi: 250, label: '100–250 km' },
  { lo: 250, hi: 500, label: '250–500 km' },
  { lo: 500, hi: 1000, label: '500–1000 km' },
  { lo: 1000, hi: 2000, label: '1000–2000 km' },
  { lo: 2000, hi: 1e9, label: '≥ 2000 km' },
];
const sxBand = (km) => (km == null ? -1 : SX_BANDS.findIndex((b) => km >= b.lo && km < b.hi));

// ── Time zones ──────────────────────────────────────────────────────────────

const sxFmtCache = new Map();
function sxFmt(tz, opts) {
  const k = tz + JSON.stringify(opts);
  if (!sxFmtCache.has(k)) sxFmtCache.set(k, new Intl.DateTimeFormat('en-GB', { timeZone: tz, hourCycle: 'h23', ...opts }));
  return sxFmtCache.get(k);
}
function sxHourOf(t, tz) {
  return Number(sxFmt(tz, { hour: '2-digit' }).formatToParts(new Date(t * 1000)).find((p) => p.type === 'hour').value) % 24;
}
const sxWhen = (t, tz = SX.tz) => sxFmt(tz, { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }).format(new Date(t * 1000));
const sxTzLabel = () => (SX.tz === 'UTC' ? 'UTC' : 'receiver local time');
const sxIso = (t) => new Date(t * 1000).toISOString().replace('.000Z', 'Z');

// ── Data ────────────────────────────────────────────────────────────────────

// Everything the views need, derived once per response and time zone.
function sxDerive() {
  const d = SX.data;
  const H = d.hours;
  const hod = [];
  for (let i = 0; i < H; i++) hod.push(sxHourOf(d.t0 + i * 3600, SX.tz));

  const n = new Array(H).fill(0), far = new Array(H).fill(null), farId = new Array(H).fill('');
  for (const b of d.beacons) {
    for (let i = 0; i < H; i++) {
      if (!b.mins[i]) continue;
      n[i]++;
      if (b.dist_km != null && (far[i] == null || b.dist_km > far[i])) { far[i] = b.dist_km; farId[i] = b.ident; }
    }
    b.avail = d.up_minutes ? b.minutes / d.up_minutes : 0;
    b.strip = new Array(24).fill(null);
    const hm = new Array(24).fill(0), hu = new Array(24).fill(0);
    for (let i = 0; i < H; i++) { hm[hod[i]] += b.mins[i]; hu[hod[i]] += d.up[i]; }
    for (let h = 0; h < 24; h++) if (hu[h]) b.strip[h] = hm[h] / hu[h];
  }

  // By hour of day: average beacons heard at once, and per distance band the
  // share of time its beacons were heard.
  const upH = new Array(24).fill(0), hoursUp = new Array(24).fill(0), nH = new Array(24).fill(0);
  for (let i = 0; i < H; i++) {
    if (!d.up[i]) continue;
    upH[hod[i]] += d.up[i];
    hoursUp[hod[i]]++;
    nH[hod[i]] += n[i];
  }
  const avgN = nH.map((v, h) => (hoursUp[h] ? v / hoursUp[h] : null));
  const bands = SX_BANDS.map((band, k) => {
    const members = d.beacons.filter((b) => sxBand(b.dist_km) === k);
    const cells = new Array(24).fill(null);
    if (members.length) {
      for (let h = 0; h < 24; h++) {
        if (!upH[h]) continue;
        let m = 0;
        for (const b of members) for (let i = 0; i < H; i++) if (hod[i] === h) m += b.mins[i];
        cells[h] = m / (members.length * upH[h]);
      }
    }
    return { ...band, count: members.length, cells };
  }).filter((b) => b.count);

  return { hod, n, far, farId, avgN, bands };
}

// api/stats, decoded: up, and each beacon's mins and snrs, as arrays with one
// entry per slot (`step` seconds: 3600, or 900 for the history's own buckets).
async function sxFetch(hours, step = 3600) {
  const r = await fetch(`api/stats?hours=${hours}${step === 3600 ? '' : `&step=${step}`}`);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const d = await r.json();
  d.step = d.step || 3600;
  d.up = sxDecode(d.up);
  for (const b of d.beacons) { b.mins = sxDecode(b.m); b.snrs = sxDecode(b.s); delete b.m; delete b.s; }
  return d;
}

async function sxLoad() {
  const seq = ++SX.seq;
  $('stats-body').classList.add('loading');
  $('stats-status').textContent = 'Loading…';
  try {
    const d = await sxFetch(SX.hours);
    if (seq !== SX.seq) return;
    SX.data = d;
    sxTzButtons();
    $('stats-status').textContent = `Updated ${sxWhen(d.now)} ${SX.tz === 'UTC' ? 'UTC' : ''}`.trim();
    sxRender();
  } catch (e) {
    if (seq === SX.seq) $('stats-status').textContent = `Could not load stats (${e.message})`;
  } finally {
    if (seq === SX.seq) $('stats-body').classList.remove('loading');
  }
}

// ── Charts (SVG) ────────────────────────────────────────────────────────────

const SVGNS = 'http://www.w3.org/2000/svg';
function sxEl(tag, attrs = {}, parent) {
  const e = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}
function sxNice(max) {
  if (!(max > 0)) return { top: 1, step: 1 };
  const raw = max / 4, mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw);
  return { top: Math.ceil(max / step) * step, step };
}
function sxTipAt(e, html) {
  tip.innerHTML = html;
  tip.style.display = 'block';
  const w = tip.offsetWidth;
  tip.style.left = `${Math.min(e.clientX + 14, innerWidth - w - 8)}px`;
  tip.style.top = `${e.clientY + 14}px`;
}
const sxTipOff = () => { tip.style.display = 'none'; };

// Frame: y grid + ticks in the text tokens, returns scales.
function sxFrame(el, { height = 150, yMax, yFmt = (v) => v, left = 40 }) {
  el.innerHTML = '';
  const W = Math.max(280, el.clientWidth), H = height + 24;
  const svg = sxEl('svg', { width: W, height: H, viewBox: `0 0 ${W} ${H}`, class: 'sx-svg' }, el);
  const m = { l: left, r: 10, t: 8, b: 22 };
  const pw = W - m.l - m.r, ph = H - m.t - m.b;
  const { top, step } = sxNice(yMax);
  const y = (v) => m.t + ph - (v / top) * ph;
  for (let v = 0; v <= top + 1e-9; v += step) {
    sxEl('line', { x1: m.l, x2: W - m.r, y1: y(v), y2: y(v), class: v ? 'sx-grid' : 'sx-axis' }, svg);
    sxEl('text', { x: m.l - 6, y: y(v) + 3, class: 'sx-tick', 'text-anchor': 'end' }, svg).textContent = yFmt(v);
  }
  return { svg, W, H, m, pw, ph, y, top };
}

// Hourly series over the window: a 2px line with a 10% wash, gaps where
// the receiver was down. Crosshair + one tooltip.
function sxLine(el, { values, t0, yFmt, tipFn, yMax }) {
  const N = values.length;
  const max = yMax ?? Math.max(1, ...values.filter((v) => v != null));
  const f = sxFrame(el, { yMax: max, yFmt });
  const x = (i) => f.m.l + (N > 1 ? (i / (N - 1)) * f.pw : f.pw / 2);
  // x ticks: day boundaries (or every few hours for a day's window) in the chosen zone.
  const span = N * 3600;
  const every = span <= 2 * 86400 ? 3 : span <= 8 * 86400 ? 24 : 24 * 3;
  let last = -1;
  for (let i = 0; i < N; i++) {
    const t = t0 + i * 3600;
    const h = sxHourOf(t, SX.tz);
    const mark = every < 24 ? h % every === 0 : h === 0 && Math.floor(i / every) !== last;
    if (!mark) continue;
    if (every >= 24) last = Math.floor(i / every);
    const lbl = every < 24 ? `${String(h).padStart(2, '0')}:00`
      : sxFmt(SX.tz, { day: '2-digit', month: 'short' }).format(new Date(t * 1000));
    sxEl('text', { x: x(i), y: f.H - 6, class: 'sx-tick', 'text-anchor': 'middle' }, f.svg).textContent = lbl;
  }
  // Runs of defined values.
  let d = '', area = '', run = [];
  const flush = () => {
    if (!run.length) return;
    d += run.map((i, k) => `${k ? 'L' : 'M'}${x(i).toFixed(1)},${f.y(values[i]).toFixed(1)}`).join('');
    area += `M${x(run[0]).toFixed(1)},${f.y(0)}` + run.map((i) => `L${x(i).toFixed(1)},${f.y(values[i]).toFixed(1)}`).join('') +
      `L${x(run[run.length - 1]).toFixed(1)},${f.y(0)}Z`;
    if (run.length === 1) d += `h0.01`;
    run = [];
  };
  values.forEach((v, i) => { if (v == null) flush(); else run.push(i); });
  flush();
  sxEl('path', { d: area, class: 'sx-area' }, f.svg);
  sxEl('path', { d, class: 'sx-line' }, f.svg);

  const cross = sxEl('line', { y1: f.m.t, y2: f.m.t + f.ph, class: 'sx-cross', visibility: 'hidden' }, f.svg);
  const dot = sxEl('circle', { r: 4, class: 'sx-dot', visibility: 'hidden' }, f.svg);
  const hit = sxEl('rect', { x: f.m.l, y: 0, width: f.pw, height: f.H, fill: 'transparent' }, f.svg);
  hit.addEventListener('pointermove', (e) => {
    const r = f.svg.getBoundingClientRect();
    const i = Math.max(0, Math.min(N - 1, Math.round(((e.clientX - r.left - f.m.l) / f.pw) * (N - 1))));
    cross.setAttribute('x1', x(i)); cross.setAttribute('x2', x(i)); cross.setAttribute('visibility', 'visible');
    if (values[i] != null) {
      dot.setAttribute('cx', x(i)); dot.setAttribute('cy', f.y(values[i])); dot.setAttribute('visibility', 'visible');
    } else dot.setAttribute('visibility', 'hidden');
    sxTipAt(e, tipFn(i));
  });
  hit.addEventListener('pointerleave', () => { cross.setAttribute('visibility', 'hidden'); dot.setAttribute('visibility', 'hidden'); sxTipOff(); });
}

// 24 columns, one per hour of day.
function sxColumns(el, { values, yFmt, tipFn }) {
  const f = sxFrame(el, { yMax: Math.max(1, ...values.filter((v) => v != null)), yFmt, height: 130 });
  const slot = f.pw / 24, bw = Math.min(24, slot - 2);
  values.forEach((v, h) => {
    const cx = f.m.l + slot * h + slot / 2;
    if (h % 3 === 0) sxEl('text', { x: cx, y: f.H - 6, class: 'sx-tick', 'text-anchor': 'middle' }, f.svg).textContent = String(h).padStart(2, '0');
    const g = sxEl('g', { class: 'sx-colg', tabindex: 0 }, f.svg);
    sxEl('rect', { x: f.m.l + slot * h, y: f.m.t, width: slot, height: f.ph, fill: 'transparent' }, g);
    if (v) {
      const y0 = f.y(0), y1 = f.y(v), r = Math.min(4, (y0 - y1), bw / 2), x0 = cx - bw / 2;
      sxEl('path', { class: 'sx-col', d: `M${x0},${y0}V${y1 + r}Q${x0},${y1} ${x0 + r},${y1}H${x0 + bw - r}Q${x0 + bw},${y1} ${x0 + bw},${y1 + r}V${y0}Z` }, g);
    }
    const show = (e) => sxTipAt(e, tipFn(h));
    g.addEventListener('pointermove', show);
    g.addEventListener('pointerleave', sxTipOff);
    g.addEventListener('focus', () => { const b = g.getBoundingClientRect(); show({ clientX: b.right, clientY: b.top }); });
    g.addEventListener('blur', sxTipOff);
  });
}

// Sequential blue on the dark surface: validated ordinal ramp, 600 → 150.
const SX_RAMP = ['#184f95', '#256abf', '#3987e5', '#6da7ec', '#b7d3f6'];
function sxRamp(v) {
  if (v == null) return null;
  const p = Math.max(0, Math.min(1, v)) * (SX_RAMP.length - 1);
  const i = Math.min(SX_RAMP.length - 2, Math.floor(p)), t = p - i;
  const a = SX_RAMP[i].match(/\w\w/g).map((h) => parseInt(h, 16)), b = SX_RAMP[i + 1].match(/\w\w/g).map((h) => parseInt(h, 16));
  return `rgb(${a.map((c, k) => Math.round(c + (b[k] - c) * t)).join(',')})`;
}
// Availability reads better on a root scale: most cells are small fractions.
const sxShade = (v) => (v == null ? null : v <= 0 ? 'var(--sx-zero)' : sxRamp(Math.sqrt(v)));

function sxHeat(el, bands, tipFn) {
  el.innerHTML = '';
  const W = Math.max(320, el.clientWidth);
  const left = 96, top = 4, cellH = 22, gap = 2;
  const cw = (W - left - 8) / 24;
  const H = top + bands.length * cellH + 22;
  const svg = sxEl('svg', { width: W, height: H, viewBox: `0 0 ${W} ${H}`, class: 'sx-svg' }, el);
  bands.forEach((b, r) => {
    const y = top + r * cellH;
    sxEl('text', { x: left - 8, y: y + cellH / 2 + 3, class: 'sx-tick', 'text-anchor': 'end' }, svg).textContent = `${b.label} (${b.count})`;
    b.cells.forEach((v, h) => {
      const g = sxEl('g', { tabindex: 0, class: 'sx-cellg' }, svg);
      const fill = sxShade(v);
      sxEl('rect', { x: left + h * cw + gap / 2, y: y + gap / 2, width: cw - gap, height: cellH - gap, rx: 2,
        class: fill ? 'sx-cell' : 'sx-cell nodata', style: fill ? `fill:${fill}` : '' }, g);
      const show = (e) => sxTipAt(e, tipFn(b, h, v));
      g.addEventListener('pointermove', show);
      g.addEventListener('pointerleave', sxTipOff);
      g.addEventListener('focus', () => { const bb = g.getBoundingClientRect(); show({ clientX: bb.right, clientY: bb.top }); });
      g.addEventListener('blur', sxTipOff);
    });
  });
  for (let h = 0; h < 24; h += 3)
    sxEl('text', { x: left + h * cw + cw / 2, y: H - 6, class: 'sx-tick', 'text-anchor': 'middle' }, svg).textContent = String(h).padStart(2, '0');
}

// 24 cells inline in a table row: that beacon's share of time heard by hour of day.
function sxStrip(strip) {
  return `<span class="sx-strip">${strip.map((v, h) => {
    const fill = sxShade(v);
    return `<i style="${fill ? `background:${fill}` : ''}" class="${fill ? '' : 'nodata'}" title="${String(h).padStart(2, '0')}:00 — ${v == null ? 'receiver down' : `${Math.round(v * 100)}%`}"></i>`;
  }).join('')}</span>`;
}

// ── CSV ─────────────────────────────────────────────────────────────────────

// Save rows as a CSV file (or TSV / semicolon-separated, with `sep`).
function csvSave(filename, header, rows, sep = ',') {
  const cell = (v) => {
    const s = v == null ? '' : String(v);
    return s.includes(sep) || /["\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  const text = [header, ...rows].map((r) => r.map(cell).join(sep)).join('\r\n') + '\r\n';
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: sep === '\t' ? 'text/tab-separated-values' : 'text/csv' }));
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 0);
}
const sxCsv = (name, header, rows) => csvSave(`ndb-${name}-${SX.hours}h-${new Date().toISOString().slice(0, 10)}.csv`, header, rows);

function sxCsvTimeline(dv) {
  const d = SX.data;
  sxCsv('timeline',
    ['hour_start_utc', 'hour_start_local', 'receiver_up_min', 'beacons_heard', 'furthest_km', 'furthest_ident'],
    d.up.map((u, i) => {
      const t = d.t0 + i * 3600;
      return [sxIso(t), d.timezone ? sxWhen(t, d.timezone) : '', u, u ? dv.n[i] : '', dv.far[i] ?? '', dv.farId[i]];
    }));
}
function sxCsvHourOfDay(dv) {
  sxCsv(`hour-of-day-${SX.tz === 'UTC' ? 'utc' : 'local'}`,
    ['hour', 'time_zone', 'avg_beacons_heard', ...dv.bands.map((b) => `heard_pct ${b.label}`)],
    dv.avgN.map((v, h) => [h, SX.tz, v == null ? '' : v.toFixed(2),
      ...dv.bands.map((b) => (b.cells[h] == null ? '' : (b.cells[h] * 100).toFixed(1)))]));
}
function sxCsvBeacons(rows) {
  sxCsv('beacons',
    ['ident', 'name', 'country', 'freq_khz', 'dist_km', 'bearing_deg', 'heard_min', 'availability_pct', 'snr_mean_db',
      'snr_best_db', 'first_heard_utc', 'last_heard_utc', ...Array.from({ length: 24 }, (_, h) => `h${String(h).padStart(2, '0')}_pct`)],
    rows.map((b) => [b.ident, b.name, b.country, (b.freq_hz / 1e3).toFixed(1), b.dist_km ?? '', b.bearing_deg ?? '',
      b.minutes ?? 0, b.heard ? (b.avail * 100).toFixed(1) : 0, b.heard ? b.snr_mean : '', b.heard ? b.snr_best : '',
      b.heard ? sxIso(b.first_s) : '', b.heard ? sxIso(b.last_s) : '',
      ...(b.heard ? b.strip : new Array(24).fill(0)).map((v) => (v == null ? '' : (v * 100).toFixed(1)))]));
}

// ── Render ──────────────────────────────────────────────────────────────────

const sxPct = (v) => (v == null ? '–' : v >= 0.995 ? '100%' : v >= 0.1 ? `${Math.round(v * 100)}%` : v > 0 ? `${(v * 100).toFixed(1)}%` : '0%');
const sxDur = (min) => (min < 60 ? `${min} min` : min < 2880 ? `${(min / 60).toFixed(min < 600 ? 1 : 0)} h` : `${(min / 1440).toFixed(1)} d`);
const sxHr = (h) => `${String(h).padStart(2, '0')}:00–${String((h + 1) % 24).padStart(2, '0')}:00`;

function sxRender() {
  const d = SX.data;
  if (!d || $('stats-modal').hidden) return;
  const dv = sxDerive();
  SX.dv = dv;
  const heard = d.beacons;
  const windowMin = d.hours * 60;

  if (!d.up_minutes) {
    $('sx-empty').hidden = false;
    $('sx-content').hidden = true;
    return;
  }
  $('sx-empty').hidden = true;
  $('sx-content').hidden = false;

  // KPIs
  const far = heard.filter((b) => b.dist_km != null).sort((a, b) => b.dist_km - a.dist_km)[0];
  const av = heard.map((b) => b.avail).sort((a, b) => a - b);
  const median = av.length ? av[Math.floor(av.length / 2)] : null;
  const published = heard.filter((b) => b.confirmed).length + d.unheard.length;
  const fresh = state.heard.filter((e) => e.confirmed && e.first_s >= d.t0).sort((a, b) => b.first_s - a.first_s);
  $('sx-kpis').innerHTML = `
    <div class="stat"><span class="stat-label">Beacons heard</span><span class="stat-val">${heard.length}<small> of ${published} published in range</small></span></div>
    <div class="stat"><span class="stat-label">Furthest heard</span><span class="stat-val">${far ? `${far.dist_km} km <small>${esc(far.ident)} ${flag(far.country)}</small>` : '–'}</span></div>
    <div class="stat"><span class="stat-label">Median availability</span><span class="stat-val">${sxPct(median)}<small> of the time up</small></span></div>
    <div class="stat"><span class="stat-label">New catches</span><span class="stat-val">${fresh.length}<small> ${fresh.length ? esc(fresh.slice(0, 5).map((e) => e.ident).join(' ')) + (fresh.length > 5 ? ' …' : '') : 'first heard in this window'}</small></span></div>
    <div class="stat"><span class="stat-label">Receiver up</span><span class="stat-val">${sxPct(d.up_minutes / windowMin)}<small> ${sxDur(d.up_minutes)} of ${sxDur(windowMin)}</small></span></div>`;

  // Over time
  const nVals = d.up.map((u, i) => (u ? dv.n[i] : null));
  sxLine($('sx-count'), {
    values: nVals, t0: d.t0, yFmt: (v) => v,
    tipFn: (i) => `<div><span class="k"></span><b class="v">${nVals[i] ?? '–'}</b> beacons</div><div class="m">${sxWhen(d.t0 + i * 3600)} · up ${d.up[i]} min</div>`,
  });
  const fVals = d.up.map((u, i) => (u ? dv.far[i] : null));
  sxLine($('sx-far'), {
    values: fVals, t0: d.t0, yFmt: (v) => (v >= 1000 ? `${v / 1000}k` : v),
    tipFn: (i) => `<div><span class="k"></span><b class="v">${fVals[i] != null ? `${fVals[i]} km` : '–'}</b> ${esc(dv.farId[i])}</div><div class="m">${sxWhen(d.t0 + i * 3600)}</div>`,
  });

  // By hour of day
  $('sx-hod-title').textContent = `By hour of day · ${sxTzLabel()}${SX.tz === 'UTC' ? '' : ` (${SX.tz})`}`;
  sxColumns($('sx-hod'), {
    values: dv.avgN, yFmt: (v) => v,
    tipFn: (h) => `<div><b class="v">${dv.avgN[h] == null ? '–' : dv.avgN[h].toFixed(1)}</b> beacons heard on average</div><div class="m">${sxHr(h)} ${SX.tz === 'UTC' ? 'UTC' : 'local'}</div>`,
  });
  sxHeat($('sx-heat'), dv.bands, (b, h, v) =>
    `<div><b class="v">${v == null ? 'no data' : sxPct(v)}</b> of the time</div><div class="m">${esc(b.label)} · ${b.count} beacon${b.count === 1 ? '' : 's'} · ${sxHr(h)}</div>`);

  sxRenderDx();
  sxRenderTable();
}

// ── DX: range by bearing, countries ─────────────────────────────────────────

const SX_SECTORS = 16;
const SX_POINTS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
const sxSector = (deg) => Math.round(deg / (360 / SX_SECTORS)) % SX_SECTORS;

// Furthest beacon per sector of the compass, from a list of beacons.
function sxFurthest(list) {
  const out = new Array(SX_SECTORS).fill(null);
  for (const b of list) {
    if (b.dist_km == null || b.bearing_deg == null) continue;
    const k = sxSector(b.bearing_deg);
    if (!out[k] || b.dist_km > out[k].dist_km) out[k] = b;
  }
  return out;
}

function sxDxData() {
  const d = SX.data;
  const log = state.heard.filter((e) => e.confirmed);
  const now = state.heardNow || d.now;
  return {
    log,
    all: sxFurthest(log),
    win: sxFurthest(d.beacons),
    hour: sxFurthest(log.filter((e) => now - e.last_s < 3600)),
  };
}

function sxRose(el, dx) {
  el.innerHTML = '';
  const S = Math.min(400, Math.max(260, el.clientWidth));
  const c = S / 2, R = c - 26;
  const svg = sxEl('svg', { width: S, height: S, viewBox: `0 0 ${S} ${S}`, class: 'sx-svg sx-rose' }, el);
  const max = Math.max(1, ...[...dx.all, ...dx.win, ...dx.hour].map((b) => b?.dist_km || 0));
  const { top, step } = sxNice(max);
  const r = (km) => (km / top) * R;
  const pt = (deg, rad) => [c + rad * Math.sin(deg * Math.PI / 180), c - rad * Math.cos(deg * Math.PI / 180)];
  for (let v = step; v <= top + 1e-9; v += step) {
    sxEl('circle', { cx: c, cy: c, r: r(v), class: 'sx-grid', fill: 'none' }, svg);
  }
  for (let k = 0; k < 8; k++) {
    const [x1, y1] = pt(k * 45, R);
    sxEl('line', { x1: c, y1: c, x2: x1, y2: y1, class: 'sx-grid' }, svg);
    const [lx, ly] = pt(k * 45, R + 14);
    sxEl('text', { x: lx, y: ly + 3, class: 'sx-tick', 'text-anchor': 'middle' }, svg).textContent = SX_POINTS[k * 2];
  }
  const w = 360 / SX_SECTORS;
  const wedge = (k, rad) => {
    const [x0, y0] = pt(k * w - w / 2, rad), [x1, y1] = pt(k * w + w / 2, rad);
    return `M${c},${c}L${x0.toFixed(1)},${y0.toFixed(1)}A${rad.toFixed(1)},${rad.toFixed(1)} 0 0 1 ${x1.toFixed(1)},${y1.toFixed(1)}Z`;
  };
  for (const [layer, cls] of [[dx.all, 'all'], [dx.win, 'win'], [dx.hour, 'hour']]) {
    layer.forEach((b, k) => { if (b) sxEl('path', { d: wedge(k, Math.max(2, r(b.dist_km))), class: `sx-wedge ${cls}` }, svg); });
  }
  // Ring labels over the wedges, haloed (see style.css).
  for (let v = step; v <= top + 1e-9; v += step) {
    sxEl('text', { x: c + 3, y: c - r(v) + 10, class: 'sx-tick' }, svg).textContent = v >= 1000 ? `${v / 1000}k km` : `${v} km`;
  }
  const line = (lbl, b) => `<div><span class="m">${lbl}</span> ${b ? `<b>${esc(b.ident)}</b> ${esc(b.name)} ${flag(b.country)} · ${b.dist_km} km` : '–'}</div>`;
  for (let k = 0; k < SX_SECTORS; k++) {
    const g = sxEl('path', { d: wedge(k, R), class: 'sx-wedge-hit', tabindex: 0 }, svg);
    const show = (e) => sxTipAt(e, `<div class="m">${SX_POINTS[k]} · ${Math.round(k * w - w / 2 + 360) % 360}–${Math.round(k * w + w / 2)}°</div>` +
      line('All time', dx.all[k]) + line('Window', dx.win[k]) + line('Last hour', dx.hour[k]));
    g.addEventListener('pointermove', show);
    g.addEventListener('pointerleave', sxTipOff);
    g.addEventListener('focus', () => { const bb = g.getBoundingClientRect(); show({ clientX: bb.right, clientY: bb.top }); });
    g.addEventListener('blur', sxTipOff);
  }
}

let sxRegion = null;
function sxCountryName(cc) {
  try { sxRegion = sxRegion || new Intl.DisplayNames(['en'], { type: 'region' }); return sxRegion.of(cc.toUpperCase()); } catch (e) { return cc; }
}

// One row per country: beacons heard in the window and ever, the furthest
// and the first.
function sxCountries(dx) {
  const d = SX.data, rows = new Map();
  const get = (cc) => {
    if (!rows.has(cc)) rows.set(cc, { cc, name: cc ? sxCountryName(cc) : 'Unknown', win: 0, ever: 0, fresh: 0, far: null, first: null });
    return rows.get(cc);
  };
  for (const b of d.beacons) get(b.country || '').win++;
  for (const e of dx.log) {
    const r = get(e.country || '');
    r.ever++;
    if (e.first_s >= d.t0) r.fresh++;
    if (e.dist_km != null && (!r.far || e.dist_km > r.far.dist_km)) r.far = e;
    if (!r.first || e.first_s < r.first.first_s) r.first = e;
  }
  return [...rows.values()].sort((a, b) => b.ever - a.ever || b.win - a.win || a.name.localeCompare(b.name));
}

function sxRenderDx() {
  const dx = sxDxData();
  SX.dx = dx;
  sxRose($('sx-rose'), dx);
  const rows = sxCountries(dx);
  SX.countries = rows;
  const winN = rows.filter((r) => r.win).length;
  $('sx-countries-hint').textContent = `${winN} in this window · ${rows.filter((r) => r.ever).length} all time`;
  $('sx-country-rows').innerHTML = rows.length ? rows.map((r) => `<tr>
      <td>${flag(r.cc)} ${esc(r.name)}${r.fresh ? ` <span class="badge new" title="${r.fresh} first heard in this window">+${r.fresh} new</span>` : ''}</td>
      <td class="num">${r.win || '–'}</td><td class="num">${r.ever}</td>
      <td>${r.far ? `<b>${esc(r.far.ident)}</b> ${r.far.dist_km} km` : '–'}</td>
      <td>${r.first ? `<b>${esc(r.first.ident)}</b> <span class="m">${sxWhen(r.first.first_s)}</span>` : '–'}</td>
    </tr>`).join('') : '<tr><td colspan="5" class="empty">Nothing heard yet.</td></tr>';
}

function sxCsvRose(dx) {
  const w = 360 / SX_SECTORS;
  const cols = (b) => (b ? [b.ident, b.name, b.country, b.dist_km] : ['', '', '', '']);
  sxCsv('bearings',
    ['sector', 'from_deg', 'to_deg', 'all_ident', 'all_name', 'all_country', 'all_km', 'window_ident', 'window_name', 'window_country', 'window_km',
      'last_hour_ident', 'last_hour_name', 'last_hour_country', 'last_hour_km'],
    SX_POINTS.map((p, k) => [p, (k * w - w / 2 + 360) % 360, k * w + w / 2, ...cols(dx.all[k]), ...cols(dx.win[k]), ...cols(dx.hour[k])]));
}
function sxCsvCountries(rows) {
  sxCsv('countries',
    ['country_code', 'country', 'heard_window', 'heard_all_time', 'new_in_window', 'furthest_ident', 'furthest_km', 'first_ident', 'first_heard_utc'],
    rows.map((r) => [r.cc, r.name, r.win, r.ever, r.fresh, r.far?.ident ?? '', r.far?.dist_km ?? '', r.first?.ident ?? '', r.first ? sxIso(r.first.first_s) : '']));
}

function sxRows() {
  const d = SX.data;
  const rows = d.beacons.map((b) => ({ ...b, heard: true }));
  if (SX.includeUnheard) for (const u of d.unheard) rows.push({ ...u, key: `${u.ident}@${(u.freq_hz / 1e3).toFixed(1)}`, heard: false, avail: 0, minutes: 0 });
  const k = SX.sort.key, dir = SX.sort.asc ? 1 : -1;
  const val = (b) => ({
    ident: b.ident, name: b.name || '~', freq: b.freq_hz, dist: b.dist_km ?? 1e9, bearing: b.bearing_deg ?? 999,
    avail: b.avail, snr: b.heard ? b.snr_mean : -1, best: b.heard ? b.snr_best : -1, last: b.heard ? b.last_s : 0,
  }[k]);
  rows.sort((a, b) => {
    const x = val(a), y = val(b);
    return (x < y ? -1 : x > y ? 1 : (a.dist_km ?? 1e9) - (b.dist_km ?? 1e9)) * dir;
  });
  return rows;
}

const sxFresh = (key) => { const e = state.heard.find((x) => x.key === key); return e && e.first_s >= SX.data.t0; };

function sxRenderTable() {
  const rows = sxRows();
  SX.rows = rows;
  $('sx-unheard-n').textContent = SX.data.unheard.length;
  document.querySelectorAll('#sx-table th[data-sort]').forEach((th) => {
    th.classList.toggle('sorted', th.dataset.sort === SX.sort.key);
    th.classList.toggle('asc', th.dataset.sort === SX.sort.key && SX.sort.asc);
  });
  $('sx-strip-h').textContent = `By hour (${SX.tz === 'UTC' ? 'UTC' : 'local'})`;
  if (!rows.length) {
    $('sx-rows').innerHTML = '<tr><td colspan="10" class="empty">Nothing heard in this window.</td></tr>';
    return;
  }
  $('sx-rows').innerHTML = rows.map((b) => {
    const open = b.key === SX.open;
    const main = `<tr data-key="${esc(b.key)}" class="${b.heard ? 'sx-row' : 'sx-unheard'}${open ? ' sel' : ''}">
      <td class="ident">${esc(b.ident)}${b.heard && sxFresh(b.key) ? '<span class="badge new" title="First heard in this window">NEW</span>' : ''}</td>
      <td class="station">${esc(b.name || '—')}<span class="cc">${flag(b.country)} ${esc(b.country || '')}</span></td>
      <td class="freq">${(b.freq_hz / 1e3).toFixed(1)}</td>
      <td class="num">${b.dist_km != null ? `${b.dist_km} km` : '–'}<span class="brg">${b.bearing_deg != null ? compass(b.bearing_deg) : ''}</span></td>
      <td class="num sx-avail"><span>${b.heard ? sxPct(b.avail) : 'never'}</span><span class="sx-meter"><i style="width:${Math.max(b.avail > 0 ? 2 : 0, b.avail * 100)}%"></i></span></td>
      <td class="num">${b.heard ? sxDur(b.minutes) : '–'}</td>
      <td class="num">${b.heard ? `${b.snr_mean.toFixed(0)} dB` : '–'}</td>
      <td class="num">${b.heard ? `${b.snr_best.toFixed(0)} dB` : '–'}</td>
      <td>${b.heard ? sxStrip(b.strip) : ''}</td>
      <td class="num">${b.heard ? sxWhen(b.last_s) : '–'}</td>
    </tr>`;
    return open && b.heard ? main + `<tr class="sx-detail"><td colspan="10"><div class="sx-detail-head">${esc(b.ident)} · mean SNR per hour while heard</div><div class="sx-chart" id="sx-detail-chart"></div></td></tr>` : main;
  }).join('');
  const open = rows.find((b) => b.key === SX.open && b.heard);
  if (open) {
    const d = SX.data;
    const vals = open.mins.map((m, i) => (m ? open.snrs[i] : null));
    sxLine($('sx-detail-chart'), {
      values: vals, t0: d.t0, yFmt: (v) => `${v}`,
      yMax: Math.max(10, ...vals.filter((v) => v != null)),
      tipFn: (i) => `<div><span class="k"></span><b class="v">${vals[i] != null ? `${vals[i]} dB` : 'not heard'}</b></div><div class="m">${sxWhen(d.t0 + i * 3600)} · heard ${open.mins[i]} of ${d.up[i]} min</div>`,
    });
  }
}

// ── Controls ────────────────────────────────────────────────────────────────

function sxTzButtons() {
  const tz = SX.data?.timezone || state.status?.receiver?.timezone || '';
  const b = document.querySelector('#stats-tz [data-tz="local"]');
  b.disabled = !tz;
  b.textContent = tz ? `Receiver (${tz})` : 'Receiver local (unknown)';
  b.dataset.zone = tz;
  if (!tz && SX.tz !== 'UTC') SX.tz = 'UTC';
  document.querySelectorAll('#stats-tz button').forEach((x) =>
    x.classList.toggle('on', (x.dataset.tz === 'utc') === (SX.tz === 'UTC')));
}

function openStats() {
  $('stats-modal').hidden = false;
  sxTzButtons();
  sxLoad();
}
function closeStats() { $('stats-modal').hidden = true; sxTipOff(); }

$('stats-open').addEventListener('click', openStats);
$('stats-modal').addEventListener('click', (e) => { if (e.target.closest('[data-close]')) closeStats(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('stats-modal').hidden) closeStats(); });
$('stats-range').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-h]');
  if (!b) return;
  SX.hours = Number(b.dataset.h);
  document.querySelectorAll('#stats-range button').forEach((x) => x.classList.toggle('on', x === b));
  sxLoad();
});
$('stats-tz').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-tz]');
  if (!b || b.disabled) return;
  SX.tz = b.dataset.tz === 'utc' ? 'UTC' : b.dataset.zone;
  sxTzButtons();
  sxRender();
});
$('stats-refresh').addEventListener('click', sxLoad);
$('sx-include-unheard').addEventListener('change', (e) => { SX.includeUnheard = e.target.checked; sxRenderTable(); });
document.querySelectorAll('#sx-table th[data-sort]').forEach((th) => th.addEventListener('click', () => {
  const k = th.dataset.sort;
  SX.sort = { key: k, asc: SX.sort.key === k ? !SX.sort.asc : !['avail', 'snr', 'best', 'last'].includes(k) };
  sxRenderTable();
}));
$('sx-rows').addEventListener('click', (e) => {
  const tr = e.target.closest('tr.sx-row');
  if (!tr) return;
  SX.open = SX.open === tr.dataset.key ? null : tr.dataset.key;
  sxRenderTable();
});
$('sx-csv-time').addEventListener('click', () => SX.dv && sxCsvTimeline(SX.dv));
$('sx-csv-hod').addEventListener('click', () => SX.dv && sxCsvHourOfDay(SX.dv));
$('sx-csv-beacons').addEventListener('click', () => SX.rows && sxCsvBeacons(SX.rows));
$('sx-csv-rose').addEventListener('click', () => SX.dx && sxCsvRose(SX.dx));
$('sx-csv-countries').addEventListener('click', () => SX.countries && sxCsvCountries(SX.countries));
let sxResize = null;
addEventListener('resize', () => {
  if ($('stats-modal').hidden) return;
  clearTimeout(sxResize);
  sxResize = setTimeout(sxRender, 150);
});
// #stats opens it straight away (a link to share, or a bookmark).
if (location.hash === '#stats') openStats();

// What timeline.js and export.js share.
Object.assign(window.ndb, { fetchStats: sxFetch, csvSave, isoTime: sxIso });
})();
