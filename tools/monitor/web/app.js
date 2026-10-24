'use strict';
// The compute monitor's page. tools/monitor/monitor.py serves it and its data (docs/compute-nodes.md): the machines
// now, from their two-second samples; their history over any range, from the minute records each machine keeps,
// drawn to the screen's pixel columns (/api/threads); the work done since the recorders started; the jobs; and the
// machines compared, from the benchmark's file when it has run.

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const APOLLO_DEFAULT = 2 * (1 / 11.72e-6) * (8 * 86400 + 3 * 3600 + 18 * 60 + 35);
const CRAY1 = 160e6;     // floating-point operations per second, at its peak
const PEOPLE = 8.2e9;
const MILESTONES = [1, 10, 100, 1e3, 1e4, 1e5, 1e6];
const HUES = [0.30, 0.80, 0.55, 0.12];  // each machine's place on the colour map, in the page's order
const LOCALE = 'en-GB';                 // English words, 24-hour clock, day before month
const FONT = '-apple-system, BlinkMacSystemFont, "SF Pro Text", "Helvetica Neue", Arial, sans-serif';
const REDUCED = matchMedia('(prefers-reduced-motion: reduce)');
const AWAY_AFTER = 120;                 // seconds without input: the owner is away from the Mac (as monitor.py)
const prefs = {
  get(k, d) { try { return localStorage.getItem(`openphysics.compute.${k}`) ?? d; } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem(`openphysics.compute.${k}`, v); } catch (e) { /* private window */ } },
};

// ---------------------------------------------------------------- the house colour map, and the page's tokens
// "studio" (src/colormap.c, CMAP_STUDIO): inferno from 0.14 to 0.90, deep violet to gold, the same nine stops
const STOPS = [0x240c4f, 0x420a68, 0x6a176e, 0x932667, 0xba3655, 0xdd513a, 0xf37819, 0xfca50a, 0xf3cf45]
  .map(h => [h >> 16, (h >> 8) & 255, h & 255]);
function studioAt(x) {
  const f = Math.min(1, Math.max(0, x)) * (STOPS.length - 1), i = Math.min(STOPS.length - 2, Math.floor(f)), u = f - i;
  return [0, 1, 2].map(k => Math.round(STOPS[i][k] + (STOPS[i + 1][k] - STOPS[i][k]) * u));
}
const LUT = Array.from({length: 256}, (_, i) => studioAt(i / 255));
const LUTCSS = LUT.map(c => `rgb(${c[0]},${c[1]},${c[2]})`);
const studio = x => LUT[Math.max(0, Math.min(255, Math.round((x || 0) * 255)))];
const studioCss = x => LUTCSS[Math.max(0, Math.min(255, Math.round((x || 0) * 255)))];
const css = (c, a) => a === undefined ? `rgb(${c[0]},${c[1]},${c[2]})` : `rgba(${c[0]},${c[1]},${c[2]},${a})`;
const TOK = {};
const hex = s => { const h = s.replace('#', '').trim(); return [0, 2, 4].map(i => parseInt(h.slice(i, i + 2), 16)); };
function readTokens() {
  const cs = getComputedStyle(document.documentElement);
  for (const k of ['page', 'card', 'ink', 'dim', 'hair', 'pill', 'accent', 'amber', 'good', 'bad'])
    TOK[k] = hex(cs.getPropertyValue(`--${k}`) || '#000000');
  TOK.dimText = TOK.dim.map((v, i) => Math.round(0.78 * v + 0.22 * TOK.ink[i]));  // as --dim-text in style.css
  const r = document.documentElement.style;
  r.setProperty('--gold', css(studio(0.95)));
  $('#ramp').style.background = `linear-gradient(90deg, ${[0, .25, .5, .75, 1].map(x => studioCss(x)).join(', ')})`;
}

// ---------------------------------------------------------------- numbers and words
const WORDS = [[1e24, 'septillion'], [1e21, 'sextillion'], [1e18, 'quintillion'], [1e15, 'quadrillion'],
  [1e12, 'trillion'], [1e9, 'billion'], [1e6, 'million']];
function fix(x) { return x.toFixed(x < 10 ? 2 : x < 100 ? 1 : 0); }
function words(x) {
  for (const [v, w] of WORDS) if (x >= v) return `${fix(x / v)} ${w}`;
  return Math.round(x).toLocaleString('en-US');
}
function grouped(x) { return BigInt(Math.max(0, Math.round(x))).toLocaleString('en-GB'); }
function si(x, unit) {
  for (const [v, p] of [[1e15, 'P'], [1e12, 'T'], [1e9, 'G'], [1e6, 'M'], [1e3, 'k']])
    if (x >= v) return `${fix(x / v)} ${p}${unit}`;
  return `${Math.round(x)} ${unit}`.trim();
}
const siParts = (x, unit) => { const s = si(x, unit), i = s.lastIndexOf(' '); return i > 0 ? [s.slice(0, i), s.slice(i + 1)] : [s, '']; };
function count(x) {
  if (x < 10) return x.toFixed(2);
  if (x < 1000) return x.toFixed(1);
  return Math.round(x).toLocaleString('en-US');
}
const pct = x => x === null || x === undefined || !isFinite(x) ? '-' : `${Math.round(100 * x)}%`;
function dur(s) {
  s = Math.max(0, s);
  if (s < 90) return `${Math.round(s)} s`;
  if (s < 3600) return `${Math.round(s / 60)} min`;
  if (s < 86400 * 2) { const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60); return m ? `${h} h ${m} min` : `${h} h`; }
  if (s < 86400 * 365 * 2) { const d = Math.floor(s / 86400), h = Math.round((s % 86400) / 3600); return h ? `${d} d ${h} h` : `${d} days`; }
  return `${count(s / (86400 * 365.25))} years`;
}
const D = t => new Date(t * 1000);
const hm = t => D(t).toLocaleTimeString(LOCALE, {hour: '2-digit', minute: '2-digit'});
const hms = t => D(t).toLocaleTimeString(LOCALE, {hour: '2-digit', minute: '2-digit', second: '2-digit'});
const dayLong = t => D(t).toLocaleDateString(LOCALE, {day: 'numeric', month: 'long'});
const dayShort = t => D(t).toLocaleDateString(LOCALE, {weekday: 'short', day: 'numeric', month: 'short'});
const sameDay = (a, b) => D(a).toDateString() === D(b).toDateString();
const when = t => sameDay(t, now()) ? hm(t) : `${dayShort(t)}, ${hm(t)}`;
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
const jobName = id => String(id).replace(/^\d{8}-\d{6}-?/, '') || id;
const now = () => Date.now() / 1000;
const sum = (xs, f) => xs.reduce((a, x) => a + f(x), 0);
const MODE_WORD = {full: 'Full speed', capped: 'Capped', paused: 'Paused', use: 'In use', away: 'Away', batt: 'On battery'};
const modeText = m => m ? `${MODE_WORD[m.mode] || (m.mode[0].toUpperCase() + m.mode.slice(1))}${m.why ? `: ${m.why}` : ''}` : '';
const PRESSURE = {1: 'Normal', 2: 'Warning', 4: 'Critical'};
// the Mac's floating-point operations are estimated so as to fall short: a share of its lab programs' instructions
const estimates = n => !!(n && n.local && n.estimate);
function estWhy(n) {
  const e = n && n.estimate;
  if (!e) return '';
  if (e.source === 'bench') return `Estimate: ${e.factor} operations per instruction of the lab's programs, the ratio the benchmark measured on this Mac. Everything else is left out.`;
  return `Estimate, conservative: ${e.factor} operations per instruction of the lab's programs (the nodes measure 0.78 and 0.41; the M2's 128-bit vectors need up to twice the instructions). Everything else is left out, so the real value is likely higher.`;
}
const estRate = s => s && s.flops_est !== undefined && s.dt ? s.flops_est / s.dt : null;

// ---------------------------------------------------------------- what the page knows
const S = {
  nodes: new Map(), order: [], apollo: APOLLO_DEFAULT, mode: 'live', mirror: null,
  connected: null, everConnected: false,
  range: prefs.get('range', 'day'), offset: 0, stat: prefs.get('stat', 'mean'),
  hist: null, histKey: '', histAt: 0, histSeq: 0, spark: {}, sparkAt: 0,
  day: null, today: null, night: null, bench: null,
  cards: new Map(), shown: {}, cursor: null, jobsOpen: new Set(),
};
if (!['live', 'hour', 'day', 'week', 'month', 'all'].includes(S.range)) S.range = 'day';
if (!['mean', 'peak'].includes(S.stat)) S.stat = 'mean';
const hue = name => studio(HUES[Math.max(0, S.order.indexOf(name)) % HUES.length]);
const live = n => n && n.state === 'online' && n.latest && now() - n.latest.t < 20;
const nodeOf = name => S.nodes.get(name);
const label = name => nodeOf(name)?.label || name;
const keyDot = name => `<span class="key" style="background:${css(hue(name))}" aria-hidden="true"></span>`;
const counts = n => n.counted !== false;
const memTotal = n => n.latest?.mem?.[1] || (n.info?.memory ? n.info.memory / 2 ** 30 : n.memory_gb) || null;

async function get(path) {
  const r = await fetch(path, {cache: 'no-store'});
  if (!r.ok) throw new Error(`${path}: ${r.status}`);
  return r.json();
}

function merge(view) {
  let n = S.nodes.get(view.name);
  if (!n) {
    n = {samples: []};
    S.nodes.set(view.name, n);
    S.order.push(view.name);
  }
  for (const k of ['name', 'label', 'about', 'planned', 'state', 'note', 'since', 'last_line', 'info', 'jobs',
    'totals', 'threads', 'memory_gb', 'local', 'counted', 'showcase', 'activity', 'estimate']) if (k in view) n[k] = view[k];
  const last = n.samples.length ? n.samples[n.samples.length - 1].t : 0;
  for (const s of view.samples || []) if (s.t > last) n.samples.push(s);
  const cut = now() - 3600;
  while (n.samples.length > 60 && n.samples[0].t < cut) n.samples.shift();
  n.latest = n.samples.length ? n.samples[n.samples.length - 1] : null;
  return n;
}

// the operations a node did after a moment, from the two-second samples (what the minute records do not hold yet)
function after(n, t0) {
  const o = {flops: 0, ins: 0, busy: 0, s: 0};
  for (const s of n.samples) if (s.t >= t0) {
    o.flops += s.flops || 0; o.ins += s.ins || 0; o.s += s.dt;
    o.busy += s.cpu.reduce((a, b) => a + b, 0) * s.dt;
  }
  return o;
}

// a smooth line through points (monotone cubic, Fritsch and Carlson): it never overshoots the data
function smooth(ctx, pts, move = true) {
  const n = pts.length;
  if (!n) return;
  if (move) ctx.moveTo(pts[0][0], pts[0][1]); else ctx.lineTo(pts[0][0], pts[0][1]);
  if (n === 1) return;
  if (n === 2) { ctx.lineTo(pts[1][0], pts[1][1]); return; }
  const m = [], t = new Array(n);
  for (let i = 0; i < n - 1; i++) { const dx = pts[i + 1][0] - pts[i][0]; m.push(dx ? (pts[i + 1][1] - pts[i][1]) / dx : 0); }
  t[0] = m[0]; t[n - 1] = m[n - 2];
  for (let i = 1; i < n - 1; i++) t[i] = m[i - 1] * m[i] <= 0 ? 0 : (m[i - 1] + m[i]) / 2;
  for (let i = 0; i < n - 1; i++) {
    if (m[i] === 0) { t[i] = 0; t[i + 1] = 0; continue; }
    const a = t[i] / m[i], b = t[i + 1] / m[i], s = a * a + b * b;
    if (s > 9) { const k = 3 / Math.sqrt(s); t[i] = k * a * m[i]; t[i + 1] = k * b * m[i]; }
  }
  for (let i = 0; i < n - 1; i++) {
    const h = (pts[i + 1][0] - pts[i][0]) / 3;
    ctx.bezierCurveTo(pts[i][0] + h, pts[i][1] + h * t[i], pts[i + 1][0] - h, pts[i + 1][1] - h * t[i + 1], pts[i + 1][0], pts[i + 1][1]);
  }
}
function fade(ctx, c, top, bottom, a0, a1) {
  const g = ctx.createLinearGradient(0, top, 0, bottom);
  g.addColorStop(0, css(c, a0)); g.addColorStop(1, css(c, a1));
  return g;
}

