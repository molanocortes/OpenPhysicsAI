// vad.js - when someone is speaking: the headset listens all the time and takes a sentence when it ends, so nothing has
// to be pressed (docs/xr.md, "Talking with the AI"). Energy in 20 ms frames against a noise floor that follows the
// room (quickly down, slowly up): a sentence starts after 80 ms clearly above it (12 dB), and ends after 0.7 s of
// quiet (below 7 dB above it); the 0.3 s before its start are kept, so its first sound is not cut. While the AI is
// speaking, its own voice coming back from the speakers must not start a sentence: then 18 dB for 240 ms are needed.
// Pure: frames in, events out (tested in tools/xr/pagetest.mjs with synthetic sound).
export const FRAME_S = 0.02;

export class Vad {
  constructor(rate) {
    this.rate = rate;
    this.n = Math.round(rate * FRAME_S); // samples a frame
    this.floor = -60;    // dB of full scale: the room's noise, followed
    this.run = 0;        // loud frames in a row
    this.quiet = 0;      // quiet frames in a row inside a sentence
    this.voiced = 0;     // frames of the sentence that were not quiet
    this.inside = false;
    this.pre = [];       // the last frames before a sentence starts
    this.frames = [];    // the sentence so far
    this.level = 0;      // 0..1, how far above the floor (for the button's ring)
    this.partial = new Float32Array(0);
  }

  // samples (any length) in; events out: {type: 'start'} when a sentence starts, {type: 'end', samples} when it
  // ends (null samples for one too short to be speech); aiSpeaking: the AI's voice may be in the microphone
  feed(samples, aiSpeaking = false) {
    const events = [];
    let buf = samples;
    if (this.partial.length) {
      buf = new Float32Array(this.partial.length + samples.length);
      buf.set(this.partial), buf.set(samples, this.partial.length);
    }
    let i = 0;
    for (; i + this.n <= buf.length; i += this.n) {
      const ev = this.frame(buf.subarray(i, i + this.n), aiSpeaking);
      if (ev) events.push(ev);
    }
    this.partial = buf.slice(i);
    return events;
  }

  frame(f, aiSpeaking) {
    let s = 0;
    for (let k = 0; k < f.length; k++) s += f[k] * f[k];
    const db = 10 * Math.log10(s / f.length + 1e-12);
    this.level = Math.max(0, Math.min(1, (db - this.floor - 6) / 30));
    const copy = Float32Array.from(f);
    if (!this.inside) {
      this.pre.push(copy);
      if (this.pre.length > 15) this.pre.shift();
      const on = this.floor + (aiSpeaking ? 18 : 12), need = aiSpeaking ? 12 : 4;
      this.run = db > on && db > -55 ? this.run + 1 : 0;
      if (this.run >= need) {
        this.inside = true, this.voiced = need, this.run = 0, this.quiet = 0;
        this.frames = this.pre.slice();
        this.pre = [];
        return { type: 'start' };
      }
      // the floor follows the room only outside a sentence: down at once, up slowly
      this.floor += (db - this.floor) * (db < this.floor ? 0.3 : 0.01);
      this.floor = Math.max(-90, Math.min(-25, this.floor));
      return null;
    }
    this.frames.push(copy);
    this.quiet = db < this.floor + 7 ? this.quiet + 1 : 0;
    if (!this.quiet) this.voiced++;
    const long = this.frames.length * FRAME_S > 20;
    if (this.quiet * FRAME_S >= 0.7 || long) {
      this.inside = false;
      const keep = this.frames.length - Math.max(0, this.quiet - 10); // 0.2 s of the quiet at its end stays
      const frames = this.frames.slice(0, keep), spoken = this.voiced * FRAME_S;
      this.frames = [], this.quiet = 0, this.voiced = 0;
      if (spoken < 0.25) return { type: 'end', samples: null }; // a cough, a knock: not a sentence
      const out = new Float32Array(frames.length * this.n);
      frames.forEach((fr, k) => out.set(fr, k * this.n));
      return { type: 'end', samples: out };
    }
    return null;
  }
}
