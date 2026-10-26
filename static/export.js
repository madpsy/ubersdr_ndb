// export.js — the CSV export: reception history from api/stats, as one row
// per reception (15 min or hourly), per beacon, or per time slot, each
// labelled day, night or greyline from the sun's elevation at the receiver,
// at the beacon and at the path's midpoint at that time.
//
// Uses the helpers app.js, stats.js and timeline.js share as window.ndb.

'use strict';

(() => {
const ndb = window.ndb;
const { $, esc, subsolar, sunElev, lightOf, pathLight } = ndb;

const EX = { data: null, dataKey: '', seq: 0, built: null };

const MODES = {
  slots: { step: 900, label: 'receptions' },
  hourly: { step: 3600, label: 'hourly' },
  summary: { step: 900, label: 'summary' },
  timeline: { step: 900, label: 'timeline' },
};

function opts() {
  const v = (id) => $(id).value;
  return {
    mode: v('ex-mode'),
    hours: Number(v('ex-hours')),
    ref: v('ex-ref'),
    lights: new Set([...document.querySelectorAll('#ex-lights input:checked')].map((x) => x.value)),
    q: v('ex-q').trim().toLowerCase(),
    minKm: v('ex-min') === '' ? null : Number(v('ex-min')),
    maxKm: v('ex-max') === '' ? null : Number(v('ex-max')),
    local: $('ex-local').checked,
    unheard: $('ex-unheard').checked,
    sep: { comma: ',', semicolon: ';', tab: '\t' }[v('ex-sep')],
  };
}

// ── Formatting ──────────────────────────────────────────────────────────────

const iso = ndb.isoTime;
const localFmt = new Map();
function local(t, tz) {
  if (!tz) return '';
  if (!localFmt.has(tz)) {
    localFmt.set(tz, new Intl.DateTimeFormat('sv-SE', { timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hourCycle: 'h23' }));
  }
  return localFmt.get(tz).format(new Date(t * 1000));
}
const f1 = (v) => (v == null ? '' : v.toFixed(1));
const pct = (a, b) => (b ? ((a / b) * 100).toFixed(1) : '');

// ── Rows ────────────────────────────────────────────────────────────────────

// The path's midpoint, where a single-hop skywave path meets the ionosphere.
function midpoint(a, b) {
  const p = ndb.greatCircle(a, b, 2);
  return p.length === 3 ? p[1] : a;
}

function beaconFilter(o) {
  const terms = o.q ? o.q.split(',').map((s) => s.trim()).filter(Boolean) : [];
  return (b) => {
    if (o.minKm != null && !(b.dist_km != null && b.dist_km >= o.minKm)) return false;
    if (o.maxKm != null && !(b.dist_km != null && b.dist_km <= o.maxKm)) return false;
    if (!terms.length) return true;
    const hay = `${b.ident} ${b.name} ${b.country} ${(b.freq_hz / 1e3).toFixed(1)}`.toLowerCase();
    return terms.some((t) => hay.includes(t));
  };
}

// The sun at a beacon, its receiver and their midpoint, for every slot.
function sunFor(d, b, sun) {
  const rx = d.rx_lat != null ? [d.rx_lat, d.rx_lon] : null;
  const mid = rx && b.lat != null ? midpoint(rx, [b.lat, b.lon]) : null;
  return sun.map((ss, i) => {
    const rxE = sun.rx[i];
    const bE = b.lat != null ? sunElev(b.lat, b.lon, ss) : null;
    const mE = mid ? sunElev(mid[0], mid[1], ss) : null;
    const rxL = rxE == null ? '' : lightOf(rxE), bL = bE == null ? '' : lightOf(bE), mL = mE == null ? '' : lightOf(mE);
    return { rxE, bE, mE, rxL, bL, mL, pL: rxL && bL ? pathLight(rxL, bL) : '' };
  });
}
const pick = (s, ref) => ({ path: s.pL, mid: s.mL, rx: s.rxL, ndb: s.bL }[ref] || '');

function build(d, o) {
  const N = d.up.length, step = d.step;
  const tz = d.timezone;
  const sun = Array.from({ length: N }, (_, i) => subsolar(d.t0 + i * step + step / 2));
  sun.rx = sun.map((ss) => (d.rx_lat != null ? sunElev(d.rx_lat, d.rx_lon, ss) : null));
  const keep = beaconFilter(o);
  const beacons = d.beacons.filter(keep);
  const timeCols = (t) => [iso(t), iso(t + step), ...(o.local ? [local(t, tz)] : [])];
  const timeHead = ['start_utc', 'end_utc', ...(o.local ? [`start_local${tz ? ` (${tz})` : ''}`] : [])];
  const where = ['ident', 'name', 'country', 'freq_khz', 'dist_km', 'bearing_deg', 'lat', 'lon'];
  const whereCols = (b) => [b.ident, b.name, b.country, (b.freq_hz / 1e3).toFixed(1), b.dist_km ?? '', b.bearing_deg ?? '', b.lat ?? '', b.lon ?? ''];

  if (o.mode === 'slots' || o.mode === 'hourly') {
    const rows = [];
    for (const b of beacons) {
      const s = sunFor(d, b, sun);
      for (let i = 0; i < N; i++) {
        if (!b.mins[i]) continue;
        const L = pick(s[i], o.ref);
        if (L && !o.lights.has(L)) continue;
        const x = s[i];
        rows.push({ t: d.t0 + i * step, dist: b.dist_km ?? 1e9, r: [
          ...timeCols(d.t0 + i * step), L, ...whereCols(b),
          b.mins[i], d.up[i], pct(b.mins[i], d.up[i]), b.snrs[i],
          f1(x.rxE), x.rxL, f1(x.bE), x.bL, f1(x.mE), x.mL, x.pL] });
      }
    }
    rows.sort((a, b) => a.t - b.t || a.dist - b.dist);
    return {
      header: [...timeHead, 'light', ...where, 'heard_min', 'receiver_up_min', 'heard_pct', 'snr_db',
        'rx_sun_elev_deg', 'rx_light', 'ndb_sun_elev_deg', 'ndb_light', 'mid_sun_elev_deg', 'mid_light', 'path_light'],
      rows: rows.map((x) => x.r),
    };
  }

  if (o.mode === 'summary') {
    const cats = o.ref === 'path' ? ['day', 'greyline', 'night', 'mixed'] : ['day', 'greyline', 'night'];
    const all = beacons.map((b) => ({ ...b, heard: true }));
    if (o.unheard) {
      for (const u of d.unheard.filter(keep)) {
        all.push({ ...u, heard: false, minutes: 0, mins: new Array(N).fill(0), snrs: new Array(N).fill(0) });
      }
    }
    const rows = all.map((b) => {
      const s = sunFor(d, b, sun);
      const c = Object.fromEntries(cats.map((k) => [k, { heard: 0, up: 0, snr: 0 }]));
      let up = 0, heard = 0, snr = 0, first = -1, last = -1;
      for (let i = 0; i < N; i++) {
        if (!d.up[i]) continue;
        const k = c[pick(s[i], o.ref)];
        up += d.up[i];
        if (k) k.up += d.up[i];
        const m = b.mins[i];
        if (!m) continue;
        heard += m; snr += m * b.snrs[i];
        if (k) { k.heard += m; k.snr += m * b.snrs[i]; }
        if (first < 0) first = i;
        last = i;
      }
      const at = (i, end) => (i < 0 ? ['', '', ...(o.local ? [''] : [])]
        : [iso(d.t0 + (i + end) * step), pick(s[i], o.ref), ...(o.local ? [local(d.t0 + (i + end) * step, tz)] : [])]);
      return [
        ...whereCols(b), heard, up, pct(heard, up), heard ? f1(snr / heard) : '', b.heard ? f1(b.snr_best) : '',
        ...cats.flatMap((k) => [c[k].heard, c[k].up, pct(c[k].heard, c[k].up), c[k].heard ? f1(c[k].snr / c[k].heard) : '']),
        ...at(first, 0), ...at(last, 1),
      ];
    });
    rows.sort((a, b) => (Number(a[4]) || 1e9) - (Number(b[4]) || 1e9));
    const when = (w) => [`${w}_heard_utc`, `${w}_heard_light`, ...(o.local ? [`${w}_heard_local`] : [])];
    return {
      header: [...where, 'heard_min', 'receiver_up_min', 'heard_pct', 'snr_mean_db', 'snr_best_db',
        ...cats.flatMap((k) => [`${k}_heard_min`, `${k}_up_min`, `${k}_heard_pct`, `${k}_snr_mean_db`]),
        ...when('first'), ...when('last')],
      rows,
    };
  }

  // timeline: one row per slot, with what was heard in it.
  const rows = [];
  for (let i = 0; i < N; i++) {
    const rxE = sun.rx[i], rxL = rxE == null ? '' : lightOf(rxE);
    if (rxL && !o.lights.has(rxL)) continue;
    const heard = beacons.filter((b) => b.mins[i]).sort((a, b) => (a.dist_km ?? 0) - (b.dist_km ?? 0));
    const far = heard.filter((b) => b.dist_km != null).pop();
    rows.push([...timeCols(d.t0 + i * step), d.up[i], f1(rxE), rxL, d.up[i] ? heard.length : '',
      far ? far.dist_km : '', far ? far.ident : '', heard.map((b) => b.ident).join(' ')]);
  }
  return {
    header: [...timeHead, 'receiver_up_min', 'rx_sun_elev_deg', 'rx_light', 'beacons_heard', 'furthest_km', 'furthest_ident', 'idents'],
    rows,
  };
}

// ── Dialog ──────────────────────────────────────────────────────────────────

async function refresh() {
  const o = opts();
  const step = MODES[o.mode].step;
  const key = `${o.hours}/${step}`;
  // Controls that mean nothing in this mode are dimmed, not hidden.
  $('ex-lights').classList.toggle('off', o.mode === 'summary');
  $('ex-unheard-wrap').classList.toggle('off', o.mode !== 'summary');
  $('ex-ref-wrap').classList.toggle('off', o.mode === 'timeline');
  const seq = ++EX.seq;
  if (key !== EX.dataKey) {
    $('ex-meta').textContent = 'Loading…';
    $('ex-save').disabled = true;
    try {
      const d = await ndb.fetchStats(o.hours, step);
      if (seq !== EX.seq) return;
      EX.data = d;
      EX.dataKey = key;
    } catch (e) {
      if (seq === EX.seq) $('ex-meta').textContent = `Could not load history (${e.message})`;
      return;
    }
  }
  EX.built = build(EX.data, o);
  const { header, rows } = EX.built;
  $('ex-save').disabled = !rows.length;
  $('ex-meta').textContent = `${rows.length.toLocaleString()} row${rows.length === 1 ? '' : 's'} · ${header.length} columns`;
  const show = rows.slice(0, 6);
  $('ex-preview').innerHTML = `<table><thead><tr>${header.map((h) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>${
    show.length ? show.map((r) => `<tr>${r.map((v) => `<td>${esc(v)}</td>`).join('')}</tr>`).join('')
      : `<tr><td colspan="${header.length}" class="empty">Nothing matches.</td></tr>`}${
    rows.length > show.length ? `<tr><td colspan="${header.length}" class="more">… ${(rows.length - show.length).toLocaleString()} more</td></tr>` : ''}</tbody></table>`;
}

function save() {
  if (!EX.built) return;
  const o = opts();
  const ext = o.sep === '\t' ? 'tsv' : 'csv';
  ndb.csvSave(`ndb-${MODES[o.mode].label}-${o.hours}h-${new Date().toISOString().slice(0, 10)}.${ext}`, EX.built.header, EX.built.rows, o.sep);
}

// Remember the choices between visits.
const FIELDS = ['ex-mode', 'ex-hours', 'ex-ref', 'ex-sep', 'ex-local', 'ex-unheard'];
function restore() {
  try {
    const v = JSON.parse(localStorage.getItem('ndb.export') || '{}');
    for (const id of FIELDS) if (id in v) { if ($(id).type === 'checkbox') $(id).checked = v[id]; else $(id).value = v[id]; }
    if (Array.isArray(v.lights)) document.querySelectorAll('#ex-lights input').forEach((x) => { x.checked = v.lights.includes(x.value); });
  } catch (e) { /* storage unavailable */ }
}
function remember() {
  try {
    const v = Object.fromEntries(FIELDS.map((id) => [id, $(id).type === 'checkbox' ? $(id).checked : $(id).value]));
    v.lights = [...document.querySelectorAll('#ex-lights input:checked')].map((x) => x.value);
    localStorage.setItem('ndb.export', JSON.stringify(v));
  } catch (e) { /* storage unavailable */ }
}

function openExport() {
  $('export-modal').hidden = false;
  // Take the main filter box as a starting point if it has something.
  if (!$('ex-q').value && $('search').value) $('ex-q').value = $('search').value;
  EX.dataKey = '';          // fresh data each time it opens
  refresh();
}
const closeExport = () => { $('export-modal').hidden = true; };

restore();
$('export-open').addEventListener('click', openExport);
$('export-modal').addEventListener('click', (e) => { if (e.target.closest('[data-close]')) closeExport(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('export-modal').hidden) closeExport(); });
let t = null;
$('export-form').addEventListener('input', () => { remember(); clearTimeout(t); t = setTimeout(refresh, 200); });
$('export-form').addEventListener('submit', (e) => { e.preventDefault(); save(); });
})();
