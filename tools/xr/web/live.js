// live.js - the provider's voice in the headset (tools/xr/live.py, docs/xr.md "The provider's voice"): the microphone
// goes to OpenAI's GPT-Live over WebRTC and its voice comes back on a media track, full duplex, so the person can talk
// over it. The Mac holds the key: the bridge trades this page's SDP offer for OpenAI's answer and answers the voice's
// requests with the app's agent. This page plays the voice, mutes the microphone when asked, and tells the bridge what
// the headset shows; what both sides say comes back in the state (live.you, live.ai).
export class Live extends EventTarget {
  constructor(link) {
    super();
    this.link = link;
    this.pc = null;
    this.audio = null;
    this.track = null;
    this.state = 'off'; // off, connecting, live
    this.muted = false;
    this.error = '';
    this.analyser = null;
  }

  get active() { return this.state === 'live'; }

  emit() { this.dispatchEvent(new Event('change')); }

  // stream: the microphone, already allowed; ctx: an AudioContext to measure the voice's loudness with
  async start(stream, ctx) {
    if (this.pc) return this.state !== 'off';
    this.state = 'connecting', this.error = '';
    this.emit();
    const pc = this.pc = new RTCPeerConnection();
    this.audio = new Audio();
    this.audio.autoplay = true;
    pc.ontrack = (e) => {
      this.audio.srcObject = e.streams[0];
      this.audio.play().catch(() => {});
      try { // how loud the voice is, for the button while it speaks
        const src = ctx.createMediaStreamSource(e.streams[0]);
        this.analyser = ctx.createAnalyser();
        this.analyser.fftSize = 512;
        src.connect(this.analyser);
      } catch (err) { this.analyser = null; }
    };
    pc.onconnectionstatechange = () => {
      if (pc === this.pc && (pc.connectionState === 'failed' || pc.connectionState === 'closed')) this.fail('The voice connection was lost.');
    };
    this.track = stream.getAudioTracks()[0];
    pc.addTrack(this.track, stream); // one audio line, sent and received
    try {
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      const r = await this.link.post('./live/start', JSON.stringify({ sdp: offer.sdp }));
      if (pc !== this.pc) return false;
      if (!r.ok || !r.sdp) return this.fail(r.error || 'The voice did not start.'), false;
      await pc.setRemoteDescription({ type: 'answer', sdp: r.sdp });
    } catch (e) {
      return this.fail('The voice could not be connected: ' + e.message), false;
    }
    this.state = 'live';
    this.startedAt = performance.now();
    this.mute(false);
    this.emit();
    return true;
  }

  mute(on) { // the microphone off (nothing is sent) or on again
    this.muted = on;
    if (this.track) this.track.enabled = !on;
    this.emit();
  }

  quiet() { // "stop": what the voice was saying stops at once; it is also told to stop (the bridge's /stop)
    if (!this.audio) return;
    this.audio.muted = true;
    clearTimeout(this.unquiet);
    this.unquiet = setTimeout(() => { if (this.audio) this.audio.muted = false; }, 1200);
  }

  get speaking() { // the voice's loudness right now, 0..1
    if (!this.analyser || !this.active) return 0;
    const d = new Uint8Array(this.analyser.fftSize);
    this.analyser.getByteTimeDomainData(d);
    let s = 0;
    for (const v of d) s += (v - 128) * (v - 128);
    return Math.min(1, Math.sqrt(s / d.length) / 32);
  }

  stop() {
    const pc = this.pc;
    this.pc = null;
    if (pc) { pc.close(); this.link.post('./live/stop', '{}'); }
    if (this.audio) this.audio.srcObject = null;
    this.analyser = null;
    this.state = 'off';
    this.emit();
  }

  fail(why) {
    this.error = why;
    this.stop();
    this.dispatchEvent(new Event('failed'));
  }
}
