// dock.js - the headset's whole interface while you talk to it, as the window's "two buttons and one bar" (docs/app/
// DESIGN.md): the legend, the latest words, and one round button. The words are what you said as it was heard, then
// the AI's answer as it is written and spoken; they fade 8 s after the answer ends. The button is a black circle with a
// microphone; while it listens it turns to the accent with a ring that breathes with your voice; while the AI works it
// is a stop square. Manual, beside it, shows the controls card. Everything else is said aloud.
import * as THREE from 'three';
import { fmt } from './card.js';

const T = { card: '#FFFFFF', ink: '#111111', ink2: '#2A2A2C', dim: '#7A7A80', hairline: '#E3E1DC', accent: '#E8551C', bad: '#D23B3B' };
const F = '-apple-system, "SF Pro Text", Inter, Roboto, system-ui, sans-serif';
const M = 'ui-monospace, "SF Mono", Menlo, monospace';
const WM = 0.26; // the dock's width in the room, m

function canvasMesh(w, h, widthM) {
  const c = document.createElement('canvas');
  c.width = w, c.height = h;
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 4;
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(widthM, widthM * h / w),
                              new THREE.MeshBasicMaterial({ map: tex, transparent: true, toneMapped: false, depthWrite: false }));
  mesh.renderOrder = 12;
  return { c, g: c.getContext('2d'), tex, mesh };
}

function round(g, x, y, w, h, r, fill, stroke) {
  g.beginPath();
  g.roundRect(x, y, w, h, r);
  if (fill) g.fillStyle = fill, g.fill();
  if (stroke) g.strokeStyle = stroke, g.lineWidth = 2, g.stroke();
}

export class Dock {
  constructor() {
    this.group = new THREE.Group();
    this.group.name = 'dock';
    this.legend = canvasMesh(1024, 230, WM);
    this.legend.mesh.position.y = 0.118;
    this.words = canvasMesh(1024, 290, WM);
    this.words.mesh.position.y = 0.046;
    const R = this.R = 0.021;
    this.circle = new THREE.Mesh(new THREE.CircleGeometry(R, 48), new THREE.MeshBasicMaterial({ color: T.ink, toneMapped: false }));
    this.circle.position.set(0, -0.03, 0.001);
    this.circle.renderOrder = 13;
    this.ring = new THREE.Mesh(new THREE.RingGeometry(R * 1.12, R * 1.32, 48),
                               new THREE.MeshBasicMaterial({ color: T.accent, transparent: true, opacity: 0, toneMapped: false, depthWrite: false }));
    this.ring.position.copy(this.circle.position);
    this.ring.renderOrder = 13;
    this.glyphs = { mic: this.glyph('mic'), stop: this.glyph('stop') };
    this.glyph_ = new THREE.Mesh(new THREE.PlaneGeometry(R * 2, R * 2),
                                 new THREE.MeshBasicMaterial({ map: this.glyphs.mic, transparent: true, toneMapped: false, depthWrite: false }));
    this.glyph_.position.set(0, -0.03, 0.002);
    this.glyph_.renderOrder = 14;
    this.manual = canvasMesh(384, 128, 0.066);
    this.manual.mesh.position.set(0.082, -0.03, 0.001);
    this.dot = new THREE.Mesh(new THREE.CircleGeometry(0.0035, 24),
                              new THREE.MeshBasicMaterial({ color: T.accent, transparent: true, opacity: 0, toneMapped: false, depthWrite: false }));
    this.dot.renderOrder = 14;
    this.group.add(this.legend.mesh, this.words.mesh, this.ring, this.circle, this.glyph_, this.manual.mesh, this.dot);
    this.circle.userData.id = this.glyph_.userData.id = 'mic';
    this.manual.mesh.userData.id = 'manual';
    this.targets = [this.circle, this.glyph_, this.manual.mesh]; // what a ray can press
    this.keys = {};
    this.state = { words: { text: '', kind: 'note', working: false, doneAt: 0, changedAt: 0 }, button: 'mic', manualOn: false };
    this.drawManual(false);
  }

  glyph(kind) { // white, on the round button: a microphone, or a stop square
    const c = document.createElement('canvas'), g = c.getContext('2d'), s = 256, r = 128;
    c.width = c.height = s;
    g.strokeStyle = g.fillStyle = '#FFFFFF', g.lineWidth = 12, g.lineCap = 'round';
    const cx = s / 2, cy = s / 2;
    if (kind === 'stop') g.fillRect(cx - 0.36 * r, cy - 0.36 * r, 0.72 * r, 0.72 * r);
    else { // the window's microphone: a capsule, its cradle, its stand and base
      round(g, cx - 0.18 * r, cy - 0.52 * r, 0.36 * r, 0.62 * r, 0.18 * r, '#FFFFFF');
      g.beginPath(), g.moveTo(cx - 0.34 * r, cy - 0.02 * r), g.quadraticCurveTo(cx - 0.34 * r, cy + 0.32 * r, cx, cy + 0.32 * r);
      g.quadraticCurveTo(cx + 0.34 * r, cy + 0.32 * r, cx + 0.34 * r, cy - 0.02 * r), g.stroke();
      g.beginPath(), g.moveTo(cx, cy + 0.32 * r), g.lineTo(cx, cy + 0.5 * r), g.stroke();
      g.beginPath(), g.moveTo(cx - 0.22 * r, cy + 0.5 * r), g.lineTo(cx + 0.22 * r, cy + 0.5 * r), g.stroke();
    }
    const t = new THREE.CanvasTexture(c);
    t.colorSpace = THREE.SRGBColorSpace;
    return t;
  }