// ---------------------------------------------------------------- the machines, now: a ring of threads each
const STATE_TEXT = {online: 'Live', connecting: 'Connecting', silent: 'Silent', offline: 'Offline', planned: 'Not set up'};
function arc(cx, cy, r, a0, a1) {
  const p = a => [cx + r * Math.cos(a * Math.PI / 180), cy + r * Math.sin(a * Math.PI / 180)];
  const [x0, y0] = p(a0), [x1, y1] = p(a1);
  return `M${x0.toFixed(2)} ${y0.toFixed(2)}A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
function gaugeSVG(n) {
  const T = n.threads || 8, kinds = n.info?.core_kinds && n.info.core_kinds.length === T ? n.info.core_kinds : null;
  const groupGap = kinds ? 9 : 0, gap = T > 1 ? 3.2 : 0, R = 52, sw = 8;
  const groups = kinds ? kinds.filter((k, i) => i === 0 || k !== kinds[i - 1]).length : 1;
  const span = 360 - groupGap * groups;
  let a = -90 + groupGap / 2, h = '';
  const labs = [];
  for (let i = 0; i < T; i++) {
    if (kinds && i && kinds[i] !== kinds[i - 1]) a += groupGap;
    const a0 = a + gap / 2, a1 = a + span / T - gap / 2;
    const d = arc(60, 60, R, a0, a1);
    h += `<path class="bg" d="${d}" stroke-width="${sw}"/><path class="fg" d="${d}" stroke-width="${sw}" pathLength="100" style="stroke-dasharray:0 101"/>`;
    if (kinds && (i === 0 || kinds[i] !== kinds[i - 1])) labs.push([kinds[i], a]);
    a += span / T;
  }
  if (kinds) for (const [k, a0] of labs) {  // E and P beside their groups
    const cnt = kinds.filter(x => x === k).length, mid = (a0 + cnt * span / T / 2) * Math.PI / 180;
    h += `<text class="lab" x="${(60 + 64.5 * Math.cos(mid)).toFixed(1)}" y="${(60 + 64.5 * Math.sin(mid) + 2.5).toFixed(1)}" text-anchor="middle">${k}</text>`;
  }
  h += `<circle class="avg" cx="60" cy="60" r="43.5" stroke-width="1.6"/><path class="avg-fg" d="${arc(60, 60, 43.5, -90, 269.99)}" stroke-width="1.6" pathLength="100" style="stroke-dasharray:0 101"/>`;
  return h;
}

function buildMachines() {
  const host = $('#machine-cards');
  for (const name of S.order) {
    if (S.cards.has(name)) continue;
    const n = nodeOf(name);
    const el = document.createElement('article');
    el.className = 'card machine';
    el.dataset.n = name;
    el.setAttribute('aria-labelledby', `m-${name}`);
    el.innerHTML = `
      <header class="m-head"><h3 class="m-name" id="m-${esc(name)}">${keyDot(name)}${esc(n.label)}</h3>
        <span class="state"><i aria-hidden="true"></i><span class="state-t"></span></span></header>
      <p class="m-about" title="${esc(n.about)}">${esc(n.about)}</p>
      <div class="gauge${n.planned ? ' planned' : ''}"><svg viewBox="0 0 120 120" aria-hidden="true">${gaugeSVG(n)}</svg>
        <div class="centre"><div class="pct">-</div><div class="sub"></div></div></div>
      <div class="m-status"><p class="mode" hidden><i aria-hidden="true"></i><span></span></p><p class="m-note" hidden></p></div>
      <div class="spark" title="Busy share over the last hour"><canvas aria-hidden="true"></canvas><span>last hour</span></div>
      <dl class="m-stats">${[0, 1, 2, 3].map(i => `<div data-i="${i}"><dt></dt><dd>-</dd></div>`).join('')}</dl>
      <div class="m-now"></div>`;
    host.appendChild(el);
    S.cards.set(name, el);
  }
}

function stat(el, i, k, html, opts = {}) {
  const d = $(`div[data-i="${i}"]`, $('.m-stats', el));
  $('dt', d).textContent = k;
  const dd = $('dd', d);
  dd.className = opts.quiet ? 'quiet' : '';
  dd.innerHTML = html;
  d.title = opts.title || '';
}
function meter(frac, colour) {
  return `<div class="meter"><i style="width:${Math.max(0, Math.min(100, 100 * (frac || 0))).toFixed(1)}%;background:${colour}"></i></div>`;
}
function macMode(n) {
  if (n.showcase) return {m: n.showcase.mode, text: modeText(n.showcase), title: `the showcase runner, since ${hm(n.showcase.since)}`};
  const a = n.activity;
  if (!a) return null;
  const p = a.power;
  const power = !p ? '' : p.ac ? 'on power' : `on battery${p.pct !== null && p.pct !== undefined ? ` ${p.pct}%` : ''}`;
  if (p && !p.ac) return {m: 'batt', text: `On battery${p.pct !== null && p.pct !== undefined ? `, ${p.pct}%` : ''}`, title: 'from pmset'};
  if (a.idle === null || a.idle === undefined) return power ? {m: 'use', text: power[0].toUpperCase() + power.slice(1)} : null;
  const away = a.idle >= AWAY_AFTER;
  return {m: away ? 'away' : 'use', text: `${away ? `Away ${dur(a.idle)}` : 'In use'}${power ? ` · ${power}` : ''}`, title: 'time since your last input, and the power source'};
}

function renderMachines() {
  for (const [name, el] of S.cards) {
    const n = nodeOf(name), s = n.latest, on = live(n);
    $('.state', el).dataset.s = n.state;
    $('.state-t', el).textContent = STATE_TEXT[n.state] || n.state;
    // the ring: each thread's share, and the average inside
    const fg = $$('.fg', el), cpu = on ? s.cpu : [];
    fg.forEach((p, i) => {
      const L = cpu[i] || 0;
      p.style.strokeDasharray = `${(L * 100).toFixed(1)} 101`;
      p.style.stroke = studioCss(L);
      p.style.opacity = on ? 1 : 0;
    });
    $('.avg-fg', el).style.strokeDasharray = `${on ? (100 * s.util).toFixed(1) : 0} 101`;
    const pctEl = $('.pct', el), sub = $('.sub', el);
    if (on) {
      pctEl.className = 'pct';
      pctEl.innerHTML = `${Math.round(100 * s.util)}<small>%</small>`;
      const kinds = n.info?.core_kinds;
      if (kinds && kinds.includes('P') && kinds.includes('E')) {
        const part = k => { const idx = kinds.map((x, i) => x === k ? i : -1).filter(i => i >= 0); return idx.reduce((a, i) => a + (s.cpu[i] || 0), 0) / idx.length; };
        sub.textContent = `P ${pct(part('P'))} · E ${pct(part('E'))}`;
      } else sub.textContent = `${s.cpu.length} ${n.local ? 'cores' : 'threads'}`;
    } else {
      pctEl.className = 'pct word';
      pctEl.textContent = n.planned ? 'Waiting' : STATE_TEXT[n.state] || '-';
      sub.textContent = n.last_line ? `last heard ${when(n.last_line)}` : '';
    }
    // the Mac's mode, or why a machine is not live
    const mode = $('.mode', el), mm = n.local ? macMode(n) : null;
    mode.hidden = !mm;
    if (mm) { mode.dataset.m = mm.m; $('span', mode).textContent = mm.text; mode.title = mm.title || ''; }
    const note = $('.m-note', el);
    let why = '';
    if (n.planned) why = 'Waiting to be set up';
    else if (n.state === 'connecting') why = 'Connecting to its recorder…';
    else if (!on) why = `${STATE_TEXT[n.state] || n.state} since ${when(n.since)}`;
    note.hidden = !why;
    note.textContent = why;
    note.title = n.note || '';
    // four numbers
    if (!on) for (let i = 0; i < 4; i++) stat(el, i, ['Operations a second', 'Clock', 'Memory', n.local ? 'Cores' : 'Threads'][i], '-');
    else if (n.local) {
      const tot = memTotal(n) || 8, lvl = s.pressure || null, col = css(lvl === 4 ? TOK.bad : lvl === 2 ? TOK.amber : TOK.good);
      stat(el, 0, 'Memory', `${s.mem[0].toFixed(1)}<small>of ${Math.round(tot)} GB</small>${meter(s.mem[0] / tot, col)}`, {title: 'app memory, wired and compressed, as Activity Monitor counts it'});
      stat(el, 1, 'Pressure', lvl ? `<span class="lvl" data-l="${lvl}">${PRESSURE[lvl] || lvl}</span>${s.free_pct !== undefined ? `<small>${s.free_pct}% free</small>` : ''}` : '-', {title: 'macOS memory pressure (kern.memorystatus_vm_pressure_level)'});
      stat(el, 2, 'Swap', s.swap ? `${s.swap[0].toFixed(1)}<small>of ${s.swap[1].toFixed(0)} GB</small>` : '-', {title: 'swap in use (vm.swapusage)'});
      const er = estRate(s);
      if (estimates(n) && er !== null) { const [v, u] = siParts(er, 'FLOP/s'); stat(el, 3, 'Operations a second', `≈ ${v}<small>${u} est.</small>`, {title: estWhy(n)}); }
      else stat(el, 3, 'Operations a second', 'Not counted on macOS', {quiet: true, title: 'macOS gives ordinary programs no floating-point counters'});
    } else {
      const busy = s.cpu.reduce((a, b) => a + b, 0), tot = memTotal(n) || s.mem[1];
      if (counts(n) && s.flops !== undefined) { const [v, u] = siParts(s.flops / s.dt, 'FLOP/s'); stat(el, 0, 'Operations a second', `${v}<small>${u}</small>`); }
      else stat(el, 0, 'Operations a second', 'Not counted', {quiet: true});
      stat(el, 1, 'Clock', s.cyc !== undefined && busy > 0.05 ? `${(s.cyc / (busy * s.dt) / 1e9).toFixed(2)}<small>GHz</small>` : '-', {title: 'average clock of the busy threads'});
      stat(el, 2, 'Memory', `${s.mem[0].toFixed(1)}<small>of ${Math.round(tot)} GB</small>${meter(s.mem[0] / tot, css(TOK.ink))}`);
      stat(el, 3, 'Threads', `${busy.toFixed(1)}<small>of ${s.cpu.length}</small>`, {title: 'threads busy, added up'});
    }
    const chips = [];
    for (const j of (n.jobs || []).filter(j => j.state === 'running'))
      chips.push(`<span class="chip job" title="${esc(j.cmd || j.id)}"><span>${esc((j.lane ? j.lane + ' · ' : '') + jobName(j.id))} · ${dur(now() - (j.started || now()))}</span></span>`);
    if (on) for (const [comm, cores] of (s.top || []).slice(0, 3)) chips.push(`<span class="chip" title="cores in use"><span>${esc(comm)} ×${cores.toFixed(1)}</span></span>`);
    $('.m-now', el).innerHTML = chips.length ? chips.join('') : (on ? 'Nothing running' : '');
  }
  drawSparks();
}

// the last hour of each machine, a small smooth area
async function loadSparks() {
  const t1 = now(), t0 = t1 - 3600;
  await Promise.all(S.order.filter(k => !nodeOf(k).planned).map(k => get(`api/threads?node=${encodeURIComponent(k)}&t0=${t0.toFixed(0)}&t1=${t1.toFixed(0)}&bins=72`)
    .then(d => { S.spark[k] = d; }).catch(() => {})));
  S.sparkAt = now();
  drawSparks();
}
function drawSparks() {
  for (const [name, el] of S.cards) {
    const d = S.spark[name], cv = $('.spark canvas', el);
    const w = cv.clientWidth;
    if (!w) continue;
    const dpr = Math.min(2, window.devicePixelRatio || 1), H = 40;
    if (cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = H * dpr; }
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, H);
    ctx.strokeStyle = css(TOK.hair); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, H - 0.5); ctx.lineTo(w, H - 0.5); ctx.stroke();
    if (!d) continue;
    const c = hue(name), X = k => (k + 0.5) / d.bins * w, Y = u => 3 + (1 - u) * (H - 5);
    runs(d.bins, k => d.busy[k] !== null && d.cover[k] > 0, (k0, k1) => {
      const pts = [];
      for (let k = k0; k <= k1; k++) pts.push([X(k), Y(d.busy[k])]);
      if (pts.length === 1) pts.push([pts[0][0] + 2, pts[0][1]]);
      ctx.beginPath(); smooth(ctx, pts); ctx.lineTo(pts[pts.length - 1][0], H); ctx.lineTo(pts[0][0], H); ctx.closePath();
      ctx.fillStyle = fade(ctx, c, 0, H, 0.30, 0.03); ctx.fill();
      ctx.beginPath(); smooth(ctx, pts);
      ctx.strokeStyle = css(c); ctx.lineWidth = 1.5; ctx.lineJoin = 'round'; ctx.stroke();
    });
  }
}

// ---------------------------------------------------------------- now, in one number
function renderHero() {
  const nodes = S.order.map(nodeOf);
  const on = nodes.filter(live), planned = nodes.filter(n => n.planned);
  const threads = sum(on, n => n.latest.cpu.length);
  const busy = sum(on, n => n.latest.cpu.reduce((x, y) => x + y, 0));
  const cnt = on.filter(n => counts(n) && n.latest.flops !== undefined);
  const rate = sum(cnt, n => n.latest.flops / n.latest.dt);
  const running = sum(nodes, n => (n.jobs || []).filter(j => j.state === 'running').length);
  const u = threads ? busy / threads : 0;
  let phrase;
  if (S.connected === false) phrase = S.lastOk ? `busy when last heard, ${hm(S.lastOk)}` : 'the monitor is not answering';
  else if (!on.length) phrase = nodes.some(n => !n.planned) ? 'no machine is answering' : 'waiting for the first machine';
  else if (on.every(n => n.latest.util >= 0.85)) phrase = on.length > 1 ? 'busy · every machine at full power' : 'busy · at full power';
  else if (u >= 0.12) phrase = `busy · room for ${Math.round(threads - busy)} more thread${Math.round(threads - busy) === 1 ? '' : 's'}`;
  else phrase = 'busy · the machines are resting';
  $('#big').innerHTML = on.length ? `${Math.round(100 * u)}<small>%</small>` : '-';
  $('#big-k').textContent = phrase;
  const away = nodes.filter(n => !n.planned && !live(n));
  $('#eyebrow').textContent = on.length ? `Now · ${on.length} of ${nodes.length - planned.length} machines live${away.length ? ` · ${away.map(n => n.label).join(', ')} not answering` : ''}` : 'Now';
  $('#fig-thr').innerHTML = on.length ? `${busy.toFixed(1)}<small>of ${threads}</small>` : '-';
  const [v, unit] = cnt.length ? siParts(rate, 'FLOP/s') : ['-', ''];
  $('#fig-flops').innerHTML = cnt.length ? `${v}<small>${esc(unit)}</small>` : '-';
  const est = on.filter(estimates).reduce((a, n) => a + (estRate(n.latest) || 0), 0), mac = on.find(estimates);
  $('#fig-flops-k').textContent = mac ? `nodes · Mac ≈ ${si(est, '').replace(/ $/, '')} est.` : on.some(n => !counts(n)) ? 'Operations a second, nodes' : 'Operations a second';
  $('#fig-flops-k').title = mac ? `Counted by the nodes' own processors. ${estWhy(mac)}` : '';
  $('#fig-jobs').textContent = String(running);
  document.title = on.length ? `${Math.round(100 * u)}% busy · Compute` : 'Compute · OpenPhysicsAI';
}

