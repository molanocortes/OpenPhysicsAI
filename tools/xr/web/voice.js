// voice.js - the spoken half of the headset: a question recorded here as 16 kHz mono speech (the Quest's browser has no
// speech recognition, so the Mac turns it into words), and the AI's answers played back in the Mac's voice, sentence by
// sentence as the AI writes them (the bridge's speech chunks), one after another without a gap.
//
// The recording context runs at 16 kHz, so the browser itself filters and resamples the microphone; where it cannot,
// the samples are low-passed here before they are resampled (a plain decimation would fold the hiss of "s" and "f"
// above 8 kHz back into the speech band): a Blackman-windowed sinc, cut off at 7 kHz, 8 ms long.
import { Vad } from './vad.js';

const RATE = 16000, CUTOFF = 7000;

export function lowpass(samples, rate, cutoff = CUTOFF) {
  const half = Math.max(8, Math.round(0.004 * rate)), taps = new Float32Array(2 * half + 1), fc = cutoff / rate;
  let sum = 0;
  for (let i = -half; i <= half; i++) {
    const sinc = i === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * i) / (Math.PI * i);
    const w = 0.42 + 0.5 * Math.cos(Math.PI * i / half) + 0.08 * Math.cos(2 * Math.PI * i / half);
    sum += taps[i + half] = sinc * w;
  }
  const out = new Float32Array(samples.length);
  for (let n = 0; n < samples.length; n++) {
    let acc = 0;
    for (let i = -half; i <= half; i++) {
      const j = n + i;
      if (j >= 0 && j < samples.length) acc += taps[i + half] * samples[j];
    }
    out[n] = acc / sum; // unit gain at zero frequency
  }
  return out;
}

export function wav16(samples, rate) { // Float32 mono at rate -> 16-bit PCM WAV at 16 kHz
  let pcm = samples;
  if (rate !== RATE) {
    const n = Math.floor(samples.length * RATE / rate), smooth = lowpass(samples, rate);
    pcm = new Float32Array(n);
    for (let i = 0; i < n; i++) { // linear between the filtered samples
      const x = i * rate / RATE, i0 = Math.floor(x), f = x - i0;
      pcm[i] = smooth[i0] * (1 - f) + (smooth[Math.min(i0 + 1, smooth.length - 1)] || 0) * f;
    }
  }
  const out = new DataView(new ArrayBuffer(44 + pcm.length * 2));
  const w = (p, s) => [...s].forEach((ch, i) => out.setUint8(p + i, ch.charCodeAt(0)));
  w(0, 'RIFF'); out.setUint32(4, 36 + pcm.length * 2, true); w(8, 'WAVEfmt '); out.setUint32(16, 16, true);
  out.setUint16(20, 1, true); out.setUint16(22, 1, true); out.setUint32(24, RATE, true); out.setUint32(28, RATE * 2, true);
  out.setUint16(32, 2, true); out.setUint16(34, 16, true); w(36, 'data'); out.setUint32(40, pcm.length * 2, true);
  for (let i = 0; i < pcm.length; i++) out.setInt16(44 + i * 2, Math.max(-1, Math.min(1, pcm[i])) * 32767, true);
  return out.buffer;
}

export class Voice extends EventTarget {
  constructor(link) {
    super();
    this.link = link;
    this.stream = null;
    this.ctx = null; // recording, at 16 kHz where the browser allows it
    this.out = null; // playback, at the device's own rate
    this.listening = false;
    this.chunks = [];
    this.rate = RATE;
    this.speechOn = true;
    this.lastChunk = null; // the last sentence queued; null until the first state (what was said before is not heard)
    this.mutedTurn = 0;    // the answer stopped here (said over, or "stop"): the rest of its sentences are not played
    this.sources = new Set();
    this.endAt = 0;        // when the last queued sentence ends, in the playback context's time
    this.chain = Promise.resolve();
    this.said = [];        // the words just played, so the AI's own voice heard by the microphone is not taken as a question
    this.handsFree = false; // listening all the time (vad.js): a sentence is taken when it ends, nothing is pressed
    this.vad = null;
  }

  // hands-free: the microphone stays open and each sentence is given out as a WAV when it ends ('utterance'); 'speechstart'
  // as one begins (to quiet the AI's voice when the person talks over it); level 0..1 for the button's ring
  listen(on) {
    if (on === this.handsFree) return true;
    if (on && !this.stream) return false;
    this.handsFree = on;
    if (!on) {
      if (this.vproc) this.vproc.disconnect(), this.vsource.disconnect(), this.vproc.onaudioprocess = null;
      this.vproc = this.vsource = this.vad = null;
      return true;
    }
    this.audio();
    let ctx = this.ctx;
    try { this.vsource = ctx.createMediaStreamSource(this.stream); } catch (e) {
      ctx.close();
      ctx = this.ctx = new AudioContext();
      this.vsource = ctx.createMediaStreamSource(this.stream);
    }
    const rate = ctx.sampleRate;
    this.vad = new Vad(rate);
    this.vproc = ctx.createScriptProcessor(1024, 1, 1);
    this.vproc.onaudioprocess = (e) => {
      if (this.listening) return; // the grip is held: that recording takes the sentence
      for (const ev of this.vad.feed(e.inputBuffer.getChannelData(0), this.speaking)) {
        if (ev.type === 'start') this.dispatchEvent(new Event('speechstart'));
        else if (ev.samples) this.dispatchEvent(new CustomEvent('utterance', { detail: wav16(ev.samples, rate) }));
        else this.dispatchEvent(new Event('silence')); // too short to be a sentence
      }
    };
    this.vsource.connect(this.vproc);
    this.vproc.connect(ctx.destination);
    return true;
  }

