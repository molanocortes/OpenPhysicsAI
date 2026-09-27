# OpenPhysicsAI — vision (stated by the owner, 2026-09-17)

Open-source, AI-native engineering simulation for engineers and agents.

Name: **OpenPhysicsAI**, chosen by the owner on 2026-09-17 (it replaces the provisional FreePhysLab of the same day).
Repository: `molanocortes/OpenPhysicsAI`, private. Known on GitHub: a dormant user account named `OpenPhysicsAI` exists
(created 2021, one repository with 0 stars, a Reddit project about an AI that derives physical laws), so an organisation
of exactly that name cannot be registered; the repository name under the owner's account is unaffected. Checked: GitHub
only; no trademark-register or domain clearance yet - do that before publication. Binaries stay `navier*` (NAVIER is the
fluid tunnel's name) until a rename is decided. Before publication also remove the absolute home-folder paths that 34
tracked files contain (done 2026-09-27).

This file is the source of truth for intent. It records what the owner said, not what any session inferred. If another
document disagrees with it, that document is wrong. Change it only on the owner's word.

## The question

**Will this part work?** Millions of parts are 3D-printed, in plastic and in metal, by people who cannot tell whether they
will hold. AI agents increasingly understand the use: the forces, the attachments, how the thing will be handled. What
they need is a way to answer the question through physics, quickly and defensibly.

## What this project is

A free, open-source, locally running **physics laboratory built for AI agents, with humans as welcome users**: solid
mechanics, thermodynamics, fluid dynamics and additive-manufacturing process simulation that an agent can download,
drive, inspect, modify and extend at will. It is **crystallised knowledge**: deterministic, validated code, so that an
agent does not have to rebuild the physics in its head or from scratch each time. Agents will use it at three depths:
call it and move on; read and understand it for a tailored problem; rebuild or re-architect it. All three must be easy.

The intended place in the world is that of Linux, Android or OpenCAD: the open platform others build on, in contrast to
tools whose business depends on restricted access.

## The end goal

A person photographs or describes an object and asks an AI. The AI runs the physics here and returns an
**engineering certificate** that the part will not break in its use. Certificates are **graded**: with little
information (unknown plastic, printer, room temperature) a low-grade statement with its assumptions; with full
engineering data a high-grade one with a safety factor, fit to rely on for things that matter. In time such
AI-generated certificates may carry legal weight. The quality of the certificates is the product.

The design question to keep asking: **why would an agent in 2035 choose to download this repository?** To save compute,
to obtain a certificate others will accept, and to have a trustworthy physical environment in which to test its ideas.

## Requirements the owner set

- **Agents first, humans too.** Headless, complete control for agents; good graphics and interface for people. The
  owner wants to see it work.
- **Local, native, fast.** A program you download and run on your computer, in fast compiled languages, low latency.
  **Not web-based.** A browser viewer for print results was the wrong direction: additive-manufacturing results must be
  shown in the native application, like the fluid and solid workspaces.
- **Open source on GitHub eventually; private for now.** The name is provisional and may change before publication.
- **Materials (owner, 2026-09-19): every material there is open data for.** The library is not confined to one alloy; the
  long aim is every material that exists, and the step-by-step path is batches of sourced records (`materials/README.md`),
  each value with its origin, never a number from memory. A sourced record replaces a demonstration one.
- **Support structures (owner, 2026-09-19) are first-class physics, not a stiffness fudge.** Supports change the heat
  path and the mechanics of a print, and the commercial tools offer many types with many parameters. The solver must
  model them with their geometry-derived properties, verify them, and expose every choice (type, overhang angle,
  spacing, wall thickness, interface, fraction) to agents and users as typed inputs with provenance.
- **Current priority: additive-manufacturing simulation**, plastic and metal: internal and residual stress, heat, the
  effect of different cooling and temperatures, warping, and whether the printed part then holds its loads.
- **Code quality is a feature.** If the project is badly structured, agents will not use it. Builds are clean, work is
  validated, and there is a pipeline that checks it.
- **The project must explain itself to agents.** Agents run out of context and skip files they are not sure they need, so
  work gets forgotten and redone. Every part of the project is linked from a map; files link to their neighbours; there
  is a history of what was built, so nothing valuable stays hidden. Entry point: `AGENTS.md`.

## Focus stated by the owner on 2026-09-17 (later the same day)

- **The claim is not made smaller.** The aim stays accurate answers to fundamental engineering questions, up to
  certificates. The route there is validation, not a narrower promise.
- **No consumer pitch.** "Will it hold?" is dropped. This is a focused engineering application.
- **The owner is the first user**: an engineer who wants his own physics environment for real daily work, and for the
  pleasure of FEM and simulation. One person with one computer and AI tools is the normal case, not a handicap.
- **Anchor application: metal powder-bed (LPBF) process simulation.** The owner ran such simulations at a university
  with Simufact Additive (Hexagon): a licence costing thousands of euros, operated manually and tediously. The
  goal is a free, open-source, AI-native simulator that answers the same questions accurately, good enough to offer to
  the university.
- **Stand on what exists.** Established open-source solvers and published knowledge (CalculiX and others) may be used as
  references, cross-checks and sources of method, within their licences.

## Strategy stated by the owner on 2026-09-19: an open platform for simulating the physical world

- **The long vision:** a world where every simulation of the physical world can be tested and run on an open-source
  platform. 3D printing is the first process; its physics (elasticity, plasticity, heat, phase change, contact) is
  shared with sheet forming, welding and civil engineering, so a process well simulated once is a foundation for
  the next. Rule for the code: physics goes in the core, each process is a thin layer with profiles.
- **Competition against reality is the engine.** Public challenges with measured outcomes, pre-registered scoring,
  replayable entries and leaderboards, in the spirit of speedrunning: the whole game is how far a simulation can
  imitate reality. Any solver may enter; the leaderboard is the standard and this code is one reference entry.
  See `challenges/README.md`.
- **The moat is not the code.** Code of this kind will be regenerable cheaply; what cannot be regenerated is measured
  data, calibration profiles with provenance, trust and reference status, and the community that keeps them
  current. Contributions of measurements outrank contributions of code (`validation/contrib/`).
- **Collective agentic work.** The tool stays useful only if many people spend some of their own compute improving
  it. Every open task is written as a prompt with an acceptance test so that a person with an agent and an hour can
  take it (`TASKS.md`). The project must be findable and legible to agents (`AGENTS.md`, the map, the MCP server).
- **Publication path:** the repository goes public with the first paper and the first leaderboard; licence for
  adoption (Apache 2.0 for code and CC BY 4.0 for data, added 2026-09-19 on the owner's go-ahead; they take effect
  at publication); the university as the first partner and data source.

## The owner's words on what to sell, 2026-09-19

The vision is what makes people say "this is interesting", so it comes first and it is stated plainly: an open physics
simulator where you can see the world simulated, from a printed bracket to planets and stars, free for everyone. It is
almost a game: a real physics simulator and at the same time a game for engineers and AIs who want to test themselves
against reality and push it further. While people compete to make it better, everyone builds an open-source foundation
that others, not interested in competing, use to run experiments, to simulate the world for useful engineering, and
to simulate the world for fun, because we can. Metal 3D printing is the first level, not the point.

## How the team works

One human owner, one orchestrating agent, three worker agents. The orchestrator writes the prompts, the owner carries
them to the workers and brings the reports back, the orchestrator reviews and integrates. Workers never edit the same
files at the same time: each works on its own git branch in its own worktree.
