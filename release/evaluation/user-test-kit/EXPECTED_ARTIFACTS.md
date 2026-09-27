# What a finished study leaves behind

This list does not contain the answer. It tells you whether a run finished and wrote its record. Replace
`<study>` with the folder you passed to `study run --dir`.

| File or folder | Present when | What to look at |
|---|---|---|
| `<study>/request.json` | the study was accepted | your definition as submitted |
| `<study>/study.json` | the study was accepted | the resolved specification: units, material, regions, load, assumptions |
| `<study>/operations.jsonl` | the study started | one line per operation NAVIER executed |
| `<study>/previews/<design>-regions.png` | the study started | the mounting and load regions drawn on each design; check they are where you meant |
| `<study>/designs/<design>/` | the study started | one project per design, with a folder per analysis in `runs/` |
| `<study>/evidence.json` | the study finished, even partly | the machine-readable record |
| `<study>/report.md` | the study finished, even partly | the readable record: outcome, per-design results, checks, assumptions, exclusions |

Also check:

- `bin/navier-ctl study check <definition>` exits with 0 (ready), 4 (needs input: read the questions) or 5 (not
  supported).
- `bin/navier-ctl study run` prints `status:` and `outcome:`. A status other than `complete` means a failure is
  listed in the report, with a suggested recovery.
- The region areas and the lever arm in the report's design table match the parts as you understand them.
