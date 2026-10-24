// card.js - the card beside the result: what is shown and the controls to change it, the value under your pointer,
// and the AI. Drawn on a canvas in the studio look (docs/app/DESIGN.md: white card, black ink, one orange accent,
// black primary buttons, hairline pills) and pressed by a ray: a controller, a hand, or the mouse on a computer.
import * as THREE from 'three';

const T = { card: '#FFFFFF', ink: '#111111', dim: '#7A7A80', hairline: '#E3E1DC', pill: '#DDDAD4', accent: '#E8551C',
            accent2: '#B0700A', good: '#1E8E4E', bad: '#D23B3B', soft: '#F2F1EE' };
const F = '-apple-system, "SF Pro Text", Inter, Roboto, system-ui, sans-serif';
const M = 'ui-monospace, "SF Mono", Menlo, monospace';
const W = 1024, H = 1400, PAD = 44;

export function fmt(v) {
  if (v === null || v === undefined || Number.isNaN(v)) return 'no value';
  const a = Math.abs(v);
  return a !== 0 && (a >= 1e5 || a < 1e-3) ? v.toExponential(2) : (+v.toPrecision(3)).toString();
}

export class Card {
  constructor(widthM = 0.36) {
    this.canvas = document.createElement('canvas');
    this.canvas.width = W, this.canvas.height = H;
    this.g = this.canvas.getContext('2d');
    this.texture = new THREE.CanvasTexture(this.canvas);
    this.texture.colorSpace = THREE.SRGBColorSpace;
    this.texture.anisotropy = 4;
    const geo = new THREE.PlaneGeometry(widthM, widthM * H / W);
    this.mesh = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ map: this.texture, transparent: true, toneMapped: false }));
    this.mesh.name = 'card';
    this.mesh.renderOrder = 10;
    this.buttons = [];
    this.used = H;
    this.key = '';
    this.press = null; // the control held down by a ray (a slider follows it)
  }

  set(model) {
    const key = JSON.stringify(model);
    if (key !== this.key) this.key = key, this.model = model, this.draw();
  }

  // the control under a point of the card (uv from a ray), with where along it for a slider
  hit(uv) {
    const x = uv.x * W, y = (1 - uv.y) * H;
    if (y > this.used) return null;
    const b = this.buttons.find((b) => x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h);
    return b ? { ...b, at: Math.max(0, Math.min(1, (x - b.x) / b.w)) } : { id: 'card' };
  }

  // ---- drawing ----
  rr(x, y, w, h, r, fill, stroke) {
    const g = this.g;
    g.beginPath();
    g.roundRect(x, y, w, h, r);
    if (fill) g.fillStyle = fill, g.fill();
    if (stroke) g.strokeStyle = stroke, g.lineWidth = 2, g.stroke();
  }
  text(s, x, y, size, color = T.ink, weight = 400, font = F) {
    const g = this.g;
    g.font = `${weight} ${size}px ${font}`;
    g.fillStyle = color;
    g.fillText(s, x, y);
    return g.measureText(s).width;
  }
  wrap(s, x, y, w, size, lh, max, color = T.ink, weight = 400) {
    const g = this.g;
    g.font = `${weight} ${size}px ${F}`;
    g.fillStyle = color;
    let line = '', n = 0;
    for (const word of String(s).split(/\s+/)) {
      const t = line ? line + ' ' + word : word;
      if (g.measureText(t).width > w && line) {
        if (n === max - 1) { g.fillText(line + ' …', x, y + n * lh); return y + (n + 1) * lh; }
        g.fillText(line, x, y + n * lh), n++, line = word;
      } else line = t;
    }
    if (line) g.fillText(line, x, y + n++ * lh);
    return y + n * lh;
  }
  button(id, x, y, w, h, label, kind = 'pill', active = false) { // kind: pill, primary, icon
    const dark = kind === 'primary' || active;
    this.rr(x, y, w, h, h / 2, dark ? T.ink : T.card, dark ? null : T.pill);
    if (label) {
      this.g.font = `500 26px ${F}`;
      const tw = this.g.measureText(label).width;
      this.text(label, x + (w - tw) / 2, y + h / 2 + 9, 26, dark ? '#FFFFFF' : T.ink, 500);
    }
    this.buttons.push({ id, x, y, w, h });
  }
  pills(prefix, x, y, items, maxW) { // items: [{id, label, active}]; wraps; returns the y below
    let cx = x, cy = y;
    for (const it of items) {
      this.g.font = `500 26px ${F}`;
      const w = Math.max(64, this.g.measureText(it.label).width + 40);
      if (cx + w > x + maxW) cx = x, cy += 62;
      this.button(prefix + it.id, cx, cy, w, 50, it.label, 'pill', it.active);
      cx += w + 10;
    }
    return cy + 62;
  }
  rule(y) {
    const g = this.g;
    g.strokeStyle = T.hairline, g.lineWidth = 2;
    g.beginPath(), g.moveTo(PAD, y), g.lineTo(W - PAD, y), g.stroke();
  }
  iconPlay(x, y, s, playing) {
    const g = this.g;
    g.fillStyle = '#FFFFFF';
    if (playing) g.fillRect(x - s * 0.35, y - s * 0.4, s * 0.25, s * 0.8), g.fillRect(x + s * 0.1, y - s * 0.4, s * 0.25, s * 0.8);
    else g.beginPath(), g.moveTo(x - s * 0.3, y - s * 0.45), g.lineTo(x + s * 0.45, y), g.lineTo(x - s * 0.3, y + s * 0.45), g.fill();
  }
  iconStep(x, y, s, dir) {
    const g = this.g;
    g.fillStyle = T.ink;
    g.beginPath(), g.moveTo(x - dir * s * 0.25, y - s * 0.4), g.lineTo(x + dir * s * 0.35, y), g.lineTo(x - dir * s * 0.25, y + s * 0.4), g.fill();
    g.fillRect(x + dir * s * 0.35 - (dir > 0 ? 0 : s * 0.1), y - s * 0.4, s * 0.1, s * 0.8);
  }
  slider(id, x, y, w, frac, marks = 0) {
    this.rr(x, y + 14, w, 12, 6, T.soft);
    this.rr(x, y + 14, Math.max(12, w * frac), 12, 6, T.ink);
    const g = this.g;
    g.beginPath(), g.arc(x + w * frac, y + 20, 18, 0, Math.PI * 2), g.fillStyle = T.card, g.fill();
    g.lineWidth = 3, g.strokeStyle = this.press === id ? T.accent : T.ink, g.stroke();
    this.buttons.push({ id, x: x - 10, y: y - 10, w: w + 20, h: 60 });
  }

  draw() {
    const g = this.g, m = this.model || {}, CW = W - 2 * PAD;
    g.clearRect(0, 0, W, H);
    this.buttons = [];
    // the card is as tall as what it holds: draw once to measure, then the background behind
    const body = () => {
      let y = 70;
      this.text('OpenPhysicsAI', PAD, y, 28, T.accent, 600);
      const live = m.connected ? 'connected to the Mac' : 'the Mac does not answer';
      g.font = `400 24px ${F}`;
      this.text(live, W - PAD - g.measureText(live).width, y, 24, m.connected ? T.dim : T.bad);
      y = this.wrap(m.title || 'Nothing to show yet', PAD, y + 56, CW, 40, 48, 2, T.ink, 500) + 4;
      if (m.kind === 'none') {
        y = this.wrap(m.note || 'Open a computed part or a lab result in the app on the Mac.', PAD, y + 8, CW, 28, 36, 3, T.dim) + 10;
      } else {
        if (!m.field) y = this.wrap(m.note || 'This result has no field to read.', PAD, y + 8, CW, 26, 34, 2, T.dim) + 6;
        else {
        // the field, its legend, and the value under the pointer marked on it
        this.text(`${m.field}${m.unit ? ' [' + m.unit + ']' : ''}`, PAD, y + 30, 28, T.dim);
        y += 50;
        const grad = g.createLinearGradient(PAD, 0, W - PAD, 0);
        (m.colormap || []).forEach((c, i, a) => grad.addColorStop(i / Math.max(1, a.length - 1), c));
        this.rr(PAD, y, CW, 24, 12, grad);
        if (m.probe && m.probe.t !== null && m.probe.t !== undefined) {
          const px = PAD + CW * Math.max(0, Math.min(1, m.probe.t));
          g.beginPath(), g.moveTo(px - 12, y - 16), g.lineTo(px + 12, y - 16), g.lineTo(px, y - 1), g.fillStyle = T.ink, g.fill();
        }
        this.text(fmt(m.lo), PAD, y + 58, 26, T.ink, 400, M);
        g.font = `400 26px ${M}`;
        const hs = fmt(m.hi);
        this.text(hs, W - PAD - g.measureText(hs).width, y + 58, 26, T.ink, 400, M);
        y += 82;
        // the value under the pointer, in a row of fixed height so that the controls below never move
        this.rr(PAD, y, CW, 108, 18, T.soft);
        if (m.probe) {
          this.text(m.probe.text, PAD + 22, y + 44, 30, T.ink, 600);
          if (m.probe.where) this.text(m.probe.where, PAD + 22, y + 88, 24, T.dim, 400, M);
        } else {
          this.text('Point at the result to read a value', PAD + 22, y + 44, 26, T.dim);
          this.text(m.pins ? `${m.pins} pinned point${m.pins > 1 ? 's' : ''}` : 'a tap pins it', PAD + 22, y + 86, 24, T.dim);
        }
        if (m.pins) this.button('pins-clear', W - PAD - 190, y + 29, 170, 50, 'Clear pins');
        y += 120;
        if (m.marks) { // what the AI marked on the result
          this.text(`${m.marks} mark${m.marks > 1 ? 's' : ''} from the AI`, PAD, y + 32, 26, T.accent, 500);
          this.button('marks-clear', W - PAD - 190, y, 170, 50, 'Clear marks');
          y += 64;
        }
        }
        // time: play, step, scrub
        const t = m.time;
        if (t && t.count > 1) {
          this.rule(y + 4), y += 26;
          this.rr(PAD, y, 76, 76, 38, T.ink);
          this.iconPlay(PAD + 38, y + 38, 34, t.playing);
          this.buttons.push({ id: 'play', x: PAD, y, w: 76, h: 76 });
          this.rr(PAD + 92, y + 8, 60, 60, 30, T.card, T.pill), this.iconStep(PAD + 122, y + 38, 26, -1);
          this.buttons.push({ id: 'prev', x: PAD + 92, y: y + 8, w: 60, h: 60 });
          this.rr(PAD + 164, y + 8, 60, 60, 30, T.card, T.pill), this.iconStep(PAD + 194, y + 38, 26, 1);
          this.buttons.push({ id: 'next', x: PAD + 164, y: y + 8, w: 60, h: 60 });
          this.text(t.label, PAD + 248, y + 34, 28, T.ink, 500);
          this.text(`${t.index + 1} of ${t.count}${t.loaded < t.count ? `  ·  ${t.loaded} loaded` : ''}`, PAD + 248, y + 68, 24, T.dim);
          y += 96;
          this.slider('scrub', PAD + 10, y, CW - 20, t.count > 1 ? t.index / (t.count - 1) : 0);
          y += 62;
        }
        if (m.fields && m.fields.length > 1) {
          this.rule(y + 4), y += 24;
          this.text('Field', PAD, y + 34, 26, T.dim);
          y = this.pills('field:', PAD + 110, y, m.fields, CW - 110) + 4;
        }
        if (m.section) {
          const s = m.section;
          this.text('Cut', PAD, y + 34, 26, T.dim);
          const items = [{ id: 'off', label: 'Off', active: !s.on }, ...'xyz'.split('').map((a, i) => ({ id: a, label: a.toUpperCase(), active: s.on && s.axis === i }))];
          if (s.on) items.push({ id: 'flip', label: 'Flip', active: !!s.flip });
          y = this.pills('cut:', PAD + 110, y, items, CW - 110);
          if (s.on) this.slider('cut', PAD + 120, y - 6, CW - 140, s.fraction), y += 58;
        }
        this.rule(y + 4), y += 24;
        const v = m.view || {};
        y = this.pills('view:', PAD, y, [
          ...(v.canPlace ? [{ id: 'place', label: v.placing ? 'Placing…' : 'Place on a surface', active: !!v.placing }] : []),
          { id: 'true', label: 'True size', active: !!v.trueSize }, { id: 'reset', label: 'Reset' }], CW);
        this.text(`${v.sizeText || ''}${v.scaleText ? '  ·  ' + v.scaleText : ''}`, PAD, y + 20, 24, T.dim);
        y += 40;
      }
      // the AI
      this.rule(y + 4), y += 50;
      const ai = m.ai || { lines: [] };
      if (ai.approval) {
        this.text(`The AI asks to use ${ai.approval.tool}`, PAD, y, 30, T.ink, 600);
        g.font = `400 24px ${M}`;
        y = this.wrap(ai.approval.detail, PAD, y + 40, CW, 24, 30, 3, T.dim) + 14;
        this.button('deny', W - PAD - 380, y, 180, 64, 'Deny');
        this.button('allow', W - PAD - 180, y, 180, 64, 'Allow', 'primary');
        y += 86;
      } else {
        for (const l of (ai.lines || []).slice(-4)) {
          const color = l.kind === 4 ? T.ink : l.kind === 3 ? T.bad : l.kind === 1 ? '#9A9AA0' : '#333333';
          const s = l.kind === 4 ? '“' + l.text + '”' : l.kind === 1 ? '· ' + l.text.replace(/^calls /, '') : l.text;
          y = this.wrap(s, PAD, y, CW - 150, 27, 35, l.kind === 0 ? 4 : 2, color, l.kind === 4 ? 500 : 400) + 8;
        }
        if (ai.running) this.text('Thinking…', PAD, y + 4, 28, T.accent, 500), y += 44;
        else if (!(ai.lines || []).length) y = this.wrap(ai.micAllowed ? 'Hold the grip and speak, or press the microphone.' : 'Allow the microphone on the page to ask by voice.', PAD, y, CW - 150, 26, 34, 2, '#9A9AA0') + 8;
      }
      if (ai.heard) this.wrap(ai.heard, PAD, y + 8, CW - 150, 24, 30, 2, T.dim), y += 40;
      // the microphone, round, orange while it listens; the voice of the answers, on or off
      const mx = W - PAD - 56, my = Math.max(y - 10, 0);
      g.beginPath(), g.arc(mx, my, 52, 0, Math.PI * 2), g.fillStyle = ai.listening ? T.accent : T.ink, g.fill();
      this.rr(mx - 12, my - 30, 24, 40, 12, '#FFFFFF');
      g.strokeStyle = '#FFFFFF', g.lineWidth = 5;
      g.beginPath(), g.arc(mx, my - 4, 22, 0.15 * Math.PI, 0.85 * Math.PI), g.stroke();
      g.beginPath(), g.moveTo(mx, my + 18), g.lineTo(mx, my + 30), g.stroke();
      this.buttons.push({ id: 'mic', x: mx - 56, y: my - 56, w: 112, h: 112 });
      this.button('speech', mx - 56 - 150, my - 25, 136, 50, ai.speechOn ? 'Voice on' : 'Voice off', 'pill', !!ai.speechOn);
      return my + 76;
    };
    // measure, then draw the card behind and the body again on it
    this.used = Math.min(H, body());
    g.clearRect(0, 0, W, H);
    this.buttons = [];
    g.save();
    g.shadowColor = 'rgba(0,0,0,0.12)', g.shadowBlur = 24, g.shadowOffsetY = 6;
    this.rr(8, 8, W - 16, this.used - 16, 36, T.card);
    g.restore();
    this.rr(8, 8, W - 16, this.used - 16, 36, null, T.hairline);
    body();
    this.texture.needsUpdate = true;
  }
}
