# OpenPhysicsAI for everyone: the design

One page, written before the build. The owner reviews the result, not this page; this page is what the result is
held to.

## Three modes, one switch

A control in the top bar, `Simple | Advanced | Agent`, always visible, remembered between launches.

| Mode | Who it is for | What it shows |
|---|---|---|
| **Simple** (a new user starts here) | someone with a part and a printer, not an engineer | five screens, one big action each, nothing a decision does not need |
| **Advanced** | the engineer | the six-step strip as it is today, every control, the terminal, MORE on every step |
| **Agent** | someone who would rather ask | one text field and STOP; the window shows what the agent does and what it finds |

Nothing is removed from Advanced. Simple and Agent run the same operations; switching mode never loses work.

## The rules every screen is held to

1. One primary action per screen, in plain words, drawn larger than anything else. The next step is always obvious.
2. Simple shows only what a decision needs. Advanced shows everything.
3. Every number has a unit and a one-line meaning. Every warning says what to do next. Every error is a sentence
   with a fix, never a code.
4. A default shows where it came from (hover). Where the software would have to guess, it asks, in words a
   non-engineer understands.
5. The first run needs no manual and no terminal.
6. A sample part reaches a first result in under two minutes.
7. The result screen says what the simulation is and is not, in plain words, before the numbers.

## Words (Simple mode)

Written once, used everywhere. The right-hand column never appears in Simple mode; it stays in Advanced.

| Say | Meaning, in the glossary under "?" | Never in Simple |
|---|---|---|
| part | The object you want to print, from an STL file. | body, geometry, surface |
| print settings | The machine, the material and how it is printed, as one named preset. Each value says where it came from. | process, inherent strain, strain set, eigenstrain, tensor |
| the plate | The metal plate the part is printed on, and cut off from afterwards. | build plate, z = 0 |
| hold | Where the part is fixed while it is printed: on the plate. | support, boundary condition, Dirichlet |
| warp | How far the part moves out of its drawn shape after printing and cutting off, in mm. | displacement, u_z, deflection |
| residual stress | The stress locked into the part by printing, in MPa; compared with the stress at which the material yields, when that is known. | von Mises, stress tensor |
| layer | How thick each slice of the simulation is, in mm. | simulation layer, lumped layer |
| cut | Separating the part from the plate with a wire. | kerf, from_x, element removal |
| detail | How finely the part is divided for the calculation. | mesh, element, voxel, hex8 |
| check | Running it again with more detail to see whether the answer holds. | convergence, CHECK MESH |
| source | Where a number comes from. | provenance |

## The Simple screens

The panel on the right holds one screen at a time. A row of five dots at the top says where you are; the big button
at the bottom is the only way forward, and a small "back" goes back.

```
1  Choose a part                    2  How will it be printed?         3  Simulate
+-------------------------------+   +-------------------------------+  +-------------------------------+
| o o o o o                     |   | o o o o o                     |  | o o o o o                     |
| Choose a part                 |   | How will it be printed?       |  | Simulate                      |
| Drop an STL file here,        |   | [ Metal powder ] [ Plastic ]  |  | Your part, PLA, 18 layers     |
| or start with a sample:       |   | Print settings                |  | About 20 seconds.             |
| [ Cantilever ] [ Bracket ]    |   | [ PLA, desktop printer    v ] |  |                               |
|                               |   |  typical settings, sourced    |  | [#######-----]  layer 6 of 18 |
|                               |   | Which face sits on the plate? |  |                               |
| Is this part 70 mm or 70 cm   |   |  click it on the part         |  |                               |
| long?  [ 70 mm ] [ 70 cm ]    |   |  suggested: the largest flat  |  |                               |
| Your file is closed and can   |   |  face, underneath             |  |                               |
| be simulated.                 |   |                               |  |                               |
|        [   Next   ]           |   |        [   Next   ]           |  |      [  Simulate  ]           |
+-------------------------------+   +-------------------------------+  +-------------------------------+

4  Results                                                         5  Report
+-------------------------------+                                  +-------------------------------+
| o o o o o                     |                                  | o o o o o                     |
| What this is: a simulation of |                                  | Report                        |
| how printing bends the part,  |                                  | The report holds what you     |
| calibrated on test bars, not  |                                  | have just read, the pictures, |
| checked on this part.         |                                  | and where every number came   |
| Warp: up to 3.6 mm, upwards,  |                                  | from.                         |
|   at the free end (marked).   |                                  |                               |
| Residual stress: up to 180 MPa|                                  |                               |
|   near the base (yields at    |                                  |                               |
|   230 MPa).                   |                                  |                               |
| Risk: none found.             |                                  |                               |
| [Play] [True size] section -- |                                  |                               |
| More...                       |                                  |                               |
|        [  Write report  ]     |                                  |     [  Open the report  ]     |
+-------------------------------+                                  +-------------------------------+
```