  get level() { return this.vad ? this.vad.level : 0; }

  // words the AI has just said, heard back by the microphone: not a question from the person
  echo(text) {
    const now = Date.now(), recent = this.said.filter((s) => s.until > now);
    this.said = recent;
    const words = (t) => t.toLowerCase().replace(/[^a-z0-9 ]/g, ' ').split(/\s+/).filter((w) => w.length > 2);
    const heard = words(text), spoken = new Set(recent.flatMap((s) => words(s.text)));
    return heard.length > 0 && heard.filter((w) => spoken.has(w)).length / heard.length > 0.6;
  }

  get allowed() { return !!this.stream; }

  async allow() {
    if (new URLSearchParams(location.search).has('fakemic')) { // the page test's microphone: a quiet tone, never otherwise
      const ctx = this.audio(), osc = ctx.createOscillator(), gain = ctx.createGain(), dst = ctx.createMediaStreamDestination();
      gain.gain.value = 0.001;
      osc.connect(gain).connect(dst);
      osc.start();
      this.stream = dst.stream;
      return;
    }
    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true },
    });
    this.audio(); // the gesture that allowed the microphone also allows sound
  }

  audio() { // both contexts, resumed inside a gesture
    if (!this.ctx) {
      try { this.ctx = new AudioContext({ sampleRate: RATE }); } catch (e) { this.ctx = new AudioContext(); }
    }
    if (!this.out) this.out = new AudioContext();
    for (const c of [this.ctx, this.out]) if (c.state === 'suspended') c.resume();
    return this.out;
  }

  start() {
    if (!this.stream || this.listening) return false;
    this.stopSpeaking();
    this.audio();
    let ctx = this.ctx;
    try {
      this.source = ctx.createMediaStreamSource(this.stream);
    } catch (e) { // a browser that will not resample a microphone: record at its rate and filter here
      ctx.close();
      ctx = this.ctx = new AudioContext();
      this.source = ctx.createMediaStreamSource(this.stream);
    }
    this.rate = ctx.sampleRate;
    this.proc = ctx.createScriptProcessor(4096, 1, 1);
    this.chunks = [];
    this.proc.onaudioprocess = (e) => this.chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));
    this.source.connect(this.proc);
    this.proc.connect(ctx.destination);
    this.listening = true;
    return true;
  }

  // the recording as WAV, or null for a tap rather than a sentence
  stop() {
    if (!this.listening) return null;
    this.listening = false;
    this.proc.disconnect();
    this.source.disconnect();
    this.proc.onaudioprocess = null;
    const n = this.chunks.reduce((a, c) => a + c.length, 0), all = new Float32Array(n);
    let o = 0;
    for (const c of this.chunks) all.set(c, o), o += c.length;
    if (n < this.rate * 0.4) return null;
    return wav16(all, this.rate);
  }

  // the bridge's sentences (state.speech: {turn, stopped, chunks: [{id, turn, text, seconds}]}): each new one fetched and
  // decoded at once, then played after the one before it, in order
  play(speech) {
    if (!speech) return;
    const chunks = (speech.chunks || []).slice().sort((a, b) => a.id - b.id);
    if (this.lastChunk === null) { this.lastChunk = chunks.length ? chunks[chunks.length - 1].id : 0; return; }
    if (speech.stopped && speech.turn > this.mutedTurn) this.mute(speech.turn);
    for (const c of chunks) {
      if (c.id <= this.lastChunk) continue;
      this.lastChunk = c.id;
      if (!this.speechOn || c.turn <= this.mutedTurn) continue;
      const turn = c.turn, decoded = fetch('./speech?id=' + c.id, { cache: 'no-store' })
        .then((r) => (r.ok ? r.arrayBuffer() : null)).then((b) => (b ? this.audio().decodeAudioData(b) : null)).catch(() => null);
      this.chain = this.chain.then(async () => {
        const buf = await decoded;
        if (!buf || turn <= this.mutedTurn || !this.speechOn) return;
        const ctx = this.audio(), at = Math.max(ctx.currentTime + 0.03, this.endAt);
        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(ctx.destination);
        src.onended = () => this.sources.delete(src);
        this.sources.add(src);
        src.start(at);
        this.endAt = at + buf.duration;
        this.said.push({ text: c.text, until: Date.now() + (at - ctx.currentTime + buf.duration) * 1000 + 1500 });
      });
    }
  }

  get speaking() { return !!this.out && this.endAt > this.out.currentTime; }

  // the answer being spoken goes quiet, and what is still to come of it is not played
  mute(turn) {
    this.mutedTurn = Math.max(this.mutedTurn, turn);
    this.stopSpeaking();
  }

  stopSpeaking() {
    for (const src of this.sources) { try { src.stop(); } catch (e) { /* already ended */ } }
    this.sources.clear();
    this.endAt = 0;
  }
}
