// scene.js - what the headset shows: the finite-element result the app exports (scene.glb), or the frames of the lab
// result the app has open, drawn by build/labxr through the bridge exactly as the window draws them (src/lab/labxr.h).
// Frames stream in, the current one first and then the ones ahead of it, within a memory budget, and play in step
// with the app: the app's frame when it is paused, its pace when it plays, and the headset's own choice the moment the
// person scrubs (the command reaches the app, which follows).
import * as THREE from 'three';
import { GLTFLoader } from './vendor/GLTFLoader.js';
import { makeVolume } from './volume.js';

const loader = new GLTFLoader();
const BUDGET = 320 << 20; // bytes of lab frames kept at once: what a Quest's browser holds comfortably

function parse(buffer) {
  return new Promise((resolve, reject) => loader.parse(buffer, './', resolve, reject));
}

const dummy3d = (() => {
  const t = new THREE.Data3DTexture(new Uint8Array(1), 1, 1, 1);
  t.format = THREE.RedFormat, t.needsUpdate = true;
  return t;
})();

// particles as lit spheres, sized as the app sizes them: a radius in metres when they have one, never smaller than
// the app's few pixels (labgpu.c pointpx); the water's droplets inside its surface are not drawn (labgpu.c)
function pointsMaterial(palette, opts) {
  const uniforms = {
    palette: { value: palette }, useField: { value: opts.color ? 0 : 1 }, tint: { value: opts.color || new THREE.Color(1, 1, 1) },
    radiusM: { value: opts.radiusM || 0 }, pixelRadius: { value: opts.pixelRadius || 2.5 }, viewportHeight: { value: 1000 },
    modelScale: { value: 1 }, waterMask: { value: opts.water ? opts.water.mask : dummy3d }, hasWater: { value: opts.water ? 1 : 0 },
    volOrigin: { value: opts.water ? opts.water.origin : new THREE.Vector3() }, volExtent: { value: opts.water ? opts.water.extent : new THREE.Vector3(1, 1, 1) },
  };
  const m = new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3, uniforms, defines: opts.hasRadius ? { HAS_RADIUS: 1 } : {},
    vertexShader: /* glsl */ `
      precision highp sampler3D;
      #ifdef HAS_RADIUS
      in float _radius;
      #endif
      uniform float radiusM, pixelRadius, viewportHeight, modelScale;
      uniform int hasWater;
      uniform sampler3D waterMask;
      uniform vec3 volOrigin, volExtent;
      out float vU;
      void main(){
        vec3 src = vec3(position.x, -position.z, position.y); // lab axes
        if (hasWater != 0 && texture(waterMask, (src - volOrigin) / volExtent).r >= .5) { gl_Position = vec4(2., 2., 2., 1.); gl_PointSize = 0.; return; }
        vec4 mv = modelViewMatrix * vec4(position, 1.);
        #ifdef HAS_RADIUS
        float r = (_radius > 0. ? _radius : radiusM) * modelScale;
        #else
        float r = radiusM * modelScale;
        #endif
        float px = max(pixelRadius * viewportHeight / 720., r * projectionMatrix[1][1] * viewportHeight * .5 / max(-mv.z, 1e-4));
        gl_PointSize = 2. * px;
        vU = uv.x;
        gl_Position = projectionMatrix * mv;
      }`,
    fragmentShader: /* glsl */ `
      layout(location = 0) out highp vec4 pc_fragColor; // GLSL 3: three.js leaves the output to the shader
      #define gl_FragColor pc_fragColor
      in float vU;
      uniform sampler2D palette;
      uniform int useField;
      uniform vec3 tint;
      void main(){
        vec2 d = 2. * gl_PointCoord - 1.;
        float r2 = dot(d, d);
        if (r2 > 1.) discard;
        vec3 n = vec3(d.x, -d.y, sqrt(1. - r2)), L = normalize(vec3(-.35, .55, .75));
        vec3 base = useField != 0 ? texture(palette, vec2(vU, .5)).rgb : tint;
        float lam = max(dot(n, L), 0.), spec = pow(max(dot(n, normalize(L + vec3(0., 0., 1.))), 0.), 48.);
        gl_FragColor = vec4(base * (.42 + .62 * lam) + vec3(.06) * spec, 1.);
        #include <tonemapping_fragment>
        #include <colorspace_fragment>
      }`,
  });
  return m;
}

