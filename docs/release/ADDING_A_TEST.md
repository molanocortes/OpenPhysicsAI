# Adding a test

Tests live in the source tree and run with `make test` from the repository root. That target builds everything
headless, runs every suite, the comparison workflow (`tools/studyflow.py`) and `navier-ctl doctor`, and stops at the
first failure.

## Which suite

| What you change or add | Where the test goes |
|---|---|
| a numerical routine (element, solver, quantity) | a C test in `tools/*test.c` (`femtest.c` for structural FEM), checked against an analytical or manufactured solution |
| an operation or its schema | `tools/opstest.c` (validation and errors) and, if MCP-visible, `tools/mcptest.py` |
| the comparison workflow, a new failure mode, a new outcome | a case in `tools/studyflow.py`, driven through MCP exactly as an AI client would |
| an end-to-end reference case | `release/reference_cases/CRITERIA.md` first, then its case in `tools/studyflow.py` |

## Rules that keep the evidence honest

1. **Write the acceptance criterion before the first run,** with a date and its justification (why that bound). If a
   run later shows the criterion was wrong, add an amendment with the observed value and the reason. Never edit the
   original text.
2. **Compare with an independent reference:**
   - an analytical solution;
   - a manufactured solution;
   - an exact identity of the discrete problem (balance, energy, scaling);
   - another code.

   A second run of the same code is a reproducibility check, not a reference.
3. **Check behaviour, not just success.** For a failure case, assert the error code, the question id, whether it is
   blocking, and that no conclusion was drawn.
4. **Keep scripted-client results separate from AI-client and user results.** A passing `studyflow.py` shows that the
   tools behave as specified; it shows nothing about a model or a person using them.

## A studyflow case, in outline

```python
def case_E14(c, work):
    print("== E14 what this case checks")
    defn = definition("e14", [design("A", "bracket_a_plain.stl"), design("B", "bracket_b_chamfer.stl")], ...)
    ok, v, _ = call(c, "study_check", {"definition": defn})
    check(ok and v["status"] == "needs_input", "E14: ...")            # the expected tool behaviour
    st, ev, _ = run_study(c, defn, work / "ws" / "e14")                # only if the case needs a run
    check(ev["comparison"]["outcome"] == "...", "E14: ...")
```

Register it in `CASES`, add the case to `release/evaluation/CASES.md` with its expected behaviour, then run
`python3 tools/studyflow.py --only E14`.

Geometry for new cases goes in `examples/bracket_comparison/make_geometry.py`: profiles on the 4 mm lattice keep
every face exactly representable by the voxel mesh at 4, 2 and 1 mm.
