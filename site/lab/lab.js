// lab.js - the lab page: choose a scenario, change it, run it in a background worker (worker.js, the lab's own module
// lab.wasm), and look at the result: a film of its frames that plays, and turns and zooms under a finger or a mouse.
// Every picture is drawn by the lab's renderer in the worker; this file only asks for them and shows them. It keeps
// the pictures of the current look as a film, within a memory budget, and asks for a fast, smaller picture while the
// view is being turned. Nothing here talks to any server but this site's own, for the page's files.

const $ = (id) => document.getElementById(id);
const main = document.querySelector("main.lab");
const canvas = $("canvas"), ctx = canvas.getContext("2d");
const ui = {
  picker: $("picker"), veil: $("veil"), veilTitle: $("veil-title"), veilLine: $("veil-line"), bar: $("bar"), barFill: $("bar-fill"),
  runBig: $("run-big"), hint: $("hint"), transport: $("transport"), play: $("play"), scrub: $("scrub"), clock: $("clock"),
  legend: $("legend"), field: $("field"), ramp: $("ramp"), lo: $("lo"), hi: $("hi"), kind: $("sc-kind"), name: $("sc-name"),
  text: $("sc-text"), controls: $("controls"), run: $("run"), stop: $("stop"), estimate: $("estimate"), coarser: $("coarser"),
  record: $("record"), stage: $("stage"),
};

const S = {
  catalogue: [], entry: null, scenario: null, edited: false,
  worker: null, ready: false, running: false, busy: false, startedAt: 0, tick: 0,
  result: null, legend: null, picture: false,
  azim: -55, elev: 30, zoom: 1,
  frame: 0, playing: true, playList: [], lastStep: 0,
  film: new Map(), sharp: new Set(), look: 0, w: 0, h: 0, budget: 0,
  drag: null, pointers: new Map(), interactive: null, fastPending: false, renderId: 0, inFlight: null,
};

// ---- the worker ----------------------------------------------------------------------------------------------------

function startWorker() {
  if (S.worker) S.worker.terminate();
  S.ready = false;
  S.busy = false;
  S.inFlight = null;
  S.worker = new Worker(main.dataset.worker, { type: "module" });
  S.worker.onmessage = (e) => onWorker(e.data);
  S.worker.onerror = (e) => fail("The lab stopped: " + (e.message || "an error in the worker") + ".");
  S.worker.postMessage({ type: "load", wasm: new URL(main.dataset.wasm, location.href).href, wasi: new URL(main.dataset.wasi, location.href).href });
}

function onWorker(m) {
  if (m.type === "loaded") {
    S.ready = true;
    ui.run.disabled = !S.scenario;
    if (S.pendingRun) {
      S.pendingRun = false;
      run();
    }
  } else if (m.type === "log") {
    progress(m.line);
  } else if (m.type === "ran") {
    S.running = false;
    S.busy = false;
    clearInterval(S.tick);
    showResult(m);
  } else if (m.type === "failed") {
    S.running = false;
    S.busy = false;
    clearInterval(S.tick);
    fail((m.record && m.record.error) || "The run failed.");
  } else if (m.type === "state") {
    S.busy = false;
    setState(m.state, m.legend);
    newLook();
  } else if (m.type === "frame") {
    const req = S.inFlight;
    S.busy = false;
    S.inFlight = null;
    gotFrame(m, req);
  } else if (m.type === "error") {
    S.busy = false;
    S.running = false;
    clearInterval(S.tick);
    fail("The lab stopped: " + m.message);
  }
  pump();
}

// ---- the catalogue and a scenario ----------------------------------------------------------------------------------

async function loadCatalogue() {
  const url = main.dataset.catalogue;
  const cat = await (await fetch(url)).json();
  S.catalogue = cat.scenarios;
  const base = new URL(url, location.href);
  ui.picker.textContent = "";
  for (const e of S.catalogue) {
    e.url = new URL(e.file, base).href;
    const b = document.createElement("button");
    b.type = "button";
    b.className = "pick";
    b.setAttribute("role", "listitem");
    b.dataset.id = e.id;
    if (e.poster) {
      const img = document.createElement("img");
      img.src = new URL(e.poster, base).href;
      img.alt = "";
      img.width = 320;
      img.height = 180;
      b.append(img);
    }
    const t = document.createElement("span");
    t.className = "pick-text";
    t.innerHTML = `<span class="pick-kind"></span><b></b><span class="pick-time"></span>`;
    t.querySelector(".pick-kind").textContent = e.kind;
    t.querySelector("b").textContent = e.name;
    t.querySelector(".pick-time").textContent = e.seconds != null ? about(e.seconds) : "";
    b.append(t);
    b.addEventListener("click", () => choose(e));
    ui.picker.append(b);
  }
  const wanted = S.catalogue.find((e) => "#" + e.id === location.hash) || S.catalogue[0];
  if (wanted) choose(wanted, { autorun: wanted.seconds != null && wanted.seconds < 3 });
}

