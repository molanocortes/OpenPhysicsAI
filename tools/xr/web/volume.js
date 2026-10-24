// volume.js - a computed lab volume ray-marched in the headset as the app does it (src/labvolume_shader.h, labgpu.c),
// from the frame's half-float samples and mask (src/lab/labxr.h). The colour of a sample is the legend's colour of its
// value; how strongly it shows grows with the value's size; gradient shading, the studio light and the room's far walls
// are presentation (docs/lab/README.md), not optical properties of the field.
//
// It works in the lab's own axes (z up, metres): the box is drawn in glTF axes and every position goes to lab axes by
// toSource. Drawn first among the opaque things (renderOrder -1) with premultiplied blending; an opaque hit (an iso
// surface, a body, water, a solid's faces) writes its depth, so parts drawn after it hide it only where they are closer.
import * as THREE from 'three';

const vertexShader = /* glsl */ `
uniform mat3 toSource;
out vec3 vSource;
void main(){
  vSource = toSource * position;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;

const fragmentShader = /* glsl */ `
layout(location = 0) out highp vec4 pc_fragColor; // GLSL 3: three.js leaves the output to the shader
#define gl_FragColor pc_fragColor
precision highp float;
precision highp sampler3D;
in vec3 vSource;
uniform sampler3D fieldTex, maskTex;
uniform sampler2D palette;
uniform mat3 toSource;
uniform mat4 fromSourceMV;      // lab axes to view space (for the depth of a hit)
uniform mat4 projectionMatrix;  // three.js declares it for the vertex stage only
uniform vec3 camSource, origin, extent, cell, lightDir;
uniform vec2 range;             // the legend's lo, hi
uniform float cutPosition, maxSteps, shade;
uniform int axis, flip, mode, walls, hasMask; // mode: 0 fog, 1 water, 2 iso, 3 solid
float material(vec3 p){ return hasMask != 0 ? texture(maskTex, (p - origin) / extent).r : 0.; }
float value(vec3 p){ return range.x + texture(fieldTex, (p - origin) / extent).r * (range.y - range.x); }
vec3 color(float v){ // the palette's texels 0..247 hold the map (labxr.h); the texture is sRGB, sampled linear
  float t = clamp((v - range.x) / max(range.y - range.x, 1e-20), 0., 1.);
  return texture(palette, vec2((0.5 + t * 247.) / 256., 0.5)).rgb;
}
float strengthOf(float v){
  return (range.x < 0. && range.y > 0.) ? clamp(abs(v) / max(abs(range.x), abs(range.y)), 0., 1.)
                                        : clamp((v - range.x) / max(range.y - range.x, 1e-20), 0., 1.);
}
float depthOf(vec3 p){ vec4 c = projectionMatrix * fromSourceMV * vec4(p, 1.); return clamp(c.z / c.w * .5 + .5, 0., 1.); }
void main(){
  vec3 ray = normalize(vSource - camSource);
  vec3 low = origin, high = origin + extent;
  float cut = 0.;
  if (axis >= 0) { cut = cutPosition; if (flip != 0) low[axis] = max(low[axis], cut); else high[axis] = min(high[axis], cut); }
  vec3 safeRay = ray;
  for (int k = 0; k < 3; k++) if (abs(safeRay[k]) < 1e-8) safeRay[k] = safeRay[k] < 0. ? -1e-8 : 1e-8;
  vec3 aa = (low - camSource) / safeRay, bb = (high - camSource) / safeRay;
  vec3 mn = min(aa, bb), mx = max(aa, bb);
  float enter = max(max(max(mn.x, mn.y), mn.z), 0.), leave = min(min(mx.x, mx.y), mx.z);
  if (enter >= leave) discard;
  float span = max(max(extent.x, extent.y), extent.z);
  float stepSize = max(min(min(cell.x, cell.y), cell.z) * .35, length(extent) / maxSteps);
  vec4 sum = vec4(0.);
  vec3 L = normalize(lightDir);
  float hitDepth = 1.;
  vec3 position = camSource + ray * (enter + stepSize * .01);
  if (axis >= 0 && abs(position[axis] - cut) < stepSize * .025 && (mode != 1 || material(position) > .5)) {
    // the plane shows the interpolated stored field, not an invented boundary condition
    vec3 n = vec3(0.); n[axis] = flip != 0 ? -1. : 1.; if (dot(n, ray) > 0.) n = -n;
    sum = vec4(color(value(position)) * (.80 + .20 * max(dot(n, L), 0.)), 1.);
    if (material(position) >= .5 && mode == 0) sum.rgb = vec3(.70, .70, .69);
    hitDepth = depthOf(position);
  } else if (mode == 3) {
    // a body: its outside faces carry the stored field, lit by their normal
    vec3 n = vec3(0.); float best = 1e30;
    for (int k = 0; k < 3; k++) {
      float a = abs(position[k] - low[k]), b = abs(position[k] - high[k]);
      if (a < best) { best = a; n = vec3(0.); n[k] = -1.; }
      if (b < best) { best = b; n = vec3(0.); n[k] = 1.; }
    }
    vec3 h = normalize(L - ray);
    float hemi = .5 + .5 * n.z; vec3 amb = mix(vec3(.44, .43, .42), vec3(.62, .62, .64), hemi) * .74;
    sum = vec4(color(value(position)) * (amb + vec3(1., .96, .9) * .78 * max(dot(n, L), 0.)) + vec3(.06) * pow(max(dot(n, h), 0.), 32.), 1.);
    hitDepth = depthOf(position);
  } else {
    // interleaved gradient noise (Jimenez 2014): an even jitter, so thin bright layers do not speckle
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(.06711056, .00583715))));
    for (int i = 0; i < 1024; i++) {
      float distance = enter + (float(i) + jitter) * stepSize;
      if (float(i) >= maxSteps * 1.8 || distance > leave || sum.a > .995) break;
      vec3 p = camSource + ray * distance;
      float m = material(p), v = value(p);
      vec3 rgb = color(v);
      float strength = strengthOf(v);
      float alpha = 1. - exp(-24. * strength * strength * stepSize / span);
      if (mode == 1 || mode == 2) { // water or vortex surfaces: only the surface where the mask crosses one half, by bisection
        if (m < .5) continue;
        float a = max(enter, distance - stepSize), b = distance;
        for (int j = 0; j < 8; j++) { float mid = (a + b) * .5; if (material(camSource + ray * mid) > .5) b = mid; else a = mid; }
        distance = (a + b) * .5; p = camSource + ray * distance; rgb = color(value(p));
      }
      if (m >= .5) {
        vec3 gradient = vec3(material(p + vec3(cell.x, 0, 0)) - material(p - vec3(cell.x, 0, 0)),
                             material(p + vec3(0, cell.y, 0)) - material(p - vec3(0, cell.y, 0)),
                             material(p + vec3(0, 0, cell.z)) - material(p - vec3(0, 0, cell.z)));
        vec3 normal = dot(gradient, gradient) > 1e-10 ? normalize(-gradient) : -ray;
        if (dot(normal, ray) > 0.) normal = -normal;
        vec3 halfVector = normalize(L - ray);
        if (mode == 2) { // an iso-surface of a computed field (vortex cores): coloured by the shown field, lit as a surface
          float spec = pow(max(dot(normal, halfVector), 0.), 48.);
          float hemi = .5 + .5 * normal.z;
          rgb = rgb * (mix(vec3(.44, .43, .42), vec3(.62, .62, .64), hemi) * .74 + vec3(1., .96, .9) * .72 * max(dot(normal, L), 0.)) + vec3(.12) * spec;
        } else if (mode == 1) {
          float spec = pow(max(dot(normal, halfVector), 0.), 120.);
          float fresnel = .02 + .98 * pow(1. - max(dot(normal, -ray), 0.), 5.);
          // studio reflection is a labelled appearance choice, not a simulated optical field
          rgb = rgb * (.45 + .55 * max(dot(normal, L), 0.)) + vec3(.55, .56, .58) * fresnel * .5 + vec3(.9) * spec;
        } else rgb = vec3(.80, .80, .79) * (.45 + .55 * max(dot(normal, L), 0.)) + vec3(.5) * pow(max(dot(normal, halfVector), 0.), 40.);
        alpha = 1.;
        if (sum.a < .5) hitDepth = depthOf(p);
      } else if (alpha > .004 && shade > 0.) {
        // shade by the gradient of the field's strength: its level surfaces lit like surfaces
        vec3 e = cell;
        vec3 gs = vec3(strengthOf(value(p + vec3(e.x, 0, 0))) - strengthOf(value(p - vec3(e.x, 0, 0))),
                       strengthOf(value(p + vec3(0, e.y, 0))) - strengthOf(value(p - vec3(0, e.y, 0))),
                       strengthOf(value(p + vec3(0, 0, e.z))) - strengthOf(value(p - vec3(0, 0, e.z))));
        float gl = length(gs);
        if (gl > 1e-6) { vec3 n = -gs / gl; if (dot(n, ray) > 0.) n = -n;
          float lam = max(dot(n, L), 0.), sp = pow(max(dot(n, normalize(L - ray)), 0.), 24.);
          float w = clamp(gl * 6., 0., 1.); rgb = rgb * mix(1., .55 + .75 * lam, w) + vec3(.25) * sp * w * strength; }
      }
      sum.rgb += (1. - sum.a) * alpha * rgb; sum.a += (1. - sum.a) * alpha;
    }
    // a room's far walls, where the ray leaves the box (a cutaway: the near walls are not drawn)
    if (walls != 0 && sum.a < .995) {
      vec3 p = camSource + ray * leave, n = vec3(0.); float best = 1e30;
      for (int k = 0; k < 3; k++) {
        float a = abs(p[k] - origin[k]), b = abs(p[k] - origin[k] - extent[k]);
        if (a < best) { best = a; n = vec3(0.); n[k] = 1.; }
        if (b < best) { best = b; n = vec3(0.); n[k] = -1.; }
      }
      vec3 q = (p - origin) / extent; float edge = min(min(min(q.x, 1. - q.x), min(q.y, 1. - q.y)), min(q.z, 1. - q.z));
      vec3 base = n.z > .5 ? vec3(.60, .60, .59) : vec3(.70, .70, .69); // studio: light walls and floor
      vec3 w = base * (.55 + .45 * max(dot(n, L), 0.)) * (.75 + .25 * smoothstep(0., .08, edge)); // linear, as in the app
      sum.rgb += (1. - sum.a) * w; sum.a = 1.;
      hitDepth = min(hitDepth, depthOf(p));
    }
  }
  if (sum.a < .002) discard;
  gl_FragDepth = hitDepth;
  gl_FragColor = sum; // premultiplied, linear
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;

