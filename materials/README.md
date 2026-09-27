# The materials library: every material we can source, every value with its origin

Owner's direction (2026-09-19): the library must grow to every material there is open data for, not one alloy.
Step by step, and never a number without a source. Up: [AGENTS.md](../AGENTS.md) · the live library the engine
embeds: [src/ctl/materials.json](../src/ctl/materials.json) · task: [TASKS.md](../TASKS.md) T14.

## The rule that makes the library trustworthy

Every property object carries `value`, `provenance` (`measured`, `published`, `calibrated`, `user_supplied`,
`demonstration`) and `source` (where the number can be checked: a URL with access date, a document in this
repository with page or table, or a measurement campaign). A record is `measured` or `published` only when every
value is; the loader in `src/ctl/matlib.c` refuses a built-in record that claims more than its values carry.
`tools/matcheck.py` enforces the same rule on the file before it is embedded (`make matcheck`, part of `make check`).

Facts are not copyrightable; tables are. Record a value with its citation; never copy a table wholesale from a
source whose licence does not allow it. Prefer sources that are open by construction.

## Where open data comes from, in order of preference

| Source | What | Terms |
|---|---|---|
| Measurement campaigns in this repository (`validation/contrib/`) | the values we can trust most, with scatter | CC BY 4.0 |
| Open-access papers (CC BY) | alloy-specific as-built properties, often from the process that matters | citation |
| NIST data (Chemistry WebBook, SRD, AM-Bench material data) | thermophysical properties of elements and compounds, US government works | public domain |
| Wikidata (CC0) and Wikipedia (CC BY-SA, cite the article and revision) | element properties, densities, melting points | open |
| The Materials Project, OQMD, AFLOW (computed, CC BY 4.0) | elastic tensors and densities of inorganic crystals, computed not measured; label `provenance: published` with `note: computed` | CC BY 4.0 |
| Supplier data sheets (Nikon SLM, EOS, Stratasys, Prusa and others) | process-specific values for AM powders and filaments | facts cited with URL and access date; no wholesale copying |
| Standards (ASTM, DIN, ISO) | minimum values by grade | cite the standard's number and edition; values from the standard's own free previews or from papers that quote them |

Not acceptable: values from memory, from a chat model, or from a database whose licence forbids redistribution
(commercial material databases, JMatPro output, handbook scans).

## What a record needs, by use

| Use | Properties |
|---|---|
| structural (static) | density, Young's modulus, Poisson's ratio; yield strength and hardening if plasticity is wanted; tensile strength |
| thermal and thermomechanical | conductivity, specific heat, expansion (tables against temperature where the source gives them), emissivity; solidus, liquidus, latent heat for melting |
| LPBF build | the structural set as built (not wrought: the source must say it is the additive condition), plus the calibrated inherent strains per machine and strategy, which live in the profiles, not in the material |
| FFF print | the structural set, glass transition or relaxation temperature, expansion, conductivity, specific heat, density |
| fluids | density and viscosity (against temperature), specific heat, conductivity |

Temperature-dependent values use the existing table form `{"t_c": [...], "value": [...]}`.

## Growth plan

Batch 1 (AM metals): AlSi10Mg (done), Ti-6Al-4V, 316L, Inconel 718, Inconel 625, 17-4PH, maraging steel 18Ni300,
CoCr (F75), CuCrZr, AlSi7Mg, Scalmalloy, pure copper, pure titanium.
Batch 1 is in the library since 2026-09-19; the documents its values were read from, with URL, access date and
checksum: [batch1-sources.md](batch1-sources.md).
Batch 2 (FFF and other polymers): PLA, ABS, PETG, PA12, PA6, TPU, PC, PEEK, PEI (ULTEM 9085), ASA, nylon-CF.
Batch 2 is in the library since 2026-09-19 (PA6 as the PA6/66 copolymer CoPA: no unfilled PA6 filament data sheet was found);
its documents: [batch2-sources.md](batch2-sources.md).
Batch 3 (common engineering metals): structural steel S235JR and S355JR, aluminium 6061-T6 and 7075-T6, 1050A H14
as the pure aluminium, stainless 1.4301 (304), brass CW614N, aluminium bronze CW307G, grey cast iron EN-GJL-250 and
squeeze cast AZ91.
Batch 3 is in the library since 2026-09-20; its documents, with URL, access date and checksum:
[batch3-sources.md](batch3-sources.md). The grades are the ones the documents actually cover: S355JR and not S355J2,
aluminium bronze and not tin bronze. Nothing in this batch is an as-built additive value, and six of the ten records
carry no Poisson's ratio because no document gave one for that alloy. `steel_plate_demo` stays beside them as
demonstration data rather than being deleted, because `tools/contactflow.py`, `tools/chtflow.py` and the Thermal Sim
three-domain example use it as a build plate.
Batch 4 (fluids and gases): water (done as demonstration), air (done as demonstration), seawater, glycerin, oils, argon.
Batch 5 (elements): every element with a solid form, from Wikidata and NIST, density, modulus where known, melting point.
Later: concrete, wood, glass, ceramics, composites; then anything with open data.

Each batch replaces demonstration records with sourced ones where the same material exists; the demonstration
record is deleted, not kept beside the sourced one, unless a test depends on it (then it is renamed `*_demo` and
marked `demonstration`).