function about(seconds) {
  return seconds < 2 ? "about a second" : "about " + Math.round(seconds) + " s on a laptop";
}

async function choose(e, { autorun = false } = {}) {
  if (S.running) stop();
  S.entry = e;
  for (const b of ui.picker.children) b.setAttribute("aria-current", b.dataset.id === e.id ? "true" : "false");
  if (location.hash !== "#" + e.id) history.replaceState(null, "", "#" + e.id);
  ui.kind.textContent = e.kind;
  ui.name.textContent = e.name;
  ui.text.textContent = e.text;
  ui.coarser.hidden = !e.coarser;
  ui.coarser.textContent = e.coarser ? "Smaller than the app's: " + e.coarser : "";
  ui.estimate.textContent = e.seconds != null ? about(e.seconds) : "";
  ui.record.hidden = true;
  clearResult();
  veil(e.name, "", true);
  ui.run.disabled = true;
  try {
    S.scenario = await (await fetch(e.url)).json();
  } catch (err) {
    if (S.entry === e) fail("The scenario did not load: " + err.message);
    return;
  }
  if (S.entry !== e) return;
  S.edited = false;
  S.azim = e.camera ? e.camera[0] : -55;
  S.elev = e.camera ? e.camera[1] : 30;
  S.zoom = 1;
  buildControls();
  ui.run.disabled = !S.ready;
  if (autorun) run();
}

// The scenario's own controls (its "controls" list: a label, a path into the scenario, a range), as sliders.
function buildControls() {
  ui.controls.textContent = "";
  for (const c of S.scenario.controls || []) {
    const value = get(S.scenario, c.path);
    if (typeof value !== "number" || !(c.max > c.min)) continue;
    const log = c.log && c.min > 0;
    const toSlider = (v) => (log ? Math.log(v / c.min) / Math.log(c.max / c.min) : (v - c.min) / (c.max - c.min));
    const fromSlider = (t) => {
      const v = log ? c.min * Math.pow(c.max / c.min, t) : c.min + t * (c.max - c.min);
      return c.integer ? Math.round(v) : Number(v.toPrecision(3));
    };
    const row = document.createElement("label");
    row.className = "control";
    row.innerHTML = `<span class="control-name"></span><output></output><input type="range" min="0" max="1000" step="1">`;
    row.querySelector(".control-name").textContent = sentence(c.label);
    const out = row.querySelector("output"), input = row.querySelector("input");
    input.value = Math.round(1000 * toSlider(value));
    out.textContent = num(value);
    input.addEventListener("input", () => {
      const v = fromSlider(input.value / 1000);
      put(S.scenario, c.path, v);
      out.textContent = num(v);
      S.edited = true;
      if (S.result) ui.run.textContent = "Run again";
    });
    ui.controls.append(row);
  }
  ui.run.textContent = "Run";
}

const sentence = (s) => s.charAt(0).toUpperCase() + s.slice(1);
function get(o, path) {
  for (const k of path.split(".")) o = o == null ? undefined : o[Array.isArray(o) ? Number(k) : k];
  return o;
}
function put(o, path, v) {
  const keys = path.split(".");
  for (const k of keys.slice(0, -1)) o = o[Array.isArray(o) ? Number(k) : k];
  o[keys[keys.length - 1]] = v;
}

// ---- running ---------------------------------------------------------------------------------------------------------

