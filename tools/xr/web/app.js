// app.js - the headset page of OpenPhysicsAI (docs/xr.md). The result on screen in the app on the Mac (a finite-element
// part, or a lab simulation and all its frames) stands in front of you in passthrough AR, or in a studio in VR, with a
// card beside it: the field and its legend, the value under your pointer, playback, the field and the cut, the scale,
// and the AI. What you change here the app changes too; what you ask is answered by Ask AI on the Mac, which is told
// what you point at and can see what you see (openphysics-mcp headset_look), and the answer is spoken.
//
// Controllers: trigger on the result carries it (both triggers: scale and turn); a short tap pins the value pointed at;
// trigger on the card presses it; thumbstick: bigger or smaller, and turn; A or X: place it on a real surface; B or Y:
// true size; hold the grip and speak. Hands: pinch does what the trigger does; the card's microphone listens.
// On a computer: drag turns, the wheel zooms, pointing reads values, a click pins one, space plays, arrows step.
import * as THREE from 'three';
import { Link } from './link.js';
import { Voice } from './voice.js';
import { Card, fmt } from './card.js';
import { Dock } from './dock.js';
import { Live } from './live.js';
import { SceneView } from './scene.js';
import { Prober } from './probe.js';
import { parse as quickCommand } from './commands.js';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const lang = params.get('lang') || navigator.language || 'en-US';
if (params.has('fakexr')) await import('./test/fakexr.js'); // the page test's stand-in WebXR runtime, never otherwise

// ---- the scene ----
const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);
renderer.xr.enabled = true;
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.NeutralToneMapping;
document.body.prepend(renderer.domElement);
const scene = new THREE.Scene();
const PAGE = new THREE.Color(0xF2F1EE); // the studio page on a screen and in VR; passthrough in AR
scene.background = PAGE;
const camera = new THREE.PerspectiveCamera(45, innerWidth / innerHeight, 0.01, 200);
{ // a soft studio around the result: a bright ceiling and walls it reflects, as in the app's own renders
  const studio = new THREE.Scene();
  studio.background = new THREE.Color(0xcfccc6);
  studio.add(new THREE.Mesh(new THREE.BoxGeometry(6, 4, 6), new THREE.MeshBasicMaterial({ color: 0xe9e7e2, side: THREE.BackSide })));
  const lamp = new THREE.Mesh(new THREE.PlaneGeometry(3, 3), new THREE.MeshBasicMaterial({ color: 0xffffff }));
  lamp.position.set(0, 1.9, 0), lamp.rotation.x = Math.PI / 2, studio.add(lamp);
  const side = lamp.clone();
  side.position.set(2.9, 0.6, 0.5), side.rotation.set(0, -Math.PI / 2, 0), side.scale.setScalar(0.6), studio.add(side);
  scene.environment = new THREE.PMREMGenerator(renderer).fromScene(studio, 0.03).texture;
}
scene.add(new THREE.HemisphereLight(0xffffff, 0xc9c6c0, 0.8));
const sun = new THREE.DirectionalLight(0xffffff, 1.2);
sun.position.set(0.6, 2.2, 1.0);
scene.add(sun);
addEventListener('resize', () => {
  camera.aspect = innerWidth / innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});

// holder: where the result stands (moved by hand); model: its scale; the content (scene.js) inside
const holder = new THREE.Group();
holder.position.set(0, 1.0, 0);
scene.add(holder);
const model = new THREE.Group();
holder.add(model);
const shadow = (() => {
  const c = document.createElement('canvas');
  c.width = c.height = 128;
  const g = c.getContext('2d'), grad = g.createRadialGradient(64, 64, 4, 64, 64, 64);
  grad.addColorStop(0, 'rgba(0,0,0,0.28)'), grad.addColorStop(1, 'rgba(0,0,0,0)');
  g.fillStyle = grad, g.fillRect(0, 0, 128, 128);
  const m = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshBasicMaterial({ map: new THREE.CanvasTexture(c), transparent: true, depthWrite: false }));
  m.rotation.x = -Math.PI / 2, m.position.y = 0.0005;
  return m;
})();
holder.add(shadow);

const link = new Link(), voice = new Voice(link), card = new Card(0.36), view = new SceneView(model), prober = new Prober(model);
const live = new Live(link); // the provider's voice (GPT-Live), when the Mac has an OpenAI key (tools/xr/live.py)
card.mesh.position.set(-0.36, 1.12, 0.05);
card.mesh.rotation.y = 0.45;
card.mesh.visible = false; // the controls: shown under Manual; talking does the rest
scene.add(card.mesh);
const dock = new Dock(); // the headset's interface: the legend, the latest words, one round button (dock.js)
dock.group.visible = false;
scene.add(dock.group);

// ---- the AI's marks (openphysics-mcp headset_mark): labelled points at places in the result's own coordinates ----
const marksGroup = new THREE.Group();
model.add(marksGroup);
let marksKey = '';
function label(text) { // a white pill with the words, in the studio look
  const c = document.createElement('canvas'), g = c.getContext('2d'), F = '500 40px -apple-system, "SF Pro Text", Inter, Roboto, sans-serif';
  g.font = F;
  c.width = Math.min(1024, Math.ceil(g.measureText(text).width) + 56), c.height = 76;
  g.font = F;
  g.fillStyle = '#FFFFFF', g.strokeStyle = '#E8551C', g.lineWidth = 4;
  g.beginPath(), g.roundRect(2, 2, c.width - 4, c.height - 4, 36), g.fill(), g.stroke();
  g.fillStyle = '#111111', g.fillText(text, 28, 52);
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, depthTest: false, transparent: true, toneMapped: false }));
  sprite.center.set(0.5, -0.4);
  sprite.userData.aspect = c.width / c.height;
  sprite.renderOrder = 30;
  return sprite;
}
function placeMarks() {
  const s = link.state, mk = s.marks, f = view.facts, c = view.content;
  const ok = !!(mk && Array.isArray(mk.marks) && mk.marks.length && mk.title === s.title && mk.scene === s.scene && f && c);
  const key = ok ? JSON.stringify([mk.made, mk.marks, c.uuid]) : '';
  if (key === marksKey) return;
  marksKey = key;
  for (const o of [...marksGroup.children]) {
    marksGroup.remove(o);
    o.traverse((x) => { if (x.material) { if (x.material.map) x.material.map.dispose(); x.material.dispose(); } if (x.geometry) x.geometry.dispose(); });
  }
  if (!ok) return;
  const back = new THREE.Matrix4().copy(f.toSource).invert();
  c.updateMatrix();
  for (const m of mk.marks.slice(0, 12)) { // the result's coordinates -> the content's -> the model's
    const p = new THREE.Vector3(...m.at).applyMatrix4(back).applyMatrix4(c.matrix), g = new THREE.Group();
    const dot = new THREE.Mesh(new THREE.SphereGeometry(1, 16, 12), new THREE.MeshBasicMaterial({ color: 0xE8551C, depthTest: false, toneMapped: false }));
    dot.renderOrder = 30;
    g.add(dot, label(m.label));
    g.position.copy(p);
    g.userData.label = m.label;
    marksGroup.add(g);
  }
  rescaleMarks();
}
function rescaleMarks() { // a mark keeps its size in the room: a 6 mm point and a label 3 cm tall
  const s = model.scale.x || 1;
  for (const g of marksGroup.children) {
    g.children[0].scale.setScalar(0.003 / s);
    const sp = g.children[1];
    sp.scale.set(0.03 * sp.userData.aspect / s, 0.03 / s, 1);
  }
}

// ---- scale: about 30 cm across, or true size, times what the person chose ----
const ui = { zoom: 1, trueSize: false, placing: false, heard: '', status: 'connecting…', manual: false,
             // the conversation: what was last heard, the turn the answer to it will have, a short note (listening…)
             conv: { you: '', askedTurn: 0, note: '', noteKind: 'note', noteDone: true } };