function factsOf(root) {
  let node = null;
  root.traverse((o) => { if (!node && o.userData && o.userData.result) node = o; });
  const r = node ? node.userData.result : {};
  const toSource = new THREE.Matrix4();
  if (r.to_source) toSource.fromArray(r.to_source);
  return { kind: r.kind, field: r.field || '', unit: r.unit || '', lo: r.lo ?? 0, hi: r.hi ?? 1, parts: r.parts || [],
           toSource, sourceUnit: r.source_unit || 'm', frame: r.frame, time: r.time, raw: r,
           pointRadius: 0, step: r.step, nsteps: r.nsteps };
}

// a GLB as the content the headset draws, with what it is in .facts
async function prepare(buffer) {
  const gltf = await parse(buffer), parser = gltf.parser, facts = factsOf(gltf.scene), raw = facts.raw;
  const paletteIndex = raw.palette ? raw.palette.texture : -1;
  const palette = paletteIndex >= 0 ? await parser.getDependency('texture', paletteIndex) : null;
  if (palette) palette.colorSpace = THREE.SRGBColorSpace, palette.flipY = false, palette.needsUpdate = true;
  let water = null, volume = null;
  if (raw.volume) {
    const v = raw.volume;
    const values = new Uint16Array(await parser.getDependency('bufferView', v.values));
    const mask = v.mask >= 0 ? new Uint8Array(await parser.getDependency('bufferView', v.mask)) : null;
    const vpal = v.palette === paletteIndex ? palette : await parser.getDependency('texture', v.palette);
    if (vpal !== palette) vpal.colorSpace = THREE.SRGBColorSpace, vpal.flipY = false, vpal.needsUpdate = true;
    volume = makeVolume(v, raw, values, mask, vpal);
    gltf.scene.add(volume);
    if (raw.points && raw.points.water_droplets && mask)
      water = { mask: volume.material.uniforms.maskTex.value, origin: volume.userData.volume.origin, extent: volume.userData.volume.extent };
  }
  let plane = null; // a planar result's values, read where the picture is pointed at
  if (raw.picture && raw.picture.values !== undefined) {
    plane = { dims: raw.picture.dims, values: new Uint16Array(await parser.getDependency('bufferView', raw.picture.values)) };
  }
  const swaps = [];
  gltf.scene.traverse((o) => {
    if (o.isMesh && o !== volume) {
      o.material.side = THREE.DoubleSide;
      if (o.material.map) o.material.map.anisotropy = 4;
      if (o.geometry.userData && o.geometry.userData.key === 'picture') { // a picture: its colours exactly, unlit
        const map = o.material.map;
        map.colorSpace = THREE.SRGBColorSpace;
        o.material.dispose();
        o.material = new THREE.MeshBasicMaterial({ map, side: THREE.DoubleSide, alphaTest: 0.5, toneMapped: false });
      }
    }
    if (o.isPoints) swaps.push(o);
  });
  for (const o of swaps) {
    const ud = o.geometry.userData || {}, field = ud.key === 'field';
    const color = field ? null : (o.material.color ? o.material.color.clone() : new THREE.Color(0.8, 0.8, 0.79));
    const radiusM = ud.radius_m || 0;
    facts.pointRadius = Math.max(facts.pointRadius, radiusM);
    o.material.dispose();
    o.material = pointsMaterial(palette, { color, radiusM, hasRadius: !!o.geometry.attributes._radius,
                                           pixelRadius: raw.points ? raw.points.pixel_radius : 2.5, water });
    o.frustumCulled = false;
    const u = o.material.uniforms, scl = new THREE.Vector3();
    o.onBeforeRender = (renderer, scene, camera) => { // per eye: its own viewport height, the model's scale
      u.viewportHeight.value = camera.viewport ? camera.viewport.w : renderer.getDrawingBufferSize(new THREE.Vector2()).y;
      u.modelScale.value = scl.setFromMatrixScale(o.matrixWorld).x;
    };
  }
  const content = new THREE.Group();
  content.add(gltf.scene);
  content.userData.facts = facts;
  facts.plane = plane;
  facts.picture = raw.picture || null;
  content.userData.bytes = buffer.byteLength;
  const stats = { triangles: 0, particles: 0, segments: 0, volume: raw.volume ? raw.volume.dims.join('x') + ' ' + raw.volume.mode : null };
  gltf.scene.traverse((o) => {
    const g = o.geometry;
    if (!g || o === volume) return;
    const n = g.index ? g.index.count : g.attributes.position.count;
    if (o.isPoints) stats.particles += n;
    else if (o.isLineSegments) stats.segments += n / 2;
    else if (o.isMesh) stats.triangles += n / 3;
  });
  content.userData.stats = stats;
  return content;
}