function renderStatus() {
  const st = $('#status'), anyLive = S.order.some(k => live(nodeOf(k)));
  document.body.classList.toggle('stale', S.connected === false && S.everConnected);  // the last numbers, dimmed
  st.dataset.s = S.connected === false ? 'down' : S.connected === null ? 'wait' : anyLive ? 'live' : 'quiet';
  $('#status-text').textContent = S.connected === false ? 'Not answering' : S.connected === null ? 'Connecting' : anyLive ? 'Live' : 'No machine live';
  $('#clock').textContent = hm(now());
  const nt = $('#notice');
  if (S.connected === false) {
    nt.dataset.kind = 'down';
    nt.innerHTML = S.everConnected ? '<b>The monitor is not answering.</b>Retrying every two seconds; the last numbers stay.'
      : '<b>The monitor is not running.</b>Start it: python3 tools/monitor/monitor.py serve';
    nt.hidden = false;
  } else if (S.mode === 'read-only') {
    nt.dataset.kind = 'copy';
    nt.innerHTML = `<b>Preview copy, read-only.</b>${S.mirror ? 'Live lines from the main monitor.' : 'Recorded history only.'}`;
    nt.hidden = false;
  } else nt.hidden = true;
}

// ---------------------------------------------------------------- the range: one control for every chart
const RANGES = {
  live: {span: 300, title: 'Last 5 minutes · live', every: 0},
  hour: {span: 3600, title: 'Last hour', every: 10},
  day: {span: 86400, title: 'Last 24 hours', every: 60},
  week: {span: 7 * 86400, title: 'Last 7 days', every: 300},
  month: {span: 30 * 86400, title: 'Last 30 days', every: 600},
  all: {span: 0, title: '', every: 600},
};
function firstRecord() {
  const ts = S.order.map(nodeOf).filter(n => !n.planned).map(n => n.totals?.since).filter(t => t > 0);
  return ts.length ? Math.min(...ts) : null;
}
function windowOf() {
  const R = RANGES[S.range], t = now();
  if (S.range === 'all') {
    const f = firstRecord();
    return {t0: Math.min(f ?? t - 3600, t - 600), t1: t};
  }
  const t1 = t - S.offset * R.span;
  return {t0: t1 - R.span, t1};
}
function spanText(t0, t1) {
  if (sameDay(t0, t1)) return `${dayShort(t0)}, ${hm(t0)} to ${hm(t1)}`;
  return `${dayShort(t0)}, ${hm(t0)} to ${dayShort(t1)}, ${hm(t1)}`;
}
function renderWindow() {
  const R = RANGES[S.range], {t0, t1} = windowOf();
  $('#window-title').textContent = S.range === 'all' ? `Since ${dayLong(t0)}, ${hm(t0)}` : S.offset === 0 ? R.title : spanText(t0, t1);
  const pageable = S.range !== 'live' && S.range !== 'all';
  const f = firstRecord();
  $('#prev').disabled = !pageable || (f !== null && t0 <= f);
  $('#next').disabled = !pageable || S.offset === 0;
  $('#latest').hidden = S.offset === 0;
  $('#prev').setAttribute('aria-label', pageable ? `Earlier ${R.title.replace(/^Last /, '')}` : 'Earlier');
  $('#next').setAttribute('aria-label', pageable ? `Later ${R.title.replace(/^Last /, '')}` : 'Later');
}

// a segmented control: buttons with role radio, the arrow keys move the choice, a black thumb slides under it
function segmented(el, value, onChange) {
  const btns = $$('button', el);
  let thumb = $('.thumb', el);
  if (!thumb) { thumb = document.createElement('span'); thumb.className = 'thumb'; thumb.setAttribute('aria-hidden', 'true'); el.prepend(thumb); }
  const place = () => {
    const b = btns.find(x => x.getAttribute('aria-checked') === 'true');
    if (!b) return;
    thumb.style.width = `${b.offsetWidth}px`;
    thumb.style.transform = `translateX(${b.offsetLeft}px)`;
  };
  const choose = (v, focus) => {
    for (const b of btns) {
      const on = b.dataset.v === v;
      b.setAttribute('aria-checked', String(on));
      b.tabIndex = on ? 0 : -1;
      if (on && focus) b.focus();
    }
    place();
  };
  choose(value);
  el.addEventListener('click', ev => {
    const b = ev.target.closest('button');
    if (!b || b.getAttribute('aria-checked') === 'true') return;
    choose(b.dataset.v);
    onChange(b.dataset.v);
  });
  el.addEventListener('keydown', ev => {
    const i = btns.findIndex(b => b.getAttribute('aria-checked') === 'true');
    let j = null;
    if (ev.key === 'ArrowRight' || ev.key === 'ArrowDown') j = Math.min(btns.length - 1, i + 1);
    else if (ev.key === 'ArrowLeft' || ev.key === 'ArrowUp') j = Math.max(0, i - 1);
    else if (ev.key === 'Home') j = 0;
    else if (ev.key === 'End') j = btns.length - 1;
    if (j === null || j === i) return;
    ev.preventDefault();
    choose(btns[j].dataset.v, true);
    onChange(btns[j].dataset.v);
  });
  new ResizeObserver(place).observe(el);
  return {choose, place};
}

// ---------------------------------------------------------------- the history: data
const histNames = () => S.hist ? S.order.filter(k => S.hist.nodes[k]) : [];

// the two-second samples the page holds, in columns, in the form /api/threads gives (for the live range)
function binSamples(n, t0, t1, bins) {
  const w = (t1 - t0) / bins, T = n.threads || n.latest?.cpu.length || 8;
  const z = () => new Array(bins).fill(0), nul = () => new Array(bins).fill(null);
  const sec = z(), cap = z(), busy = z(), bmax = nul(), fl = z(), flsec = z(), flmax = nul(), mem = z(), memsec = z();
  const run = nul(), ptsec = z(), tm = nul(), tp = nul(), tops = nul(), swap = z(), swsec = z(), pres = nul(), away = z(), batt = z();
  for (const s of n.samples) {
    const a = s.t - s.dt, b = s.t;
    if (b <= t0 || a >= t1) continue;
    const k0 = Math.max(0, Math.floor((a - t0) / w)), k1 = Math.min(bins - 1, Math.ceil((b - t0) / w) - 1);
    for (let k = k0; k <= k1; k++) {
      const f = (Math.min(b, t0 + (k + 1) * w) - Math.max(a, t0 + k * w)) / (b - a);
      if (f <= 0) continue;
      const ds = s.dt * f, cpu = s.cpu;
      sec[k] += ds; cap[k] += cpu.length * ds; busy[k] += cpu.reduce((x, y) => x + y, 0) * ds;
      bmax[k] = Math.max(bmax[k] ?? 0, s.util);
      if (counts(n) && s.flops !== undefined) { fl[k] += s.flops * f; flsec[k] += ds; flmax[k] = Math.max(flmax[k] ?? 0, s.flops / s.dt); }
      mem[k] += s.mem[0] * ds; memsec[k] += ds;
      run[k] = Math.max(run[k] ?? 0, s.run || 0);
      ptsec[k] += ds;
      tm[k] = tm[k] || new Array(cpu.length).fill(0); tp[k] = tp[k] || new Array(cpu.length).fill(0);
      cpu.forEach((u, i) => { tm[k][i] += u * ds; tp[k][i] = Math.max(tp[k][i], u); });
      if (s.top && s.top.length) { tops[k] = tops[k] || {}; for (const [nm, c] of s.top) tops[k][nm] = (tops[k][nm] || 0) + c * ds; }
      if (s.swap) {
        swap[k] += s.swap[0] * ds; swsec[k] += ds; pres[k] = Math.max(pres[k] ?? 0, s.pressure || 0);
        if ((s.idle || 0) >= AWAY_AFTER) away[k] += ds;
        if (s.power && s.power.ac === false) batt[k] += ds;
      }
    }
  }
  const out = {bins, width: w, threads: T, kinds: n.info?.core_kinds || null, counted: counts(n),
    cover: sec.map(x => Math.min(1, x / w)), busy: busy.map((b, k) => cap[k] > 0 ? b / cap[k] : null), busy_max: bmax,
    flops: fl.map((x, k) => flsec[k] > 0 ? x / flsec[k] : null), flops_max: flmax,
    mem: mem.map((x, k) => memsec[k] > 0 ? x / memsec[k] : null), run, pt: ptsec.map(x => Math.min(1, x / w)),
    mean: tm.map((m, k) => m ? m.map(v => v / ptsec[k]) : null), peak: tp,
    top: tops.map((d, k) => d ? Object.entries(d).sort((a, b) => b[1] - a[1]).slice(0, 3).map(([nm, v]) => [nm, v / sec[k]]).filter(e => e[1] >= 0.05) : null)};
  if (swsec.some(x => x > 0)) {
    out.swap = swap.map((x, k) => swsec[k] > 0 ? x / swsec[k] : null);
    out.pressure = pres;
    out.act = swsec.map((x, k) => x <= 0 ? null : batt[k] >= 0.5 * x ? 'batt' : away[k] >= 0.5 * x ? 'away' : 'use');
  }
  if (n.showcase) out.modes = [[Math.max(t0, n.showcase.since), t1, n.showcase.mode, n.showcase.why]];
  return out;
}

