// probe.js - the value under the pointer, read from the numbers the scene carries (each vertex's stored _VALUE, each
// particle's, the volume's half-float samples), never from the colours: interpolated across a triangle as the colour
// is, a particle's own, and in a volume the place where the picture becomes opaque (volume.js probeVolume). Its place
// is given in the result's own coordinates (the scene's to_source: a part's build frame in mm, or the lab's axes in m).
// A tap pins a reading; the pins and the current reading are what the AI is told the person points at.
import * as THREE from 'three';
import { probeVolume, TO_SOURCE, half } from './volume.js';
import { fmt } from './card.js';

const ray = new THREE.Raycaster();
ray.params.Points.threshold = 0.004;
ray.params.Line.threshold = 0.002;

function barycentric(p, a, b, c) { // weights of a, b, c at p (in the triangle's plane)
  const v0 = b.clone().sub(a), v1 = c.clone().sub(a), v2 = p.clone().sub(a);
  const d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
  const den = d00 * d11 - d01 * d01;
  if (Math.abs(den) < 1e-30) return [1, 0, 0];
  const v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
  return [1 - v - w, v, w];
}

export class Prober {
  constructor(parent) {
    const dot = (color) => new THREE.Mesh(new THREE.SphereGeometry(1, 16, 12), new THREE.MeshBasicMaterial({ color, depthTest: false, toneMapped: false }));
    this.marker = dot(0x111111);
    this.marker.renderOrder = 20;
    this.marker.visible = false;
    parent.add(this.marker);
    this.parent = parent;
    this.pins = [];
    this.current = null;
    this.lastSeen = 0;
    this.dot = dot;
  }

  // the reading along a ray (world), against the shown content; null when nothing is hit
  read(origin, direction, content, facts) {
    if (!content || !facts) return null;
    ray.set(origin, direction);
    const scale = new THREE.Vector3().setFromMatrixScale(content.matrixWorld).x || 1;
    ray.params.Points.threshold = Math.max(0.003, (facts.pointRadius || 0) * scale * 1.5);
    let best = null;
    const meshes = [];
    content.traverse((o) => { if ((o.isMesh || o.isPoints) && o.name !== 'volume' && o.visible) meshes.push(o); });
    for (const h of ray.intersectObjects(meshes, false)) {
      const o = h.object, geo = o.geometry, val = geo.attributes._value, ud = geo.userData || {};
      let value = null;
      if (o.isPoints) {
        if (val) value = val.getX(h.index);
      } else if (ud.key === 'picture' && facts.plane && h.uv) { // a plane: its value grid, rows from the bottom
        const [gw, gh] = facts.plane.dims, x = h.uv.x * gw - 0.5, y = (1 - h.uv.y) * gh - 0.5;
        const i0 = Math.max(0, Math.min(gw - 1, Math.floor(x))), j0 = Math.max(0, Math.min(gh - 1, Math.floor(y)));
        const i1 = Math.min(gw - 1, i0 + 1), j1 = Math.min(gh - 1, j0 + 1), tx = Math.max(0, Math.min(1, x - i0)), ty = Math.max(0, Math.min(1, y - j0));
        const at = (i, j) => half(facts.plane.values[j * gw + i]);
        const t = (1 - ty) * ((1 - tx) * at(i0, j0) + tx * at(i1, j0)) + ty * ((1 - tx) * at(i0, j1) + tx * at(i1, j1));
        value = facts.lo + t * (facts.hi - facts.lo);
      } else if (val && h.face) {
        const inv = new THREE.Matrix4().copy(o.matrixWorld).invert(), p = h.point.clone().applyMatrix4(inv);
        const pos = geo.attributes.position, A = new THREE.Vector3().fromBufferAttribute(pos, h.face.a);
        const B = new THREE.Vector3().fromBufferAttribute(pos, h.face.b), C = new THREE.Vector3().fromBufferAttribute(pos, h.face.c);
        const [wa, wb, wc] = barycentric(p, A, B, C);
        value = wa * val.getX(h.face.a) + wb * val.getX(h.face.b) + wc * val.getX(h.face.c);
      }
      const part = geo.attributes._part ? geo.attributes._part.getX(o.isPoints ? h.index : h.face.a) : ud.part;
      const field = ud.key === 'field' || (ud.key === 'picture' && !!facts.plane);
      best = { point: h.point.clone(), distance: h.distance, value: Number.isFinite(value) ? value : null, part,
               what: ud.key === 'look' ? ud.look : ud.key === 'picture' ? 'plane' : ud.key === 'field' ? 'surface' : 'body', hasField: field };
      if (o.isPoints) best.what = 'particle';
      break;
    }
    // the volume: what the ray sees in it, in the lab's axes
    const vol = content.getObjectByName('volume');
    if (vol) {
      const inv = new THREE.Matrix4().copy(vol.matrixWorld).invert();
      const o3 = origin.clone().applyMatrix4(inv).applyMatrix3(TO_SOURCE);
      const d3 = origin.clone().add(direction).applyMatrix4(inv).applyMatrix3(TO_SOURCE).sub(o3).normalize();
      const hit = probeVolume(vol.userData.volume, o3, d3);
      if (hit) {
        const back = new THREE.Matrix3().copy(TO_SOURCE).invert();
        const world = hit.point.clone().applyMatrix3(back).applyMatrix4(vol.matrixWorld);
        const distance = world.distanceTo(origin);
        if (!best || distance < best.distance)
          best = { point: world, distance, value: hit.value, part: null, what: hit.what, hasField: true };
      }
    }
    if (!best) return null;
    return this.describe(best, content, facts);
  }