function fit() {
  const s = view.size;
  return s ? 0.30 / Math.max(s.x, s.y, s.z, 1e-6) : 1;
}
function applyScale() {
  const s = ui.trueSize ? 1 : fit() * ui.zoom;
  model.scale.setScalar(s);
  const sz = view.size;
  // in the headset, a result taller than a table or wider than a desk stands on the real floor at true size, so you can
  // walk around it or into it (a 3 m fire plume, a concert hall); back on the table when it is small again
  if (renderer.xr.isPresenting && ui.tableY !== undefined) {
    const big = ui.trueSize && sz && (sz.y > 1.2 || Math.max(sz.x, sz.z) > 2);
    holder.position.y = big ? 0 : ui.tableY;
  }
  shadow.scale.setScalar(sz ? Math.max(sz.x, sz.z) * s * 1.6 + 0.02 : 0.3);
  prober.rescale();
  rescaleMarks();
}
view.addEventListener('change', () => { if (!ui.trueSize) ui.zoom = 1; applyScale(); reaim(); placeMarks(); refreshCard(); });
let cardFrameAt = 0;
view.addEventListener('frame', () => { // while it plays, the card's clock moves four times a second, not at every frame
  const now = performance.now();
  placeMarks();
  if (!view.playing || now - cardFrameAt > 250) cardFrameAt = now, reaim(), refreshCard();
});
// a new frame or field under a resting pointer: read the value again there (in the headset every frame does)
function reaim() {
  if (renderer.xr.isPresenting || !mouse.ray) return;
  const a = aim(mouse.ray[0], mouse.ray[1]);
  prober.show(a.reading || null);
  if (!a.reading) prober.current = null;
}

// ---- the Mac ----
link.addEventListener('state', (e) => {
  const s = e.detail.state;
  if (s.scene === 'fem') view.showFem(s.version);
  else if (s.scene === 'lab' && s.lab) view.setLab(s.lab);
  else view.clear();
  voice.play(s.speech); // the AI's words, sentence by sentence as it writes them (what was said before the page opened is not)
  if (s.look_want && s.look_want !== lookAnswered) { // the AI asked to see what the person sees (headset_look)
    const id = lookAnswered = s.look_want;
    nextPicture().then((p) => p && link.look(p.blob, id, 'now', p.view));
  }
  placeMarks();
  ui.status = s.app_seen_s == null ? 'The OpenPhysicsAI app is not running on the Mac.' : s.has_scene ? `${s.title}` : (s.note || '');
  ui.offline = s.app_seen_s == null ? ui.status : '';
  liveWords(s.live || {});
  runsEnded(((e.detail.before && e.detail.before.agent) || {}).computing || [], s);
  refreshCard();
});
link.addEventListener('offline', () => { ui.offline = 'The bridge on the Mac does not answer.'; refreshCard(); });
link.start();

// ---- what the card shows ----
function fields() {
  const s = link.state;
  if (s.scene === 'lab' && s.lab) return (s.lab.fields || []).map((f) => ({ id: f, label: f, active: s.lab.view && s.lab.view.field === f }));
  if (s.scene === 'fem' && s.fem) return (s.fem.fields || []).map((f) => ({ id: f.name, label: f.label, active: f.name === s.fem.field_name }));
  return [];
}
function timeRow() {
  const s = link.state;
  if (s.scene === 'lab') {
    const t = view.time();
    if (!t) return null;
    const unit = t.unit === 's' ? (Math.abs(t.t) < 1e-3 && t.t !== 0 ? 'µs' : Math.abs(t.t) < 1 ? 'ms' : 's') : t.unit;
    const k = unit === 'µs' ? 1e6 : unit === 'ms' ? 1e3 : 1;
    return { ...t, label: `t = ${fmt(t.t * k)} ${unit}` };
  }
  if (s.scene === 'fem' && s.fem && s.fem.nsteps > 1)
    return { index: s.fem.step - 1, count: s.fem.nsteps, playing: s.fem.playing, loaded: s.fem.nsteps, label: `stored time ${s.fem.step}` };
  return null;
}
function sectionRow() {
  const s = link.state;
  if (s.scene === 'lab' && s.lab && s.lab.native3d) {
    const sec = s.lab.view.section;
    return { on: sec.axis >= 0, axis: sec.axis, fraction: sec.fraction, flip: sec.flip, cmd: 'lab' };
  }
  if (s.scene === 'fem' && s.fem) return { ...s.fem.section, cmd: 'fem' };
  return null;
}
function refreshCard() {
  const s = link.state, f = view.facts, r = prober.current, sz = view.size;
  const scale = model.scale.x;
  card.set({
    connected: link.online && s.app_seen_s != null, title: s.title || '',
    note: view.error || (f && f.picture && f.picture.rgb ? 'A picture of the result: it has no field to read.' : s.note),
    kind: view.content ? view.kind : 'none', field: f ? f.field : '', unit: f ? f.unit : '', lo: f ? f.lo : 0, hi: f ? f.hi : 1,
    colormap: (s.scene === 'lab' ? s.lab && s.lab.colormap : s.fem && s.fem.colormap) || [],
    probe: r ? { text: r.text, where: r.where, t: r.t } : null, pins: prober.pins.length, marks: marksGroup.children.length,
    time: timeRow(), fields: fields(), section: sectionRow(),
    view: { trueSize: ui.trueSize, placing: ui.placing, canPlace: !!hitTestable,
            scaleText: ui.trueSize ? 'true size' : `${fmt(scale)}× life size`,
            sizeText: sz ? `${fmt(sz.x * 1000)} × ${fmt(sz.z * 1000)} × ${fmt(sz.y * 1000)} mm` : '' },
    ai: { lines: (s.agent && s.agent.lines) || [], running: s.agent && s.agent.running, approval: s.agent && s.agent.approval,
          listening: voice.listening, heard: ui.heard, speechOn: voice.speechOn, micAllowed: voice.allowed },
  });
  card.mesh.visible = ui.manual;
  refreshDock();
}