// lab axes from glTF axes: lab (x, y, z) = glTF (x, -z, y)
const TO_SOURCE = new THREE.Matrix3().set(1, 0, 0, 0, 0, -1, 0, 1, 0);
const MODES = { fog: 0, water: 1, iso: 2, solid: 3 };
const LIGHT = new THREE.Vector3(-0.45, -0.35, 0.82).normalize(); // src/lab/style.h STYLE_LIGHT, toward the key light

function texture3d(data, dims, type, format) {
  const t = new THREE.Data3DTexture(data, dims[0], dims[1], dims[2]);
  t.type = type, t.format = format;
  t.minFilter = t.magFilter = THREE.LinearFilter;
  t.wrapS = t.wrapT = t.wrapR = THREE.ClampToEdgeWrapping;
  t.unpackAlignment = 1;
  t.needsUpdate = true;
  return t;
}

// a volume of one frame: info is extras.result.volume, values a Uint16Array of half floats, mask a Uint8Array or null
export function makeVolume(info, frame, values, mask, palette) {
  const dims = info.dims, o = info.origin, c = info.cell;
  const extent = new THREE.Vector3(c[0] * dims[0], c[1] * dims[1], c[2] * dims[2]);
  const origin = new THREE.Vector3(o[0], o[1], o[2]);
  const fieldTex = texture3d(values, dims, THREE.HalfFloatType, THREE.RedFormat);
  const maskTex = mask ? texture3d(mask, dims, THREE.UnsignedByteType, THREE.RedFormat)
    : texture3d(new Uint8Array(1), [1, 1, 1], THREE.UnsignedByteType, THREE.RedFormat);
  const section = frame.section;
  const uniforms = {
    fieldTex: { value: fieldTex }, maskTex: { value: maskTex }, palette: { value: palette },
    toSource: { value: TO_SOURCE }, fromSourceMV: { value: new THREE.Matrix4() },
    camSource: { value: new THREE.Vector3() }, origin: { value: origin }, extent: { value: extent },
    cell: { value: new THREE.Vector3(c[0], c[1], c[2]) }, lightDir: { value: LIGHT },
    range: { value: new THREE.Vector2(frame.lo, frame.hi) }, maxSteps: { value: 200 }, shade: { value: 1 },
    cutPosition: { value: section ? section.position : 0 }, axis: { value: section ? section.axis : -1 },
    flip: { value: section && section.flip ? 1 : 0 }, mode: { value: MODES[info.mode] ?? 0 },
    walls: { value: info.walls ? 1 : 0 }, hasMask: { value: mask ? 1 : 0 },
  };
  const material = new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3, vertexShader, fragmentShader, uniforms, side: THREE.BackSide,
    transparent: false, depthWrite: true, depthTest: true, premultipliedAlpha: true,
    blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor,
    blendSrcAlpha: THREE.OneFactor, blendDstAlpha: THREE.OneMinusSrcAlphaFactor,
  });
  material.toneMapped = true;
  // the box in glTF axes: lab x stays x, lab z is up (y), lab y is -z
  const geo = new THREE.BoxGeometry(extent.x, extent.z, extent.y);
  geo.translate(origin.x + extent.x / 2, origin.z + extent.z / 2, -(origin.y + extent.y / 2));
  const mesh = new THREE.Mesh(geo, material);
  mesh.name = 'volume';
  mesh.renderOrder = -1;
  mesh.frustumCulled = false;
  const inv = new THREE.Matrix4(), fromSource = new THREE.Matrix4().set(1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1);
  const cam = new THREE.Vector3();
  mesh.onBeforeRender = (renderer, scene, camera) => { // per eye: the camera in lab axes, and lab axes to view space
    inv.copy(mesh.matrixWorld).invert();
    cam.setFromMatrixPosition(camera.matrixWorld).applyMatrix4(inv).applyMatrix3(TO_SOURCE);
    uniforms.camSource.value.copy(cam);
    uniforms.fromSourceMV.value.multiplyMatrices(camera.matrixWorldInverse, mesh.matrixWorld).multiply(fromSource);
  };
  mesh.userData.volume = { info, values, mask, origin, extent, lo: frame.lo, hi: frame.hi, section };
  mesh.dispose = () => { geo.dispose(); material.dispose(); fieldTex.dispose(); maskTex.dispose(); };
  return mesh;
}