Rendered screenshots of the built screens: [1b](simple-1b-part-chosen.png), [3](simple-3-simulate.png), [3b](simple-3b-running.png), [4](simple-4-results.png), [5](simple-5-report.png); Agent mode: [agent-2-result.png](agent-2-result.png). The cold start from the bundle and screens 1 and 2 were
removed on 2026-10-21, to be recaptured.

## Agent mode

The panel holds one text field, "What do you want to know about this part?", a STOP button and the transcript. The
view is the agent's view: every job the agent starts is followed automatically (said once, when the mode is chosen),
each operation it calls is one line in plain words, and results are drawn as they arrive. The agent is a command the
user already has (`claude -p`, `codex exec`, or any command), chosen in a settings screen. The app looks for what is
installed and never assumes. It writes an MCP configuration pointing that command at this window's own engine,
passes the user's words as the prompt, and holds no API key. STOP ends the child process. With no tool installed,
the panel says so and how to get one.

## Almost no buttons: the focus interface (owner, 2026-10-04; made smaller on 2026-10-07, next section)

"I as a human feel overwhelmed with so many buttons." On screen there is the scene and, at most:

- the name (back to the gallery), **Ask AI | Manual**, and **...** at the top;
- **Ask AI** (the default): one line to type in, the agent's latest words above it, a play bar when a result is open;
  beside the line one chip naming the model and effort the next question goes with ("Opus 4.8 · High"), which opens
  a menu with the tool's own models and effort levels in its own words (owner: the user must know which AI runs and
  choose its model and effort): Claude Code's model menu (Default, Opus, Sonnet, Haiku and the extra models its
  `~/.claude.json` lists for the account) with `--effort` low to max, or Codex's `~/.codex/models_cache.json` with
  each model's reasoning levels and `config.toml`'s choice as the default. A version ("4.8") is shown only once the
  tool has reported it, in this window or in a recorded run, never guessed. The conversation sidebar is hidden until
  the user opens it (top bar, or a click on the latest reply); it scrolls with the wheel or its bar, and a down arrow
  returns to the newest words. Simple changes to a simulation must never hit a permission wall (owner, 2026-10-04):
  the lab's own tools (`lab_run` and the rest, docs/mcp-clients.md) need no permission, and the same menu picks the
  tool's own permission mode (Claude Code: Ask first, Accept edits, Auto, Bypass; Codex: Read only, Auto, Full
  access). What Claude Code would ask appears as one card with **Allow** and **Deny** above the box;
- **Manual**: one primary action at the bottom (Start, Play or Run) and a card with two to four physical inputs of
  that simulation (never output settings such as saved frames or cells);
- the legend, bottom left; clicking it shows the next field.

Everything else (layers, sections, meshes, solver numbers, forces, the terminal, the step-by-step solid workflow's
extras) is behind **...**, which opens the full controls: the panels as they were. Headless runs and scripts start with
the full controls (`hud controls`; `hud simple` is the focus view), so tests and agents find every control where it
always was. A new control goes behind **...** unless the owner asks for it on the main screen.

The gallery shows pictures, a search, **Saved**, and one **Start from your own part** that opens its five choices.
A gallery simulation with a stored result (in `~/OpenPhysicsAI-Projects/lab`, kept by `tools/showcase/gallery_thumbs.py`)
opens at once; one without opens its setup.

## Two buttons and one bar: the minimal window (owner, 2026-10-07)

"Minimalize the UI, it should be mostly agentic and the manual mode should have just a pair of buttons, remove all the
text and buttons." Chosen with the owner the same day (GOALS.md G18), and it replaces the focus interface above where
they differ:

- at the top: the name (back to the gallery) and **Agentic | Manual**; no scene title, no **...**, no sidebar toggle;
- **Manual**: exactly **Open** (the gallery) and **Run**, bottom right. Run does what fits what is open: a lab
  simulation runs, the flow starts and pauses, the open part is solved. A refusal shows above the two for a few
  seconds, then fades;
- **Agentic**: one bar at the bottom, the text field and one round button: the microphone, or stop while the agent
  works. A press on the microphone listens with the Mac's own speech recognition (`build/openphysics-mic`, on the device
  when the language allows; `make voice`), a second press sends the words. The agent's words for the latest question
  show above the bar and fade after 8 s. The placeholder names the AI that answers ("Ask Claude Code by text or
  voice"); its model, effort and permission are set with `agent model|effort|permission <value>`. When the AI asks to
  use a tool, its card with **Allow** and **Deny** appears above the bar. With a part open, a click on it points at a
  face for the AI (the next question names it), and before a run the AI shows the setup it made of the person's words:
  held faces violet, loaded faces gold-brown, its words on a card with **Run it** and **Not right** (GOALS.md G17 step
  6, `python3 tools/plain_setup.py`);
- the legend, bottom left, with its title, the colour bar and its two end values; a clipped bar still says "99th pct";
- the gallery: pictures with one line each, **Your part** (its five choices) and **Saved**; no headline, subtitles,
  search or featured picture. In Agentic the same bar sits under it.

Everything else stays behind the hidden `hud controls` command, for tests and developers. Checked by
`python3 tools/minimal_window.py` (real clicks, a stand-in recogniser and a stand-in agent).

## Colour and type: the studio look (owner, 2026-10-04)

The owner chose this look from references: a light, calm interface with large light type (like a modern product
page), 3D scenes that look like product renders in a photo studio, and one colour map for every physics. It replaces
the dark navy and cyan look of the first versions, which no one chose on purpose; agents kept copying it because it
was in the code. Use these values, never raw colours of your own.

**Interface** (`src/ui.h`):

| Token | Value | Use |
|---|---|---|
| `UI_PAGE` | `#F2F1EE` | the page behind everything (warm off-white) |
| `UI_CARD`, `UI_BG` | `#FFFFFF` | cards and panels, with a soft shadow (`ui_shadow`) instead of a border where they float |
| `UI_TEXT`, `UI_INK` | `#111111` | text; primary buttons and chosen pills are black with white text |
| `UI_DIM` | `#7A7A80` | secondary text |
| `UI_HAIRLINE` | `#E3E1DC` | separators; pills have a `#DDDAD4` hairline |
| `UI_ACCENT` | `#E8551C` | the one accent: focus, hover rings, a slider being dragged, a small mark on the primary button |
| `UI_ACCENT2`, `UI_GOOD`, `UI_BAD` | `#B0700A`, `#1E8E4E`, `#D23B3B` | read this first, it worked, it did not |

**Type** (`src/ui.c`): SF Pro, read from `/System/Library/Fonts/SFNS.ttf` at a weight on its axis, for words; SF
Mono for numbers, values and the terminal; `FONT_DISPLAY` (30 pt, light) and `FONT_HERO` (52 pt, light) for headings
on the home screen. Labels written in capitals in the code are shown in sentence case (`ui_sentence_case`).
Section headings are black, not coloured.

**3D scenes** (`src/render.c`, `src/labgpu.c`, `src/render/swrender.c`): a seamless light room (backdrop and floor
the same colour, a faint grid, a soft floor shadow, ambient occlusion), matte white materials under a big soft key
light, a sky dome and a floor bounce, no bloom, graphite edges. Fields use one house map, **studio**
(`CMAP_STUDIO`, inferno from 0.14 to 0.90: deep violet to gold); signed fields go blue through the room's neutral to
orange. A body without a field stays white.

**What the home screen shows**: a scenario appears when it has a picture in `docs/media/thumbs`, made in this look
by `tools/showcase/gallery_thumbs.py`. A simulation that does not look good is left out by leaving out its picture;
it stays reachable by name, by the terminal and by agents.

## What is not in this design, and why

- A drag-and-drop file target drawn in the panel: the window already accepts a dropped file anywhere, so the panel
  says so instead of drawing a second target.
- Supports: the build has no supports yet, and the result screen says so rather than hiding it.