function dispose(content) {
  if (!content) return;
  content.traverse((o) => {
    if (o.dispose && o.name === 'volume') o.dispose();
    else if (o.geometry) {
      o.geometry.dispose();
      for (const m of [].concat(o.material || [])) {
        for (const k of ['map']) if (m[k]) m[k].dispose();
        if (m.uniforms && m.uniforms.palette && m.uniforms.palette.value) m.uniforms.palette.value.dispose();
        m.dispose();
      }
    }
  });
}

export class SceneView extends EventTarget {
  constructor(model) {
    super();
    this.model = model;       // the group the content goes in (scaled and placed by the page)
    this.content = null;      // what is drawn now
    this.kind = 'none';
    this.femVersion = -1;
    this.lab = null;          // the app's lab state
    this.info = null;         // the result's info (frames, times, fields, box)
    this.key = '';            // the view key the cached frames were drawn with
    this.frames = new Map();  // frame -> content
    this.loading = new Set();
    this.bytes = 0;
    this.shown = -1;          // the frame drawn now
    this.target = 0;          // the frame to draw
    this.playing = false;
    this.clock = 0;
    this.local = 0;           // until then the headset's own choice of frame and playback stands
    this.box = null;          // the result's box in glTF axes, for its placement
    this.error = '';
  }

  // ---- what the app shows ----
  async showFem(version) {
    if (version === this.femVersion && this.kind === 'fem') return;
    this.femVersion = version;
    try {
      const r = await fetch('./scene.glb?v=' + version, { cache: 'no-store' });
      if (!r.ok) return;
      const content = await prepare(await r.arrayBuffer());
      if (this.femVersion !== version) return dispose(content);
      this.clearLab();
      this.kind = 'fem';
      const b = new THREE.Box3().setFromObject(content);
      this.box = { lo: b.min.clone(), hi: b.max.clone() };
      this.swap(content);
      this.dispatchEvent(new Event('change'));
    } catch (e) { this.error = 'the result could not be read: ' + e.message; }
  }

