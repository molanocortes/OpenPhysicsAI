# src/web: the lab in a web page

The physics lab compiled to WebAssembly, so that the website's lab page (`site/lab/`, <https://openphysicsai.org/lab/>)
runs the same solvers and the same renderer as `build/labrun` and `build/labfilm`, on the visitor's phone or
computer. Up: [docs/map/MODULES.md](../../docs/map/MODULES.md).

| File | What it is |
|---|---|
| `labweb.c` | the module's functions for the page: run the scenario at `/lab/scenario.json` (`lw_run`, `lw_record`), open the result with the first look the app gives it (`lw_open`, after `src/labapp.c`), change the look (`lw_view`: field, range, section, flux lines, zoom), describe it (`lw_state`, `lw_legend`), and draw a frame at any size and camera angle (`lw_render`) |
| `threads_serial.c` | `src/threads.h` on one thread, in place of `src/threads.c`: a page's module has no threads, and a solver computes on it what it computes on a one-thread pool natively |

The page's side is in `site/lab/`: `wasi.js` (the thirteen WASI calls the module makes, over files kept in memory),
`worker.js` (the module in a background thread), `lab.js` (the page), `catalogue.json` (the scenarios).

## Build and check

```bash
python3 tools/web_build.py --check    # from origin/main: build/web/site/lab/, every scenario checked
python3 tools/site_build.py           # the website, with the lab page, into build/site/
```

`tools/web_build.py` compiles from a `git archive` of the branch the website describes (`origin/main` by default;
`--ref .` builds the working tree to try a change), adding this folder. The flags are the native core's: C11, `-O2`,
IEEE arithmetic, no fast-math, no floating-point contraction. `--check` runs every scenario of the page twice, in
WebAssembly under Node through `site/lab/wasi.js` and natively from the same sources on one thread, and compares the
results; where the files are not identical it compares every field (`tools/labdiff.c`). On 2026-10-08 three of the
seven results were identical byte for byte (orbit, glass, hall); for the other four the solver's diagnostics were
identical and the fields agreed to 2e-7 of their largest value (motor), 8e-12 (black hole), 5e-14 (radar: 8 bytes of
2 MB differ) and exactly (battery, whose few differing bytes are outside the fields): differences in the last bits of
some numbers, most likely where the two C libraries round a function such as a sine differently. It also draws each
scenario's card picture with the module itself. `tools/site_build.py` refuses a module that was not built and checked
from the commit the site describes, with this folder and the catalogue as they are.

What the browser build leaves out: the GPU engines (Metal), so a scenario that asks for one is told to use the CPU
(`src/lab/flow/gpu_none.c`); the 3D magnetic backends, which need MFEM; and threads. It runs on one processor core
without a GPU, so the page's scenarios are the app's (`examples/lab/`) made smaller, and each says by how much.

## The toolchain

[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) 34.0 (clang 23 and wasi-libc), unpacked under
`build/third_party/wasi-sdk-34.0-*`; `WASI_SDK` names another copy. It is not part of the repository: download the
release archive for the build machine from the wasi-sdk releases page and unpack it there. The module links parts of
wasi-libc and compiler-rt; their notices ship beside it (`site/lab/NOTICES.txt`, see
[docs/THIRD_PARTY.md](../../docs/THIRD_PARTY.md)).

Measured on 2026-10-07 (Apple M2, processor time): the module is 0.74 MB; a scenario runs in WebAssembly at 0.6 to
0.9 of the speed of the same code compiled natively on one thread. 128-bit SIMD gained about 5 % with the same
results, and is left off so that browsers older than Safari 16.4 still run the lab.