function plotWidth() {
  const cv = $('#c-busy');
  const w = cv.clientWidth || $('#charts').clientWidth - 44;
  const g = geo(w);
  return Math.max(60, Math.round(w - g.gl - g.gr));
}

async function loadHistory(force) {
  const {t0, t1} = windowOf();
  const names = S.order.filter(k => !nodeOf(k).planned);
  if (S.range === 'live') {
    const bins = Math.round((t1 - t0) / 2);
    S.hist = {t0, t1, live: true, nodes: Object.fromEntries(names.map(k => [k, binSamples(nodeOf(k), t0, t1, bins)]))};
    S.histKey = 'live';
    drawHistory();
    return;
  }
  const bins = plotWidth();
  const key = `${S.range}:${S.offset}:${bins}`;
  if (!force && key === S.histKey && now() - S.histAt < RANGES[S.range].every) return;
  const seq = ++S.histSeq;
  $('#charts').classList.add('loading');
  const res = await Promise.all(names.map(k => get(`api/threads?node=${encodeURIComponent(k)}&t0=${t0.toFixed(0)}&t1=${t1.toFixed(0)}&bins=${bins}`)
    .then(d => [k, d]).catch(() => [k, null])));
  if (seq !== S.histSeq) return;  // a newer range was chosen meanwhile
  $('#charts').classList.remove('loading');
  if (res.every(([, d]) => !d)) { if (!S.hist) drawHistory(); return; }
  S.hist = {t0: +t0.toFixed(0), t1: +t1.toFixed(0), live: false, nodes: Object.fromEntries(res.filter(([, d]) => d))};
  S.histKey = key;
  S.histAt = now();
  drawHistory();
}

// ---------------------------------------------------------------- the history: drawing
function geo(w) { return w < 560 ? {gl: 36, gr: 4, axis: 20} : {gl: 44, gr: 6, axis: 22}; }
function setup(cv, cssH) {
  const dpr = Math.min(2, window.devicePixelRatio || 1), w = Math.max(60, cv.clientWidth || 600);
  cv.style.height = `${cssH}px`;
  const W = Math.round(w * dpr), H = Math.round(cssH * dpr);
  if (cv.width !== W || cv.height !== H) { cv.width = W; cv.height = H; }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, cssH);
  return {ctx, w};
}
const xOf = (h, w, g) => t => g.gl + (t - h.t0) / (h.t1 - h.t0) * (w - g.gl - g.gr);
const STEPS = [10, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 2 * 86400, 7 * 86400, 14 * 86400, 28 * 86400];
function ticks(t0, t1, pw) {
  const minPx = pw < 480 ? 66 : 88;
  const step = STEPS.find(s => s / (t1 - t0) * pw >= minPx) || STEPS[STEPS.length - 1];
  const out = [];
  if (step < 86400) {
    const d = D(t0);
    const into = d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds();
    d.setHours(0, 0, 0, 0);
    for (let t = d / 1000 + Math.ceil(into / step) * step; t <= t1; t += step) if (t >= t0) out.push(t);
  } else {
    const d = D(t0);
    d.setHours(0, 0, 0, 0);
    if (d / 1000 < t0) d.setDate(d.getDate() + 1);
    for (let i = 0; d / 1000 <= t1 && i < 400; i++, d.setDate(d.getDate() + step / 86400)) out.push(d / 1000);
  }
  return {step, ticks: out};
}
function tickText(t, step) {
  const d = D(t);
  if (step < 60) return hms(t);
  if (step < 86400) return d.getHours() === 0 && d.getMinutes() === 0 ? d.toLocaleDateString(LOCALE, {weekday: 'short', day: 'numeric'}) : hm(t);
  if (step < 7 * 86400) return d.toLocaleDateString(LOCALE, {weekday: 'short', day: 'numeric'});
  return d.toLocaleDateString(LOCALE, {day: 'numeric', month: 'short'});
}
function xAxis(ctx, h, X, w, g, y, labels = true) {
  const {step, ticks: ts} = ticks(h.t0, h.t1, w - g.gl - g.gr);
  ctx.font = `11px ${FONT}`;
  ctx.fillStyle = css(TOK.dimText);
  ctx.strokeStyle = css(TOK.pill);
  ctx.lineWidth = 1;
  ctx.textAlign = 'center';
  for (const t of ts) {
    const x = Math.round(X(t)) + 0.5;
    ctx.beginPath(); ctx.moveTo(x, y - 3); ctx.lineTo(x, y); ctx.stroke();
    if (!labels) continue;
    const s = tickText(t, step), half = ctx.measureText(s).width / 2;
    ctx.fillText(s, Math.min(w - g.gr - half, Math.max(g.gl + half, x)), y + 14);
  }
  ctx.textAlign = 'left';
}
function yGrid(ctx, w, g, levels, Y, fmt) {
  ctx.font = `11px ${FONT}`;
  ctx.textAlign = 'right';
  for (const v of levels) {
    const y = Math.round(Y(v)) + 0.5;
    ctx.strokeStyle = css(v === levels[0] ? TOK.pill : TOK.hair);
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(g.gl, y); ctx.lineTo(w - g.gr, y); ctx.stroke();
    const s = fmt(v);
    if (s) { ctx.fillStyle = css(TOK.dimText); ctx.fillText(s, g.gl - 7, y + 4); }
  }
  ctx.textAlign = 'left';
}
function nights(ctx, h, X, top, bottom) {
  if (h.t1 - h.t0 < 1800 || h.t1 - h.t0 > 8 * 86400) return;
  const d = D(h.t0);
  d.setHours(22, 0, 0, 0); d.setDate(d.getDate() - 1);
  for (; d / 1000 < h.t1; d.setDate(d.getDate() + 1)) {
    const a = d / 1000, b = a + 9 * 3600;
    if (b < h.t0) continue;
    const x0 = X(Math.max(a, h.t0)), x1 = X(Math.min(b, h.t1));
    ctx.fillStyle = css(TOK.page, 0.7);
    ctx.fillRect(x0, top, x1 - x0, bottom - top);
  }
}
function niceMax(v) {
  if (!(v > 0)) return 1e9;
  const e = Math.pow(10, Math.floor(Math.log10(v)));
  return [1, 2, 2.5, 5, 10].map(m => m * e).find(m => m >= v * 1.08) || 10 * e;
}
function runs(n, ok, each) {  // each stretch of columns that have a value
  let k = 0;
  while (k < n) {
    while (k < n && !ok(k)) k++;
    const k0 = k;
    while (k < n && ok(k)) k++;
    if (k > k0) each(k0, k - 1);
  }
}
function emptyNote(ctx, w, g, top, bottom, text) {
  ctx.fillStyle = css(TOK.dimText);
  ctx.font = `13px ${FONT}`;
  ctx.textAlign = 'center';
  ctx.fillText(text, g.gl + (w - g.gl - g.gr) / 2, (top + bottom) / 2 + 4);
  ctx.textAlign = 'left';
}
function nothingText() {
  const f = firstRecord(), {t1} = windowOf();
  return f && t1 <= f ? `Recording began ${dayLong(f)}, ${hm(f)}` : 'Nothing recorded';
}
// one machine's line: a smooth curve through its columns, with a soft fill under it
function areaLine(ctx, d, X, Y, base, c, vals, fill, width) {
  let any = false;
  runs(d.bins, k => vals[k] !== null && vals[k] !== undefined && d.cover[k] > 0, (k0, k1) => {
    any = true;
    const pts = [];
    for (let k = k0; k <= k1; k++) pts.push([X(k), Y(vals[k])]);
    if (pts.length === 1) { pts[0][0] -= 1.5; pts.push([pts[0][0] + 3, pts[0][1]]); }
    if (fill) {
      ctx.beginPath(); smooth(ctx, pts); ctx.lineTo(pts[pts.length - 1][0], base); ctx.lineTo(pts[0][0], base); ctx.closePath();
      ctx.fillStyle = fill; ctx.fill();
    }
    ctx.beginPath(); smooth(ctx, pts);
    ctx.strokeStyle = css(c); ctx.lineWidth = width; ctx.lineJoin = 'round'; ctx.lineCap = 'round'; ctx.stroke();
  });
  return any;
}

function drawBusy() {
  const h = S.hist, cv = $('#c-busy');
  const narrow = cv.clientWidth < 560, H = narrow ? 140 : 176;
  const {ctx, w} = setup(cv, H);
  const g = geo(w), top = 8, bottom = H - g.axis, X = xOf(h, w, g);
  const Y = u => top + (1 - u) * (bottom - top);
  nights(ctx, h, X, top, bottom);
  yGrid(ctx, w, g, [0, 0.25, 0.5, 0.75, 1], Y, v => v === 0.25 || v === 0.75 ? '' : `${v * 100}%`);
  xAxis(ctx, h, X, w, g, bottom);
  let any = false;
  for (const name of histNames()) {
    const d = h.nodes[name], c = hue(name), xc = k => X(h.t0 + (k + 0.5) * d.width);
    const vals = S.stat === 'peak' ? d.busy_max.map((v, k) => v ?? d.busy[k]) : d.busy;
    any = areaLine(ctx, d, xc, Y, bottom, c, vals, fade(ctx, c, top, bottom, 0.13, 0.0), narrow ? 1.6 : 1.8) || any;
  }
  if (!any) emptyNote(ctx, w, g, top, bottom, nothingText());
}

function drawFlops() {
  const h = S.hist, cv = $('#c-flops');
  const narrow = cv.clientWidth < 560, H = narrow ? 110 : 128;
  const {ctx, w} = setup(cv, H);
  const g = geo(w), top = 8, bottom = H - g.axis, X = xOf(h, w, g);
  const names = histNames().filter(k => h.nodes[k].counted || h.nodes[k].flops_est);  // the measured first, the estimate on top
  names.sort((a, b) => (h.nodes[a].counted ? 0 : 1) - (h.nodes[b].counted ? 0 : 1));
  const n = Math.max(0, ...names.map(k => h.nodes[k].bins));
  const width = names.length ? h.nodes[names[0]].width : 1;
  const keyOf = name => h.nodes[name].counted ? (S.stat === 'peak' ? 'flops_max' : 'flops') : (S.stat === 'peak' ? 'flops_est_max' : 'flops_est');
  let peak = 0;
  const stack = [];
  for (let k = 0; k < n; k++) {
    let acc = 0;
    stack.push(names.map(name => { const v = (h.nodes[name][keyOf(name)] || [])[k]; const lo = acc; acc += v || 0; return v === null || v === undefined ? null : [lo, acc]; }));
    peak = Math.max(peak, acc);
  }
  const ymax = niceMax(peak), Y = v => top + (1 - v / ymax) * (bottom - top);
  nights(ctx, h, X, top, bottom);
  yGrid(ctx, w, g, [0, ymax / 2, ymax], Y, v => v ? (narrow ? si(v, '').replace(' ', '') : si(v, '')) : '0');
  xAxis(ctx, h, X, w, g, bottom);
  let any = false;
  names.forEach((name, j) => {
    const c = hue(name), xc = k => X(h.t0 + (k + 0.5) * width);
    runs(n, k => stack[k][j] !== null, (k0, k1) => {
      any = true;
      const upper = [], lower = [];
      for (let k = k0; k <= k1; k++) { upper.push([xc(k), Y(stack[k][j][1])]); lower.push([xc(k), Y(stack[k][j][0])]); }
      if (upper.length === 1) { upper.push([upper[0][0] + 3, upper[0][1]]); lower.push([lower[0][0] + 3, lower[0][1]]); }
      ctx.beginPath(); smooth(ctx, upper); smooth(ctx, lower.reverse(), false); ctx.closePath();
      if (h.nodes[name].counted) { ctx.fillStyle = fade(ctx, c, top, bottom, 0.78, 0.42); ctx.fill(); }
      else { ctx.fillStyle = fade(ctx, c, top, bottom, 0.34, 0.16); ctx.fill(); ctx.fillStyle = hatch(ctx); ctx.fill(); }  // an estimate
    });
  });
  const estN = names.filter(k => !h.nodes[k].counted).map(label), off = histNames().filter(k => !h.nodes[k].counted && !h.nodes[k].flops_est).map(label);
  $('#flops-k').textContent = `per second${estN.length ? ` · ${estN.join(', ')} estimated (hatched)` : ''}${off.length ? ` · ${off.join(', ')} not counted` : ''}`;
  $('#flops-k').title = estN.length ? estWhy(S.order.map(nodeOf).find(estimates)) : '';
  if (!any) emptyNote(ctx, w, g, top, bottom, names.length ? nothingText() : 'Not counted on these machines');
}