function run() {
  if (!S.scenario) return;
  if (!S.ready) {
    S.pendingRun = true;
    return;
  }
  clearResult();
  S.running = true;
  S.busy = true;
  S.startedAt = performance.now();
  ui.run.disabled = true;
  ui.stop.hidden = false;
  ui.record.hidden = true;
  veil("Computing on this device", "Starting the solver", false);
  ui.bar.hidden = false;
  ui.barFill.style.width = "0%";
  clearInterval(S.tick);
  S.tick = setInterval(() => {
    ui.veilTitle.textContent = "Computing on this device · " + Math.round((performance.now() - S.startedAt) / 1000) + " s";
  }, 500);
  S.worker.postMessage({ type: "run", scenario: JSON.stringify(S.scenario), view: S.entry.view || {} });
}

// a line of the solver's progress: "frame 12/40" or "step 300 of 2000" is a fraction; failing that, "t 600 s" is one
// of the scenario's end time
function progress(line) {
  if (!S.running) return;
  ui.veilLine.textContent = line.length > 90 ? line.slice(0, 87) + "..." : line;
  let done = null;
  const m = line.match(/(\d+)\s*(?:\/|of)\s*(\d+)/);
  if (m && Number(m[2]) > 0) done = Number(m[1]) / Number(m[2]);
  else {
    const t = line.match(/\bt\s*=?\s*([0-9.eE+-]+)\s*s\b/), run = (S.scenario && S.scenario.run) || {};
    const end = run.end_s || run.end_time_s;
    if (t && end > 0) done = Number(t[1]) / end;
  }
  if (done !== null && isFinite(done)) ui.barFill.style.width = Math.max(0, Math.min(100, 100 * done)) + "%";
}

function stop() {
  clearInterval(S.tick);
  S.running = false;
  startWorker(); // a run cannot be interrupted from inside: a fresh worker replaces this one
  ui.stop.hidden = true;
  ui.run.disabled = false;
  veil(S.entry ? S.entry.name : "", "Stopped. Change something, or run it again.", true);
}

function fail(message) {
  ui.stop.hidden = true;
  ui.run.disabled = !S.ready;
  ui.bar.hidden = true;
  veil("This run did not finish", message, true);
}

function veil(title, line, button) {
  ui.veil.hidden = false;
  ui.veilTitle.textContent = title;
  ui.veilLine.textContent = line;
  ui.bar.hidden = true;
  ui.runBig.hidden = !button;
  ui.runBig.textContent = S.result ? "Run again" : "Run it here";
  ui.hint.hidden = true;
}

function clearResult() {
  S.result = null;
  for (const bmp of S.film.values()) if (bmp && bmp.close) bmp.close();
  S.film.clear();
  S.sharp.clear();
  S.look++;
  ui.transport.hidden = true;
  ui.legend.hidden = true;
  ui.hint.hidden = true;
  ui.stage.classList.remove("turnable");
  ctx.clearRect(0, 0, canvas.width, canvas.height);
}

function showResult(m) {
  ui.stop.hidden = true;
  ui.run.disabled = false;
  ui.run.textContent = "Run again";
  ui.veil.hidden = true;
  ui.bar.hidden = true;
  S.result = m.state;
  S.picture = ["r", "g", "b"].every((f) => f in (m.state.fields || {}));
  setState(m.state, m.legend);
  ui.stage.classList.toggle("turnable", !S.picture);
  ui.hint.hidden = S.picture;
  setTimeout(() => (ui.hint.hidden = true), 4000);
  ui.transport.hidden = m.state.frames < 2;
  ui.scrub.max = String(m.state.frames - 1);
  S.frame = 0;
  setPlaying(m.state.frames > 1);
  record(m);
  newLook();
}

// what the solver reports, under the picture: the run itself, then its own diagnostics
function record(m) {
  const rows = [["computed here in", m.seconds.toFixed(1) + " s"], ["frames", String(m.state.frames)],
    ["result", (m.size / 1e6).toFixed(1) + " MB, in this tab's memory"]];
  for (const [k, v] of flat(m.record.diagnostics || {}))
    if (rows.length < 12 && k !== "frames") rows.push([k.replace(/_/g, " "), v]);
  ui.record.textContent = "";
  for (const [k, v] of rows) {
    const dt = document.createElement("dt"), dd = document.createElement("dd");
    dt.textContent = k;
    dd.textContent = v;
    ui.record.append(dt, dd);
  }
  ui.record.hidden = false;
}