// ---- the conversation: the latest words, and the one round button ----
function aiTurn() {
  const a = link.state.agent || {};
  return { say: a.say || null, busy: !!(a.busy || a.running), approval: a.approval || null };
}
function runName(r) { return r.name.replace(/[_-]+/g, ' '); }
// a simulation the AI started has ended: "Ready" once the app shows its result, or why it stopped (the agent's
// "stopped" list: a value the solver refused after the AI had answered, a crash), each said once
const runsWatch = { waiting: new Set(), told: new Set() };
function runsEnded(was, s) {
  const a = s.agent || {}, now = a.computing || [], shownName = ((s.lab && s.lab.path) || '').split('/').pop().replace(/\.lab$/, '');
  was.filter((r) => !now.some((x) => x.name === r.name)).forEach((r) => runsWatch.waiting.add(r.name));
  const failed = (a.stopped || []).filter((r) => !runsWatch.told.has(r.name + '@' + r.ended));
  failed.forEach((r) => { runsWatch.told.add(r.name + '@' + r.ended); runsWatch.waiting.delete(r.name); });
  if (failed.length) return setNote(failed.map((r) => `${runName(r)} stopped: ${r.why}`).join('  ·  '), 'error');
  if (s.scene === 'lab' && runsWatch.waiting.has(shownName)) {
    runsWatch.waiting.delete(shownName);
    setNote('Ready: ' + runName({ name: shownName }));
  }
}
function wordsModel() { // only the words since the latest question; a short note while listening or understanding; the
  // simulations the AI started, while they compute
  const { say, busy } = aiTurn(), c = ui.conv, comp = (link.state.agent || {}).computing || [];
  if (ui.offline) return { text: ui.offline, kind: 'error', working: false, done: false };
  if (c.note) return { text: c.note, kind: c.noteKind, working: false, done: c.noteDone };
  const L = link.state.live || {};
  if (live.active) { // the provider's voice: what the person says as it is heard, then what the voice says
    const tail = (t) => t.length > 220 ? '…' + t.slice(-220).replace(/^\S*\s/, '') : t;
    if (L.you && L.you_s != null && L.you_s < 2.5 && (L.ai_s == null || L.you_s <= L.ai_s))
      return { text: tail(L.you), kind: 'you', working: false, done: false };
    if (busy || L.delegating) return { text: L.ai ? tail(L.ai) : 'Working on it…', kind: 'ai', working: true, done: false };
    if (comp.length) return { text: comp.map((r) => `Computing ${runName(r)}${r.done != null ? ': ' + Math.round(r.done * 100) + ' %' : '…'}`).join('  ·  '),
                              kind: 'note', working: true, done: false };
    if (L.ai) return { text: tail(L.ai), kind: 'ai', working: false, done: L.ai_s != null && L.ai_s > 1.5 };
    return { text: live.muted ? 'The microphone is off.' : 'Listening. Just speak.', kind: 'note', working: false, done: true };
  }
  const answer = say && c.askedTurn && say.turn >= c.askedTurn && (say.text || busy);
  if (answer && busy) return { text: say.text || '', kind: 'ai', working: true, done: false };
  if (comp.length) return { text: comp.map((r) => `Computing ${runName(r)}${r.done != null ? ': ' + Math.round(r.done * 100) + ' %' : '…'}`).join('  ·  '),
                            kind: 'note', working: true, done: false };
  if (answer) return { text: say.text || '', kind: 'ai', working: false, done: !!say.done };
  if (c.you) return { text: c.you, kind: 'you', working: busy, done: !busy && !c.waiting };
  return { text: '', kind: 'note', working: busy, done: true };
}
function buttonState() { // stop while the AI answers or speaks, the accent while it listens, else the microphone
  if (live.active) return aiTurn().busy || (link.state.live || {}).delegating ? 'stop' : live.muted ? 'mic' : 'listening';
  return aiTurn().busy || voice.speaking ? 'stop' : voice.handsFree || voice.listening ? 'listening' : 'mic';
}
function colormapOf(s) { return (s.scene === 'lab' ? s.lab && s.lab.colormap : s.fem && s.fem.colormap) || []; }
function refreshDock() {
  const s = link.state, f = view.facts;
  const legend = f && f.field && !(f.picture && f.picture.rgb) ? { field: f.field, unit: f.unit, lo: f.lo, hi: f.hi, colormap: colormapOf(s) } : null;
  dock.set({ legend, words: wordsModel(), button: buttonState(), manual: ui.manual });
  refreshPage(legend);
}
// the same on a computer's screen, as the window has it: the legend at the bottom left, one bar, the words above it
const pageWords = { key: '', timer: 0 };
function refreshPage(legend) {
  if (renderer.xr.isPresenting) return;
  const w = wordsModel(), el = $('words'), key = JSON.stringify([w.text, w.kind, w.working, w.done]);
  if (key !== pageWords.key) {
    pageWords.key = key;
    el.textContent = w.text;
    el.className = 'words ' + w.kind + (w.working ? ' working' : '');
    el.hidden = !w.text;
    clearTimeout(pageWords.timer);
    el.style.transition = 'none', el.style.opacity = '1';
    if (w.done && w.text) pageWords.timer = setTimeout(() => { el.style.transition = 'opacity 2s'; el.style.opacity = '0'; }, 8000);
  }
  const lg = $('legend');
  lg.hidden = !legend;
  if (legend) {
    $('lg-title').textContent = `${legend.field}${legend.unit ? ' [' + legend.unit + ']' : ''}`;
    $('lg-bar').style.background = `linear-gradient(to right, ${(legend.colormap.length ? legend.colormap : ['#ccc', '#ccc']).join(', ')})`;
    $('lg-lo').textContent = fmt(legend.lo), $('lg-hi').textContent = fmt(legend.hi);
  }
  $('talk').className = buttonState();
  $('talk').setAttribute('aria-label', buttonState() === 'stop' ? 'Stop the AI' : voice.handsFree ? 'Stop listening' : 'Talk');
  $('agentic').classList.toggle('on', !ui.manual), $('manual').classList.toggle('on', ui.manual);
}
// the person's words through the provider's voice: a phrase ends after 0.8 s without new words; a plain command in it
// is done here at once, and the bridge is told so that the voice does not hand it to the agent
const heardLive = { seen: 0, text: '', at: 0, context: '', contextAt: 0 };
function liveWords(L) {
  const n = L.you_n || 0;
  if (n < heardLive.seen) heardLive.seen = 0; // a new session
  if (n > heardLive.seen && live.active) {
    heardLive.text += (L.you || '').slice(-(n - heardLive.seen));
    heardLive.at = performance.now();
  }
  heardLive.seen = n;
}
function livePhrase() {
  if (!live.active) return;
  const now = performance.now();
  if (heardLive.text && now - heardLive.at > 800) {
    const t = heardLive.text.trim();
    heardLive.text = '';
    if (quick(t)) link.post('./live/handled', JSON.stringify({ text: t }));
  }
  const ctx = context(); // what the headset shows and the person points at, for the questions the voice hands over
  if (ctx !== heardLive.context && now - heardLive.contextAt > 500) {
    heardLive.context = ctx, heardLive.contextAt = now;
    link.post('./live/context', JSON.stringify({ context: ctx }));
  }
  const L = link.state.live || {}; // five quiet minutes: the voice is closed (it is billed while open)
  const quiet = Math.min(L.you_s ?? Infinity, L.ai_s ?? Infinity, (now - (live.startedAt || now)) / 1000);
  if (quiet > 300 && !aiTurn().busy) live.stop(), setNote('The voice paused after five quiet minutes; press the button to talk again.');
}
function setNote(text, kind = 'note', done = true) {
  ui.conv.note = text, ui.conv.noteKind = kind, ui.conv.noteDone = done;
  refreshCard();
}
function stopAI() { // "stop", the button while the AI works: its answer ends, what is not yet said of it is not
  const { say } = aiTurn();
  voice.mute(say ? say.turn : 0);
  live.quiet();
  link.stop();
  setNote('Stopped.');
}
// the provider's voice: the microphone to GPT-Live (the Mac's own voice and listening are off meanwhile); where it cannot
// start (no key on the Mac, no network), the Mac's own listening takes over
async function startLive() {
  if (!voice.allowed) await allowMic();
  if (!voice.allowed) return setNote('The microphone is not allowed on this page.', 'error'), false;
  voice.listen(false);
  setNote('Connecting the voice…', 'note', false);
  const ok = await live.start(voice.stream, voice.audio());
  if (!ok) {
    setNote(live.error || 'The voice could not start.', 'error');
    voice.listen(true);
    return false;
  }
  setNote('');
  return true;
}
function wantLive() { return (link.state.voice_provider || '') === 'openai-live'; }
async function pressTalk() { // the one round button
  if (buttonState() === 'stop') return stopAI();
  if (live.active) return live.mute(!live.muted), setNote(live.muted ? 'The microphone is off.' : 'Listening. Just speak.');
  if (wantLive() && live.state === 'off') return startLive();
  if (!voice.allowed) await allowMic();
  if (!voice.allowed) return setNote('The microphone is not allowed on this page.', 'error');
  voice.listen(!voice.handsFree);
  setNote(voice.handsFree ? 'Listening. Just speak.' : '');
}
live.addEventListener('change', () => refreshCard());
function dockPress(id) {
  if (id === 'mic') pressTalk();
  else if (id === 'manual') ui.manual = !ui.manual, refreshCard();
}