function drawMem() {
  const h = S.hist, cv = $('#c-mem');
  const narrow = cv.clientWidth < 560;
  const mac = histNames().find(k => h.nodes[k].pressure);
  const band = mac ? 9 : 0;
  const H = (narrow ? 96 : 112) + band;
  const {ctx, w} = setup(cv, H);
  const g = geo(w), top = 8, bottom = H - g.axis - band, X = xOf(h, w, g);
  const Y = u => top + (1 - Math.min(1, u)) * (bottom - top);
  nights(ctx, h, X, top, bottom);
  yGrid(ctx, w, g, [0, 0.5, 1], Y, v => `${v * 100}%`);
  xAxis(ctx, h, X, w, g, H - g.axis);
  let any = false;
  for (const name of histNames()) {
    const d = h.nodes[name], n = nodeOf(name), tot = memTotal(n), c = hue(name);
    if (!tot) continue;
    const xc = k => X(h.t0 + (k + 0.5) * d.width);
    any = areaLine(ctx, d, xc, Y, bottom, c, d.mem.map(v => v === null ? null : v / tot), null, narrow ? 1.5 : 1.7) || any;
  }
  if (mac) {  // the MacBook's memory pressure, a thin band: normal, warning, critical
    const d = h.nodes[mac], y = bottom + 4;
    for (let k = 0; k < d.bins; k++) {
      const p = d.pressure[k];
      if (!p) continue;
      const x0 = X(h.t0 + k * d.width), x1 = X(h.t0 + (k + 1) * d.width);
      ctx.fillStyle = css(p >= 4 ? TOK.bad : p >= 2 ? TOK.amber : TOK.good, 0.85);
      ctx.fillRect(x0, y, Math.max(1, x1 - x0) + 0.3, 4);
    }
  }
  if (!any) emptyNote(ctx, w, g, top, bottom, nothingText());
}

let HATCH = null;
function hatch(ctx) {
  if (HATCH) return HATCH;
  const c = document.createElement('canvas');
  c.width = c.height = 8;
  const x = c.getContext('2d');
  x.strokeStyle = css(TOK.card, 0.32); x.lineWidth = 1.5;
  x.beginPath(); x.moveTo(-2, 10); x.lineTo(10, -2); x.moveTo(6, 10); x.lineTo(10, 6); x.moveTo(-2, 2); x.lineTo(2, -2); x.stroke();
  HATCH = ctx.createPattern(c, 'repeat');
  return HATCH;
}
const BAND = () => ({full: TOK.ink, away: TOK.ink, capped: TOK.dim, use: TOK.dim, paused: TOK.pill, batt: TOK.pill});

function threadRows(d, narrow) {
  const T = d.threads || (d.mean.find(Boolean) || []).length || 8;
  const rowH = narrow ? 6 : 8, gap = 1, kinds = d.kinds && d.kinds.length === T ? d.kinds : null;
  const ys = [];
  let y = 0;
  for (let i = 0; i < T; i++) {
    if (i && kinds && kinds[i] !== kinds[i - 1]) y += 3;  // a little air between efficiency and performance cores
    ys.push(y);
    y += rowH + gap;
  }
  return {T, rowH, ys, height: y - gap, kinds};
}

function drawThreads() {
  const h = S.hist, host = $('#thread-plots');
  const names = histNames();
  for (const el of $$('.tp', host)) if (!names.includes(el.dataset.n)) el.remove();
  let bandKind = null;
  names.forEach((name, idx) => {
    let el = $(`.tp[data-n="${name}"]`, host);
    if (!el) {
      el = document.createElement('div');
      el.className = 'tp'; el.dataset.n = name;
      el.innerHTML = `<div class="tp-head"><span class="name">${keyDot(name)}${esc(label(name))}</span></div>
        <canvas class="plot" tabindex="0" role="img" data-n="${esc(name)}"></canvas>`;
      host.appendChild(el);
    }
    if (el !== host.children[idx]) host.insertBefore(el, host.children[idx]);
    const d = h.nodes[name], cv = $('canvas', el), narrow = cv.clientWidth < 560;
    const R = threadRows(d, narrow), last = idx === names.length - 1;
    const modes = d.modes && d.modes.length ? d.modes : null, act = !modes && d.act ? d.act : null;
    const band = modes || act ? 9 : 0, axis = last ? geo(cv.clientWidth).axis : 5;
    const H = R.height + band + axis;
    const {ctx, w} = setup(cv, H);
    const g = geo(w), X = xOf(h, w, g);
    ctx.fillStyle = css(TOK.page);
    ctx.fillRect(g.gl, 0, w - g.gl - g.gr, R.height);
    const hatched = [];
    let anyRec = false;
    for (let k = 0; k < d.bins; k++) {
      if (!(d.cover[k] > 0)) continue;
      anyRec = true;
      const x0 = X(h.t0 + k * d.width), x1 = X(h.t0 + (k + 1) * d.width), cw = Math.max(1, x1 - x0) + 0.4;
      const arr = S.stat === 'peak' ? d.peak[k] : d.mean[k];
      if (arr && d.pt[k] > 0) {
        for (let i = 0; i < R.T; i++) { ctx.fillStyle = studioCss(arr[i]); ctx.fillRect(x0, R.ys[i], cw, R.rowH); }
      } else if (d.busy[k] !== null) {
        ctx.fillStyle = studioCss(S.stat === 'peak' ? (d.busy_max[k] ?? d.busy[k]) : d.busy[k]);
        ctx.fillRect(x0, 0, cw, R.height);
        hatched.push([x0, cw]);
      }
    }
    if (hatched.length) {
      ctx.save();
      ctx.beginPath();
      for (const [x0, cw] of hatched) ctx.rect(x0, 0, cw, R.height);
      ctx.clip();
      ctx.fillStyle = hatch(ctx);
      ctx.fillRect(g.gl, 0, w - g.gl - g.gr, R.height);
      ctx.restore();
    }
    ctx.font = `600 9px ${FONT}`;
    ctx.fillStyle = css(TOK.dimText);
    ctx.textAlign = 'right';
    if (R.kinds) {
      for (const kd of ['E', 'P']) {
        const idxs = R.kinds.map((x, i) => x === kd ? i : -1).filter(i => i >= 0);
        if (!idxs.length) continue;
        const y0 = R.ys[idxs[0]], y1 = R.ys[idxs[idxs.length - 1]] + R.rowH;
        ctx.fillText(kd, g.gl - 7, (y0 + y1) / 2 + 3);
      }
    } else {
      ctx.font = `9px ${FONT}`;
      ctx.fillText('1', g.gl - 7, R.ys[0] + R.rowH);
      ctx.fillText(String(R.T), g.gl - 7, R.ys[R.T - 1] + R.rowH);
    }
    ctx.textAlign = 'left';
    if (modes || act) {  // the Mac's mode over time: the showcase runner's if its log says, else in use, away, on battery
      bandKind = modes ? 'runner' : 'activity';
      const y = R.height + 3, bc = BAND();
      ctx.fillStyle = css(TOK.page);
      ctx.fillRect(g.gl, y, w - g.gl - g.gr, 5);
      if (modes) for (const [a, b, m] of modes) { ctx.fillStyle = css(bc[m] || TOK.dim); ctx.fillRect(X(a), y, Math.max(1, X(b) - X(a)), 5); }
      else for (let k = 0; k < d.bins; k++) {
        if (!act[k]) continue;
        const x0 = X(h.t0 + k * d.width), x1 = X(h.t0 + (k + 1) * d.width);
        ctx.fillStyle = css(bc[act[k]]); ctx.fillRect(x0, y, Math.max(1, x1 - x0) + 0.3, 5);
      }
    }
    xAxis(ctx, h, X, w, g, R.height + band + (last ? 0 : 3), last);
    if (!anyRec) emptyNote(ctx, w, g, 0, R.height, nothingText());
    cv.setAttribute('aria-label', `${label(name)}, each ${R.kinds ? 'core' : 'thread'}, ${$('#window-title').textContent}, by its ${S.stat === 'peak' ? 'peak' : 'average'} busy share. Arrow keys move in time, up and down between ${R.kinds ? 'cores' : 'threads'}.`);
  });
  const mk = $('#mode-key');
  mk.hidden = !bandKind;
  if (bandKind) {
    const bc = BAND(), keys = bandKind === 'runner' ? ['full', 'capped', 'paused'] : ['away', 'use', 'batt'];
    mk.innerHTML = keys.map(m => `<i class="mode-i" style="background:${css(bc[m])}"></i>${MODE_WORD[m]}`).join('');
    mk.title = bandKind === 'runner' ? "the showcase runner's mode on the MacBook" : 'the MacBook: away (no input for two minutes), in use, on battery';
  }
}

function drawHistory() {
  if (!S.hist) return;
  renderWindow();
  renderLegend();
  drawBusy();
  drawFlops();
  drawMem();
  drawThreads();
  renderTiles();
  renderFoot();
  if (S.cursor && S.cursor.k !== undefined) showCursor(S.cursor.k, S.cursor.name, S.cursor.thread, S.cursor.src, true);
}

function renderLegend() {
  const h = S.hist, parts = [], said = [];
  for (const name of histNames()) {
    const d = h.nodes[name];
    let capS = 0, busyS = 0;
    for (let k = 0; k < d.bins; k++) if (d.cover[k] > 0 && d.busy[k] !== null) { const s = d.cover[k] * d.width * d.threads; capS += s; busyS += d.busy[k] * s; }
    parts.push(`<span title="average busy share in this window">${keyDot(name)}${esc(label(name))}<b>${capS ? pct(busyS / capS) : '-'}</b></span>`);
    said.push(`${label(name)} ${capS ? pct(busyS / capS) : 'nothing recorded'}`);
  }
  $('#legend').innerHTML = parts.join('');
  $('#c-busy').setAttribute('aria-label', `Busy share, ${$('#window-title').textContent}: ${said.join(', ')}. Arrow keys read each moment.`);
}

function renderTiles() {
  const h = S.hist;
  let cap = 0, busy = 0, ops = 0, counted = false, fast = 0, fastWho = null, est = 0;
  for (const name of histNames()) {
    const d = h.nodes[name];
    for (let k = 0; k < d.bins; k++) {
      if (!(d.cover[k] > 0) || d.busy[k] === null) continue;
      const s = d.cover[k] * d.width;
      cap += d.threads * s; busy += d.busy[k] * d.threads * s;
      if (d.flops[k] !== null && d.counted) { ops += d.flops[k] * s; counted = true; }
      if (d.flops_est && d.flops_est[k] !== null) est += d.flops_est[k] * s;
      if (d.counted && (d.flops_max[k] || 0) > fast) { fast = d.flops_max[k]; fastWho = name; }
    }
  }
  const [fv, fu] = fast ? siParts(fast, 'FLOP/s') : ['-', ''];
  const figs = [
    ['Busy, average', cap ? `${Math.round(100 * busy / cap)}<small class="pc">%</small>` : '-', 'all machines'],
    ['Operations', counted ? esc(words(ops)) : '-', `floating-point, nodes${est >= 1e6 ? ` · Mac ≈ ${esc(words(est))} est.` : ''}`],
    ['Work', cap ? `${count(busy / 3600)}<small>core-h</small>` : '-', 'busy threads'],
    ['Fastest', fast ? `${fv}<small>${fu}</small>` : '-', fast ? esc(label(fastWho)) : 'nothing counted'],
  ];
  $('#range-figs').innerHTML = figs.map(([k, v, s]) => `<div class="tile"><div class="tk">${k}</div><div class="tv">${v}</div><div class="ts">${s}</div></div>`).join('');
}

function renderFoot() {
  const h = S.hist, parts = [];
  for (const name of histNames()) {
    const d = h.nodes[name];
    parts.push(`${label(name)} ${dur(d.cover.reduce((a, c) => a + c, 0) * d.width)}`);
  }
  $('#chart-foot').textContent = histNames().length ? `Recorded: ${parts.join(' · ')} · a column is ${h.live ? '2 s' : dur(h.nodes[histNames()[0]].width)} · shaded 22:00 to 07:00` : '';
}