function* flat(o, prefix = "") {
  for (const [k, v] of Object.entries(o)) {
    const key = prefix ? prefix + " " + k : k;
    if (typeof v === "number") yield [key, num(v)];
    else if (typeof v === "string" && v.length < 40) yield [key, v];
    else if (Array.isArray(v) && v.length && v.every((x) => typeof x === "number"))
      yield [key, v.length + " values, " + num(Math.min(...v)) + " to " + num(Math.max(...v))];
    else if (v && typeof v === "object" && !Array.isArray(v)) yield* flat(v, key);
  }
}

function num(v) {
  if (!isFinite(v)) return String(v);
  if (v === 0) return "0";
  const a = Math.abs(v);
  if (a >= 1e6 || a < 1e-3) return v.toExponential(3).replace(/\.?0+e/, "e").replace("e+", "e");
  return String(Number(v.toPrecision(5)));
}

// ---- the look: field, legend, time ---------------------------------------------------------------------------------

function setState(state, legend) {
  S.result = state;
  ui.field.textContent = "";
  const names = Object.keys(state.fields || {}).filter((f) => !["solid", "material", "body", "r", "g", "b"].includes(f));
  for (const f of names) {
    const o = document.createElement("option");
    o.value = f;
    o.textContent = f + (state.fields[f] && state.fields[f] !== "1" ? " (" + state.fields[f] + ")" : "");
    ui.field.append(o);
  }
  ui.field.value = state.field;
  const colours = [];
  for (let i = 0; i < legend.length / 3; i++) colours.push(`rgb(${legend[3 * i]},${legend[3 * i + 1]},${legend[3 * i + 2]})`);
  ui.ramp.style.background = `linear-gradient(90deg, ${colours.join(",")})`;
  const unit = state.unit && state.unit !== "1" ? " " + state.unit : "";
  ui.lo.textContent = num(state.lo) + unit;
  ui.hi.textContent = num(state.hi) + unit;
  ui.legend.hidden = !names.length || S.picture || !(state.hi > state.lo) || (S.entry && S.entry.legend === false);
}

function clockText(i) {
  // the frames' own unit when they are not times: the catalogue's (a motor's frames are rotor angles), or the header's
  const t = S.result.times[i] || 0, unit = (S.entry && S.entry.time_unit) || S.result.time_unit;
  if (unit === "°") return num(t) + "°";
  if (unit) return num(t) + " " + unit;
  const a = Math.abs(t);
  if (a === 0) return "0 s";
  if (a < 1e-6) return num(t * 1e9) + " ns";
  if (a < 1e-3) return num(t * 1e6) + " µs";
  if (a < 1) return num(t * 1e3) + " ms";
  if (a < 120) return num(t) + " s";
  if (a < 7200) return num(t / 60) + " min";
  if (a < 172800) return num(t / 3600) + " h";
  return num(t / 86400) + " days";
}

// ---- pictures: a film for the current look, and fast ones while turning ---------------------------------------------

function size() {
  const r = ui.stage.getBoundingClientRect();
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = Math.max(320, Math.min(1280, Math.round(r.width * dpr)));
  const h = Math.round((w * r.height) / Math.max(1, r.width));
  return [w - (w % 2), h - (h % 2)];
}

// A new look (field, size or camera): the film starts again, from the frame on screen.
function newLook() {
  if (!S.result) return;
  const [w, h] = size();
  S.w = w;
  S.h = h;
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w; // (setting the size clears the canvas, so only when it changes)
    canvas.height = h;
  }
  S.look++;
  for (const bmp of S.film.values()) if (bmp && bmp.close) bmp.close();
  S.film.clear();
  S.sharp.clear();
  // as many frames as the budget holds, evenly spread; the rest are drawn when the scrubber stops on them
  const n = S.result.frames;
  S.budget = Math.max(4, Math.floor((matchMedia("(max-width: 700px)").matches ? 90e6 : 180e6) / (w * h * 4)));
  const k = Math.max(1, Math.ceil(n / S.budget));
  S.playList = [];
  for (let i = 0; i < n; i += k) S.playList.push(i);
  if (S.playList[S.playList.length - 1] !== n - 1 && S.playList.length < S.budget) S.playList.push(n - 1);
  draw();
  pump();
}

