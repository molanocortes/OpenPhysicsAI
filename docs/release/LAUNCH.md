# Launch checklist: from private repository to the open reference

Owner's decision on 2026-09-19: the project goes public as the open reference for physics simulation against measured
reality. Nothing on this list is done until it is ticked with a date. Up: [AGENTS.md](../../AGENTS.md).

## Before the repository goes public

- [ ] Licences in place: `LICENSE` (Apache 2.0, added 2026-09-19) and `LICENSE-DATA` (CC BY 4.0). The owner confirms
      both; the contribution page stops saying "pending".
- [x] The Hochschule Anhalt material (the LPBF cantilever measurements, commercial-simulator results, reports,
      specimen geometry, the protocol, the two challenges built on it and the tools that only read it) and the
      internal working files (session hand-offs, orchestration prompts, recovery copies, snapshots, raw AI-client
      transcripts) removed from the working tree on 2026-09-27, by the owner's decision. The cylinder head had been
      removed the same day for copyright.
- [ ] **A fresh start, not a rewritten history** (owner's decision, 2026-09-27). The public repository begins from a
      single new commit of the cleaned tree. The existing history, which still carries everything removed above, stays
      in the private repository `molanocortes/OpenPhysicsAI` and is never pushed to the public one. Create the public
      repository new (not as a fork, mirror or transfer of the private one), so that no pull request ref or cached
      object carries the old history, and make its first commit from a copy of the working tree without `.git`. Before
      that commit, check the copy: no path removed above is present, and `bash tools/check.sh F` reports none.
- [ ] Third-party material checked: the open-access paper that defines the sample cantilever (CC BY 4.0, cited in
      `samples/samples.json`); the data sheets and papers the materials library cites (facts cited, tables never
      copied); no JMatPro, no manuals, nothing from the private folders.
- [x] `bash tools/check.sh F` reports zero tracked files with absolute home paths (task T9). 2026-09-27, with no
      exemption left in the check.
- [ ] `make check` green; `make leaderboard` runs; the interface walkthroughs pass on an idle machine.
- [ ] Program names: decide whether the binaries stay `navier*` or become `openphysics*`; the app's title and the
      README agree with the decision.
- [ ] `README.md` front page reviewed by one person who has never seen the project.

## Launch day

- [ ] The public repository created from the single cleaned commit (above) and set to public.
- [ ] GitHub Pages enabled on `docs/leaderboard/` (the workflow already generates it); the URL written into
      `challenges/README.md` and the README.
- [ ] Data set deposited on Zenodo from `.zenodo.json`; the DOI written into `CITATION.cff`.
- [ ] MCP server submitted to the public registry from `mcp/server.json` after checking the registry's current schema.
- [ ] Social preview image set in the GitHub settings (`docs/images/fluid-car-wake.png` or the cantilever build).
- [ ] The first announcement: one page, the challenge, the board, the kit, the tasks; posted where AM engineers and
      agent builders read (the AM-Bench community, the simulation forums, the MCP and agent communities). No claims
      beyond the leaderboard's own words.

## First month after launch

- [ ] Import the first NIST AM-Bench challenge (T11).
- [ ] Open the agent leaderboard with the first recorded runs.
- [x] Linux build merged (T1) so agents on servers can run it. 2026-09-19: all 21 suites green on ubuntu-latest.
- [ ] At least one campaign from the kit by someone who is not the owner.
- [ ] A "model release day" routine written down: which tasks to run a new model on, how to record it.
