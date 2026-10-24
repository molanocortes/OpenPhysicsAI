// worker.js - the lab's module in a background thread of the page, so that a run of many seconds never freezes it.
// One message at a time, in order: load, run, view, render. The page (lab.js) keeps the queue short: it asks for the
// next picture only when the last one has come back. Runs cannot be interrupted from inside; the page stops one by
// ending this worker and starting another.

let lab = null; // the module's exports
let fs = null;
let wasiModule = null;

const text = (ptr) => {
  const b = new Uint8Array(lab.memory.buffer, ptr);
  return new TextDecoder().decode(b.subarray(0, b.indexOf(0)));
};
const put = (s) => {
  const bytes = new TextEncoder().encode(s + "\0");
  const ptr = lab.lw_buffer(bytes.length);
  new Uint8Array(lab.memory.buffer, ptr, bytes.length).set(bytes);
  return ptr;
};

async function load(wasmUrl, wasiUrl) {
  wasiModule = await import(wasiUrl);
  const module = await WebAssembly.compileStreaming(fetch(wasmUrl)).catch(async () =>
    // a server that does not say application/wasm: compile from the bytes instead
    WebAssembly.compile(await (await fetch(wasmUrl)).arrayBuffer()));
  const missing = wasiModule.missingImports(module);
  if (missing.length) throw new Error("the lab needs calls this page does not provide: " + missing.join(", "));
  fs = new wasiModule.MemFS();
  let line = "";
  const w = wasiModule.wasi(fs, {
    err: (t) => {
      // the solver's progress, a line at a time
      line += t;
      const lines = line.split("\n");
      line = lines.pop();
      for (const l of lines) if (l.trim()) postMessage({ type: "log", line: l.trim() });
    },
  });
  const instance = await WebAssembly.instantiate(module, { wasi_snapshot_preview1: w.imports });
  w.bind(instance);
  lab = instance.exports;
  lab._initialize();
}

function state() {
  return JSON.parse(text(lab.lw_state()));
}

function legend() {
  return new Uint8Array(lab.memory.buffer, lab.lw_legend(64), 64 * 3).slice();
}

onmessage = async (e) => {
  const m = e.data;
  try {
    if (m.type === "load") {
      await load(m.wasm, m.wasi);
      postMessage({ type: "loaded" });
    } else if (m.type === "run") {
      fs.remove("lab/result.lab");
      fs.write("lab/scenario.json", new TextEncoder().encode(m.scenario));
      const t0 = performance.now();
      const rc = lab.lw_run(0);
      const record = JSON.parse(text(lab.lw_record()));
      const seconds = (performance.now() - t0) / 1000;
      if (rc) {
        postMessage({ type: "failed", record, seconds });
        return;
      }
      const frames = lab.lw_open();
      if (frames < 1) {
        postMessage({ type: "failed", record: JSON.parse(text(lab.lw_record())), seconds });
        return;
      }
      if (m.view && Object.keys(m.view).length) lab.lw_view(put(JSON.stringify(m.view)));
      const size = fs.read("lab/result.lab").length;
      postMessage({ type: "ran", record, seconds, size, state: state(), legend: legend() });
    } else if (m.type === "view") {
      lab.lw_view(put(JSON.stringify(m.view)));
      postMessage({ type: "state", state: state(), legend: legend() });
    } else if (m.type === "render") {
      const t0 = performance.now();
      const ptr = lab.lw_render(m.frame, m.w, m.h, m.azim, m.elev, m.zoom || 1, m.fast ? 1 : 0);
      if (!ptr) {
        postMessage({ type: "frame", id: m.id, frame: m.frame, failed: true });
        return;
      }
      const pixels = new Uint8ClampedArray(lab.memory.buffer, ptr, m.w * m.h * 4).slice();
      postMessage({ type: "frame", id: m.id, frame: m.frame, w: m.w, h: m.h, fast: m.fast, ms: performance.now() - t0, pixels },
        [pixels.buffer]);
    }
  } catch (err) {
    postMessage({ type: "error", message: String((err && err.message) || err), during: m.type, id: m.id });
  }
};
