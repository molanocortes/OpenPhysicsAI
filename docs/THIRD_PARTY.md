# Third-party code, data and methods

Up: [AGENTS.md](../AGENTS.md) (rule 1)

Since 2026-09-26 the lab builds on existing open-source work instead of writing everything itself (the owner: stand on
the shoulders of giants, to be the best open-source physics simulator). Every library, repository or dataset brought
into the build is listed here, before or with the commit that adds it.

Conditions (AGENTS.md rule 1): a licence compatible with an open-source release, permissive preferred (MIT, BSD, zlib,
Apache, public domain), copyleft (GPL, AGPL) only by the owner's explicit decision; a pinned version, vendored under
`third_party/` or fetched by the build, so that a fresh clone builds on the development machine without Homebrew; and
the lab's own verification still applies to whatever the library computes.

| Name | Version | Licence | Where | What it does for the lab | Added |
|---|---|---|---|---|---|
| [MFEM](https://mfem.org/) | 4.8; archive SHA256 `65472f732d273832c64b2c39460649dd862df674222c71bfa82cf2da76705052` | [BSD-3-Clause](../third_party/MFEM-LICENSE) | fetched under `build/third_party/` by `tools/build_mfem.py` (`make mfem`) | Double-precision 3D finite elements: Nedelec edge elements for magnetostatics (the 3D motor), H1 elements; verified by magnet3dtest, mag3dtest, motor3dtest | 2026-09-26 |
| Accelerate (Apple), Sparse Solvers | the macOS SDK's | system framework, part of macOS | linked, not shipped | Sparse Cholesky and LDL^T factorisation (src/lab/magnet/spdirect.c) for the 3D finite-element systems | 2026-09-26 |
| LG M50 cell parameters (Chen2020), from [PyBaMM](https://github.com/pybamm-team/PyBaMM) | `packages/pybamm/src/pybamm/input/parameters/lithium_ion/Chen2020.py` on `main`, read 2026-09-26 | BSD-3-Clause (PyBaMM); the values are those of Chen et al., J. Electrochem. Soc. 167 (2020) 080534 | typed into `src/lab/battery/dfn.c` (`dfn_spec_lgm50`) and `tools/lab_scenarios.py` | The cell's electrode, separator and electrolyte parameters, its open-circuit potential fits and the layer data its thermal properties are computed from; verified by battest | 2026-09-26 |

Methods and data taken from publications are cited where they are used (solver headers, scenario `source` fields,
docs/lab/*.md); this table is for code and datasets that enter the build or the repository.
