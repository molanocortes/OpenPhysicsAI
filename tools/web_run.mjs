// web_run.mjs - a lab module run under Node through the page's own WASI layer (site/lab/wasi.js), so that what is
// checked here is what a browser runs. Used by tools/web_build.py --check.
//
//   node tools/web_run.mjs build/web/labrun.wasm SCENARIO.json OUT.lab
//        labrun's own command line: the result file, and the JSON record on stdout
//   node tools/web_run.mjs build/web/lab.wasm SCENARIO.json OUTDIR [--view JSON] [--size WxH] [--azim A] [--elev E]
//        [--frames N] [--at F] [--result RESULT.lab]
//        the page's calls: lw_run, lw_open, lw_view, lw_state, lw_render; writes N frames as PNG to OUTDIR (one frame:
//        the one at the fraction F of the run) and prints the record, the state and the time each picture took.
//        --result opens a result already computed instead of running the scenario.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { MemFS, ProcExit, wasi, missingImports } from "../site/lab/wasi.js";

const argv = process.argv.slice(2);
const [wasmPath, scenario, out] = argv;
const opt = (name, def) => {
  const i = argv.indexOf(name);
  return i > 0 && i + 1 < argv.length ? argv[i + 1] : def;
};
const module = new WebAssembly.Module(readFileSync(wasmPath));
const missing = missingImports(module);
if (missing.length) {
  console.error("web_run: the module needs WASI calls site/lab/wasi.js does not provide: " + missing.join(" "));
  process.exit(2);
}
const fs = new MemFS();
const command = WebAssembly.Module.exports(module).some((e) => e.name === "_start");
if (scenario !== "-") fs.write(command ? "in/scenario.json" : "lab/scenario.json", readFileSync(scenario));
const w = wasi(fs, {
  args: ["labrun", "/in/scenario.json", "/out/result.lab"],
  out: (t) => process.stdout.write(t),
  err: (t) => process.stderr.write(t),
});
const instance = new WebAssembly.Instance(module, { wasi_snapshot_preview1: w.imports });
w.bind(instance);
const x = instance.exports;

if (command) {
  let code = 0;
  try {
    x._start();
  } catch (e) {
    if (!(e instanceof ProcExit)) throw e;
    code = e.code;
  }
  const result = fs.read("out/result.lab");
  if (result) writeFileSync(out, result);
  process.exit(code);
}

// the page's path
x._initialize();
const text = (ptr) => {
  const b = new Uint8Array(x.memory.buffer, ptr);
  return new TextDecoder().decode(b.subarray(0, b.indexOf(0)));
};
const put = (s) => {
  const bytes = new TextEncoder().encode(s + "\0");
  const ptr = x.lw_buffer(bytes.length);
  new Uint8Array(x.memory.buffer, ptr, bytes.length).set(bytes);
  return ptr;
};
let t = performance.now();
const given = opt("--result", "");
if (given) fs.write("lab/result.lab", readFileSync(given));
else {
  const rc = x.lw_run(0);
  console.log("record " + text(x.lw_record()) + " (" + ((performance.now() - t) / 1000).toFixed(1) + " s)");
  if (rc) process.exit(1);
}
const n = x.lw_open();
if (n < 1) process.exit(1);
const view = opt("--view", "");
if (view && x.lw_view(put(view))) console.error("web_run: lw_view refused " + view);
console.log("state " + text(x.lw_state()));
const [W, H] = opt("--size", "960x540").split("x").map(Number);
const azim = Number(opt("--azim", "-55")), elev = Number(opt("--elev", "30"));
const count = Math.min(n, Number(opt("--frames", "4")));
mkdirSync(out, { recursive: true });
for (let k = 0; k < count; k++) {
  const i = count > 1 ? Math.round((k * (n - 1)) / (count - 1)) : Math.round(Number(opt("--at", "1")) * (n - 1));
  for (const fast of [1, 0]) {
    t = performance.now();
    const ptr = x.lw_render(i, W, H, azim, elev, 1, fast);
    const ms = performance.now() - t;
    if (!ptr) {
      console.error("web_run: frame " + i + " did not render");
      process.exit(1);
    }
    console.log("frame " + i + (fast ? " fast" : " full") + " " + ms.toFixed(0) + " ms");
    if (!fast) writeFileSync(`${out}/frame_${String(k).padStart(2, "0")}.png`, png(new Uint8Array(x.memory.buffer, ptr, W * H * 4), W, H));
  }
}
const legend = new Uint8Array(x.memory.buffer, x.lw_legend(256), 768).slice();
const strip = new Uint8Array(256 * 16 * 4);
for (let y = 0; y < 16; y++) for (let i = 0; i < 256; i++) strip.set([...legend.subarray(3 * i, 3 * i + 3), 255], 4 * (y * 256 + i));
writeFileSync(`${out}/legend.png`, png(strip, 256, 16));

// an RGBA picture as a PNG file
function png(rgba, w, h) {
  const raw = new Uint8Array((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) raw.set(rgba.subarray(y * w * 4, (y + 1) * w * 4), y * (w * 4 + 1) + 1);
  const chunk = (type, data) => {
    const b = Buffer.alloc(12 + data.length);
    b.writeUInt32BE(data.length, 0);
    b.write(type, 4, "latin1");
    Buffer.from(data).copy(b, 8);
    b.writeUInt32BE(crc(b.subarray(4, 8 + data.length)), 8 + data.length);
    return b;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr.set([8, 6, 0, 0, 0], 8);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", new Uint8Array(0))]);
}
function crc(bytes) {
  let c = ~0;
  for (const b of bytes) {
    c ^= b;
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return ~c >>> 0;
}