// ---------------------------------------------------------------- the history: reading a moment (pointer, touch, keys)
function canvasInfo(cv) {
  const h = S.hist, w = cv.clientWidth, g = geo(w);
  return {h, w, g, rect: cv.getBoundingClientRect()};
}
function binAt(cv, clientX) {
  const {h, w, g, rect} = canvasInfo(cv);
  const x = clientX - rect.left;
  if (x < g.gl || x > w - g.gr || !histNames().length) return null;
  const t = h.t0 + (x - g.gl) / (w - g.gl - g.gr) * (h.t1 - h.t0);
  const d = h.nodes[histNames()[0]];
  return Math.max(0, Math.min(d.bins - 1, Math.floor((t - h.t0) / d.width)));
}
function threadAt(cv, clientY) {
  const name = cv.dataset.n;
  if (!name) return null;
  const d = S.hist.nodes[name];
  if (!d) return null;
  const R = threadRows(d, cv.clientWidth < 560), y = clientY - cv.getBoundingClientRect().top;
  for (let i = 0; i < R.T; i++) if (y >= R.ys[i] - 0.5 && y <= R.ys[i] + R.rowH + 0.5) return i;
  return null;
}
function jobsAt(n, a, b) {
  return (n.jobs || []).filter(j => j.started && j.started < b && (j.state === 'running' ? true : (j.ended || j.started + (j.seconds || 0)) > a));
}
function readoutHTML(k, focus, thread) {
  const h = S.hist, names = histNames(), w = h.nodes[names[0]].width;
  const a = h.t0 + k * w, b = a + w;
  const span = h.live || w < 60 ? `${dayShort(a)}, ${hms(a)} to ${hms(b)}` : sameDay(a, b) ? `${dayShort(a)}, ${hm(a)} to ${hm(b)}` : spanText(a, b);
  let html = `<div class="r-when">${esc(span)}</div>`;
  const plain = [span];
  for (const name of names) {
    const d = h.nodes[name], n = nodeOf(name);
    html += `<div class="r-node"><div class="r-name">${keyDot(name)}${esc(label(name))}</div>`;
    if (!(d.cover[k] > 0)) { html += '<div class="r-sub">not recorded</div></div>'; plain.push(`${label(name)}: not recorded`); continue; }
    const bits = [`${pct(d.busy[k])} busy`, `peak ${pct(d.busy_max[k])}`];
    if (d.counted && d.flops[k] !== null) bits.push(esc(si(d.flops[k], 'FLOP/s')));
    else if (d.flops_est && d.flops_est[k] !== null) bits.push(`≈ ${esc(si(d.flops_est[k], 'FLOP/s'))} est.`);
    const tot = memTotal(n);
    if (d.mem[k] !== null) bits.push(`${d.mem[k].toFixed(1)}${tot ? `/${Math.round(tot)}` : ''} GB`);
    html += `<div>${bits.join(' · ')}</div>`;
    plain.push(`${label(name)}: ${bits.join(', ')}`);
    if (d.swap && d.swap[k] !== null) html += `<div class="r-sub">swap ${d.swap[k].toFixed(1)} GB · pressure ${(PRESSURE[d.pressure[k]] || '-').toLowerCase()}</div>`;
    if (name === focus && thread !== null && thread !== undefined) {
      const arr = d.mean[k], pk = d.peak[k], kinds = d.kinds;
      const tn = kinds ? `${kinds[thread] === 'P' ? 'Performance' : 'Efficiency'} core ${kinds.slice(0, thread + 1).filter(x => x === kinds[thread]).length}` : `Thread ${thread + 1}`;
      if (arr && d.pt[k] > 0) { html += `<div class="r-thread"><b>${tn}</b> ${pct(arr[thread])} · peak ${pct(pk ? pk[thread] : null)}</div>`; plain.push(`${tn}: ${pct(arr[thread])}`); }
      else html += `<div class="r-sub">${tn}: average only in this record</div>`;
    }
    const jobs = jobsAt(n, a, b);
    if (jobs.length) html += `<div class="r-sub">ran ${jobs.slice(0, 2).map(j => esc((j.lane ? j.lane + ' · ' : '') + jobName(j.id))).join(', ')}${jobs.length > 2 ? ` +${jobs.length - 2}` : ''}</div>`;
    else if (d.top && d.top[k] && d.top[k].length) html += `<div class="r-sub">${d.top[k].map(([nm, c]) => `${esc(nm)} ×${c.toFixed(1)}`).join(' · ')}</div>`;
    else if (d.run && d.run[k]) html += `<div class="r-sub">${d.run[k]} job${d.run[k] > 1 ? 's' : ''} running</div>`;
    const mid = (a + b) / 2, seg = (d.modes || []).find(s => s[0] <= mid && mid < s[1]);
    if (seg) html += `<div class="r-sub">${esc(modeText({mode: seg[2], why: seg[3]}))}</div>`;
    else if (d.act && d.act[k]) html += `<div class="r-sub">${MODE_WORD[d.act[k]]}</div>`;
    html += '</div>';
  }
  return {html, plain: plain.join('. ')};
}
const docked = () => matchMedia('(max-width: 720px), (pointer: coarse)').matches;
function showCursor(k, focus, thread, src, quiet, py) {
  if (!S.hist || !histNames().length) return;
  const d0 = S.hist.nodes[histNames()[0]];
  k = Math.max(0, Math.min(d0.bins - 1, k));
  S.cursor = {k, name: focus, thread, src, py: py ?? S.cursor?.py};
  const box = $('#charts'), bx = box.getBoundingClientRect();
  const cvs = $$('canvas.plot', box), first = cvs[0].getBoundingClientRect(), lastR = cvs[cvs.length - 1].getBoundingClientRect();
  const {h, w, g, rect} = canvasInfo(cvs[0]);
  const x = rect.left - bx.left + xOf(h, w, g)(h.t0 + (k + 0.5) * d0.width);
  const ch = $('#crosshair');
  ch.style.left = `${Math.round(x)}px`;
  ch.style.top = `${first.top - bx.top}px`;
  ch.style.height = `${lastR.bottom - first.top}px`;
  ch.hidden = false;
  const ro = $('#readout'), {html, plain} = readoutHTML(k, focus, thread);
  const dock = docked() && src !== 'key';
  ro.classList.toggle('docked', dock);
  ro.innerHTML = html + (dock ? '<button type="button" class="icon-btn r-close" aria-label="Close"><svg viewBox="0 0 14 14"><path d="M3 3l8 8M11 3l-8 8"/></svg></button>' : '');
  ro.hidden = false;
  if (!dock) {
    const pyy = S.cursor.py ?? (first.top - bx.top + 20);
    const rw = ro.offsetWidth, rh = ro.offsetHeight;
    let left = x + 18;
    if (left + rw > bx.width - 8) left = x - 18 - rw;
    ro.style.left = `${Math.max(8, left)}px`;
    ro.style.top = `${Math.max(8, Math.min(bx.height - rh - 8, pyy - 24))}px`;
  } else { ro.style.left = ''; ro.style.top = ''; }
  if (!quiet) $('#readout-live').textContent = plain;
}
function hideCursor() {
  S.cursor = null;
  $('#crosshair').hidden = true;
  $('#readout').hidden = true;
}
function wireCharts() {
  const box = $('#charts');
  let touching = false;
  const at = ev => {
    const cv = ev.target.closest('canvas.plot');
    if (!cv || !S.hist) return null;
    const k = binAt(cv, ev.clientX);
    if (k === null) return null;
    return {k, name: cv.dataset.n || null, thread: threadAt(cv, ev.clientY), py: ev.clientY - box.getBoundingClientRect().top};
  };
  box.addEventListener('pointermove', ev => {
    if (ev.pointerType !== 'mouse' && !touching) return;
    const p = at(ev);
    if (!p) { if (ev.pointerType === 'mouse') hideCursor(); return; }
    showCursor(p.k, p.name, p.thread, ev.pointerType, false, p.py);
  });
  let lastDown = 0;
  box.addEventListener('pointerdown', ev => {
    lastDown = performance.now();
    if (ev.pointerType === 'mouse' && !docked()) return;  // a mouse reads by hovering; a finger, or a click on a phone, by touching
    const p = at(ev);
    if (!p) return;
    touching = ev.pointerType !== 'mouse';
    showCursor(p.k, p.name, p.thread, ev.pointerType === 'mouse' ? 'tap' : ev.pointerType, false, p.py);
  });
  const end = () => { touching = false; };
  box.addEventListener('pointerup', end);
  box.addEventListener('pointercancel', end);
  box.addEventListener('pointerleave', ev => { if (ev.pointerType === 'mouse') hideCursor(); });
  document.addEventListener('click', ev => {
    if (ev.target.closest('.r-close')) { hideCursor(); return; }
    if ($('#readout').classList.contains('docked') && !ev.target.closest('#charts') && !ev.target.closest('#readout')) hideCursor();
  });
  box.addEventListener('keydown', ev => {
    const cv = ev.target.closest('canvas.plot');
    if (!cv || !S.hist || !histNames().length) return;
    const bins = S.hist.nodes[histNames()[0]].bins, big = Math.max(1, Math.round(bins / 20));
    const cur = S.cursor && S.cursor.k !== undefined ? S.cursor : {k: bins - 1, thread: 0};
    let k = cur.k, th = cv.dataset.n ? (cur.name === cv.dataset.n && cur.thread !== null && cur.thread !== undefined ? cur.thread : 0) : null;
    const T = cv.dataset.n ? threadRows(S.hist.nodes[cv.dataset.n], cv.clientWidth < 560).T : 0;
    if (ev.key === 'ArrowLeft') k -= ev.shiftKey ? big : 1;
    else if (ev.key === 'ArrowRight') k += ev.shiftKey ? big : 1;
    else if (ev.key === 'Home') k = 0;
    else if (ev.key === 'End') k = bins - 1;
    else if (ev.key === 'ArrowUp' && th !== null) th = Math.max(0, th - 1);
    else if (ev.key === 'ArrowDown' && th !== null) th = Math.min(T - 1, th + 1);
    else if (ev.key === 'Escape') { hideCursor(); return; }
    else return;
    ev.preventDefault();
    showCursor(k, cv.dataset.n || null, th, 'key', false, cv.getBoundingClientRect().top - box.getBoundingClientRect().top + 30);
  });
  box.addEventListener('focusin', ev => {
    const cv = ev.target.closest('canvas.plot');
    if (performance.now() - lastDown < 500) return;  // focus that came with a click or a touch: the pointer decides
    if (!cv || !S.hist || !histNames().length || (S.cursor && S.cursor.k !== undefined)) return;
    const d = S.hist.nodes[histNames()[0]];
    let k = d.bins - 1;
    while (k > 0 && !(d.cover[k] > 0)) k--;
    showCursor(k, cv.dataset.n || null, cv.dataset.n ? 0 : null, 'key', false, cv.getBoundingClientRect().top - box.getBoundingClientRect().top + 30);
  });
  box.addEventListener('focusout', ev => { if (!box.contains(ev.relatedTarget)) hideCursor(); });
}

