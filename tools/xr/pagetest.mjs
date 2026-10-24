#!/usr/bin/env node
// pagetest.mjs - the headset page (tools/xr/web) in headless Chrome, driven through the DevTools protocol: real mouse
// events on the window for pointing, clicking and dragging, and the headset's controller path through synthetic
// controller events (a controller aimed by a pose, then select). Run by tools/xr/xr_test.py, which writes the result,
// starts the app and the bridge, and states the criteria (its part 5); this prints one ok or FAIL line per check and a
// last line of JSON with what the page drew, for the harness to compare with the scene files.
//   node tools/xr/pagetest.mjs URL SCREENSHOT.png
// Node 22 or later (its WebSocket), and Chrome (CHROME=path to use another). With XRTEST_MCP=path/to/openphysics-mcp and the
// bridge's OPENPHYSICS_XR_DIR (the harness sets both) it also asks for the page's picture through the AI's own headset_look. For a stalled run: PAGETEST_DEBUG=1 prints
// each step with the page's frame time, heap and scene size and pauses a page that stops answering to show where it
// is; PAGETEST_SKIP=point,play,slider,cut,controller leaves parts out; PAGETEST_TYPE=js enters the typed commands
// through the page's own key handler; PAGETEST_ANGLE=swiftshader, PAGETEST_FLAGS='...' and PAGETEST_CHROMELOG=1 change
// or show Chrome's side. On 2026-10-07 the full run stalled inside Chrome (no JavaScript running, the page's own
// measures flat) while the laptop's load average was 8 to 12 with its swap nearly full; the same run had passed twice
// on a quieter machine. On 2026-10-08 it stalled in six runs of six at load averages of 7 to 11 (swap 3.5 of 5 GB):
// after the typed "cut along z", at "remove the cut" or at 9b; with PAGETEST_DEBUG the first JavaScript to run after
// the pause request was the Enter key's handler, so for seconds before it the page ran nothing, not even its animation
// frames: Chrome had stopped giving the page frames and tasks. Puppeteer's flags against background throttling
// (--disable-renderer-backgrounding, --disable-background-timer-throttling, --disable-backgrounding-occluded-windows,
// --disable-hang-monitor) did not change it; PAGETEST_ANGLE=swiftshader got further (past 9b's picture, 45 s) and
// stalled on a plain evaluation. The page's script from before that day's change stalled the same way. Two more runs
// at a load of 5 to 6 stalled at the same place. A lead: the stalls follow the typed cut, where the page's own probe
// counts its shader programs going from 7 to 12 (the cut's geometry brings new materials; the volume shader takes the
// cut as uniforms), so Chrome's GPU side compiling them is the likely place it stops giving the page frames.
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const [url, shot] = process.argv.slice(2);
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const W = 1000, H = 750;
const skip = (part) => (process.env.PAGETEST_SKIP || '').split(',').includes(part); // diagnostics: leave a part out
let failures = 0;
const t0 = Date.now();
let perfProbe = null; // debug: the page's frame time, heap and scene size beside every check
const check = (ok, what) => { console.log((ok ? '  ok   ' : '  FAIL ') + what + `  (${((Date.now() - t0) / 1000).toFixed(1)} s)`); if (!ok) failures++;
  if (perfProbe) perfProbe().then((p) => console.log('    . perf ' + JSON.stringify(p))).catch(() => {});
  return ok; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const profile = mkdtempSync(path.join(tmpdir(), 'openphysics-xrtest-chrome-'));
let pageErrors = [];
const recent = []; // every console message, for a report when the page stalls
process.on('SIGTERM', () => { // stopped from outside: say where, and leave nothing behind
  console.log('  FAIL stopped from outside after ' + ((Date.now() - t0) / 1000).toFixed(1) + ' s; page errors: ' + pageErrors.slice(0, 5).join(' | '));
  console.log('  last console messages: ' + recent.slice(-12).join(' | '));
  try { if (mcp) mcp.p.kill('SIGKILL'); chrome.kill('SIGKILL'); rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch (e) { /* leaving */ }
  console.log(JSON.stringify(findings));
  process.exit(1);
});
const chrome = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, `--window-size=${W},${H}`,
  '--no-first-run', '--no-default-browser-check', '--mute-audio', '--autoplay-policy=no-user-gesture-required',
  ...(process.env.PAGETEST_ANGLE ? ['--use-angle=' + process.env.PAGETEST_ANGLE] : []),
  ...(process.env.PAGETEST_CHROMELOG ? ['--enable-logging=stderr', '--v=0'] : []),
  ...(process.env.PAGETEST_FLAGS ? process.env.PAGETEST_FLAGS.split(' ') : []), 'about:blank'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
if (process.env.PAGETEST_CHROMELOG) chrome.stderr.on('data', (d) => { for (const l of String(d).split('\n')) if (l.trim()) console.log('    chrome: ' + l.slice(0, 240)); });
let ws, mcp = null, closing = false;
const findings = {};
// the AI's tools as an AI client runs them: openphysics-mcp over stdio, with the bridge's folder (OPENPHYSICS_XR_DIR)
function startMcp() {
  const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
  const p = spawn(process.env.XRTEST_MCP, ['--embedded', '--workspace', path.join(profile, 'mcp-ws'), '--lab', root,
    '--lab-results', path.join(profile, 'mcp-lab')], { stdio: ['pipe', 'pipe', 'ignore'] });
  const waiting = new Map();
  let n = 0;
  createInterface({ input: p.stdout }).on('line', (l) => {
    try { const m = JSON.parse(l); if (waiting.has(m.id)) waiting.get(m.id)(m), waiting.delete(m.id); } catch (e) { /* not ours */ }
  });
  const rpc = (method, params = {}) => new Promise((resolve) => {
    const id = ++n;
    waiting.set(id, resolve);
    p.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
  });
  return { p, rpc, ready: rpc('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'pagetest', version: '1' } })
    .then(() => p.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n')) };
}
async function tool(name, args = {}) { // an MCP tool's result, or null after 20 s
  if (!mcp) mcp = startMcp();
  if (!(await Promise.race([mcp.ready.then(() => true), sleep(15000).then(() => false)]))) return null;
  return (await Promise.race([mcp.rpc('tools/call', { name, arguments: args }), sleep(20000).then(() => null)]))?.result || null;
}
// the bridge's files in the headset folder: the page's health report, and the sessions' log beside the folder
const XRDIR = process.env.OPENPHYSICS_XR_DIR || process.env.NAVIER_XR_DIR;
const readJson = (f) => { try { return JSON.parse(readFileSync(f, 'utf8')); } catch (e) { return null; } };
const healthFile = () => (XRDIR ? readJson(path.join(XRDIR, 'health.json')) : null);
const lastSession = () => { try { const l = readFileSync(path.join(path.dirname(XRDIR), 'xr-sessions.jsonl'), 'utf8').trim().split('\n'); return JSON.parse(l[l.length - 1]); } catch (e) { return null; } };
async function waitFor(fn, ms) { const end = Date.now() + ms; let v = null; while (Date.now() < end && !(v = fn())) await sleep(200); return v; }
async function headsetLook(args = {}) { // -> { error, view, moment, width, height, jpeg (base64) }
  const r = await tool('headset_look', args);
  if (!r) return { error: 'no answer from openphysics-mcp' };
  const img = (r.content || []).find((c) => c.type === 'image'), sc = r.structuredContent || {};
  return { error: r.isError ? (sc.error && sc.error.message) || 'error' : null, ...(sc.value || {}), jpeg: img && img.mimeType === 'image/jpeg' ? img.data : null };
}
const near = (a, b, tol) => a && b && a.every((v, i) => Math.abs(v - b[i]) <= tol);
try {
  const wsUrl = await new Promise((resolve, reject) => {
    let buf = '';
    chrome.stderr.on('data', (d) => { buf += d; const m = buf.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); });
    setTimeout(() => reject(new Error('Chrome did not start')), 30000);
  });
  ws = new WebSocket(wsUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  ws.onclose = () => { // Chrome went away (on 2026-10-07, twice, while the laptop's swap was full): say so, and where
    if (closing) return;
    console.log('  FAIL Chrome closed the DevTools connection after ' + ((Date.now() - t0) / 1000).toFixed(1) + ' s; last console messages: ' + recent.slice(-6).join(' | '));
    console.log(JSON.stringify(findings));
    process.exit(1);
  };
  let next = 0;
  const pending = new Map(), errors = pageErrors;
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
    if (m.method === 'Runtime.consoleAPICalled' || m.method === 'Log.entryAdded') {
      const e = m.params.entry || m.params;
      recent.push(`${((Date.now() - t0) / 1000).toFixed(1)}s ${e.level || e.type}: ${e.text || (e.args || []).map((a) => a.value ?? a.description).join(' ')}`.slice(0, 200));
      if (recent.length > 50) recent.shift();
    }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
    if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') errors.push(m.params.entry.text + (m.params.entry.url ? ' ' + m.params.entry.url : ''));
  };
  const send = (method, params = {}, sessionId) => new Promise((resolve) => {
    const id = ++next;
    pending.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params, sessionId }));
  });
  const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
  const { result: { sessionId } } = await send('Target.attachToTarget', { targetId, flatten: true });
  const S = (m, p) => send(m, p, sessionId);
  await S('Runtime.enable'), await S('Page.enable'), await S('Log.enable');
  await S('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  await S('Page.navigate', { url });
  if (process.env.PAGETEST_DEBUG) {
    const g = await send('SystemInfo.getInfo');
    const dev = g.result && g.result.gpu && g.result.gpu.devices ? g.result.gpu.devices.map((d) => d.deviceString || d.vendorString).join(', ') : '?';
    console.log('    . GPU: ' + dev + ' | ' + JSON.stringify((g.result && g.result.gpu && g.result.gpu.featureStatus) || {}).slice(0, 300));
  }
  const js = async (expr) => {
    const r = await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || 'evaluation failed');
    return r.result.result.value;
  };
  const until = async (expr, ms = 30000) => {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      try { if (await js(expr)) return true; } catch (e) { /* the page is still loading */ }
      await sleep(200);
    }
    return false;
  };
  const mouse = (type, x, y, pressed = false) => S('Input.dispatchMouseEvent', {
    type, x, y, button: type === 'mouseMoved' ? 'none' : 'left', buttons: pressed ? 1 : 0, clickCount: type === 'mouseMoved' ? 0 : 1 });
  const click = async (x, y) => { await mouse('mouseMoved', x, y); await mouse('mousePressed', x, y, true); await mouse('mouseReleased', x, y); };
  const at = async (expr) => { const p = await js(expr); return p ? [Math.round(p.x * W), Math.round(p.y * H)] : null; };
  // a JPEG decoded by the page: its size and its mean colour on 5 x 5 pixels at points 0..1 from the top left
  const decode = (b64, pts) => js(`(async () => {
    const bm = await createImageBitmap(await (await fetch('data:image/jpeg;base64,${b64}')).blob());
    const c = new OffscreenCanvas(bm.width, bm.height), g = c.getContext('2d');
    g.drawImage(bm, 0, 0);
    const px = ([u, v]) => { const x = Math.max(2, Math.min(bm.width - 3, Math.floor(u * bm.width))), y = Math.max(2, Math.min(bm.height - 3, Math.floor(v * bm.height)));
      const d = g.getImageData(x - 2, y - 2, 5, 5).data, s = [0, 0, 0];
      for (let i = 0; i < 100; i += 4) s[0] += d[i], s[1] += d[i + 1], s[2] += d[i + 2];
      return s.map((t) => Math.round(t / 25)); };
    return { w: bm.width, h: bm.height, at: ${JSON.stringify(pts)}.map(px) };
  })()`);
  const STUDIO = [242, 241, 238]; // the page's colour, #F2F1EE
  const O = 'window.openphysics';
  if (process.env.PAGETEST_DEBUG) perfProbe = () => js(`${O} && ${O}.perf()`);

  // 1. the result arrives: every frame, drawn
  check(await until(`${O} && ${O}.view().kind === 'lab' && ${O}.view().frames.length === ${O}.state().lab.nframes && ${O}.view().shown >= 0`),
        'the page shows the lab result with every frame loaded');
  findings.first = await js(`${O}.view()`);
  // 10b. the page opens as the window does: no controls card, the legend, one bar with one round button; Manual shows the
  // card, under which the card's own checks below run
  const look0 = await js(`(() => ({ card: ${O}.cardShown(), legend: !document.getElementById('legend').hidden,
    bar: [...document.querySelectorAll('#bar > *')].map((e) => e.id).join(','), talk: document.getElementById('talk').className }))()`);
  check(!look0.card && look0.legend && look0.bar === 'q,talk' && look0.talk === 'mic',
        `the page opens as the window does: no controls card, the legend, one bar (${look0.bar}) with one round button (${look0.talk})`);
  await js(`${O}.manual(true)`);
  check(await js(`${O}.cardShown()`), 'Manual shows the controls card');
  check(await until(`${O}.marks().length === 1 && ${O}.marks()[0].label === 'test mark'`, 10000), "the AI's mark is drawn with its label");
  // 2. a field chosen on the card, by a click: the app switches it and the page draws every frame again
  const keyBefore = await js(`${O}.state().lab.view_key`);
  let p = await at(`${O}.buttonAt('field:T')`);
  check(!!p, 'the card offers the field T');
  if (p) await click(...p);
  check(await until(`${O}.state().lab.view.field === 'T' && ${O}.state().lab.view_key !== '${keyBefore}' && ${O}.view().facts && ${O}.view().facts.field === 'T' && ${O}.view().frames.length === ${O}.state().lab.nframes`),
        'a click on the field T: the app shows T and the page has every frame again in T');
  findings.T = await js(`${O}.view()`);
  // 3. pointing: the value read on the block's top face is the stored field there (a closed form: T is linear)
  if (!skip('point')) {
  await js(`${O}.hold(false)`);
  const frame = await js(`${O}.view().shown`);
  p = await at(`${O}.project(${0.5 + 0.1 * frame}, 1.0, -0.5)`);
  let reading = null;
  for (let k = 0; k < 6 && !reading; k++) { // the result turns slowly until it is pointed at
    await mouse('mouseMoved', p[0] + k, p[1]);
    await sleep(150);
    reading = await js(`${O}.reading()`);
    if (!reading || reading.part !== 'block') reading = null, p = await at(`${O}.project(${0.5 + 0.1 * frame}, 1.0, -0.5)`);
  }
  check(!!reading, 'pointing at the block reads a value there');
  if (reading) {
    const [x, y, z] = reading.source, want = 6 * (x - 0.1 * frame) + 1.5 * y + 3 * z + frame;
    check(Math.abs(reading.value - want) <= 1e-4 * Math.max(1, Math.abs(want)),
          `the value read, ${reading.value.toFixed(5)} K at (${x.toFixed(4)}, ${y.toFixed(4)}, ${z.toFixed(4)}) m, is the stored field there, ${want.toFixed(5)} K`);
    check(Math.abs(z - 1) < 1e-3 && x > 0 && y > 0 && y < 1, 'it is read on the top face, where the pointer is');
  }
  findings.reading = reading;
  // 4. a click without moving pins it, and the AI is told about it
  if (p) { await mouse('mousePressed', p[0] + 5, p[1], true); await mouse('mouseReleased', p[0] + 5, p[1]); }
  check(await until(`${O}.pins() === 1`, 3000), 'a click on the result pins the value');
  const ctx = await js(`${O}.context()`);
  check(/pinned points: 1\) part 'block'/.test(ctx) && /where T is/.test(ctx), `the AI's context names the pin: "${ctx.slice(0, 160)}..."`);
  findings.context = ctx;
  }
  // 5. playback and frames from the card
  if (!skip('play')) {
  p = await at(`${O}.buttonAt('play')`);
  await click(...p);
  check(await until(`${O}.state().lab.playing === true`, 5000), 'play on the card: the app plays');
  await click(...p);
  check(await until(`${O}.state().lab.playing === false`, 5000), 'pause on the card: the app stops');
  const f0 = await js(`${O}.state().lab.frame`), n = await js(`${O}.state().lab.nframes`);
  p = await at(`${O}.buttonAt('next')`);
  await click(...p);
  check(await until(`${O}.state().lab.frame === ${(f0 + 1) % n} && ${O}.view().shown === ${(f0 + 1) % n}`, 5000), 'next on the card: the app and the page show the next frame');
  }
  // 6. dragging the time slider from its middle to its start: frame 0 in the app
  if (!skip('slider')) {
  const s0 = await at(`${O}.buttonAt('scrub', 0.5)`), s1 = await at(`${O}.buttonAt('scrub', 0.02)`);
  if (s0 && s1) {
    await mouse('mouseMoved', s0[0], s0[1]);
    await mouse('mousePressed', s0[0], s0[1], true);
    for (let k = 1; k <= 8; k++) await mouse('mouseMoved', Math.round(s0[0] + (s1[0] - s0[0]) * k / 8), Math.round(s0[1] + (s1[1] - s0[1]) * k / 8), true), await sleep(20);
    await mouse('mouseReleased', s1[0], s1[1]);
    check(await until(`${O}.state().lab.frame === 0 && ${O}.view().shown === 0`, 5000), 'the time slider dragged to its start: frame 0 in the app and the page');
    findings.slider = [s0, s1];
  }
  }
  // 7. the cut: a click on Z, then the controller path to switch it off
  if (!skip('cut')) {
  p = await at(`${O}.buttonAt('cut:z')`);
  await click(...p);
  check(await until(`${O}.state().lab.view.section.axis === 2`, 5000), 'Z on the card: the app cuts the result across z');
  check(await until(`${O}.view().key === ${O}.state().lab.view_key && ${O}.view().section && ${O}.view().section.axis === 2 && ${O}.view().frames.length === ${O}.state().lab.nframes`, 15000),
        'and the page draws every frame again with the cut (the shown frame was drawn with it)');
  findings.cut = await js(`${O}.view()`);
  }
  if (!skip('controller')) {
  check(await js(`${O}.pointController(0, { card: 'cut:off' })`), 'a controller aims at Off');
  await js(`${O}.controllerEvent(0, 'selectstart')`), await js(`${O}.controllerEvent(0, 'selectend')`);
  check(await until(`${O}.state().lab.view.section.axis === -1`, 5000), 'the controller presses Off: the app shows the whole result again');
  // 8. a controller tap on the result pins a second value; a controller on the field chip changes the field
  await until(`${O}.view().frames.length === ${O}.state().lab.nframes`, 15000);
  const fr = await js(`${O}.view().shown`);
  // the second cell's top face: a tap within a centimetre of a pin would take that pin away instead
  check(await js(`${O}.pointController(1, { point: [${1.5 + 0.1 * fr}, 1.0, -0.5] })`), 'a controller aims at the block');
  await js(`${O}.controllerEvent(1, 'selectstart')`), await js(`${O}.controllerEvent(1, 'selectend')`);
  check(await until(`${O}.pins() === 2`, 3000), 'a controller tap on the result pins a second value');
  check(await js(`${O}.pointController(0, { card: 'field:stress' })`), 'a controller aims at the field stress');
  await js(`${O}.controllerEvent(0, 'selectstart')`), await js(`${O}.controllerEvent(0, 'selectend')`);
  check(await until(`${O}.state().lab.view.field === 'stress'`, 5000), 'the controller picks stress: the app shows it');
  check(await until(`${O}.view().facts && ${O}.view().facts.field === 'stress' && ${O}.view().stats.volume`, 15000), 'and the page draws stress with its volume');
  findings.stress = await js(`${O}.view()`);
  }
  // 8b. plain commands typed in the question box, with real key events: done at once, and the app follows
  const box = await js(`(() => { const r = document.getElementById('q').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
  const dbg = (m) => { if (process.env.PAGETEST_DEBUG) console.log('    . ' + m + `  (${((Date.now() - t0) / 1000).toFixed(1)} s)`); };
  if (process.env.PAGETEST_DEBUG) { // a stalled page: pause its JavaScript and print where it is
    let last = Date.now(), paused = false;
    const mark = () => { last = Date.now(); };
    ws.addEventListener('message', (e) => {
      const m = JSON.parse(e.data);
      if (m.method === 'Debugger.paused') {
        console.log('    . paused in: ' + m.params.callFrames.slice(0, 12).map((f) => `${f.functionName || '(anon)'}@${(f.url || '').split('/').pop()}:${f.location.lineNumber + 1}`).join(' < '));
        S('Debugger.resume');
      }
      if (m.id) mark();
    });
    setInterval(() => {
      if (!paused && Date.now() - last > 8000) {
        paused = true;
        console.log('    . no answer for 8 s: pausing the page');
        S('Debugger.enable').then(() => S('Debugger.pause'));
      }
    }, 1000).unref();
  }
  const type = async (text) => {
    if (process.env.PAGETEST_TYPE === 'js') { // the page's own key handler, without the browser's synthetic text input
      await js(`(() => { const q = document.getElementById('q'); q.focus(); q.value = ${JSON.stringify(text)};
        q.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' })); })()`);
      return;
    }
    dbg('click the box for ' + text);
    await click(Math.round(box[0]), Math.round(box[1]));
    dbg('insert');
    await S('Input.insertText', { text });
    dbg('enter');
    for (const t of ['keyDown', 'keyUp']) await S('Input.dispatchKeyEvent', { type: t, key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13 });
    dbg('typed');
  };
  await until(`${O}.view().frames.length === ${O}.state().lab.nframes`, 15000);
  const fA = await js(`${O}.state().lab.frame`), nA = await js(`${O}.state().lab.nframes`);
  await type('next frame');
  check(await until(`${O}.state().lab.frame === ${(fA + 1) % nA}`, 5000), '"next frame" typed: the app shows the next frame');
  await type('show the temperature');
  check(await until(`${O}.state().lab.view.field === 'T'`, 5000), '"show the temperature" typed: the app shows T');
  await type('cut along z');
  check(await until(`${O}.state().lab.view.section.axis === 2`, 5000), '"cut along z" typed: the app cuts across z');
  await type('remove the cut');
  check(await until(`${O}.state().lab.view.section.axis === -1`, 5000), '"remove the cut" typed: the whole result again');
  check(await js(`document.getElementById('q').value === ''`), 'the question box is empty after each command');
  const grammar = await js(`import('./commands.js').then(({ parse }) => {
    const lab = [{ id: 'speed', label: 'speed' }, { id: 'T', label: 'T' }, { id: 'Q', label: 'Q' }, { id: 'p', label: 'p' }];
    const fem = [{ id: 'von_mises', label: 'von Mises stress' }, { id: 'temperature', label: 'temperature' }, { id: 'displacement', label: 'displacement' }];
    const cases = [['Play.', lab, 'play'], ['Go to frame twelve', lab, 'frame:12'], ['Show me the temperature', lab, 'field:T'], ['pressure', lab, 'field:p'],
      ['show the vortices', lab, 'field:Q'], ['cut it across the x axis', lab, 'cut:x'], ['True size please', lab, 'truesize'],
      ['show the stress', fem, 'field:von_mises'], ['show the deformation', fem, 'field:displacement'],
      ['show me where the stress is highest', fem, null], ['run the fire at 200 kW', lab, null], ['what is the temperature', lab, null]];
    const got = (a) => a ? a.do + (a.field ? ':' + a.field : a.axis ? ':' + a.axis : a.frame !== undefined ? ':' + a.frame : '') : null;
    return cases.filter(([t, f, want]) => got(parse(t, f)) !== want).map(([t]) => t);
  })`);
  check(grammar.length === 0, `the command grammar maps its sample sentences and leaves questions to the AI${grammar.length ? ': ' + grammar.join(' | ') : ''}`);
  // 9b. what the AI sees: the page's picture through the AI's own headset_look, against the window
  if (process.env.XRTEST_MCP && !skip('look')) {
    dbg('9b: hold');
    await js(`${O}.hold(true)`); // no slow turn while the picture and the window are compared
    dbg('9b: paused, every frame');
    await until(`${O}.view().frames.length === ${O}.state().lab.nframes && !${O}.state().lab.playing`, 15000);
    dbg('9b: headset_look');
    const lk = await headsetLook();
    dbg('9b: headset_look answered');
    check(!lk.error && lk.jpeg && lk.view === 'screen' && lk.moment === 'now', `headset_look over MCP: the page answers with a picture of the window's view${lk.error ? ': ' + lk.error : ''}`);
    if (lk.jpeg) {
      const fr = await js(`${O}.view().shown`), pt = `${1.0 + 0.1 * fr}, 1.0, -0.25`; // the block's top, between the two pins
      const lp = await js(`${O}.lookProject(${pt})`), sp = await js(`${O}.project(${pt})`);
      const screen = (await js(`${O}.screenPixel(${sp.x}, ${sp.y})`)).slice(0, 3);
      const d = await decode(lk.jpeg, [[lp.x, lp.y], [0.98, 0.02]]);
      check(d.w === 960 && d.h === 720 && lk.width === 960 && lk.height === 720, `it is ${d.w} x ${d.h} (the bridge read ${lk.width} x ${lk.height})`);
      check(near(d.at[1], STUDIO, 10), `its background is the studio page's colour (${d.at[1]})`);
      check(near(d.at[0], screen, 12), `at the block its colour is the window's there: ${d.at[0]} in the picture, ${screen} on the screen`);
      findings.look = { picture: d.at, screen, at: lp };
    }
    dbg('9b: question picture');
    const q = await Promise.race([js(`${O}.questionPicture()`), sleep(10000).then(() => null)]), lq = await headsetLook({ moment: 'question' });
    check(q && q.ok && lq.jpeg && Buffer.from(lq.jpeg, 'base64').length === q.bytes && lq.moment === 'question',
          `a picture taken as a question is asked is what moment "question" returns (${q ? q.bytes : 0} bytes)`);
    await js(`${O}.hold(false)`);
  }
  // 10a. hands-free: the voice activity detector on synthetic sound
  const vad = await js(`import('./vad.js').then(({ Vad }) => {
    const rate = 16000, NOISE = 0.003, noiseRms = NOISE / Math.sqrt(12), voiceRms = Math.sqrt(0.5 + 0.125);
    let seed = 1; const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647 - 0.5);
    const noise = (s) => Float32Array.from({ length: Math.round(s * rate) }, () => NOISE * rnd());
    const voice = (s, db) => { const a = noiseRms * 10 ** (db / 20) / voiceRms;
      return Float32Array.from({ length: Math.round(s * rate) }, (_, i) => a * (Math.sin(2 * Math.PI * 220 * i / rate) + 0.5 * Math.sin(2 * Math.PI * 440 * i / rate)) + NOISE * rnd()); };
    const run = (parts, ai) => { const v = new Vad(rate);
      return parts.flatMap((p, k) => v.feed(p, ai && k > 0)).filter((e) => e.type === 'end' && e.samples).map((e) => e.samples.length / rate); };
    return { sentence: run([noise(1), voice(0.8, 25), noise(1.2)]), click: run([noise(0.5), voice(0.05, 30), noise(1)]), noise: run([noise(3)]),
             echo: run([noise(1), voice(0.8, 15), noise(1)], true), over: run([noise(1), voice(0.8, 25), noise(1)], true) };
  })`);
  check(vad.sentence.length === 1 && vad.sentence[0] >= 0.8 && vad.sentence[0] <= 1.3 && !vad.click.length && !vad.noise.length,
        `hands-free: a 0.8 s voice is one sentence (${vad.sentence.map((x) => x.toFixed(2))} s), a click and noise are none`);
  check(!vad.echo.length && vad.over.length === 1, 'while the AI speaks its own voice coming back is not taken, a person speaking over it is');
  // 10c. words heard reach the AI, its answer streams into the line; the round button stops it
  await js(`${O}.hear('please give me a long answer')`);
  check(await until(`/Sentence number/.test(${O}.dock().page.words) && ${O}.dock().page.talk === 'stop'`, 10000),
        'words heard reach the AI and its answer streams into the words line; the round button is stop while it answers');
  const tb = await js(`(() => { const r = document.getElementById('talk').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
  await click(Math.round(tb[0]), Math.round(tb[1]));
  check(await until(`${O}.state().speech.stopped && ${O}.dock().page.words === 'Stopped.' && ${O}.state().agent.busy === false`, 5000),
        'pressing it ends the answer: the bridge stops the speech, the AI is free, the line says so');
  // 10. the headset's own code, with the stand-in WebXR runtime (?fakexr=1): it emulates no Quest, it makes the code run
  if (!skip('xr')) {
    await S('Page.navigate', { url: url + (url.includes('?') ? '&' : '?') + 'fakexr=1' });
    check(await until(`${O} && window.fakexr && ${O}.view().frames.length === ${O}.state().lab.nframes && ${O}.view().shown >= 0`),
          'the page with the stand-in runtime has every frame');
    const p = await at(`(() => { const r = document.getElementById('ar').getBoundingClientRect(); return r.width ? { x: (r.left + r.width / 2) / ${W}, y: (r.top + r.height / 2) / ${H} } : null; })()`);
    check(!!p, 'Enter AR is offered');
    if (p) await click(...p);
    check(await until(`${O}.xr().presenting && ${O}.xr().background === 'passthrough' && ${O}.xr().tableY !== undefined`, 10000), 'Enter AR: the page presents in passthrough');
    const x0 = await js(`${O}.xr()`);
    if (process.env.XRTEST_MCP && XRDIR && !skip('health')) { // how the headset runs, as the bridge keeps it
      const h = await waitFor(() => { const v = healthFile(); return v && v.presenting && v.fps > 0 ? v : null; }, 6000);
      check(!!h && h.mode === 'immersive-ar' && h.fps > 0 && (h.features || []).includes('hit-test') && !!(h.device && h.device.gpu),
            `the page reports how it runs: ${h ? `${h.mode}, ${h.fps} frames a second over its own frames, features ${(h.features || []).join(' ')}, GPU ${h.device && h.device.gpu}` : 'no report within 6 s'}`);
      await js(`setTimeout(() => { throw new Error('xrtest deliberate error'); })`);
      const he = await waitFor(() => { const v = healthFile(); return v && (v.errors || []).some((e) => /xrtest deliberate/.test(e.text)) ? v : null; }, 3000);
      const st = await tool('headset_status');
      const sh = st && st.structuredContent && st.structuredContent.value && st.structuredContent.value.headset;
      check(!!he && !!sh && (sh.errors || []).some((e) => /xrtest deliberate/.test(e.text)), 'an error thrown in the page is in the report within 3 s, and headset_status gives it to the AI');
    }
    // the head is at (0, 1.6, 0) looking down -z, its left eye 0.032 m to the left
    check(Math.abs(x0.holder[2] + 0.5) < 0.01 && Math.abs(x0.holder[1] - 1.18) < 0.01 && Math.abs(x0.holder[0]) < 0.04,
          `the result stands 0.5 m in front of the head, 0.42 m below the eyes (at ${x0.holder.map((v) => v.toFixed(3)).join(', ')})`);
    check(x0.card[0] < x0.holder[0] - 0.3 && Math.abs(x0.cardYaw - Math.atan2(-0.032 - x0.card[0], 0 - x0.card[2])) < 0.05, 'the card stands to its left, facing the head');
    const fr = await js(`${O}.view().shown`);
    const dk = await js(`${O}.dock()`);
    check(dk.visible && !(await js(`${O}.cardShown()`)) && dk.at[0] > x0.holder[0] + 0.2 && Math.abs(dk.yaw - Math.atan2(-0.032 - dk.at[0], 0 - dk.at[2])) < 0.05,
          `the dock stands to the result's right, facing the head, and the card is hidden (dock at ${dk.at.map((v) => v.toFixed(2)).join(', ')})`);
    await js(`${O}.hear('please give me a long answer')`);
    await until(`${O}.dock().button === 'stop'`, 8000);
    await js(`window.fakexr.aim(1, [0.1, 1.4, 0], ${JSON.stringify(await js(`${O}.dockWorld('mic')`))})`), await sleep(100);
    await js('window.fakexr.select(1, true)'), await js('window.fakexr.select(1, false)');
    check(await until(`${O}.dock().button !== 'stop' && ${O}.state().speech.stopped`, 4000), 'a controller pointed at the round button presses it: the AI stops');
    check(await until(`${O}.dock().wordsOpacity < 0.01`, 11000), "the dock's words fade to nothing within 10.5 s of the answer's end");
    if (process.env.XRTEST_MCP && !skip('look')) { // what the AI sees in the headset: from the head, looking down at it
      await js('window.fakexr.head([0, 1.6, 0], 0, -Math.PI / 6)'), await sleep(200);
      const lk = await headsetLook();
      const lp = await js(`${O}.lookProject(${1.0 + 0.1 * fr}, 1.0, -0.25)`);
      const d = lk.jpeg ? await decode(lk.jpeg, [[lp.x, lp.y], [0.98, 0.02]]) : null;
      check(!lk.error && d && lk.view === 'ar' && lp.x > 0 && lp.x < 1 && lp.y > 0.5 && lp.y < 1 && !near(d.at[0], STUDIO, 30),
            `headset_look in AR: taken from the head, the block in its lower half (at ${lp.x.toFixed(2)}, ${lp.y.toFixed(2)}), drawn there (${d && d.at[0]})${lk.error ? ': ' + lk.error : ''}`);
      check(d && near(d.at[1], STUDIO, 10), `where passthrough shows the room the picture has the studio's colour (${d && d.at[1]}), not the room nor black`);
      await js('window.fakexr.head([0, 1.6, 0], 0, 0)'), await sleep(100);
    }
    const block = await js(`${O}.world(${1.5 + 0.1 * fr}, 1.0, -0.5)`); // the top of the block's second cell
    await js(`window.fakexr.aim(1, [0.1, 1.4, 0], ${JSON.stringify(block)})`);
    check(await until(`${O}.reading() && ${O}.reading().part === 'block'`, 3000), 'a controller pointed at the block reads the block');
    const s0 = await js(`${O}.xr().scale`);
    await js('window.fakexr.stick(1, 0, -0.9)'), await sleep(500), await js('window.fakexr.stick(1, 0, 0)'), await sleep(100);
    check(await js(`${O}.xr().scale`) > s0 * 1.5, 'the thumbstick makes it bigger');
    const press = async (i, k) => { await js(`window.fakexr.button(${i}, ${k}, true)`); await sleep(150); await js(`window.fakexr.button(${i}, ${k}, false)`); await sleep(200); };
    await press(1, 5);
    const t1 = await js(`${O}.xr()`);
    check(t1.trueSize && Math.abs(t1.scale - 1) < 1e-9 && Math.abs(t1.holder[1]) < 1e-6, 'B: true size, and the 9.5 m result stands on the floor');
    await press(1, 5);
    check(Math.abs((await js(`${O}.xr()`)).holder[1] - 1.18) < 0.01, 'B again: back on the table');
    await js('window.fakexr.aim(1, [0.2, 1.3, 0], [0.3, 0.75, -0.8])');
    await press(1, 4);
    check(await until(`${O}.xr().placing`, 2000), 'A: placing on a surface');
    await js('window.fakexr.hit([0.3, 0.75, -0.8])');
    check(await until(`${O}.xr().reticle`, 2000), 'the ring shows where the room\'s surface was found');
    await js('window.fakexr.select(1, true)'), await js('window.fakexr.select(1, false)'), await sleep(200);
    const pl = await js(`${O}.xr()`);
    check(!pl.placing && Math.abs(pl.holder[0] - 0.3) < 1e-6 && Math.abs(pl.holder[1] - 0.75) < 1e-6 && Math.abs(pl.holder[2] + 0.8) < 1e-6 &&
          Math.abs(pl.yaw - Math.atan2(0 - 0.3, 0 + 0.8)) < 0.05, 'the trigger sets it down there, facing the head (within 3 degrees: three.js stands the headset camera between the eyes, a little behind them)');
    const mid = await js(`${O}.world(${1.0 + 0.1 * fr}, 0.5, -0.5)`); // the middle of the block
    await js(`window.fakexr.aim(0, [-0.1, 1.3, 0], ${JSON.stringify(mid)}); window.fakexr.aim(1, [0.1, 1.3, 0], ${JSON.stringify(mid)})`);
    await sleep(100);
    const zb = await js(`${O}.xr().zoom`);
    await js('window.fakexr.select(0, true); window.fakexr.select(1, true)'), await sleep(100);
    await js(`window.fakexr.aim(0, [-0.4, 1.3, 0], ${JSON.stringify(mid)}); window.fakexr.aim(1, [0.4, 1.3, 0], ${JSON.stringify(mid)})`), await sleep(300);
    const za = await js(`${O}.xr().zoom`);
    await js('window.fakexr.select(0, false); window.fakexr.select(1, false)'), await sleep(100);
    check(Math.abs(za / zb - 4) < 0.4, `two hands from 0.2 m to 0.8 m apart scale it ${(za / zb).toFixed(2)} times`);
    const y0 = (await js(`${O}.xr()`)).cardYaw;
    await js('window.fakexr.head([1.4, 1.6, -0.2], Math.PI / 2)');
    check(await until(`Math.abs(${O}.xr().cardYaw - ${y0}) > 0.5`, 4000), 'the card turns toward a head that has walked round');
    await js('window.fakexr.session.end()');
    check(await until(`!${O}.xr().presenting && ${O}.xr().background === 'studio'`, 3000), 'ending the session gives the studio back');
    if (process.env.XRTEST_MCP && XRDIR && !skip('health')) {
      const ended = await waitFor(() => { const v = healthFile(); return v && v.presenting === false ? v : null; }, 3000), last = lastSession();
      check(!!ended && last && last.mode === 'immersive-ar' && last.frames > 0 && !!last.ended,
            `after the session the report says so, and its last numbers are kept beside the folder (${last ? `${last.frames} frames, ${last.fps} a second, lowest ${last.fps_low}, worst frame ${last.frame_ms_worst} ms` : 'none'})`);
      findings.health = last;
    }
    findings.xr = { frames: await js('window.fakexr.frames') };
  }
  // 9. the voice: the resampler keeps speech and drops what would fold into it
  const audio = await js(`import('./voice.js').then(({ wav16 }) => {
    const tone = (f) => { const x = new Float32Array(48000); for (let i = 0; i < x.length; i++) x[i] = 0.5 * Math.sin(2 * Math.PI * f * i / 48000); return x; };
    const rms = (buf) => { const d = new DataView(buf), n = (buf.byteLength - 44) / 2; let s = 0;
      for (let i = 400; i < n - 400; i++) { const v = d.getInt16(44 + 2 * i, true) / 32767; s += v * v; } return Math.sqrt(s / (n - 800)); };
    const a = wav16(tone(1000), 48000), b = wav16(tone(12000), 48000), d = new DataView(a);
    return { riff: String.fromCharCode(...new Uint8Array(a, 0, 4)), rate: d.getUint32(24, true), channels: d.getUint16(22, true),
             bits: d.getUint16(34, true), samples: (a.byteLength - 44) / 2,
             pass: 20 * Math.log10(rms(a) / (0.5 / Math.SQRT2)), stop: 20 * Math.log10(Math.max(rms(b), 1e-7) / (0.5 / Math.SQRT2)) }; // silence is below -130 dB
  })`);
  check(audio.riff === 'RIFF' && audio.rate === 16000 && audio.channels === 1 && audio.bits === 16 && audio.samples === 16000,
        'the voice is 16 kHz mono 16-bit WAV, a second for a second');
  check(Math.abs(audio.pass) < 0.5 && audio.stop < -40, `resampled from 48 kHz: 1 kHz passes (${audio.pass.toFixed(2)} dB), 12 kHz is ${audio.stop.toFixed(1)} dB down`);
  findings.audio = audio;
  await sleep(500);
  // the picture is for a person to look at, not a criterion: a Chrome that draws no frame does not hold the test up
  const shotData = await Promise.race([S('Page.captureScreenshot', { format: 'png' }), sleep(15000).then(() => null)]);
  if (shot && shotData && shotData.result) writeFileSync(shot, Buffer.from(shotData.result.data, 'base64'));
  else console.log('    (no screenshot: Chrome drew no frame within 15 s)');
  const real = errors.filter((e) => !/favicon|xrtest deliberate error/.test(e));
  check(real.length === 0, `no error in the page${real.length ? ': ' + real.slice(0, 3).join(' | ') : ''}`);
} catch (e) {
  check(false, 'the page test ran: ' + e.message);
} finally {
  closing = true;
  try { ws && ws.close(); } catch (e) { /* closing */ }
  if (mcp) mcp.p.kill('SIGKILL');
  chrome.kill('SIGKILL');
  await sleep(300);
  try { rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch (e) { /* Chrome still closing */ }
}
console.log(JSON.stringify(findings));
process.exit(failures ? 1 : 0);
