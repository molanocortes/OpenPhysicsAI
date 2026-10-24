# Application trials: can an agent build what nobody programmed? (G24 step 4)

Up: [the agent leaderboard](../README.md) · goal: [G24 in ROADMAP.md](../../../ROADMAP.md) · the platform under test:
[the physics kernel](../../../docs/lab/kernel.md)

G24 is measured by the time an agent needs, starting from this platform, to reach a verified simulation of something
nobody programmed here, and by how far its answer lands from reality. A trial gives a fresh agent one problem and
nothing else:

- the platform's location (start at `AGENTS.md`), with its programs already built, and a working folder of its own;
- the problem with its inputs given as values;
- the rules: get the answer from a simulation set up on the platform, show that it is converged, do not derive it from
  a formula or look it up, no run longer than ten minutes.

The reference is held back from the agent: an exact solution that the trial's author computed, or a measurement
(sealed while it is a held-out answer). Each trial is one JSON record in this folder: the prompt verbatim (the
platform's folder written `<platform checkout>` and the agent's own `<working folder>`), the model and the tool, the wall time, the agent's answer and its convergence
evidence, the reference and the error, what the agent said about the documentation, and what was changed because of
it.

| Trial | Date | Model | Problem | Reference | Agent's answer | Error | Wall time |
|---|---|---|---|---|---|---|---|
| [fuel rod](2026-10-08-fuel-rod.json) | 2026-10-08 | claude-opus-5-5 | when a fuel rod's axis reaches 1000 K once its heat switches on: heat as a field, a solid cylinder, time | 5.32636 s, exact (series) | 5.32636 s, converged in mesh, order and time step | under 1e-5 s | 15 min 17 s, 164,058 tokens |
| [bimetal strip](2026-10-08-bimetal.json) | 2026-10-08 | claude-opus-5-5 | a thermostatic bimetal's specific thermal curvature from its two layers' published data | 39.0e-6 /K +- 4 %, measured (Aperam IMPHY 108SP, TB20110) | 37.7e-6 /K, converged | -3.4 %, inside the maker's +- 4 % | 8 min 24 s, 162,445 tokens |

The first trial checked the time to a verified answer, not reality: its reference was exact. It found seven places where
the documentation or the platform slowed the agent, each fixed the same day (`changed_because_of_it` in its record).

The second trial was the first against a measurement: the agent's answer lands 3.4 % under the maker's measured
curvature, inside its 4 % tolerance; the kernel reproduces the textbook model of the strip to 0.03 % (K17), so the gap is
the published inputs (Invar's expansion alone spans 2.8 %), listed in its record. Its five frictions were fixed the same
night.

Next: a trial where the platform does more than the textbook can, against a measurement held back: a contact problem
(G24 step 3 has rigid obstacles now) or a part whose shape a formula cannot handle.
