#!/usr/bin/env python3
"""matcheck.py - the materials library must carry a source on every value it claims.

Checks src/ctl/materials.json before it is embedded:
  - every record has id, name, family, status, provenance and at least density and Young's modulus
  - status is one of demonstration, user_supplied, calibrated, measured, published
  - a record whose status is measured or published has, on EVERY property object, a provenance and a source that is
    not empty and does not look like a placeholder; a table {"t_c": [...], "value": [...]} needs the same
  - a demonstration record says so in its provenance text
  - ids are unique, lower case, and every value is finite and positive where physics demands it
Exit 0 when clean; 1 with the list of problems. No dependencies.
"""
import json
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
LIB = ROOT / "src" / "ctl" / "materials.json"
STATUSES = {"demonstration", "user_supplied", "calibrated", "measured", "published"}
VALUE_PROVENANCE = STATUSES | {"inferred", "computed"}  # a single value may be inferred or computed, if its source says from what
PROPS_POSITIVE = {"density_kg_m3", "youngs_modulus_pa", "conductivity_w_per_mk", "specific_heat_j_per_kgk",
                  "yield_strength_pa", "strength_pa", "hardening_modulus_pa", "latent_heat_j_per_kg"}
PLACEHOLDER = re.compile(r"^(|todo|tbd|unknown|n/?a|from memory|-)$", re.I)


def values_of(obj):
    if isinstance(obj, dict) and "t_c" in obj:
        return list(obj.get("value", []))
    if isinstance(obj, dict):
        v = obj.get("value")
        return values_of(v) if isinstance(v, dict) else [v]
    return [obj]


def main():
    data = json.loads(LIB.read_text())
    records = data if isinstance(data, list) else data.get("materials", [])
    problems, ids = [], set()
    for r in records:
        rid = r.get("id", "?")
        if rid in ids:
            problems.append(f"{rid}: duplicate id")
        ids.add(rid)
        if rid != rid.lower():
            problems.append(f"{rid}: id must be lower case")
        needed = ["id", "name", "family", "status", "provenance", "density_kg_m3"]
        if r.get("family") != "fluid":
            needed.append("youngs_modulus_pa")
        for k in needed:
            if k not in r:
                problems.append(f"{rid}: missing {k}")
        status = r.get("status")
        if status not in STATUSES:
            problems.append(f"{rid}: status '{status}' not allowed")
        if status == "demonstration":
            text = (str(r.get("provenance", "")) + " " + str(r.get("name", ""))).lower()
            if not any(w in text for w in ("demonstration", "not calibrated", "not traceable", "illustrative")):
                problems.append(f"{rid}: a demonstration record must say so in its provenance or name")
        sourced = status in ("measured", "published")
        for key, obj in r.items():
            if key in ("id", "name", "family", "processes", "status", "provenance", "not_modelled", "powder", "note"):
                continue
            if sourced:
                if not isinstance(obj, dict) or "provenance" not in obj or PLACEHOLDER.match(str(obj.get("source", "")).strip()):
                    problems.append(f"{rid}: {key} needs provenance and a real source (record is {status})")
                elif obj.get("provenance") not in VALUE_PROVENANCE:
                    problems.append(f"{rid}: {key} provenance '{obj.get('provenance')}' not allowed")
            for v in values_of(obj):
                if isinstance(v, (int, float)):
                    if not math.isfinite(v):
                        problems.append(f"{rid}: {key} is not finite")
                    elif key in PROPS_POSITIVE and v <= 0:
                        problems.append(f"{rid}: {key} must be positive")
            if isinstance(obj, dict) and "t_c" in obj:
                t, v = obj.get("t_c", []), obj.get("value", [])
                if len(t) != len(v) or len(t) < 2 or any(b <= a for a, b in zip(t, t[1:])):
                    problems.append(f"{rid}: {key} table must have equal-length, increasing t_c")
        pr = r.get("poisson_ratio")
        for v in values_of(pr) if pr is not None else []:
            if isinstance(v, (int, float)) and not (-1.0 < v < 0.5):
                problems.append(f"{rid}: poisson_ratio {v} outside (-1, 0.5)")
    n_sourced = sum(1 for r in records if r.get("status") in ("measured", "published"))
    print(f"matcheck: {len(records)} materials, {n_sourced} with a source on every value, {len(records) - n_sourced} demonstration or other")
    for p in problems:
        print("  " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