// ---- reading the volume on the CPU: what a probe ray sees ----

const halfTable = (() => { // half float bits to float, once
  const t = new Float32Array(65536);
  for (let h = 0; h < 65536; h++) {
    const s = h & 0x8000 ? -1 : 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    t[h] = e === 0 ? s * m * 2 ** -24 : e === 31 ? (m ? NaN : s * Infinity) : s * (1 + m / 1024) * 2 ** (e - 15);
  }
  return t;
})();

function sampler(vol) { // trilinear, as the GPU samples it, in lab axes; returns [value, mask]
  const { info, values, mask, origin, extent, lo, hi } = vol, [nx, ny, nz] = info.dims;
  const at = (arr, i, j, k, f) => f(arr[i + nx * (j + ny * k)]);
  const tri = (arr, f, p) => {
    const u = (p.x - origin.x) / extent.x * nx - 0.5, v = (p.y - origin.y) / extent.y * ny - 0.5, w = (p.z - origin.z) / extent.z * nz - 0.5;
    const i0 = Math.max(0, Math.min(nx - 1, Math.floor(u))), j0 = Math.max(0, Math.min(ny - 1, Math.floor(v))), k0 = Math.max(0, Math.min(nz - 1, Math.floor(w)));
    const i1 = Math.min(nx - 1, i0 + 1), j1 = Math.min(ny - 1, j0 + 1), k1 = Math.min(nz - 1, k0 + 1);
    const fu = Math.max(0, Math.min(1, u - i0)), fv = Math.max(0, Math.min(1, v - j0)), fw = Math.max(0, Math.min(1, w - k0));
    const c = (i, j, k) => at(arr, i, j, k, f);
    const x00 = c(i0, j0, k0) * (1 - fu) + c(i1, j0, k0) * fu, x10 = c(i0, j1, k0) * (1 - fu) + c(i1, j1, k0) * fu;
    const x01 = c(i0, j0, k1) * (1 - fu) + c(i1, j0, k1) * fu, x11 = c(i0, j1, k1) * (1 - fu) + c(i1, j1, k1) * fu;
    return (x00 * (1 - fv) + x10 * fv) * (1 - fw) + (x01 * (1 - fv) + x11 * fv) * fw;
  };
  return (p) => [lo + tri(values, (h) => halfTable[h], p) * (hi - lo), mask ? tri(mask, (b) => b / 255, p) : 0];
}