// The next picture the worker should draw: a quick one while the view turns; then the film's missing frames, from
// the one shown onwards, drawn without anti-aliasing so that they come fast enough to play; then, when the film is
// paused, the frame on screen again with anti-aliasing.
function pump() {
  if (!S.worker || !S.ready || S.busy || S.running || !S.result) return;
  if (S.pendingView) {
    S.busy = true;
    S.worker.postMessage({ type: "view", view: S.pendingView });
    S.pendingView = null;
    return;
  }
  let req = null;
  if (S.interactive) {
    req = S.interactive;
    S.interactive = null;
  } else if (!S.drag) {
    let next = S.film.has(S.frame) ? null : S.frame;
    if (next === null) {
      const start = Math.max(0, S.playList.findIndex((i) => i >= S.frame));
      for (let j = 0; j < S.playList.length && next === null; j++) {
        const i = S.playList[(start + j) % S.playList.length];
        if (!S.film.has(i)) next = i;
      }
    }
    if (next !== null) req = { kind: "film", frame: next, w: S.w, h: S.h, fast: true };
    else if (!S.playing && !S.sharp.has(S.frame)) req = { kind: "sharp", frame: S.frame, w: S.w, h: S.h, fast: false };
  }
  if (!req) return;
  S.busy = true;
  req.id = ++S.renderId;
  req.look = S.look;
  req.azim = S.azim;
  req.elev = S.elev;
  req.zoom = S.zoom;
  S.inFlight = req;
  S.worker.postMessage({ type: "render", ...req });
}

async function gotFrame(m, req) {
  if (!S.result || !req || req.look !== S.look) return; // a picture of a look that has since changed
  if (m.failed) {
    if (req.kind !== "turn") S.film.set(req.frame, null); // drawn as nothing, and not asked for again
    return;
  }
  const bmp = await createImageBitmap(new ImageData(m.pixels, m.w, m.h));
  if (req.look !== S.look) return bmp.close && bmp.close();
  if (req.kind === "turn") {
    // a quick picture while turning: drawn at once, scaled up to the canvas, never kept
    ctx.drawImage(bmp, 0, 0, canvas.width, canvas.height);
    bmp.close && bmp.close();
    return;
  }
  if (m.w !== S.w || m.h !== S.h) return bmp.close && bmp.close();
  if (req.kind === "sharp") S.sharp.add(req.frame);
  const old = S.film.get(req.frame);
  if (old && old.close) old.close();
  if (S.film.size >= S.budget && !S.playList.includes(req.frame)) {
    const drop = [...S.film.keys()].find((i) => !S.playList.includes(i) && i !== S.frame);
    if (drop !== undefined) {
      const b = S.film.get(drop);
      if (b && b.close) b.close();
      S.film.delete(drop);
      S.sharp.delete(drop);
    }
  }
  S.film.set(req.frame, bmp);
  if (req.frame === S.frame && !S.drag) draw();
}

function draw() {
  const bmp = S.film.get(S.frame);
  if (bmp) ctx.drawImage(bmp, 0, 0, canvas.width, canvas.height); // (null: a frame that could not be drawn)
  if (S.result) {
    ui.scrub.value = String(S.frame);
    ui.clock.textContent = clockText(S.frame);
  }
}

// playback: the film's frames at a pace that shows the whole run in about six seconds
function loop(t) {
  requestAnimationFrame(loop);
  if (!S.result || !S.playing || S.drag || S.playList.length < 2) return;
  const fps = Math.max(6, Math.min(20, S.playList.length / 6));
  if (t - S.lastStep < 1000 / fps) return;
  const at = S.playList.indexOf(S.frame);
  const next = S.playList[(at + 1) % S.playList.length];
  if (!S.film.has(next)) return; // wait for the worker to draw it
  S.lastStep = t;
  S.frame = next;
  draw();
}

function setPlaying(on) {
  S.playing = on;
  ui.play.classList.toggle("paused", !on);
  ui.play.setAttribute("aria-label", on ? "Pause" : "Play");
  pump(); // paused: the frame on screen is drawn again, sharp
}

// ---- turning and zooming --------------------------------------------------------------------------------------------

function turned() {
  // a fast picture now, at half size; the film of the new view follows when the hand lets go
  const w = Math.max(160, Math.round(S.w / 2)), h = Math.round((w * S.h) / S.w);
  S.interactive = { kind: "turn", frame: S.frame, w: w - (w % 2), h: h - (h % 2), fast: true };
  pump();
}