// ---------------------------------------------------------------- jobs, lane by lane
function renderJobs() {
  let html = '';
  for (const name of S.order) {
    const n = nodeOf(name);
    if (n.planned) continue;
    const jobs = [...(n.jobs || [])];
    const lanes = new Map();
    for (const j of jobs) {
      const lane = j.lane || (n.local ? 'showcase' : '');
      if (!lanes.has(lane)) lanes.set(lane, []);
      lanes.get(lane).push(j);
    }
    const t = j => j.state === 'running' ? Infinity : (j.ended || j.started || 0);
    const order = [...lanes.entries()].map(([lane, js]) => [lane, js.sort((a, b) => t(b) - t(a))]).sort((a, b) => t(b[1][0]) - t(a[1][0]));
    const open = S.jobsOpen.has(name);
    html += `<article class="card jobs-card"><h3>${keyDot(name)}${esc(n.label)}</h3>`;
    if (!jobs.length) html += `<p class="empty" style="margin-top:10px">${n.local ? 'No showcase runs yet' : 'No jobs yet'}</p>`;
    let hidden = 0;
    for (const [lane, js] of order) {
      const running = js.filter(j => j.state === 'running').length;
      const show = open ? js : js.filter((j, i) => j.state === 'running' || i < 2);
      hidden += js.length - show.length;
      html += `<div class="lane"><div class="lane-h"><span>${esc(lane || 'Shared copy')}</span><span>${running ? `${running} running` : `${js.length} done`}</span></div>`;
      for (const j of show) {
        let mark, tm;
        if (j.state === 'running') { mark = '<span class="mark run" aria-label="running">●</span>'; tm = `${dur(now() - (j.started || now()))}${j.cpu_s ? ` · ${count(j.cpu_s / 3600)} core-h` : ''}`; }
        else if (j.state === 'lost') { mark = '<span class="mark bad" aria-label="lost">?</span>'; tm = 'no exit code'; }
        else if (j.exit === 0 || j.state === 'done') { mark = '<span class="mark ok" aria-label="finished">✓</span>'; tm = `${dur(j.seconds || 0)}${j.ended ? ` · ${when(j.ended)}` : ''}`; }
        else { mark = '<span class="mark bad" aria-label="failed">✗</span>'; tm = `exit ${j.exit ?? '?'} · ${dur(j.seconds || 0)}`; }
        html += `<div class="job" title="${esc(j.cmd || '')}">${mark}<div class="name">${esc(jobName(j.id))}</div><div class="t">${tm}</div></div>`;
      }
      html += '</div>';
    }
    if (hidden || open) html += `<button type="button" class="more" data-n="${esc(name)}" aria-expanded="${open}">${open ? 'Fewer' : `All ${jobs.length}`}</button>`;
    html += '</article>';
  }
  $('#jobs-list').innerHTML = html || '<p class="empty">No machines yet</p>';
}

// ---------------------------------------------------------------- work done: the odometer and its comparisons
function renderOdometer() {
  let flops = 0, ins = 0, busy = 0;
  const per = {};
  const nodes = S.order.map(nodeOf).filter(n => !n.planned && n.totals);
  for (const n of nodes) {
    const t = n.totals, p = after(n, t.upto || t.since || 0);
    const o = {flops: (t.flops || 0) + p.flops, ins: (t.ins || 0) + p.ins, busy: (t.busy || 0) + p.busy};
    if (live(n) && counts(n) && !REDUCED.matches) {  // between two samples, run on at the last sample's pace
      const s = n.latest, ahead = Math.min(3, Math.max(0, now() - s.t));
      o.flops += (s.flops || 0) / s.dt * ahead; o.ins += (s.ins || 0) / s.dt * ahead;
    }
    const key = `all:${n.name}`;  // a total that only grows never runs backwards
    if (S.shown[key] && S.shown[key].flops > o.flops) Object.assign(o, S.shown[key]);
    S.shown[key] = {...o};
    per[n.name] = o;
    if (counts(n)) { flops += o.flops; ins += o.ins; }
    busy += o.busy;
  }
  $('#odo-digits').textContent = grouped(flops);
  const since = firstRecord();
  let est = 0;
  for (const n of nodes.filter(estimates)) {
    est += n.totals.flops_est || 0;
    for (const s of n.samples) if (s.t >= (n.totals.upto || 0) && s.flops_est) est += s.flops_est;
    per[n.name].est = est;
  }
  $('#odo-words').textContent = `${flops >= 1e6 ? words(flops) + ' ' : ''}floating-point operations, counted on the nodes${est >= 1e6 ? ` · about ${words(est)} more estimated on the MacBook` : ''}`;
  $('#odo-words').title = est ? estWhy(nodes.find(estimates)) : '';
  $('#work-sub').textContent = since ? `Since ${dayLong(since)}, ${hm(since)}` : '';
  $('#odo-apollo').textContent = count(ins / S.apollo);
  const cray = flops / CRAY1;
  $('#odo-cray').textContent = cray < 86400 * 2 ? dur(cray) : `${count(cray / 86400)} days`;
  $('#odo-people').textContent = dur(flops / PEOPLE);
  $('#odo-core').textContent = count(busy / 3600);
  const share = $('#odo-share'), legend = $('#odo-share-legend');
  const parts = nodes.map(n => n.name), cparts = parts.filter(k => counts(nodeOf(k)) || estimates(nodeOf(k)));
  if (share.children.length !== cparts.length || legend.children.length !== parts.length) {
    share.innerHTML = cparts.map(k => `<i data-n="${esc(k)}" class="${counts(nodeOf(k)) ? '' : 'est'}" style="background-color:${css(hue(k))}"></i>`).join('');
    legend.innerHTML = parts.map(k => `<span data-n="${esc(k)}">${keyDot(k)}${esc(label(k))}<b></b></span>`).join('');
  }
  const all = flops + est;  // the share of each, the Mac's estimate apart and marked
  for (const k of parts) {
    const n = nodeOf(k), f = counts(n) ? (per[k] ? per[k].flops : 0) : (per[k]?.est || 0), w = all > 0 ? 100 * f / all : 0;
    const bar = $(`i[data-n="${k}"]`, share);
    if (bar) bar.style.width = `${w}%`;
    $(`span[data-n="${k}"] b`, legend).textContent = counts(n) ? (all > 0 ? `${Math.round(w)}%` : '-') : estimates(n) ? (all > 0 ? `≈ ${w < 1 && w > 0 ? '<1' : Math.round(w)}% est.` : 'est.') : 'not counted';
    $(`span[data-n="${k}"]`, legend).title = estimates(n) ? estWhy(n) : '';
  }
}

function renderPeriods() {
  const line = sm => {
    if (!sm) return '-';
    let ops = 0, busy = 0, cap = 0, rec = 0, est = 0;
    for (const [k, d] of Object.entries(sm.nodes)) {
      const n = nodeOf(k);
      if (!n || !d) continue;
      if (counts(n)) ops += d.flops;
      est += d.flops_est || 0;
      busy += d.busy; cap += (d.cores || n.threads || 0) * d.covered; rec += d.covered;
    }
    return rec ? `${words(ops)} operations${est >= 1e6 ? ` (+ ≈ ${words(est)} est. on the Mac)` : ''} · ${pct(busy / cap)} busy` : 'nothing recorded';
  };
  const out = [`<div><dt>Today</dt><dd>${esc(line(S.today))}</dd></div>`];
  if (S.night) out.push(`<div><dt>${esc(S.night.label)}</dt><dd>${esc(line(S.night))}</dd></div>`);
  $('#periods').innerHTML = out.join('');
}
function nightRange() {
  const d = new Date(), h = d.getHours(), a = new Date(d);
  a.setHours(22, 0, 0, 0);
  if (h < 22) a.setDate(a.getDate() - 1);
  const b = new Date(a); b.setDate(b.getDate() + 1); b.setHours(8, 0, 0, 0);
  const open = h >= 22 || h < 8;
  return {since: a / 1000, until: open ? null : b / 1000, label: open ? `Tonight, since ${hm(a / 1000)}` : `Last night, ${hm(a / 1000)} to ${hm(b / 1000)}`};
}

// ---------------------------------------------------------------- the moon
function moonPath(cx, cy, r, p) {
  if (p <= 0.001) return '';
  if (p >= 0.999) return `M${cx - r} ${cy}A${r} ${r} 0 1 1 ${cx + r} ${cy}A${r} ${r} 0 1 1 ${cx - r} ${cy}Z`;
  const rx = r * Math.abs(1 - 2 * p);
  return `M${cx} ${cy - r}A${r} ${r} 0 0 1 ${cx} ${cy + r}A${rx} ${r} 0 0 ${p < 0.5 ? 0 : 1} ${cx} ${cy - r}Z`;
}
const CRATERS = [[-38, -30, 17], [30, -44, 10], [44, 18, 21], [-16, 40, 13], [-52, 22, 8], [8, -6, 9], [22, 56, 7], [-26, -62, 6]];

function renderMoon() {
  const total = S.order.reduce((a, k) => {
    const n = nodeOf(k);
    if (n.planned || !n.totals || !counts(n)) return a;
    return a + n.totals.ins + after(n, n.totals.upto || 0).ins;
  }, 0);
  const missions = total / S.apollo;
  const next = MILESTONES.find(m => m > missions) || null;
  const prev = [...MILESTONES].reverse().find(m => m <= missions) || 0;
  const p = !next ? 1 : prev ? Math.log10(missions / prev) / Math.log10(next / prev) : missions / next;
  const cx = 120, cy = 120, R = 100, gold = css(studio(1)), deep = css(studio(0.9));
  $('#moon').innerHTML = `
    <defs><radialGradient id="mg" cx="40%" cy="38%" r="70%"><stop offset="0" stop-color="${gold}"/><stop offset="1" stop-color="${deep}"/></radialGradient>
      <clipPath id="lit"><path d="${moonPath(cx, cy, R, p)}"/></clipPath></defs>
    <circle cx="${cx}" cy="${cy}" r="${R}" fill="${css(TOK.page)}" stroke="${css(TOK.hair)}" stroke-width="2"/>
    ${CRATERS.map(([x, y, r]) => `<circle cx="${cx + x}" cy="${cy + y}" r="${r}" fill="${css(TOK.pill)}" opacity=".55"/>`).join('')}
    <path d="${moonPath(cx, cy, R, p)}" fill="url(#mg)"/>
    <g clip-path="url(#lit)">${CRATERS.map(([x, y, r]) => `<circle cx="${cx + x}" cy="${cy + y}" r="${r}" fill="${deep}" opacity=".28"/>`).join('')}</g>`;
  const lab = m => m >= 1e6 ? '1M' : m >= 1e3 ? `${m / 1e3}k` : String(m);
  $('#ladder').innerHTML = MILESTONES.map(m => `<li class="${missions >= m ? 'done' : m === next ? 'next' : ''}"><div class="pip"></div><div class="lab">${lab(m)}</div><span class="sr-only">${missions >= m ? 'reached' : m === next ? 'next' : 'ahead'}</span></li>`).join('');
  let pace = 0;
  if (S.day) for (const k in S.day.nodes) {
    const d = S.day.nodes[k], n = nodeOf(k);
    if (!n || !counts(n)) continue;
    const span = Math.min(86400, now() - (n?.totals?.since || now()));
    if (d.covered > 0) pace += d.ins / Math.max(span, 60);
  }
  const perHour = pace * 3600 / S.apollo;
  let txt = `<b>${count(missions)}</b> Apollo 11 missions of computing so far`;
  if (next && perHour > 0.01) txt += ` · <b>${count(perHour)}</b> an hour · ${lab(next)} in ${dur((next - missions) * S.apollo / pace)}`;
  else if (!next) txt += ' · a million: done';
  $('#moon-text').innerHTML = txt;
}

