// link.js - the page's line to the Mac (tools/xr/xr_bridge.py): the app's state, typed and spoken questions with what
// the person points at and a picture of what they see, the view commands the headset may give (tools/xr/commands.json),
// the answer to a permission prompt. Nothing here decides anything: the app on the Mac holds the result and runs the commands.
export class Link extends EventTarget {
  constructor() {
    super();
    this.state = { scene: 'none', agent: { lines: [] } };
    this.online = false;
    this.lastCommand = '';
  }

  // the state as the bridge pushes it, each time it changes (server-sent events: no poll between a word written on the Mac
  // and the headset); polled every 350 ms where the stream does not work (an older bridge)
  start(every = 350) {
    if (typeof EventSource !== 'undefined' && !this.polling) {
      let opened = false;
      const es = this.stream = new EventSource('./events');
      es.onmessage = (e) => { opened = true; try { this.accept(JSON.parse(e.data)); } catch (err) { /* a broken line */ } };
      es.onerror = () => {
        if (!opened) { es.close(); this.polling = true; this.start(every); return; }
        if (this.online) this.dispatchEvent(new CustomEvent('offline'));
        this.online = false; // the browser reconnects by itself
      };
      return;
    }
    const tick = async () => {
      await this.poll();
      setTimeout(tick, every);
    };
    tick();
  }

  accept(s) {
    const before = this.state;
    this.state = s;
    this.online = true;
    this.dispatchEvent(new CustomEvent('state', { detail: { state: s, before } }));
  }

  async poll() {
    try {
      const r = await fetch('./state', { cache: 'no-store' });
      this.accept(await r.json());
    } catch (e) {
      if (this.online) this.dispatchEvent(new CustomEvent('offline'));
      this.online = false;
    }
  }

  async post(path, body, type = 'application/json') {
    try {
      const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': type }, body });
      return { ok: r.ok, status: r.status, ...(await r.json().catch(() => ({}))) };
    } catch (e) {
      return { ok: false, status: 0, error: 'the Mac does not answer' };
    }
  }

  command(cmd) {
    this.lastCommand = cmd;
    this.dispatchEvent(new CustomEvent('command', { detail: cmd }));
    return this.post('./command', JSON.stringify({ cmd }));
  }
  ask(text, context) { return this.post('./ask', JSON.stringify({ text, context })); }
  answer(allow) { return this.post('./answer', JSON.stringify({ allow })); }
  stop() { return this.post('./stop', '{}'); } // the AI's answer ends; what is not yet said of it is not
  // the words of a spoken question; ask: whether the bridge passes them on to the AI itself (the page asks it when the
  // words are not a plain view command it does at once)
  voice(wav, lang, context, ask = true) {
    return this.post('./voice?lang=' + encodeURIComponent(lang) + '&ctx=' + encodeURIComponent(context || '') + (ask ? '' : '&ask=0'), wav, 'audio/wav');
  }
  pins(pins) { return this.post('./pins', JSON.stringify({ pins })); }
  health(report) { return this.post('./health', JSON.stringify(report)); } // how the page runs (app.js), for whoever looks after it
  // a picture of what the person sees (JPEG), for the AI's headset_look: moment 'now' answers its request id, 'question'
  // is taken as a question is asked; view: ar, vr or screen
  look(jpeg, id, moment, view) {
    return this.post('./look?id=' + encodeURIComponent(id) + '&moment=' + moment + '&view=' + view, jpeg, 'image/jpeg');
  }
  clearMarks() { return this.post('./marks/clear', '{}'); }
}
