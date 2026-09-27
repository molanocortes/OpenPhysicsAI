# `src/render/` — software renderer

Images without OpenGL, so a headless agent can *see* what it set up: geometry with highlighted selections, patch
labels and FEM fields with a legend. One file plus its header.

Up: [module map](../../docs/map/MODULES.md#srcrender--software-renderer) · [AGENTS.md](../../AGENTS.md).

## Read first

| File | What it gives you |
|---|---|
| `swrender.h` | the whole interface: `sw_image_init`, `sw_camera_look`, `sw_camera_preset` (named views with a known camera), the `sw_draw_*` family |
| `swrender.c` | triangle and line rasterisation with a depth buffer, 5×7 bitmap text, colour maps, legends |

## Talks to

- [`core/`](../core/README.md) — `png.h` encodes the result, with no image library.
- [`ctl/`](../ctl/README.md) — `viewrender.c` and `views.c` turn a project or a field into an image; the operations
  `view_render`, `selection_preview`, `results_render` and `interface_preview` return it.

## Tests

```bash
make headless
./build/rendertest build/rendertest_out
python3 tools/pngcheck.py build/rendertest_out   # independent decode of what the C encoder produced
```

## Why it exists in this form

An agent that cannot see the model cannot check its own set-up. The camera presets are **named and reproducible**, so
an image can be cited in a record: the same preset on the same geometry gives the same view. Pixel picking
(`view_pick`) maps a click back to a face, which is how the app and an agent can point at the same thing.