// ---- what the AI is told about the headset when it is asked something here ----
function context() {
  const t = timeRow(), f = view.facts, parts = [];
  if (f && f.field) parts.push(`the headset shows ${f.field}${f.unit ? ' [' + f.unit + ']' : ''}${t ? `, ${t.label} (${t.index + 1} of ${t.count})` : ''}${ui.trueSize ? ', at true size' : ''}`);
  const p = prober.context(f);
  if (p) parts.push(p);
  return parts.join('; ');
}
// a plain view command is done here at once; anything else is a question for the AI, with what the headset shows
function quick(text) {
  const a = quickCommand(text, fields());
  if (!a) return false;
  const s = link.state, lab = s.scene === 'lab', t = timeRow(), sec = sectionRow();
  if (a.do === 'play' || a.do === 'pause') { if (t && t.playing !== (a.do === 'play')) press({ id: 'play' }, 'down'); }
  else if (a.do === 'next' || a.do === 'prev') press({ id: a.do }, 'down');
  else if ((a.do === 'first' || a.do === 'last' || a.do === 'frame') && t) {
    // a frame said aloud is the number the card shows, counted from 1
    const f = a.do === 'first' ? 0 : a.do === 'last' ? t.count - 1 : Math.max(0, Math.min(t.count - 1, a.frame - 1));
    if (lab) view.setFrame(f), command(`lab frame ${f}`);
    else command(`fem step ${f + 1}`);
  } else if (a.do === 'cut' && sec) press({ id: 'cut:' + a.axis }, 'down');
  else if (a.do === 'field') command(`${lab ? 'lab' : 'fem'} field ${a.field}`);
  else if (a.do === 'truesize') ui.trueSize = a.on, applyScale();
  else if (a.do === 'zoom') ui.trueSize = false, ui.zoom = Math.min(40, Math.max(0.05, ui.zoom * a.by)), applyScale();
  else if (a.do === 'reset') press({ id: 'view:reset' }, 'down');
  else if (a.do === 'clearpins') press({ id: 'pins-clear' }, 'down');
  else if (a.do === 'clearmarks') press({ id: 'marks-clear' }, 'down');
  else if (a.do === 'speech') { voice.speechOn = a.on; if (!a.on) voice.stopSpeaking(); }
  else if (a.do === 'manual') ui.manual = a.on;
  else if (a.do === 'stop') { // the AI's answer if it is answering, else the playback
    if (aiTurn().busy || voice.speaking) { stopAI(); return true; }
    if (t && t.playing) press({ id: 'play' }, 'down');
  } else return false;
  setNote('Done: ' + text.trim());
  return true;
}
// what the person sees as they ask goes to the Mac first, for headset_look's moment "question"; a question never waits
// more than a second and a half for its picture
async function questionPicture(taken) {
  const p = await Promise.race([taken || nextPicture(), new Promise((r) => setTimeout(() => r(null), 1500))]);
  return p ? link.look(p.blob, 'q' + Date.now(), 'question', p.view) : { ok: false, error: 'no picture' };
}
// words from the person, said or typed: a plain command is done here; anything else goes to the AI with what they see
async function heard(text, ctx, seen) {
  if (!text.trim() || quick(text)) return;
  const c = ui.conv;
  c.note = '', c.you = text.trim(), c.waiting = true;
  c.askedTurn = ((aiTurn().say || {}).turn || 0) + 1; // the answer to it is the next turn (one said over an answer too)
  refreshCard();
  await questionPicture(seen);
  const r = await link.ask(text, ctx);
  c.waiting = false;
  if (!r.ok) setNote(r.error || 'The question did not reach the Mac.', 'error');
}
function ask(text) { return heard(text, context(), null); }
// a sentence heard (hands-free, or the grip held): into words on the Mac, then a command or a question
function understand(wav) {
  setNote('Understanding…', 'note', false);
  const ctx = context(), seen = nextPicture(); // what they see as they finish speaking, kept if it is a question
  link.voice(wav, lang, ctx, false).then((r) => {
    if (!r.text) return setNote(r.error || 'Not understood.', 'error');
    if (voice.echo(r.text)) return setNote(''); // the AI's own voice, heard back: not a question
    heard(r.text, ctx, seen);
  });
}
$('q').onkeydown = (e) => { if (e.key === 'Enter') { ask($('q').value); $('q').value = ''; } };
$('talk').onclick = () => pressTalk();
$('agentic').onclick = () => { ui.manual = false; refreshCard(); };
$('manual').onclick = () => { ui.manual = true; refreshCard(); };
async function allowMic() {
  try { await voice.allow(); } catch (e) { setNote('No microphone: ' + e.name, 'error'); }
  refreshCard();
}
voice.addEventListener('speechstart', () => { // the person talks: the AI's voice stops at once
  if (voice.speaking) voice.mute(((aiTurn().say || {}).turn) || 0);
  setNote('Listening…', 'note', false);
});
voice.addEventListener('silence', () => { if (ui.conv.note === 'Listening…') setNote(''); });
voice.addEventListener('utterance', (e) => understand(e.detail));
function talk(on) { // the grip held: the same, pressed
  if (on) { if (voice.start()) setNote('Listening…', 'note', false); return; }
  const wav = voice.stop();
  if (!wav) setNote('');
  else understand(wav);
}

// the pins as the AI reads them (openphysics-mcp headset_status): part, place in the result's coordinates, value, frame
function reportPins() {
  const f = view.facts, t = timeRow();
  link.pins(prober.pins.map(({ reading: r }) => ({
    part: r.partName || undefined, at: [r.source.x, r.source.y, r.source.z], unit: r.unit, what: r.what,
    field: f ? f.field : undefined, value: r.hasField && r.value !== null ? r.value : undefined, value_unit: f ? f.unit : undefined,
    frame: t ? t.index : undefined })));
}

// ---- the card's controls: here at once, and the same command to the app ----
function command(cmd) { link.command(cmd); }
function press(hit, phase) { // phase: down, move, up
  if (!hit || hit.id === 'card') return;
  const s = link.state, lab = s.scene === 'lab', id = hit.id;
  if (id === 'scrub' || id === 'cut') {
    card.press = phase === 'up' ? null : id;
    const t = timeRow();
    if (id === 'scrub' && t) {
      const f = Math.round(hit.at * (t.count - 1));
      if (lab) view.setFrame(f);
      if (phase === 'up') command(lab ? `lab frame ${f}` : `fem step ${f + 1}`);
    }
    if (id === 'cut' && phase === 'up') {
      const sec = sectionRow();
      if (sec && sec.on) command(`${sec.cmd} section ${'xyz'[sec.axis]} ${hit.at.toFixed(3)}`);
    }
    refreshCard();
    return;
  }
  if (phase !== 'down') return;
  const t = timeRow(), sec = sectionRow();
  if (id === 'play' && t) {
    const on = !t.playing;
    if (lab) { // paused here, the app stops on the very frame the headset shows ("lab frame" also pauses)
      const f = view.shown >= 0 ? view.shown : t.index;
      view.setPlaying(on);
      if (!on) view.setFrame(f);
      command(on ? 'lab play' : `lab frame ${f}`);
    } else command(`fem ${on ? 'play' : 'pause'}`);
  } else if ((id === 'prev' || id === 'next') && t) {
    const f = (t.index + (id === 'next' ? 1 : -1) + t.count) % t.count;
    if (lab) view.setFrame(f), command(`lab frame ${f}`);
    else command(`fem step ${id}`);
  } else if (id.startsWith('field:')) command(`${lab ? 'lab' : 'fem'} field ${id.slice(6)}`);
  else if (id.startsWith('cut:') && sec) {
    const a = id.slice(4);
    command(a === 'off' ? `${sec.cmd} section off` : a === 'flip' ? `${sec.cmd} section flip` : `${sec.cmd} section ${a} ${sec.on ? sec.fraction : 0.5}`);
  } else if (id === 'view:true') ui.trueSize = !ui.trueSize, applyScale();
  else if (id === 'view:reset') ui.trueSize = false, ui.zoom = 1, prober.clear(), applyScale(), placed = false, resetDesktopView();
  else if (id === 'view:place') startPlacing(lastController);
  else if (id === 'pins-clear') prober.clear(), reportPins();
  else if (id === 'marks-clear') link.clearMarks();
  else if (id === 'allow' || id === 'deny') link.answer(id === 'allow');
  else if (id === 'mic') { if (!voice.allowed) allowMic(); else talk(!voice.listening); }
  else if (id === 'speech') { voice.speechOn = !voice.speechOn; if (!voice.speechOn) voice.stopSpeaking(); }
  refreshCard();
}

// ---- rays: what a pointer is on ----
const raycaster = new THREE.Raycaster();
function aim(origin, dir) { // the dock's button, the card's control (under Manual), else the value on the result
  raycaster.set(origin, dir);
  if (dock.group.visible) {
    const h = raycaster.intersectObjects(dock.targets, false)[0];
    if (h) return { dock: dock.hit(h.object), distance: h.distance };
  }
  const c = card.mesh.visible ? raycaster.intersectObject(card.mesh)[0] : null;
  const b = c ? card.hit(c.uv) : null;
  if (b) return { card: b, distance: c.distance };
  const reading = prober.read(origin, dir, view.content, view.facts);
  return { reading, distance: reading ? reading.distance : Infinity };
}