  setLab(lab) {
    if (!lab) return;
    const fresh = !this.lab || !sameResult(this.lab, lab);
    const prev = this.lab;
    this.lab = lab;
    if (this.kind !== 'lab' || fresh) {
      if (this.kind === 'fem' && this.content) this.swap(null);
      this.kind = 'lab';
      this.clearFrames();
      this.info = null, this.box = null, this.shown = -1;
      this.key = lab.view_key;
      this.fetchInfo(lab);
    } else if (lab.view_key !== this.key) { // the field, range or section changed: every frame again, the shown one first
      this.key = lab.view_key;
      for (const [f, c] of this.frames) if (f !== this.shown) dispose(c);
      const keep = this.frames.get(this.shown);
      this.frames.clear(), this.bytes = 0;
      this.stale = keep || null;
    }
    // a run shown while it computes: the times of its new frames, and its box once it has ended (kept while it grows,
    // so that it does not move under the person's eyes)
    if (!fresh && this.info && (lab.nframes > (this.info.times || []).length || (prev.growing && !lab.growing)))
      this.fetchInfo(lab, !!lab.growing);
    // follow the app, unless the person has just chosen otherwise here
    if (performance.now() > this.local) {
      if (!prev || prev.playing !== lab.playing) this.playing = lab.playing, this.clock = 0, this.base = lab.frame;
      if (!lab.playing) this.target = lab.frame;
    }
    this.pump();
  }

  async fetchInfo(lab, keepBox = false) {
    if (keepBox) { // while the result grows: one at a time, at most every 3 s (a new result's or the end's always)
      const now = performance.now();
      if (this.infoBusy || now - (this.infoAt || 0) < 3000) return;
      this.infoAt = now;
    }
    this.infoBusy = true;
    try {
      const r = await fetch('./lab/info', { cache: 'no-store' });
      const info = await r.json();
      if (!r.ok) throw new Error(info.error || 'no info');
      if (this.lab !== lab && !sameResult(this.lab, lab)) return;
      this.info = info;
      if (keepBox && this.box) {
        // the box stays as it was while the result grows
      } else if (info.picture) { // a plane, or a picture, stands upright: its box is already in glTF axes
        this.box = { lo: new THREE.Vector3(...info.picture.gltf_lo), hi: new THREE.Vector3(...info.picture.gltf_hi) };
      } else if (info.box) { // lab axes to glTF axes: x, z up, -y
        const [a, b] = [info.box.lo, info.box.hi];
        this.box = { lo: new THREE.Vector3(a[0], a[2], -b[1]), hi: new THREE.Vector3(b[0], b[2], -a[1]) };
      }
      this.error = info.native3d || info.picture ? '' : 'this lab result cannot be drawn in the headset';
      this.dispatchEvent(new Event('change'));
      this.pump();
    } catch (e) { this.error = 'the lab result could not be read: ' + e.message; }
    finally { this.infoBusy = false; }
  }

  clearFrames() {
    for (const c of this.frames.values()) if (c !== this.content) dispose(c);
    this.frames.clear(), this.loading.clear(), this.bytes = 0, this.stale = null;
  }
  clearLab() {
    this.clearFrames();
    this.lab = null, this.info = null;
  }

  // ---- frames: the target first, then ahead of it, two at a time, within the budget ----
  pump() {
    if (this.kind !== 'lab' || !this.lab || !this.info || !(this.info.native3d || this.info.picture)) return;
    const n = this.lab.nframes;
    for (let k = 0; k < n && this.loading.size < 2; k++) {
      const f = (this.target + k) % n;
      if (this.frames.has(f) || this.loading.has(f)) continue;
      if (this.bytes > BUDGET && k > 0) break;
      this.load(f);
    }
  }

  async load(f) {
    const key = this.key;
    this.loading.add(f);
    try {
      const r = await fetch(`./lab/frame.glb?f=${f}&key=${encodeURIComponent(key)}`, { cache: 'no-store' });
      if (r.status === 409) return; // the view changed on the Mac: the next state brings its key
      if (!r.ok) { this.error = (await r.json().catch(() => ({}))).error || 'frame ' + f + ' could not be drawn'; return; }
      const content = await prepare(await r.arrayBuffer());
      if (key !== this.key || this.kind !== 'lab') return dispose(content);
      this.frames.set(f, content);
      this.bytes += content.userData.bytes;
      this.evict();
      this.error = '';
    } catch (e) {
      this.error = 'frame ' + f + ': ' + e.message;
    } finally {
      this.loading.delete(f);
      setTimeout(() => this.pump(), 0);
    }
  }

