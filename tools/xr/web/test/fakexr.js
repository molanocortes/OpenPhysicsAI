// fakexr.js - a stand-in WebXR runtime for the page test only (app.js loads it with ?fakexr=1): just enough of an
// immersive session for three.js r165's WebXRManager and the page's headset code to run without a headset. A session
// with two eyes, two controllers with their gamepads, select and squeeze, hit testing, driven by the test through
// window.fakexr. It emulates no device and proves nothing about a Quest; it makes the headset code paths execute, so
// their mistakes show (tools/xr/xr_test.py, part 7).

const identity = () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
// a pose at pos turned by yaw about the vertical, then tilted by pitch (up positive), column-major as WebXR gives them;
// yaw 0 and pitch 0 look down -z
function pose(pos, yaw = 0, pitch = 0) {
  const c = Math.cos(yaw), s = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
  return new Float32Array([c, 0, -s, 0, s * sp, cp, c * sp, 0, s * cp, -sp, c * cp, 0, pos[0], pos[1], pos[2], 1]);
}
// a pose at from whose -z points at to (how a controller's ray is held)
function aimed(from, to) {
  let f = [to[0] - from[0], to[1] - from[1], to[2] - from[2]];
  const n = Math.hypot(...f) || 1;
  f = f.map((x) => x / n);
  const z = f.map((x) => -x), up = Math.abs(f[1]) > 0.99 ? [0, 0, 1] : [0, 1, 0];
  let x = [up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]];
  const m = Math.hypot(...x) || 1;
  x = x.map((v) => v / m);
  const y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]];
  return new Float32Array([...x, 0, ...y, 0, ...z, 0, ...from, 1]);
}
function perspective(fovy, aspect, near, far) {
  const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
  return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0]);
}
function eye(head, dx) { // an eye beside the head, along its own x
  const m = new Float32Array(head);
  for (let k = 0; k < 3; k++) m[12 + k] += m[k] * dx;
  return m;
}

class Space { constructor() { this.matrix = identity(); } }
class Source {
  constructor(handedness) {
    this.handedness = handedness, this.targetRayMode = 'tracked-pointer', this.profiles = ['oculus-touch-v3'], this.hand = null;
    this.targetRaySpace = new Space(), this.gripSpace = new Space();
    this.gamepad = { connected: true, mapping: 'xr-standard', axes: [0, 0, 0, 0],
                     buttons: Array.from({ length: 7 }, () => ({ pressed: false, touched: false, value: 0 })) };
  }
}
class Layer {
  constructor(session, gl, init) {
    this.framebuffer = null, this.framebufferWidth = gl.drawingBufferWidth, this.framebufferHeight = gl.drawingBufferHeight;
    this.antialias = true, this.ignoreDepthValues = false, this.fixedFoveation = 0, this.init = init;
  }
  getViewport(view) {
    const w = Math.floor(this.framebufferWidth / 2);
    return { x: view.eye === 'left' ? 0 : w, y: 0, width: w, height: this.framebufferHeight };
  }
}
const state = { head: pose([0, 1.6, 0]), hit: null, session: null, frames: 0 };
class Frame {
  constructor(session) { this.session = session; }
  getViewerPose() {
    const L = this.session.renderState.baseLayer, aspect = L ? (L.framebufferWidth / 2) / L.framebufferHeight : 1;
    const proj = perspective(Math.PI / 2, aspect, this.session.renderState.depthNear, this.session.renderState.depthFar);
    return { transform: { matrix: state.head }, emulatedPosition: false,
             views: [['left', -0.032], ['right', 0.032]].map(([e, dx]) => ({ eye: e, transform: { matrix: eye(state.head, dx) }, projectionMatrix: proj })) };
  }
  getPose(space) { return space ? { transform: { matrix: space.matrix }, emulatedPosition: false } : null; }
  getHitTestResults(source) {
    return state.hit && !source.cancelled ? [{ getPose: () => ({ transform: { matrix: state.hit } }) }] : [];
  }
}
class Session extends EventTarget {
  constructor(mode, init) {
    super();
    this.mode = mode, this.ended = false, this.visibilityState = 'visible';
    this.renderState = { baseLayer: null, depthNear: 0.01, depthFar: 1000, inlineVerticalFieldOfView: null };
    this.environmentBlendMode = mode === 'immersive-ar' ? 'alpha-blend' : 'opaque';
    this.enabledFeatures = [...(init.requiredFeatures || []), ...(init.optionalFeatures || [])];
    this.inputSources = [new Source('left'), new Source('right')];
    setTimeout(() => this.emit('inputsourceschange', { added: this.inputSources, removed: [] }), 30);
  }
  emit(type, props) { const e = new Event(type); Object.assign(e, props); this.dispatchEvent(e); }
  updateRenderState(s) { Object.assign(this.renderState, s); }
  async requestReferenceSpace() { return new Space(); }
  requestAnimationFrame(cb) { // a timer, not the window's frames, so a hidden page goes on too
    return setTimeout(() => { if (!this.ended) state.frames++, cb(performance.now(), new Frame(this)); }, 16);
  }
  cancelAnimationFrame(id) { clearTimeout(id); }
  async requestHitTestSource(opts) { const s = { space: opts.space, cancelled: false }; s.cancel = () => { s.cancelled = true; }; return s; }
  async end() { if (!this.ended) this.ended = true, this.emit('end', {}); }
}

Object.defineProperty(navigator, 'xr', { configurable: true, value: {
  isSessionSupported: async (mode) => mode === 'immersive-ar' || mode === 'immersive-vr',
  requestSession: async (mode, init) => (state.session = new Session(mode, init || {})),
} });
window.XRWebGLLayer = Layer;
WebGL2RenderingContext.prototype.makeXRCompatible = function () { return Promise.resolve(); };

// what the test does with it: where the head is, where each controller points, its buttons, its stick, select and
// squeeze, and where the room's surface is found
window.fakexr = {
  get session() { return state.session; },
  get frames() { return state.frames; },
  head(pos, yaw = 0, pitch = 0) { state.head = pose(pos, yaw, pitch); },
  aim(i, from, to) { const src = state.session.inputSources[i]; src.targetRaySpace.matrix = src.gripSpace.matrix = aimed(from, to); },
  button(i, k, on) { const b = state.session.inputSources[i].gamepad.buttons[k]; b.pressed = on, b.value = on ? 1 : 0; },
  stick(i, x, y) { const a = state.session.inputSources[i].gamepad.axes; a[2] = x, a[3] = y; },
  select(i, on) {
    const s = state.session, src = s.inputSources[i], f = new Frame(s);
    s.emit(on ? 'selectstart' : 'selectend', { inputSource: src, frame: f });
    if (!on) s.emit('select', { inputSource: src, frame: f });
  },
  squeeze(i, on) { const s = state.session; s.emit(on ? 'squeezestart' : 'squeezeend', { inputSource: s.inputSources[i], frame: new Frame(s) }); },
  hit(pos) { state.hit = pos ? pose(pos) : null; },
};