// ---- controllers and hands ----
const controllers = [], tmpM = new THREE.Matrix4();
let lastController = null, hitTestable = false, placed = true;
function rayOf(c) {
  tmpM.identity().extractRotation(c.matrixWorld);
  return [new THREE.Vector3().setFromMatrixPosition(c.matrixWorld), new THREE.Vector3(0, 0, -1).applyMatrix4(tmpM).normalize()];
}
for (let i = 0; i < 2; i++) {
  const c = renderer.xr.getController(i);
  const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3(0, 0, -1)]),
                              new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.6 }));
  line.scale.z = 0.6;
  c.add(line);
  c.userData = { line, grab: null, press: null };
  scene.add(c);
  controllers.push(c);
  c.addEventListener('connected', (e) => { c.userData.source = e.data; c.userData.gamepad = e.data.gamepad; c.userData.hand = !!e.data.hand; });
  c.addEventListener('disconnected', () => { c.userData.source = null; c.userData.gamepad = null; });
  c.addEventListener('selectstart', () => {
    lastController = c;
    if (ui.placing) return placeAt(c);
    const [o, d] = rayOf(c), a = aim(o, d);
    if (a.dock) { dockPress(a.dock.id); return; }
    if (a.card) { c.userData.press = a.card; press(a.card, 'down'); return; }
    if (a.reading || hitsModel(o, d)) {
      c.userData.grab = { t: performance.now(), pos: holder.position.clone(), quat: holder.quaternion.clone(), reading: a.reading,
                          from: o.clone() };
      const other = controllers.find((x) => x !== c && x.userData.grab);
      if (other) startTwoHands(); else c.attach(holder);
    }
  });
  c.addEventListener('selectend', () => {
    if (c.userData.press) { // a slider let go off its track keeps where it was last
      const h = aimCard(c), p = c.userData.press;
      press(h && h.id === p.id ? h : p, 'up');
      c.userData.press = null;
      return;
    }
    const gr = c.userData.grab;
    if (!gr) return;
    c.userData.grab = null;
    if (two) { endTwoHands(); return; }
    scene.attach(holder);
    const [o] = rayOf(c);
    if (performance.now() - gr.t < 300 && o.distanceTo(gr.from) < 0.02) { // a tap: back where it was, and a pin
      holder.position.copy(gr.pos), holder.quaternion.copy(gr.quat);
      prober.pin(gr.reading);
      reportPins();
      refreshCard();
    }
  });
  c.addEventListener('squeezestart', () => talk(true));
  c.addEventListener('squeezeend', () => talk(false));
}
function aimCard(c) {
  if (!card.mesh.visible) return null;
  const [o, d] = rayOf(c);
  raycaster.set(o, d);
  const h = raycaster.intersectObject(card.mesh)[0];
  return h ? card.hit(h.uv) : null;
}
function hitsModel(o, d) {
  raycaster.set(o, d);
  return !!(view.content && raycaster.intersectObject(view.content, true).length);
}

// two hands on the result: the distance between them scales it, the turn of the line between them turns it
let two = null;
function startTwoHands() {
  scene.attach(holder);
  const [a, b] = controllers.map((c) => rayOf(c)[0]);
  two = { d0: Math.max(a.distanceTo(b), 1e-3), zoom0: ui.zoom, yaw0: Math.atan2(b.x - a.x, b.z - a.z), rot0: holder.rotation.y,
          mid0: a.clone().add(b).multiplyScalar(0.5), pos0: holder.position.clone() };
  ui.trueSize = false;
}
function twoHands() {
  if (!two) return;
  const [a, b] = controllers.map((c) => rayOf(c)[0]);
  ui.zoom = Math.min(40, Math.max(0.05, two.zoom0 * a.distanceTo(b) / two.d0));
  const dyaw = Math.atan2(b.x - a.x, b.z - a.z) - two.yaw0;
  holder.rotation.y = two.rot0 + dyaw;
  const mid = a.clone().add(b).multiplyScalar(0.5);
  holder.position.copy(two.pos0).add(mid.sub(two.mid0));
  applyScale();
}
function endTwoHands() {
  two = null;
  for (const c of controllers) c.userData.grab = null;
  refreshCard();
}

// placing on a real surface: the WebXR hit test along the controller's ray, a ring where it meets the room
const reticle = new THREE.Mesh(new THREE.RingGeometry(0.04, 0.05, 40).rotateX(-Math.PI / 2), new THREE.MeshBasicMaterial({ color: 0xE8551C }));
reticle.matrixAutoUpdate = false, reticle.visible = false;
scene.add(reticle);
let hitSource = null;
async function startPlacing(c) {
  const session = renderer.xr.getSession(), src = c && c.userData.source;
  if (!session || !hitTestable || !src) return;
  try {
    if (hitSource) hitSource.cancel();
    hitSource = await session.requestHitTestSource({ space: src.targetRaySpace });
    ui.placing = true;
  } catch (e) { ui.placing = false; }
  refreshCard();
}
function placeAt() {
  if (reticle.visible) {
    const p = new THREE.Vector3().setFromMatrixPosition(reticle.matrix), head = new THREE.Vector3();
    renderer.xr.getCamera().getWorldPosition(head);
    holder.position.copy(p);
    holder.rotation.set(0, Math.atan2(head.x - p.x, head.z - p.z), 0);
    ui.tableY = p.y;
    placeCard(head);
  }
  if (hitSource) hitSource.cancel(), hitSource = null;
  ui.placing = false, reticle.visible = false;
  refreshCard();
}
function placeCard(head) { // the card to the result's left, the dock to its right, both facing you at reading height
  const right = new THREE.Vector3().subVectors(holder.position, head).setY(0).normalize();
  const left = new THREE.Vector3(right.z, 0, -right.x);
  card.mesh.position.copy(holder.position).addScaledVector(left, 0.36);
  card.mesh.position.y = Math.max(holder.position.y + 0.16, head.y - 0.14);
  card.mesh.lookAt(head.x, card.mesh.position.y, head.z);
  dock.group.position.copy(holder.position).addScaledVector(left, -0.3);
  dock.group.position.y = Math.max(holder.position.y + 0.08, head.y - 0.26);
  dock.group.lookAt(head.x, dock.group.position.y, head.z);
}
// a panel turns to face you lazily: only once you have walked a good way round, and then smoothly
function turnToward(obj, head, dt) {
  const want = Math.atan2(head.x - obj.position.x, head.z - obj.position.z);
  let d = want - obj.rotation.y;
  d = Math.atan2(Math.sin(d), Math.cos(d));
  if (Math.abs(d) > 0.45) obj.userData.turning = true;
  if (obj.userData.turning && !controllers.some((c) => c.userData.press)) {
    obj.rotation.set(0, obj.rotation.y + Math.sign(d) * Math.min(Math.abs(d), 1.6 * dt), 0);
    if (Math.abs(d) < 0.08) obj.userData.turning = false;
  }
}

// ---- AR and VR ----
async function enter(mode) {
  if (!voice.allowed) await allowMic().catch(() => {}); // asked before entering, so no prompt opens inside the headset
  voice.audio();
  const session = await navigator.xr.requestSession(mode, {
    requiredFeatures: ['local-floor'], optionalFeatures: ['hand-tracking', ...(mode === 'immersive-ar' ? ['hit-test'] : [])],
  });
  const volume = !!(view.content && view.content.getObjectByName('volume'));
  // one part is cheap to draw sharper than the default; a ray-marched volume is not, and keeps the default
  renderer.xr.setFramebufferScaleFactor(volume ? 1.0 : 1.5);
  renderer.xr.setFoveation(volume ? 0.3 : 0); // the card's text stays sharp when nothing heavy is drawn
  health.session = { mode, start: performance.now(), frames: 0, worst: 0, low: null, scale: volume ? 1.0 : 1.5, foveation: volume ? 0.3 : 0 };
  health.window = [];
  renderer.xr.setReferenceSpaceType('local-floor');
  await renderer.xr.setSession(session);
  hitTestable = mode === 'immersive-ar' && !!(session.enabledFeatures ? session.enabledFeatures.includes('hit-test') : true);
  scene.background = mode === 'immersive-ar' ? null : PAGE;
  placed = false;
  dock.group.visible = true;
  if (wantLive()) startLive(); // the provider's voice, or the Mac's listening where it cannot start
  else if (voice.allowed) voice.listen(true); // in the headset nothing is pressed to talk: it listens
  session.addEventListener('end', () => {
    scene.background = PAGE; hitTestable = false; ui.placing = false; dock.group.visible = false; resetDesktopView(); refreshCard();
    live.stop(); // the provider's voice is billed while it is open
    sendHealth(); // the session's last numbers, while they are still its own
    health.session = null, health.window = [];
  });
  sendHealth();
  refreshCard();
}
if (navigator.xr) {
  navigator.xr.isSessionSupported('immersive-ar').then((ok) => { $('ar').hidden = !ok; }).catch(() => {});
  navigator.xr.isSessionSupported('immersive-vr').then((ok) => { $('vr').hidden = !ok; }).catch(() => {});
}
$('ar').onclick = () => enter('immersive-ar').catch((e) => setNote('AR could not start: ' + e.message, 'error'));
$('vr').onclick = () => enter('immersive-vr').catch((e) => setNote('VR could not start: ' + e.message, 'error'));