  describe(r, content, facts) {
    const local = r.point.clone().applyMatrix4(new THREE.Matrix4().copy(content.matrixWorld).invert());
    const src = local.applyMatrix4(facts.toSource);
    const unit = facts.sourceUnit;
    const partName = r.part !== null && r.part !== undefined && facts.parts ? facts.parts[r.part] : null;
    // a picture without metres (the black hole) has no place to give
    const where = unit ? `x ${fmt(src.x)}  y ${fmt(src.y)}  z ${fmt(src.z)} ${unit}` : '';
    const val = r.hasField && r.value !== null ? `${facts.field} ${fmt(r.value)} ${facts.unit}`.trim() : r.hasField ? `${facts.field}: no value` : null;
    const text = [partName, val || (r.what !== 'surface' ? r.what : null)].filter(Boolean).join(' · ') || r.what;
    const span = facts.hi - facts.lo;
    return { ...r, source: src, unit, partName, where, text,
             t: r.hasField && r.value !== null && span > 0 ? (r.value - facts.lo) / span : null };
  }

  show(reading) {
    this.current = reading || (performance.now() - this.lastSeen < 4000 ? this.current : null);
    if (reading) this.lastSeen = performance.now();
    this.marker.visible = !!reading;
    if (reading) {
      this.parent.worldToLocal(this.marker.position.copy(reading.point));
      this.marker.scale.setScalar(0.003 / this.scale());
    }
  }

  // pins live in the model's frame, so they stay on the result as it moves, plays and changes field
  pin(reading) {
    if (!reading) return;
    const local = this.parent.worldToLocal(reading.point.clone());
    const near = this.pins.findIndex((p) => p.mesh.position.distanceTo(local) * this.scale() < 0.01);
    if (near >= 0) { // a tap on a pin takes it away
      this.parent.remove(this.pins[near].mesh);
      this.pins.splice(near, 1);
      return;
    }
    const mesh = this.dot(0xE8551C);
    mesh.renderOrder = 21;
    mesh.position.copy(local);
    mesh.scale.setScalar(0.004 / this.scale());
    this.parent.add(mesh);
    this.pins.push({ reading, mesh });
    if (this.pins.length > 8) this.parent.remove(this.pins.shift().mesh);
  }

  scale() { return new THREE.Vector3().setFromMatrixScale(this.parent.matrixWorld).x || 1; }

  rescale() { // markers keep their size in the room when the result is scaled
    const s = this.scale();
    for (const p of this.pins) p.mesh.scale.setScalar(0.004 / s);
  }

  clear() {
    for (const p of this.pins) this.parent.remove(p.mesh);
    this.pins = [];
  }

  // what the AI is told: the reading pointed at in the last few seconds, and the pinned ones
  context(facts) {
    if (!facts) return '';
    const say = (r) => `${r.partName ? `part '${r.partName}'` : `the ${r.what}`}${r.where ? ', ' + r.where.replace(/  /g, ', ') : ''}${r.value !== null && r.hasField ? `, where ${facts.field} is ${fmt(r.value)} ${facts.unit}` : ''}`;
    const parts = [];
    if (this.current && performance.now() - this.lastSeen < 4000) parts.push('the person points at ' + say(this.current));
    if (this.pins.length) parts.push('pinned points: ' + this.pins.map((p, i) => `${i + 1}) ${say(p.reading)}`).join('; '));
    return parts.join('; ');
  }
}
