# `src/core/` — dependency-free foundations

JSON, JSON Schema, units, hashing, PNG, paths and error codes. No third-party code, and nothing here knows about
physics, projects or transports. Everything else in the project is built on this directory.

Up: [module map](../../docs/map/MODULES.md#srccore--dependency-free-foundations) · [AGENTS.md](../../AGENTS.md).

## Read first

| File | What it gives you |
|---|---|
| `json.h` | the strict RFC 8259 parser, DOM and writer used for every request, project file and result header |
| `jschema.h` | the JSON Schema (draft 2020-12 subset) validator that checks every operation's input **and applies its defaults** |
| `units.h` | unit expressions → SI with dimension checking: why the project can refuse a number without a unit |
| `paths.h` | `path_resolve_new`, `path_within`, atomic writes: how access roots are enforced |
| `sha256.h` | the hashes in every run specification and evidence record |

Also here: `png.h`/`png.c` (a complete PNG encoder including deflate, so headless images need no library),
`base64.h` (images in control responses), `sbuf.h` (growable buffer), `errors.h` (the stable error codes every
transport reports).

## Talks to

Nothing inside the project — this is the bottom layer. It is used by [`ctl/`](../ctl/README.md),
[`net/`](../net/README.md), [`fem/`](../fem/README.md), [`geom/`](../geom/README.md) and
[`render/`](../render/README.md).

## Tests

```bash
make headless && ./build/coretest          # JSON, schema, units, SHA-256, base64, paths
python3 tools/pngcheck.py build/rendertest_out   # decodes png.c output independently
```

## Conventions that matter

- Numbers are IEEE doubles; the core is compiled **without** fast-math so NaN and summation order are honoured.
- `json_dump` never emits raw newlines unless asked, so compact output is safe for newline-delimited framing.
- A path is only usable after `path_resolve_new` + `path_within` against the engine's roots.