// the first place along a ray (lab axes) where the picture becomes opaque: a section plane, an iso surface, water, a
// body, a solid's face, or the fog once half of what is seen; null when the ray only passes through thin fog
export function probeVolume(vol, origin3, dir3) {
  const { info, origin, extent, lo, hi, section } = vol;
  const low = origin.clone(), high = origin.clone().add(extent);
  if (section && section.axis >= 0) {
    const k = 'xyz'[section.axis];
    if (section.flip) low[k] = Math.max(low[k], section.position); else high[k] = Math.min(high[k], section.position);
  }
  let enter = 0, leave = Infinity, axisIn = -1;
  for (let k = 0; k < 3; k++) {
    const c = 'xyz'[k], d = Math.abs(dir3[c]) < 1e-12 ? 1e-12 : dir3[c];
    const a = (low[c] - origin3[c]) / d, b = (high[c] - origin3[c]) / d;
    if (Math.min(a, b) > enter) enter = Math.min(a, b), axisIn = k;
    leave = Math.min(leave, Math.max(a, b));
  }
  if (!(enter < leave)) return null;
  const sample = sampler(vol), at = (t) => origin3.clone().addScaledVector(dir3, t);
  const span = Math.max(extent.x, extent.y, extent.z), cell = Math.min(...info.cell);
  const signed = lo < 0 && hi > 0, mag = Math.max(Math.abs(lo), Math.abs(hi));
  const strength = (v) => Math.min(1, Math.max(0, signed ? Math.abs(v) / mag : (v - lo) / Math.max(hi - lo, 1e-20)));
  const first = at(enter + 1e-9);
  if ((section && section.axis === axisIn) || info.mode === 'solid') return { point: first, value: sample(first)[0], what: section && section.axis === axisIn ? 'section' : 'surface' };
  const step = Math.max(cell * 0.35, Math.hypot(extent.x, extent.y, extent.z) / 600);
  let alpha = 0;
  for (let t = enter; t <= leave; t += step) {
    const p = at(t), [v, m] = sample(p);
    if (m >= 0.5 && info.mode !== 'fog') return { point: p, value: v, what: info.mode === 'iso' ? 'iso surface' : info.mode === 'water' ? 'water surface' : 'body' };
    if (m >= 0.5) return { point: p, value: v, what: 'body' };
    if (info.mode === 'iso' || info.mode === 'water') continue; // only their surfaces are drawn, as in the app
    const s = strength(v);
    alpha += (1 - alpha) * (1 - Math.exp(-24 * s * s * step / span));
    if (alpha >= 0.5) return { point: p, value: v, what: 'volume' };
  }
  return null;
}

export { TO_SOURCE };
export const half = (h) => halfTable[h]; // a half float's value