// the hand lets go: a new view gets its film; a tap that moved nothing plays or pauses
function settle() {
  const d = S.drag;
  S.drag = null;
  if (d && !d.wheel && d.azim === S.azim && d.elev === S.elev && d.zoom === S.zoom) {
    setPlaying(!S.playing);
    return;
  }
  newLook();
}

ui.stage.addEventListener("pointerdown", (e) => {
  if (!S.result) return; // (a picture, the black hole's, cannot be turned, but a tap still plays or pauses it)
  ui.stage.setPointerCapture(e.pointerId);
  S.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  S.drag = { azim: S.azim, elev: S.elev, zoom: S.zoom, x: e.clientX, y: e.clientY, spread: spread() };
  ui.hint.hidden = true;
});
ui.stage.addEventListener("pointermove", (e) => {
  if (!S.drag || !S.pointers.has(e.pointerId) || S.picture) return;
  S.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (S.pointers.size < 2 && Math.hypot(e.clientX - S.drag.x, e.clientY - S.drag.y) < 4) return; // not yet a drag
  if (S.pointers.size >= 2) {
    const s = spread();
    if (S.drag.spread > 0 && s > 0) S.zoom = clampZoom((S.drag.zoom * s) / S.drag.spread);
  } else {
    S.azim = S.drag.azim - (e.clientX - S.drag.x) * 0.4;
    S.elev = Math.max(-5, Math.min(89, S.drag.elev + (e.clientY - S.drag.y) * 0.3));
  }
  turned();
});
const release = (e) => {
  if (!S.pointers.delete(e.pointerId) || !S.drag) return;
  if (S.pointers.size === 0) settle();
  else S.drag = { azim: S.azim, elev: S.elev, zoom: S.zoom, x: [...S.pointers.values()][0].x, y: [...S.pointers.values()][0].y, spread: spread() };
};
ui.stage.addEventListener("pointerup", release);
ui.stage.addEventListener("pointercancel", release);
function spread() {
  const p = [...S.pointers.values()];
  return p.length >= 2 ? Math.hypot(p[0].x - p[1].x, p[0].y - p[1].y) : 0;
}
const clampZoom = (z) => Math.max(0.4, Math.min(8, z));

let wheelTimer = 0;
ui.stage.addEventListener("wheel", (e) => {
  if (!S.result || S.picture) return;
  e.preventDefault();
  S.zoom = clampZoom(S.zoom * Math.exp(-e.deltaY * 0.0015));
  S.drag = S.drag || { wheel: true };
  turned();
  clearTimeout(wheelTimer);
  wheelTimer = setTimeout(() => S.drag && S.drag.wheel && settle(), 250);
}, { passive: false });

ui.stage.addEventListener("keydown", (e) => {
  if (!S.result) return;
  const step = { ArrowLeft: [8, 0], ArrowRight: [-8, 0], ArrowUp: [0, 6], ArrowDown: [0, -6] }[e.key];
  if (step && !S.picture) {
    e.preventDefault();
    S.azim += step[0];
    S.elev = Math.max(-5, Math.min(89, S.elev + step[1]));
    newLook();
  } else if (e.key === " ") {
    e.preventDefault();
    setPlaying(!S.playing);
  }
});

// ---- controls -------------------------------------------------------------------------------------------------------

ui.run.addEventListener("click", run);
ui.runBig.addEventListener("click", run);
ui.stop.addEventListener("click", stop);
ui.play.addEventListener("click", () => setPlaying(!S.playing));
ui.scrub.addEventListener("input", () => {
  setPlaying(false);
  S.frame = Number(ui.scrub.value);
  draw();
  pump();
});
ui.field.addEventListener("change", () => {
  if (!S.result) return;
  S.pendingView = { field: ui.field.value };
  pump();
});

let resizeTimer = 0;
new ResizeObserver(() => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    if (!S.result) return;
    const [w, h] = size();
    if (Math.abs(w - S.w) > 8 || Math.abs(h - S.h) > 8) newLook();
  }, 200);
}).observe(ui.stage);

// ---- start ----------------------------------------------------------------------------------------------------------

if (!("Worker" in window) || typeof WebAssembly !== "object" || !("createImageBitmap" in window)) {
  $("unsupported").hidden = false;
  veil("This browser cannot run the lab", "", false);
} else {
  startWorker();
  loadCatalogue().catch((e) => fail("The list of scenarios did not load: " + e.message));
  requestAnimationFrame(loop);
}