// ---------------------------------------------------------------- machines compared: the benchmark, and side by side
function fmtVal(v) {
  if (v === null || v === undefined || !isFinite(v)) return null;
  const a = Math.abs(v);
  return a >= 1000 ? Math.round(v).toLocaleString('en-US') : a >= 100 ? v.toFixed(0) : a >= 10 ? v.toFixed(1) : v.toFixed(2);
}
function renderBench() {
  const host = $('#bench'), sub = $('#bench-sub'), b = S.bench;
  const nodes = S.order.map(nodeOf);
  const spec = (k, extra) => { const n = nodeOf(k); return `<article class="card spec"><h4>${keyDot(k)}${esc(n.label)}</h4><p>${esc(n.about)}</p>${extra}</article>`; };
  if (!b || !b.ok) {
    sub.textContent = b && b.error ? 'benchmark file unreadable' : 'benchmark pending';
    host.innerHTML = nodes.map(n => spec(n.name, `<span class="pending" title="${esc(b && b.error ? b.error : 'the benchmark has not run yet; nothing is estimated here')}">Benchmark pending</span>`)).join('');
    return;
  }
  const B = b.bench, machines = B.machines || {};
  const keys = [...new Set([...S.order.filter(k => k in machines || (B.tests || []).some(t => t.results && k in t.results)), ...Object.keys(machines)])];
  const colour = k => S.order.includes(k) ? css(hue(k)) : css(TOK.dim);
  const name = k => S.order.includes(k) ? label(k) : k;
  sub.textContent = B.date ? `measured ${B.date}` : '';
  let html = '';
  (B.tests || []).forEach((t, i) => {
    const res = t.results || {}, hib = t.higher_is_better !== false;
    const vals = keys.map(k => typeof res[k] === 'number' && isFinite(res[k]) ? res[k] : null);
    const ok = vals.filter(v => v !== null);
    const max = ok.length ? Math.max(...ok.map(Math.abs)) : 0;
    const best = ok.length > 1 ? (hib ? Math.max(...ok) : Math.min(...ok)) : null;
    const id = `m-${i}`;
    html += `<article class="card test"><div class="test-head"><h4>${esc(t.title || t.id || `Test ${i + 1}`)}<span class="unit">${esc(t.unit || '')}${t.unit ? ' · ' : ''}${hib ? 'higher is better' : 'lower is better'}</span></h4>`;
    if (t.method) html += `<button type="button" class="info-btn" aria-expanded="false" aria-controls="${id}" aria-label="How it was measured" title="How it was measured"><svg viewBox="0 0 14 14" aria-hidden="true"><circle cx="7" cy="7" r="5.6"/><path d="M7 6.3v3.6M7 4.2v.1"/></svg></button>`;
    html += '</div>';
    if (t.method) html += `<p class="method" id="${id}" hidden>${esc(t.method)}</p>`;
    html += '<div class="bars">';
    keys.forEach((k, j) => {
      const v = vals[j], bg = t.background && t.background[k];
      html += `<div class="bar-row" title="${bg ? `measured under: ${esc(bg)}` : ''}"><span class="who"><span class="key" style="background:${colour(k)}"></span>${esc(name(k))}</span>
        <span class="val">${v === null ? '<span class="quiet">not measured</span>' : `${v === best ? '<span class="best" aria-hidden="true"></span><span class="sr-only">best: </span>' : ''}${fmtVal(v)} <span class="quiet">${esc(t.unit || '')}</span>`}</span>
        <div class="track" aria-hidden="true"><i style="width:${v === null || !max ? 0 : Math.max(1.5, 100 * Math.abs(v) / max).toFixed(1)}%;background:${colour(k)}"></i></div>
        ${bg ? `<span class="bg">under ${esc(bg)}</span>` : ''}</div>`;
    });
    html += '</div></article>';
  });
  host.innerHTML = html || nodes.map(n => spec(n.name, '<span class="pending">No tests in the file yet</span>')).join('');
}

function renderCompare() {
  const cols = S.order.map(nodeOf);
  const day24 = n => S.day && S.day.nodes[n.name];
  const allFlops = n => n.totals && counts(n) ? n.totals.flops + after(n, n.totals.upto || 0).flops : null;
  const NC = 'not counted';
  const rows = [
    ['Processor', n => n.info ? String(n.info.cpu || '').replace(/\(R\)|\(TM\)|CPU /g, '').replace(/\s+@.*/, '') : (n.about || '-')],
    ['Threads · memory', n => `${n.threads || '-'} · ${n.memory_gb ? n.memory_gb + ' GB' : '-'}`],
    ['Busy now', n => live(n) ? pct(n.latest.util) : '-', n => live(n) ? n.latest.util : null],
    ['Busy, 24 h', n => day24(n)?.util != null ? `${pct(day24(n).util)} of ${dur(day24(n).covered)}` : '-', n => day24(n)?.util ?? null],
    ['Operations, 24 h', n => !counts(n) ? (day24(n)?.flops_est ? `≈ ${words(day24(n).flops_est)} est.` : NC) : day24(n) ? words(day24(n).flops) : '-', n => counts(n) ? day24(n)?.flops ?? null : null],
    ['Operations, all', n => !counts(n) ? (n.totals?.flops_est ? `≈ ${words(n.totals.flops_est)} est.` : NC) : allFlops(n) !== null ? words(allFlops(n)) : '-', n => allFlops(n)],
    ['Fastest minute', n => !counts(n) ? (n.totals?.peak_flops_est ? `≈ ${si(n.totals.peak_flops_est, 'FLOP/s')} est.` : NC) : n.totals?.peak_flops ? si(n.totals.peak_flops, 'FLOP/s') : '-', n => counts(n) ? n.totals?.peak_flops || null : null],
    ['Clock while busy', n => !counts(n) ? NC : day24(n)?.ghz ? `${day24(n).ghz.toFixed(2)} GHz` : '-'],
    ['Instructions a cycle', n => !counts(n) ? NC : day24(n)?.ipc ? day24(n).ipc.toFixed(2) : '-'],
    ['Jobs done · failed', n => { const j = n.jobs || []; return n.planned ? '-' : `${j.filter(x => x.exit === 0).length} · ${j.filter(x => x.exit > 0 || x.state === 'lost' || x.state === 'failed').length}`; }],
    ['Recording since', n => n.totals?.since ? `${dayLong(n.totals.since)}, ${hm(n.totals.since)}` : '-'],
  ];
  let html = `<thead><tr><th scope="col"><span class="sr-only">Measure</span></th>${cols.map(n => `<th scope="col"><span>${keyDot(n.name)}${esc(n.label)}</span></th>`).join('')}</tr></thead><tbody>`;
  for (const [lab, f, score] of rows) {
    const vals = score ? cols.map(score) : [];
    const best = vals.filter(v => v !== null && v > 0).length > 1 ? Math.max(...vals.filter(v => v !== null)) : null;
    html += `<tr><th scope="row" class="k">${lab}</th>${cols.map((n, i) => `<td class="${best !== null && vals[i] === best ? 'win' : ''}">${esc(f(n))}${best !== null && vals[i] === best ? '<span class="sr-only"> (highest)</span>' : ''}</td>`).join('')}</tr>`;
  }
  $('#cmp').innerHTML = html + '</tbody>';
  $('#cmp-list').innerHTML = cols.map(n => `<div class="cmp-m"><h4>${keyDot(n.name)}${esc(n.label)}</h4><dl>${rows.map(([lab, f]) => `<div><dt>${lab}</dt><dd>${esc(f(n))}</dd></div>`).join('')}</dl></div>`).join('');
}

// ---------------------------------------------------------------- while you were away
const LAST = 'openphysics.compute.lastSeen';
function remember() { try { localStorage.setItem(LAST, String(now())); } catch (e) { /* private window */ } }
async function away() {
  let last = 0;
  try { last = parseFloat(localStorage.getItem(LAST)) || 0; } catch (e) { /* private window */ }
  remember();
  if (!last || now() - last < 1800) return;
  const sm = await get(`api/summary?since=${last}`).catch(() => null);
  if (!sm) return;
  const who = [];
  let flops = 0, ins = 0;
  for (const k of S.order) {
    const d = sm.nodes[k], n = nodeOf(k);
    if (!d || !n) continue;
    if (counts(n)) { flops += d.flops; ins += d.ins; }
    who.push(d.covered ? `${n.label} ${Math.round(100 * d.util)}%` : `${n.label} off`);
  }
  const el = $('#away');
  el.innerHTML = `<svg class="glyph" viewBox="0 0 44 44" aria-hidden="true"><circle cx="22" cy="22" r="17" fill="${css(TOK.page)}" stroke="${css(TOK.hair)}"/><path d="${moonPath(22, 22, 17, 0.62)}" fill="${css(studio(0.95))}"/></svg>
    <div><div class="when">While you were away · ${dur(now() - last)}</div><div class="what">${esc(who.join(' · '))}${flops ? ` · ${esc(words(flops))} operations · ${count(ins / S.apollo)} Apollo missions` : ''}</div></div>
    <button type="button" class="icon-btn" id="away-close" aria-label="Dismiss"><svg viewBox="0 0 14 14"><path d="M3 3l8 8M11 3l-8 8"/></svg></button>`;
  el.hidden = false;
  $('#away-close').addEventListener('click', () => { el.hidden = true; });
}

// ---------------------------------------------------------------- data and rhythm
async function pollLive() {
  if (document.hidden) return;
  const ts = S.order.map(nodeOf).filter(live).map(n => n.latest.t);
  const since = ts.length ? Math.max(...ts) - 10 : now() - 600;
  try {
    const d = await get(`api/live?since=${since.toFixed(2)}`);
    for (const v of d.nodes) merge(v);
    if (S.order.some(k => !S.cards.has(k))) buildMachines();
    S.connected = true; S.everConnected = true; S.lastOk = now();
  } catch (e) { S.connected = false; }
  renderMachines(); renderHero(); renderStatus(); renderMoon();
  if (S.range === 'live') loadHistory();
}
async function loadState() {
  try {
    const d = await get('api/state');
    S.apollo = d.apollo_instructions || APOLLO_DEFAULT;
    S.mode = d.mode || 'live'; S.mirror = d.mirror || null;
    for (const v of d.nodes) merge(v);
    S.connected = true; S.everConnected = true;
    return true;
  } catch (e) { S.connected = false; return false; }
}
async function loadSummaries() {
  try {
    S.day = await get(`api/summary?since=${(now() - 86400).toFixed(0)}`);
    const m = new Date(); m.setHours(0, 0, 0, 0);
    S.today = await get(`api/summary?since=${(m / 1000).toFixed(0)}`);
    const nr = nightRange();
    S.night = await get(`api/summary?since=${nr.since.toFixed(0)}${nr.until ? `&until=${nr.until.toFixed(0)}` : ''}`);
    S.night.label = nr.label;
  } catch (e) { /* next time */ }
  renderCompare(); renderMoon(); renderPeriods();
}
async function loadBench() {
  try { S.bench = await get('api/bench'); } catch (e) { S.bench = S.bench || null; }
  renderBench();
}

let lastText = 0;
function frame(ts) {  // the one moving number: the odometer, a few times a second (or every two seconds, without motion)
  if (ts - lastText > (REDUCED.matches ? 2000 : 150)) { lastText = ts; renderOdometer(); }
  requestAnimationFrame(frame);
}

function setRange(v) {
  S.range = v; S.offset = 0; prefs.set('range', v);
  hideCursor();
  renderWindow();
  loadHistory(true);
}

async function boot() {
  readTokens();
  segmented($('#range'), S.range, setRange);
  segmented($('#stat'), S.stat, v => { S.stat = v; prefs.set('stat', v); drawHistory(); });
  $('#prev').addEventListener('click', () => { S.offset += 1; hideCursor(); renderWindow(); loadHistory(true); });
  $('#next').addEventListener('click', () => { S.offset = Math.max(0, S.offset - 1); hideCursor(); renderWindow(); loadHistory(true); });
  $('#latest').addEventListener('click', () => { S.offset = 0; hideCursor(); renderWindow(); loadHistory(true); });
  $('#jobs-list').addEventListener('click', ev => {
    const b = ev.target.closest('button.more');
    if (!b) return;
    const k = b.dataset.n;
    if (S.jobsOpen.has(k)) S.jobsOpen.delete(k); else S.jobsOpen.add(k);
    renderJobs();
    $(`#jobs-list button.more[data-n="${k}"]`)?.focus();
  });
  $('#bench').addEventListener('click', ev => {
    const b = ev.target.closest('.info-btn');
    if (!b) return;
    const open = b.getAttribute('aria-expanded') !== 'true';
    b.setAttribute('aria-expanded', String(open));
    document.getElementById(b.getAttribute('aria-controls')).hidden = !open;
  });
  wireCharts();
  await loadState();
  buildMachines();
  renderMachines(); renderHero(); renderStatus(); renderJobs(); renderMoon(); renderWindow(); renderBench();
  await Promise.all([loadHistory(true), loadSummaries(), loadBench(), loadSparks()]);
  away();
  requestAnimationFrame(frame);
  setInterval(pollLive, 2000);
  setInterval(async () => { if (!document.hidden && await loadState()) { buildMachines(); renderJobs(); renderCompare(); } }, 15000);
  setInterval(() => { if (!document.hidden && S.range !== 'live' && S.offset === 0) loadHistory(); }, 5000);
  setInterval(() => { if (!document.hidden) loadSparks(); }, 30000);
  setInterval(() => { if (!document.hidden) loadSummaries(); }, 60000);
  setInterval(() => { if (!document.hidden) loadBench(); }, 300000);
  setInterval(remember, 60000);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) remember();
    else loadState().then(() => { renderMachines(); renderJobs(); loadHistory(true); loadSummaries(); loadSparks(); });
  });
  let pending = 0, lastW = plotWidth();
  window.addEventListener('resize', () => {
    clearTimeout(pending);
    pending = setTimeout(() => {
      const w = plotWidth();
      drawSparks();
      if (S.range !== 'live' && Math.abs(w - lastW) > 0.1 * lastW) { lastW = w; loadHistory(true); } else drawHistory();
    }, 150);
  });
}
boot();