  // what it shows: legend {field, unit, lo, hi, colormap} or null; words {text, kind: you|ai|note|error, working, done};
  // button: mic, listening or stop; manual: the controls card is shown
  set({ legend, words, button, manual }) {
    const k = (o) => JSON.stringify(o);
    if (k(legend) !== this.keys.legend) this.keys.legend = k(legend), this.drawLegend(legend);
    if (words && k(words) !== this.keys.words) {
      const w = this.state.words, now = performance.now();
      if (words.text !== w.text) w.changedAt = now;
      if (words.done && !w.done) w.doneAt = now;
      Object.assign(w, words);
      if (!words.done) w.doneAt = 0;
      this.keys.words = k(words);
      this.drawWords(w);
    }
    this.state.button = button;
    this.glyph_.material.map = button === 'stop' ? this.glyphs.stop : this.glyphs.mic;
    this.circle.material.color.set(button === 'listening' ? T.accent : T.ink);
    if (!!manual !== this.state.manualOn) this.state.manualOn = !!manual, this.drawManual(!!manual);
  }

  // every frame: the ring breathes with the voice heard, the dot while the AI works, the words fade 8 s after the end
  update(level = 0) {
    const t = performance.now() / 1000, w = this.state.words;
    const listening = this.state.button === 'listening';
    this.ring.material.opacity = listening ? 0.25 + 0.55 * Math.max(level, 0.5 + 0.5 * Math.sin(4 * t)) * (0.4 + 0.6 * level) : 0;
    this.ring.scale.setScalar(1 + 0.12 * level);
    this.dot.material.opacity = w.working ? 0.35 + 0.65 * (0.5 + 0.5 * Math.sin(5 * t)) : 0;
    const since = w.done && w.doneAt ? (performance.now() - w.doneAt) / 1000 : 0;
    const alpha = !w.text ? 0 : since < 8 ? 1 : Math.max(0, 1 - (since - 8) / 2);
    this.words.mesh.material.opacity = alpha;
    this.words.mesh.visible = alpha > 0.01;
  }

  hit(object) { return object && object.userData.id ? { id: object.userData.id } : null; }

  drawLegend(l) {
    const { c, g, tex, mesh } = this.legend;
    g.clearRect(0, 0, c.width, c.height);
    mesh.visible = !!(l && l.field);
    if (mesh.visible) {
      round(g, 6, 6, c.width - 12, c.height - 12, 34, 'rgba(255,255,255,0.94)', T.hairline);
      g.font = `500 40px ${F}`, g.fillStyle = T.ink;
      g.fillText(`${l.field}${l.unit ? ' [' + l.unit + ']' : ''}`, 44, 74);
      const grad = g.createLinearGradient(44, 0, c.width - 44, 0);
      (l.colormap || []).forEach((col, i, a) => grad.addColorStop(i / Math.max(1, a.length - 1), col));
      round(g, 44, 100, c.width - 88, 30, 15, grad);
      g.font = `400 36px ${M}`, g.fillStyle = T.dim;
      g.fillText(fmt(l.lo), 44, 186);
      const hi = fmt(l.hi);
      g.fillText(hi, c.width - 44 - g.measureText(hi).width, 186);
    }
    tex.needsUpdate = true;
  }

  drawWords(w) { // at most three lines; the latest words win
    const { c, g, tex, mesh } = this.words;
    g.clearRect(0, 0, c.width, c.height);
    const size = 40, lh = 52, x0 = w.working ? 84 : 48, width = c.width - x0 - 48;
    g.font = `${w.kind === 'you' ? 500 : 400} ${size}px ${F}`;
    const lines = [];
    let line = '';
    for (const word of String(w.text || '').split(/\s+/).filter(Boolean)) {
      const t = line ? line + ' ' + word : word;
      if (g.measureText(t).width > width && line) lines.push(line), line = word;
      else line = t;
    }
    if (line) lines.push(line);
    const shown = lines.length > 3 ? ['…' + lines[lines.length - 3]].concat(lines.slice(-2)) : lines;
    const h = 40 + shown.length * lh, y0 = c.height - h - 6;
    if (shown.length) {
      round(g, 6, y0, c.width - 12, h, 30, 'rgba(255,255,255,0.92)', T.hairline);
      g.fillStyle = w.kind === 'error' ? T.bad : w.kind === 'note' ? T.dim : T.ink;
      shown.forEach((s, i) => g.fillText(s, x0, y0 + 20 + (i + 0.78) * lh));
      // the breathing dot sits at the left of the first line while the AI works
      const px = (36 / c.width - 0.5) * WM, py = (0.5 - (y0 + 20 + 0.45 * lh) / c.height) * WM * c.height / c.width;
      this.dot.position.set(px, mesh.position.y + py, 0.002);
    }
    tex.needsUpdate = true;
  }

  drawManual(on) {
    const { c, g, tex } = this.manual;
    g.clearRect(0, 0, c.width, c.height);
    round(g, 6, 14, c.width - 12, c.height - 28, (c.height - 28) / 2, on ? T.ink : 'rgba(255,255,255,0.94)', on ? null : T.hairline);
    g.font = `500 44px ${F}`, g.fillStyle = on ? '#FFFFFF' : T.ink;
    const s = 'Manual', tw = g.measureText(s).width;
    g.fillText(s, (c.width - tw) / 2, c.height / 2 + 15);
    tex.needsUpdate = true;
  }
}