  evict() { // over the budget: the frames furthest ahead of the target go first
    const n = this.lab ? this.lab.nframes : 1;
    while (this.bytes > BUDGET * 1.15 && this.frames.size > 2) {
      let worst = -1, far = -1;
      for (const f of this.frames.keys()) {
        const d = (f - this.target + n) % n;
        if (f !== this.shown && d > far) far = d, worst = f;
      }
      if (worst < 0) break;
      const c = this.frames.get(worst);
      this.frames.delete(worst), this.bytes -= c.userData.bytes, dispose(c);
    }
  }

  // ---- what the person does here (the page sends the same to the app) ----
  hold() { this.local = performance.now() + 1500; }
  setPlaying(on) { this.hold(); this.playing = on; this.clock = 0; this.base = this.shown >= 0 ? this.shown : this.target; }
  setFrame(f) {
    const n = this.lab ? this.lab.nframes : 1;
    this.hold();
    this.playing = false;
    this.target = ((f % n) + n) % n;
    this.pump();
  }

  update(dt) {
    if (this.kind !== 'lab' || !this.lab) return;
    const n = this.lab.nframes;
    if (this.playing && n > 1) {
      this.clock += dt * (this.lab.fps || 12);
      const next = (this.base + Math.floor(this.clock)) % n;
      if (this.frames.has(next)) this.target = next;
      else if (this.clock > 0) this.clock -= dt * (this.lab.fps || 12), this.pump(); // wait for the frame, then go on
    }
    const c = this.frames.get(this.target) || (this.shown < 0 ? this.stale : null);
    if (c && (c !== this.content)) {
      this.swap(c);
      this.shown = this.target;
      this.stale = null;
      this.dispatchEvent(new Event('frame'));
    }
  }

  swap(content) {
    const old = this.content;
    if (old) this.model.remove(old);
    this.content = content;
    if (content) {
      if (this.kind === 'lab' && this.box) { // the result's base centred over the holder, every frame alike
        const b = this.box;
        content.position.set(-(b.lo.x + b.hi.x) / 2, -b.lo.y, -(b.lo.z + b.hi.z) / 2);
      }
      this.model.add(content);
    }
    if (old && this.kind === 'fem' && old !== content) dispose(old);
    if (old && this.kind === 'lab' && ![...this.frames.values()].includes(old) && old !== content) dispose(old);
  }

  clear() { // the app shows nothing the headset can draw
    if (this.kind === 'none') return;
    this.clearFrames();
    const old = this.content;
    if (old) this.model.remove(old), dispose(old);
    this.content = null, this.kind = 'none', this.lab = null, this.info = null, this.box = null, this.shown = -1;
    this.femVersion = -1;
    this.dispatchEvent(new Event('change'));
  }

  get facts() { return this.content ? this.content.userData.facts : null; }
  get size() { // the result's extent in metres (glTF axes)
    if (!this.box) return null;
    return new THREE.Vector3().subVectors(this.box.hi, this.box.lo);
  }
  time() { // the shown frame's time and the count, for the card
    if (this.kind !== 'lab' || !this.lab) return null;
    const f = this.shown >= 0 ? this.shown : this.target, times = this.info ? this.info.times : null;
    return { index: f, count: this.lab.nframes, t: times && times[f] !== undefined ? times[f] : this.lab.time, unit: this.lab.time_unit || 's',
             playing: this.playing, loaded: this.frames.size };
  }
}

// the same result as the app opened it: by its version where the app gives one (a run shown while it computes keeps
// its version as frames are added), else by its file's time
function sameResult(a, b) {
  return a.path === b.path && (b.version !== undefined ? a.version === b.version : a.mtime === b.mtime);
}

export { dispose };
