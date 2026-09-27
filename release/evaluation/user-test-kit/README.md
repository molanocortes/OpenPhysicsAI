# Independent-user test kit (NAVIER 0.3.0-rc2)

**Status: pending. No participant has used this kit, and no usability result exists.** The developer's own runs (the
clean-install test and the reference run in `facilitator/`) are not external usability evidence and are never reported
as such.

## What the test answers

Can someone who has not seen NAVIER before, working only from the package and this kit:

- install it;
- set up an unfamiliar comparison;
- recognise when the tool's record says a result is uncertain;
- report the result without claims the record does not support?

## What the owner has to do

The owner has to do these steps; the developer did not do them.

1. **Recruit one or more participants.** A suitable participant does engineering or design work with CAD files, has
   not used or developed NAVIER, and has an Apple-silicon Mac (macOS 14 or later) with about 1 GB of free disk space.
   Python 3 is optional. A participant may use their own MCP-capable AI client (for example Claude Code); record which.
2. **Obtain informed consent** for recording their feedback, timings and diagnostics, and for sharing them
   anonymised. How the owner does this is outside this kit.
3. **Hand over, and nothing else:**
   - the package `navier-0.3.0-rc2-macos-arm64.tar.gz` with its `.sha256`;
   - `QUICK_START.md` and `TASK.md`;
   - the `task_geometry/` folder;
   - `EXPECTED_ARTIFACTS.md`, `FEEDBACK_FORM.md` and `DIAGNOSTICS.md`.

   Do **not** hand over `facilitator/`.
4. **Do not coach.** If the participant is stuck for more than 15 minutes, you may point them to a document by name,
   and record that as an intervention on the form.
5. **Collect:**
   - the completed feedback form;
   - the study folder, without `results.nvr` files (`DIAGNOSTICS.md` shows how);
   - the diagnostics bundle when something failed;
   - the participant's written answer to the task.
6. **Compare** the answer with `facilitator/REFERENCE.md` using the checklist there. Record the results in
   `release/evaluation/user-test-kit/RESULTS.md` (create it). State each participant's setup and any intervention.

## Files

| File | For | Content |
|---|---|---|
| `QUICK_START.md` | participant | install, check the installation, run the packaged example |
| `TASK.md` | participant | the unfamiliar task: two mounting plates of equal mass |
| `task_geometry/plate_uniform.stl`, `plate_stepped.stl` | participant | the task geometry (mm); `make_task_geometry.py` regenerates it |
| `EXPECTED_ARTIFACTS.md` | participant | which files a finished study leaves, without the answer |
| `FEEDBACK_FORM.md` | participant | times, obstacles, errors, confidence, free text |
| `DIAGNOSTICS.md` | participant | what to send when something fails, and how to remove personal paths |
| `facilitator/REFERENCE.md` | facilitator only | the developer's reference definition and result, a grading checklist, known pitfalls |

## Checksums of the task geometry

```
cca1c768edf296c3900640aa483a8f425eb7446a9563e145da348b6ee0ad4ff6  task_geometry/plate_uniform.stl
b95f89b5110855e2e5acd3e429dd485fba6dc4d6e25bec8ec1ed80bb266f01eb  task_geometry/plate_stepped.stl
```