// ---- on a computer: turn, zoom, point, click ----
const orbit = { az: 0.55, el: 0.32, dist: 0.95 };
function resetDesktopView() {
  if (renderer.xr.isPresenting) return;
  holder.position.set(0, 1.0, 0), holder.rotation.set(0, 0, 0);
  card.mesh.position.set(-0.36, 1.12, 0.05), card.mesh.rotation.set(0, 0.45, 0);
  orbit.az = 0.55, orbit.el = 0.32, orbit.dist = 0.95;
}
function desktopCamera() {
  const target = new THREE.Vector3(-0.1, 1.1, 0);
  camera.position.set(target.x + orbit.dist * Math.sin(orbit.az) * Math.cos(orbit.el), target.y + orbit.dist * Math.sin(orbit.el),
                      target.z + orbit.dist * Math.cos(orbit.az) * Math.cos(orbit.el));
  camera.lookAt(target);
}
const mouse = { down: null, last: null, aim: null };
function mouseRay(e) {
  const r = renderer.domElement.getBoundingClientRect();
  const ndc = new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(ndc, camera);
  return [raycaster.ray.origin.clone(), raycaster.ray.direction.clone()];
}
renderer.domElement.addEventListener('pointerdown', (e) => {
  const [o, d] = mouseRay(e), a = aim(o, d);
  mouse.down = { x: e.clientX, y: e.clientY, card: a.card, reading: a.reading, moved: false };
  if (a.card) press(a.card, 'down');
  renderer.domElement.setPointerCapture(e.pointerId);
});
renderer.domElement.addEventListener('pointermove', (e) => {
  const [o, d] = mouseRay(e);
  if (mouse.down) {
    const dx = e.clientX - mouse.down.x, dy = e.clientY - mouse.down.y;
    if (Math.hypot(dx, dy) > 4) mouse.down.moved = true;
    if (mouse.down.card && (mouse.down.card.id === 'scrub' || mouse.down.card.id === 'cut')) {
      const h = aim(o, d).card;
      if (h && h.id === mouse.down.card.id) press(h, 'move'), mouse.down.card = h;
    } else if (!mouse.down.card && mouse.last) {
      orbit.az -= (e.clientX - mouse.last.x) * 0.006;
      orbit.el = Math.max(-0.3, Math.min(1.3, orbit.el + (e.clientY - mouse.last.y) * 0.004));
    }
  } else {
    const a = aim(o, d);
    prober.show(a.reading || null);
    if (a.reading || prober.marker.visible !== !!a.reading) refreshCard();
  }
  mouse.ray = [o, d];
  mouse.last = { x: e.clientX, y: e.clientY };
});
renderer.domElement.addEventListener('pointerup', (e) => {
  const m = mouse.down;
  mouse.down = null;
  if (!m) return;
  if (m.card) press(m.card, 'up');
  else if (!m.moved && m.reading) prober.pin(m.reading), reportPins(), refreshCard();
});
renderer.domElement.addEventListener('wheel', (e) => {
  e.preventDefault();
  orbit.dist = Math.max(0.2, Math.min(6, orbit.dist * Math.exp(e.deltaY * 0.001)));
}, { passive: false });
addEventListener('keydown', (e) => {
  if (e.target === $('q')) return;
  const t = timeRow();
  if (e.key === ' ' && t) press({ id: 'play' }, 'down'), e.preventDefault();
  if ((e.key === 'ArrowRight' || e.key === 'ArrowLeft') && t) press({ id: e.key === 'ArrowRight' ? 'next' : 'prev' }, 'down');
});

// ---- what the person sees, as a picture for the AI (openphysics-mcp headset_look) ----
// The result, the card, the pointers, the pins and the marks, drawn once more from between the person's eyes with a
// 70 degree view (on a screen: the window's own view), into a picture of its own. Never the room: the headset gives the
// page no camera picture, and where passthrough would show the room the picture has the studio's colour.
const LOOK_W = 960, LOOK_H = 720;
const lookCamera = new THREE.PerspectiveCamera(70, LOOK_W / LOOK_H, 0.01, 200);
lookCamera.viewport = new THREE.Vector4(0, 0, LOOK_W, LOOK_H); // a particle's size in pixels reads it (scene.js)
let lookTarget = null, lookAnswered = null;
const pictureWaiting = []; // who waits for the next picture: taken after the next frame is drawn
function nextPicture() { return new Promise((resolve) => pictureWaiting.push(resolve)); }
function picture() { // drawn now and read back at once: RGBA, top row first
  if (!lookTarget) {
    lookTarget = new THREE.WebGLRenderTarget(LOOK_W, LOOK_H, { samples: 4 });
    // drawn as the headset's own frames are: tone-mapped and sRGB-encoded by the shaders into plain 8-bit storage.
    // three r165 does that for the screen and for its own XR target only, which it knows by this flag
    lookTarget.texture.colorSpace = THREE.SRGBColorSpace;
    lookTarget.texture.internalFormat = 'RGBA8';
    lookTarget.isXRRenderTarget = true;
  }
  const xr = renderer.xr.isPresenting;
  if (xr) renderer.xr.getCamera().matrixWorld.decompose(lookCamera.position, lookCamera.quaternion, lookCamera.scale), lookCamera.fov = 70;
  else lookCamera.position.copy(camera.position), lookCamera.quaternion.copy(camera.quaternion), lookCamera.fov = camera.fov;
  lookCamera.scale.set(1, 1, 1);
  lookCamera.updateProjectionMatrix();
  lookCamera.updateMatrixWorld(true);
  const prev = renderer.getRenderTarget();
  renderer.xr.enabled = false; // one view of our own, as three's CubeCamera draws inside a session
  renderer.setRenderTarget(lookTarget);
  renderer.render(scene, lookCamera);
  renderer.setRenderTarget(prev);
  renderer.xr.enabled = true;
  const raw = new Uint8Array(LOOK_W * LOOK_H * 4), out = new Uint8ClampedArray(raw.length), h = PAGE.getHex();
  renderer.readRenderTargetPixels(lookTarget, 0, 0, LOOK_W, LOOK_H, raw);
  const bg = [(h >> 16) & 255, (h >> 8) & 255, h & 255];
  for (let y = 0; y < LOOK_H; y++) { // bottom row first in GL; over the studio colour where it is not opaque (AR)
    const src = (LOOK_H - 1 - y) * LOOK_W * 4, dst = y * LOOK_W * 4;
    for (let x = 0; x < LOOK_W * 4; x += 4) {
      const a = 1 - raw[src + x + 3] / 255;
      out[dst + x] = raw[src + x] + a * bg[0], out[dst + x + 1] = raw[src + x + 1] + a * bg[1], out[dst + x + 2] = raw[src + x + 2] + a * bg[2];
      out[dst + x + 3] = 255;
    }
  }
  return { pixels: out, view: xr ? (scene.background ? 'vr' : 'ar') : 'screen' };
}
function jpeg(pixels) {
  const data = new ImageData(pixels, LOOK_W, LOOK_H);
  if (typeof OffscreenCanvas !== 'undefined') {
    const c = new OffscreenCanvas(LOOK_W, LOOK_H);
    c.getContext('2d').putImageData(data, 0, 0);
    return c.convertToBlob({ type: 'image/jpeg', quality: 0.85 });
  }
  const c = document.createElement('canvas');
  c.width = LOOK_W, c.height = LOOK_H;
  c.getContext('2d').putImageData(data, 0, 0);
  return new Promise((resolve) => c.toBlob(resolve, 'image/jpeg', 0.85));
}
function takePictures() { // after a frame is drawn, for all who wait
  const waiting = pictureWaiting.splice(0);
  let p;
  try { p = picture(); } catch (e) { renderer.xr.enabled = true; waiting.forEach((r) => r(null)); return; }
  jpeg(p.pixels).then((blob) => waiting.forEach((r) => r(blob ? { blob, view: p.view } : null)), () => waiting.forEach((r) => r(null)));
}

