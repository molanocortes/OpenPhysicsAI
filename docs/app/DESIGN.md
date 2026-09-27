# NAVIER-AM for everyone: the design

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
| Drop an STL file here,        |   | [ Metal powder ] [ Plastic ]  |  | Your part, AlSi10Mg, 18 layers|
| or start with a sample:       |   | Print settings                |  | About 20 seconds.             |
| [ Cantilever ] [ Bracket ]    |   | [ AlSi10Mg on an SLM 280  v ] |  |                               |
|                               |   |  fitted on test cantilevers   |  | [#######-----]  layer 6 of 18 |
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

Rendered screenshots of the built screens: [1](simple-1-part.png), [1b](simple-1b-part-chosen.png), [2](simple-2-print.png), [3](simple-3-simulate.png), [3b](simple-3b-running.png), [4](simple-4-results.png), [5](simple-5-report.png); Agent mode: [agent-2-result.png](agent-2-result.png); cold start from the bundle: [first-run/](first-run/).

## Agent mode

The panel holds one text field, "What do you want to know about this part?", a STOP button and the transcript. The
view is the agent's view: every job the agent starts is followed automatically (said once, when the mode is chosen),
each operation it calls is one line in plain words, and results are drawn as they arrive. The agent is a command the
user already has (`claude -p`, `codex exec`, or any command), chosen in a settings screen. The app looks for what is
installed and never assumes. It writes an MCP configuration pointing that command at this window's own engine,
passes the user's words as the prompt, and holds no API key. STOP ends the child process. With no tool installed,
the panel says so and how to get one.

## Colour and type

- Results default to **viridis**: perceptually uniform and readable with the common colour-vision deficiencies.
  Turbo stays one click away on the legend.
- Simple mode draws its text one size up from Advanced, with the primary button at full contrast
  (near-white on the accent) and secondary text never below 4.5:1 against the panel.
- One accent colour means "do this"; amber means "read this before you trust the number"; red means "this did
  not work, and here is the fix".

## What is not in this design, and why

- A drag-and-drop file target drawn in the panel: the window already accepts a dropped file anywhere, so the panel
  says so instead of drawing a second target.
- Supports: the build has no supports yet, and the result screen says so rather than hiding it.