// ---- how the headset runs, for whoever looks after it: the bridge keeps the report (openphysics-mcp headset_status reads it)
// and writes each headset session's last one beside the headset folder, so a trial in the Quest leaves its numbers ----
const health = { errors: [], window: [], session: null, sentAt: 0, device: null };
function noteError(text) {
  health.errors.push({ t: Math.round(Date.now() / 1000), text: String(text).slice(0, 300) });
  if (health.errors.length > 10) health.errors.shift();
  sendHealth(1000);
}
addEventListener('error', (e) => noteError((e.message || 'error') + (e.filename ? ` (${e.filename.split('/').pop()}:${e.lineno})` : '')));
addEventListener('unhandledrejection', (e) => noteError('unhandled: ' + ((e.reason && e.reason.message) || e.reason)));
function device() { // once: what the page runs on
  if (health.device) return health.device;
  const gl = renderer.getContext(), dbg = gl.getExtension('WEBGL_debug_renderer_info');
  return (health.device = { ua: navigator.userAgent, gpu: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
                            dpr: devicePixelRatio, max_texture: renderer.capabilities.maxTextureSize });
}
function noteFrame(ms) { // a headset frame: the last 5 s, and the session's worst
  const S = health.session;
  if (!S) return;
  S.frames++, S.worst = Math.max(S.worst, ms);
  health.window.push(ms);
  if (health.window.length > 1000) health.window.shift();
  let sum = 0, k = health.window.length;
  while (k > 0 && sum < 5000) sum += health.window[--k];
  if (sum >= 5000) { // a full 5 s window: its rate, and the lowest of the session
    const fps = (health.window.length - k) / (sum / 1000);
    S.low = S.low === null ? fps : Math.min(S.low, fps);
  }
}
function report() {
  const S = health.session, session = renderer.xr.getSession(), w = health.window, ms = w.slice(-Math.min(w.length, 360));
  const total = ms.reduce((a, b) => a + b, 0), layer = renderer.xr.getBaseLayer ? renderer.xr.getBaseLayer() : null;
  const vol = view.content && view.content.getObjectByName('volume'), st = view.content ? view.content.userData.stats : null;
  return {
    presenting: renderer.xr.isPresenting, mode: S ? S.mode : null,
    fps: S && total > 0 ? +(ms.length / (total / 1000)).toFixed(1) : null, fps_low: S && S.low !== null ? +S.low.toFixed(1) : null,
    frame_ms_worst: S ? +S.worst.toFixed(1) : null, frames: S ? S.frames : 0, session_s: S ? Math.round((performance.now() - S.start) / 1000) : 0,
    volume_steps: vol ? Math.round(vol.material.uniforms.maxSteps.value) : null,
    features: session && session.enabledFeatures ? [...session.enabledFeatures] : null, blend: session ? session.environmentBlendMode || null : null,
    frame_rate: session && session.frameRate ? session.frameRate : null,
    frame_rates: session && session.supportedFrameRates ? [...session.supportedFrameRates] : null,
    eye_pixels: layer ? [Math.round(layer.framebufferWidth / 2), layer.framebufferHeight] : null,
    foveation: S ? S.foveation : null, scale: S ? S.scale : null,
    content: view.content ? { kind: view.kind, title: link.state.title || '', ...(st || {}), mb: +((view.bytes || 0) / 1e6).toFixed(1) } : null,
    errors: health.errors, device: device(),
    heap_mb: performance.memory ? +(performance.memory.usedJSHeapSize / 1e6).toFixed(1) : null,
  };
}
function sendHealth(within = 0) { // now, or as soon as `within` ms have passed since the last report
  const now = performance.now(), wait = within - (now - health.sentAt);
  if (wait > 0) {
    if (!health.later) health.later = setTimeout(() => { health.later = null; sendHealth(); }, wait);
    return;
  }
  health.sentAt = now;
  link.health(report());
}
setInterval(() => sendHealth(), 5000);

// ---- every frame ----
const head = new THREE.Vector3(), fwd = new THREE.Vector3();
let lastT = 0, frameMs = 14, quality = 200, dockAt = 0;
let perfMs = 0, drawnAt = 0;
const fpsCap = Number(params.get('fps')) || 0; // ?fps=N: at most N frames a second on a screen (a test on a busy machine)
renderer.setAnimationLoop((t, frame) => {
  if (fpsCap && !renderer.xr.isPresenting) {
    if (t - drawnAt < 1000 / fpsCap) return;
    drawnAt = t;
  }
  if (lastT) perfMs = perfMs * 0.9 + (t - lastT) * 0.1; // how long frames take, for the page test
  const dt = lastT ? Math.min(0.1, (t - lastT) / 1000) : 0;
  lastT = t;
  view.update(dt);
  if (renderer.xr.isPresenting) {
    const xc = renderer.xr.getCamera();
    if (!placed) { // in front of you at table height, the card to its left, both facing you
      xc.getWorldPosition(head), xc.getWorldDirection(fwd);
      fwd.y = 0, fwd.normalize();
      holder.position.copy(head).addScaledVector(fwd, 0.5);
      holder.position.y = head.y - 0.42;
      holder.rotation.set(0, Math.atan2(-fwd.x, -fwd.z), 0);
      placeCard(head);
      ui.tableY = holder.position.y;
      placed = true;
    }
    xc.getWorldPosition(head); // the card and the dock turn to face you, lazily
    turnToward(card.mesh, head, dt), turnToward(dock.group, head, dt);
    dock.update(voice.level);
    let pointed = null;
    for (const c of controllers) {
      const gp = c.userData.gamepad;
      const [o, d] = rayOf(c);
      if (c.userData.press && (c.userData.press.id === 'scrub' || c.userData.press.id === 'cut')) {
        const h = aimCard(c);
        if (h && h.id === c.userData.press.id) c.userData.press = h, press(h, 'move');
      } else if (!c.userData.grab && !ui.placing) {
        const a = aim(o, d);
        if (a.reading && (!pointed || a.distance < pointed.distance)) pointed = a.reading;
        c.userData.line.scale.z = a.card || a.dock ? a.distance : a.reading ? a.reading.distance : 0.6;
      }
      if (!gp) continue;
      const sx = gp.axes[2] || 0, sy = gp.axes[3] || 0; // thumbstick: up bigger, down smaller; sideways turns
      if (Math.abs(sy) > 0.25 && !ui.trueSize) ui.zoom = Math.min(40, Math.max(0.05, ui.zoom * (1 - sy * 2 * dt))), applyScale();
      if (Math.abs(sx) > 0.35) holder.rotation.y -= sx * 1.6 * dt;
      const btn = (k) => !!(gp.buttons[k] && gp.buttons[k].pressed);
      const a = btn(4), b = btn(5), prev = c.userData.buttons || {};
      if (a && !prev.a) startPlacing(c);
      if (b && !prev.b) ui.trueSize = !ui.trueSize, applyScale(), refreshCard();
      c.userData.buttons = { a, b };
    }
    twoHands();
    if (!mouse.down) {
      const before = prober.current;
      prober.show(pointed);
      if (pointed !== before) refreshCard();
    }
    if (ui.placing && hitSource && frame) {
      const hits = frame.getHitTestResults(hitSource), ref = renderer.xr.getReferenceSpace();
      const pose = hits.length ? hits[0].getPose(ref) : null;
      reticle.visible = !!pose;
      if (pose) reticle.matrix.fromArray(pose.transform.matrix);
    }
    // a ray-marched volume follows the headset's pace: fewer steps when frames come late, more when there is room
    frameMs = frameMs * 0.95 + (dt * 1000) * 0.05;
    if (dt > 0) noteFrame(dt * 1000);
    const vol = view.content && view.content.getObjectByName('volume');
    if (vol) {
      quality = frameMs > 15 ? Math.max(64, quality * 0.97) : frameMs < 12 ? Math.min(360, quality * 1.01) : quality;
      vol.material.uniforms.maxSteps.value = quality;
    }
  } else {
    desktopCamera();
    if (view.content && !mouse.down && !prober.current) holder.rotation.y += dt * 0.12; // on a screen it turns slowly
  }
  renderer.render(scene, camera);
  if (pictureWaiting.length) takePictures();
  if (t - dockAt > 200) dockAt = t, refreshDock(), livePhrase(); // the button follows the voice; phrases heard live
});
resetDesktopView();
refreshCard();

// ---- for the page test (tools/xr/xr_test.py): what is shown, and the controls by name ----
window.openphysics = {
  state: () => link.state,
  view: () => ({ kind: view.kind, shown: view.shown, target: view.target, playing: view.playing, frames: [...view.frames.keys()].sort((a, b) => a - b),
                 stats: view.content ? view.content.userData.stats : null, facts: view.facts ? { field: view.facts.field, unit: view.facts.unit, lo: view.facts.lo, hi: view.facts.hi, parts: view.facts.parts } : null,
                 section: view.facts && view.facts.raw ? view.facts.raw.section || null : null, key: view.key,
                 scale: model.scale.x, error: view.error, bytes: view.bytes }),
  buttons: () => card.buttons.map((b) => b.id),
  press: (id, at = 0.5) => press({ id, at }, 'down') || press({ id, at }, 'up'),
  aimAt: (x, y) => { // a point of the window, as the mouse points (0..1 from the top left)
    const r = renderer.domElement.getBoundingClientRect();
    const [o, d] = mouseRay({ clientX: r.left + x * r.width, clientY: r.top + y * r.height });
    const a = aim(o, d);
    prober.show(a.reading || null);
    refreshCard();
    return a.card ? { card: a.card.id } : a.reading ? { text: a.reading.text, where: a.reading.where, value: a.reading.value, part: a.reading.partName } : null;
  },
  project: (x, y, z) => { // where a point of the result (its own glTF axes) is on the window, 0..1
    if (!view.content) return null;
    const p = new THREE.Vector3(x, y, z).applyMatrix4(view.content.matrixWorld).project(camera);
    return { x: (p.x + 1) / 2, y: (1 - p.y) / 2 };
  },
  pin: () => { prober.pin(prober.current); reportPins(); refreshCard(); return prober.pins.length; },
  marks: () => marksGroup.children.map((g) => ({ label: g.userData.label, at: g.position.toArray() })),
  buttonAt: (id, at = 0.5) => { // a card control on the window (at: how far along it), 0..1 from the top left
    const b = card.buttons.find((x) => x.id === id);
    if (!b) return null;
    const geo = card.mesh.geometry.parameters, cw = card.canvas.width, ch = card.canvas.height;
    const local = new THREE.Vector3(((b.x + b.w * at) / cw - 0.5) * geo.width, (0.5 - (b.y + b.h / 2) / ch) * geo.height, 0);
    const p = local.applyMatrix4(card.mesh.matrixWorld).project(camera);
    return { x: (p.x + 1) / 2, y: (1 - p.y) / 2 };
  },
  context,
  lastCommand: () => link.lastCommand,
  pins: () => prober.pins.length,
  xr: () => ({ presenting: renderer.xr.isPresenting, holder: holder.position.toArray(), yaw: holder.rotation.y, scale: model.scale.x,
               trueSize: ui.trueSize, zoom: ui.zoom, placing: ui.placing, reticle: reticle.visible, tableY: ui.tableY,
               card: card.mesh.position.toArray(), cardYaw: card.mesh.rotation.y, background: scene.background ? 'studio' : 'passthrough' }),
  world: (x, y, z) => view.content ? new THREE.Vector3(x, y, z).applyMatrix4(view.content.matrixWorld).toArray() : null,
  cardWorld: (id) => { // a card control's place in the room
    const b = card.buttons.find((x) => x.id === id), geo = card.mesh.geometry.parameters;
    return b ? new THREE.Vector3(((b.x + b.w / 2) / card.canvas.width - 0.5) * geo.width, (0.5 - (b.y + b.h / 2) / card.canvas.height) * geo.height, 0)
      .applyMatrix4(card.mesh.matrixWorld).toArray() : null;
  },
  reading: () => { const r = prober.current; return r ? { value: r.value, source: [r.source.x, r.source.y, r.source.z], part: r.partName, what: r.what } : null; },
  // a controller aimed from the camera at a card control or a point of the result (glTF axes), and its events: the
  // headset's input path, run without a headset (the page test)
  pointController: (i, target) => {
    const c = controllers[i], from = camera.position.clone();
    let to;
    if (target.card) {
      const b = card.buttons.find((x) => x.id === target.card), geo = card.mesh.geometry.parameters;
      if (!b) return false;
      to = new THREE.Vector3(((b.x + b.w * (target.at ?? 0.5)) / card.canvas.width - 0.5) * geo.width,
                             (0.5 - (b.y + b.h / 2) / card.canvas.height) * geo.height, 0).applyMatrix4(card.mesh.matrixWorld);
    } else to = new THREE.Vector3(...target.point).applyMatrix4(view.content.matrixWorld);
    c.matrix.lookAt(from, to, new THREE.Vector3(0, 1, 0)).setPosition(from); // the ray's space is placed by its matrix
    c.matrix.decompose(c.position, c.quaternion, c.scale);
    c.updateMatrixWorld(true);
    return true;
  },
  controllerEvent: (i, type) => controllers[i].dispatchEvent({ type, data: {} }),
  perf: () => { let objects = 0; scene.traverse(() => objects++);
    return { frameMs: +perfMs.toFixed(1), heapMB: performance.memory ? +(performance.memory.usedJSHeapSize / 1e6).toFixed(1) : null, objects,
             programs: renderer.info.programs ? renderer.info.programs.length : null, geometries: renderer.info.memory.geometries, textures: renderer.info.memory.textures }; },
  step: (n = 1) => { for (let i = 0; i < n; i++) view.update(1 / 60), renderer.render(scene, camera); return view.shown; }, // a hidden page has no frames of its own
  hold: (on) => { mouse.down = on ? { x: 0, y: 0, moved: true } : null; }, // stops the slow turn while a test reads values
  // what the AI's picture shows: where a point of the result (glTF axes) is in the last picture, 0..1 from the top left;
  // the window's own colour at a point (drawn and read at once); a picture taken and sent as a question's would be
  lookProject: (x, y, z) => {
    if (!view.content) return null;
    const p = new THREE.Vector3(x, y, z).applyMatrix4(view.content.matrixWorld).project(lookCamera);
    return { x: (p.x + 1) / 2, y: (1 - p.y) / 2, inFront: p.z < 1 };
  },
  screenPixel: (x, y) => {
    renderer.render(scene, camera);
    const gl = renderer.getContext(), b = new Uint8Array(4);
    gl.readPixels(Math.floor(x * gl.drawingBufferWidth), Math.floor((1 - y) * gl.drawingBufferHeight), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
    return [...b];
  },
  health: () => report(),
  manual: (on) => { if (on !== undefined) ui.manual = !!on, refreshCard(); return ui.manual; },
  dock: () => ({ visible: dock.group.visible, words: { ...dock.state.words }, button: dock.state.button, manual: dock.state.manualOn,
                 legend: dock.keys.legend ? JSON.parse(dock.keys.legend) : null, wordsOpacity: dock.words.mesh.material.opacity,
                 at: dock.group.position.toArray(), yaw: dock.group.rotation.y, page: { words: $('words').textContent,
                 wordsHidden: $('words').hidden, talk: $('talk').className, legend: !$('legend').hidden } }),
  dockPress: (id) => dockPress(id),
  cardShown: () => card.mesh.visible,
  dockWorld: (id) => { // a dock control's place in the room
    const o = id === 'manual' ? dock.manual.mesh : dock.circle;
    return o.getWorldPosition(new THREE.Vector3()).toArray();
  },
  hear: (text) => heard(text, context(), null), // as if these words had been said and recognised
  questionPicture: async () => {
    const p = await Promise.race([nextPicture(), new Promise((r) => setTimeout(() => r(null), 5000))]);
    if (!p) return null;
    const r = await link.look(p.blob, 'q' + Date.now(), 'question', p.view);
    return { ok: r.ok, bytes: p.blob.size, view: p.view };
  },
};
