/* study_report.c - the readable Engineering Evidence Record (Markdown), generated only from evidence.json */
#include "../core/sbuf.h"
#include "study_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void num(StrBuf *b, double v, const char *fmt) {
    if (isfinite(v)) sb_printf(b, fmt, v);
    else sb_puts(b, "n/a");
}

/* table-safe text: pipes and newlines would break a Markdown table */
static void cell(StrBuf *b, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '|') sb_puts(b, "\\|");
        else if (*p == '\n' || *p == '\r') sb_putc(b, ' ');
        else sb_putc(b, *p);
    }
}

static void vec(StrBuf *b, const JsonValue *a, const char *fmt) {
    double v[3];
    if (!json_get_numbers(a, v, 3)) {
        sb_puts(b, "n/a");
        return;
    }
    sb_putc(b, '(');
    for (int k = 0; k < 3; k++) {
        if (k) sb_puts(b, ", ");
        num(b, v[k], fmt);
    }
    sb_putc(b, ')');
}

static void bullets(StrBuf *b, const JsonValue *arr) {
    for (size_t i = 0; i < json_len(arr); i++) {
        const JsonValue *s = json_at(arr, i);
        sb_puts(b, "- ");
        sb_puts(b, json_str(s) ? json_str(s) : json_get_str(s, "text", ""));
        sb_putc(b, '\n');
    }
    if (!json_len(arr)) sb_puts(b, "- none\n");
}

char *study_report_markdown(const JsonValue *ev) {
    StrBuf b;
    sb_init(&b);
    const JsonValue *st = json_get(ev, "study"), *q = json_get(ev, "question"), *cmp = json_get(ev, "comparison"), *mc = json_get(ev, "modeled_conditions");
    sb_printf(&b, "# Engineering Evidence Record: %s\n\n", json_get_str(st, "name", ""));
    sb_printf(&b, "Status: **%s**. Generated %s. Study hash `%s`.\n\n", json_get_str(ev, "status", ""), json_get_str(ev, "generated", ""),
              json_get_str(st, "study_hash", ""));
    sb_printf(&b, "> %s. Every number below is read from `evidence.json` in this folder; the Traceability section says where.\n\n",
              json_get_str(ev, "nature", ""));

    sb_puts(&b, "## Question\n\n");
    sb_printf(&b, "%s\n\n", json_get_str(q, "text", ""));
    if (json_get(q, "decision")) sb_printf(&b, "Decision to support: %s\n\n", json_get_str(q, "decision", ""));

    sb_puts(&b, "## Outcome\n\n");
    sb_printf(&b, "**%s**\n\nOutcome code: `%s`", json_get_str(cmp, "statement", "no comparison"), json_get_str(cmp, "outcome", ""));
    if (json_get(cmp, "element_size_mm")) {
        sb_puts(&b, " (compared at the finest mesh every design completed validly: ");
        num(&b, json_get_num(cmp, "element_size_mm", NAN), "%.4g");
        sb_puts(&b, " mm)");
    }
    sb_puts(&b, ".\n\n");
    const JsonValue *vals = json_get(cmp, "values");
    if (json_len(vals)) {
        sb_puts(&b, "| Design | Load-region displacement (mm) | Last change of the primary quantity (relative to the finer mesh) | Convergence criterion | Discretisation-error estimate of the primary quantity | Stiffness (N/mm) | Mass, geometry (kg) | Stiffness / mass (N/mm/kg) | Peak displacement (mm) |\n");
        sb_puts(&b, "|---|---|---|---|---|---|---|---|---|\n");
        for (size_t i = 0; i < json_len(vals); i++) {
            const JsonValue *v = json_at(vals, i);
            sb_printf(&b, "| %s | ", json_get_str(v, "design", ""));
            num(&b, json_get_num(v, "load_region_displacement_mm", NAN), "%.5g");
            sb_puts(&b, " | ");
            num(&b, 100 * json_get_num(v, "last_relative_change", NAN), "%+.2f");
            sb_puts(&b, "% | ");
            sb_puts(&b, json_get_bool(v, "convergence_criterion_met", false) ? "met" : "**not met**");
            sb_puts(&b, " | ");
            if (json_get_bool(v, "estimate_available", false)) {
                num(&b, json_get_num(v, "discretisation_error_estimate", NAN), "%.2g");
                sb_puts(&b, " (");
                num(&b, 100 * json_get_num(v, "relative_discretisation_error_estimate", NAN), "%.2g");
                sb_puts(&b, "%; safety factor ");
                num(&b, json_get_num(v, "safety_factor", NAN), "%.3g");
                sb_puts(&b, ")");
            } else {
                sb_puts(&b, "not available: ");
                cell(&b, json_str(json_at(json_get(v, "estimate_unavailable_reasons"), 0)) ? json_str(json_at(json_get(v, "estimate_unavailable_reasons"), 0)) : "conditions not met");
            }
            sb_puts(&b, " | ");
            num(&b, json_get_num(v, "stiffness_n_per_mm", NAN), "%.5g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(v, "mass_geometry_kg", NAN), "%.4g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(v, "stiffness_per_mass_n_per_mm_kg", NAN), "%.5g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(v, "peak_displacement_mm", NAN), "%.4g");
            sb_puts(&b, " |\n");
        }
        sb_printf(&b, "\nPrimary quantity: `%s` (%s is better). The peak displacement is supporting information: the question concerns the load region. "
                      "A discretisation-error estimate is a grid convergence index: an estimate, not a bound or a confidence interval; it is offered only "
                      "when its conditions hold (see the refinement of each design).\n\n",
                  json_get_str(cmp, "primary_quantity", ""), json_get_str(cmp, "better", ""));
        const JsonValue *bm = json_get(cmp, "ranking_by_mesh");
        if (json_len(bm)) {
            sb_printf(&b, "Ranking on the tested meshes (%s on all %lld):\n\n| h (mm) | Ranking, best first | Difference between the best two (relative to the second) |\n|---|---|---|\n",
                      json_get_bool(cmp, "ranking_consistent_on_tested_meshes", false) ? "the same" : "**not the same**", json_get_int(cmp, "meshes_compared", 0));
            for (size_t i = 0; i < json_len(bm); i++) {
                const JsonValue *m = json_at(bm, i), *rk = json_get(m, "ranking");
                num(&b, json_get_num(m, "element_size_mm", NAN), "| %.4g | ");
                for (size_t k = 0; k < json_len(rk); k++) sb_printf(&b, "%s%s", k ? ", " : "", json_str(json_at(rk, k)));
                sb_puts(&b, " | ");
                num(&b, 100 * json_get_num(m, "relative_difference", NAN), "%.2f");
                sb_puts(&b, "% |\n");
            }
            sb_puts(&b, "\n");
        }
    }
    const JsonValue *rob = json_get(cmp, "ranking_robustness");
    if (json_len(rob)) {
        sb_puts(&b, "Robustness of the ranking:\n\n");
        for (size_t i = 0; i < json_len(rob); i++) {
            const JsonValue *r = json_at(rob, i);
            sb_printf(&b, "- %s", json_get_str(r, "sensitivity", ""));
            if (json_get(r, "parameter")) sb_printf(&b, " (%s)", json_get_str(r, "parameter", ""));
            if (json_get(r, "ranking_unchanged")) sb_printf(&b, ": ranking %s", json_get_bool(r, "ranking_unchanged", false) ? "unchanged" : "**changes**");
            if (json_get(r, "largest_relative_change")) {
                sb_puts(&b, ", largest change ");
                num(&b, 100 * json_get_num(r, "largest_relative_change", NAN), "%.3g");
                sb_puts(&b, "%");
            }
            if (json_get(r, "reason")) sb_printf(&b, ". %s", json_get_str(r, "reason", ""));
            sb_puts(&b, "\n");
        }
        sb_puts(&b, "\n");
    }

    sb_puts(&b, "## Modeled conditions\n\n");
    const JsonValue *mat = json_get(mc, "material"), *mf = json_get(mc, "manufacturing"), *mt = json_get(mc, "mounting"), *ld = json_get(mc, "load");
    const JsonValue *an = json_get(mc, "analysis");
    sb_printf(&b, "- **Material:** `%s` (%s); status %s, source %s. ", json_get_str(mat, "id", "?"), json_get_str(mat, "name", ""),
              json_get_str(mat, "status", "?"), json_get_str(mat, "source", "?"));
    sb_puts(&b, "E = ");
    num(&b, 1e-9 * json_get_num(mat, "youngs_modulus_pa", NAN), "%.4g");
    sb_puts(&b, " GPa, nu = ");
    num(&b, json_get_num(mat, "poisson_ratio", NAN), "%.3g");
    sb_puts(&b, ", density ");
    num(&b, json_get_num(mat, "density_kg_m3", NAN), "%.5g");
    sb_printf(&b, " kg/m3. Reference: %s\n", json_get_str(mat, "reference", "none given"));
    sb_printf(&b, "- **Manufacturing:** %s (source %s)", json_get_str(mf, "process", "unspecified"), json_get_str(mf, "source", ""));
    if (json_get(mf, "orientation")) sb_printf(&b, "; orientation %s", json_get_str(mf, "orientation", ""));
    if (json_get(mf, "infill_percent")) sb_printf(&b, "; infill %.0f%%", json_get_num(mf, "infill_percent", NAN));
    sb_puts(&b, "\n");
    sb_printf(&b, "- **Mounting:** %s, on each design's mounting region (source %s). %s\n", json_get_str(mt, "idealization", ""),
              json_get_str(mt, "source", ""), json_get_str(mt, "description", ""));
    sb_puts(&b, "- **Load:** ");
    if (!strcmp(json_get_str(ld, "kind", ""), "payload_mass")) {
        num(&b, json_get_num(ld, "mass_kg", NAN), "%.6g");
        sb_puts(&b, " kg payload x g = ");
        num(&b, json_get_num(ld, "gravity_m_s2", NAN), "%.6g");
        sb_printf(&b, " m/s2 (source %s) = ", json_get_str(ld, "gravity_source", ""));
    }
    num(&b, json_get_num(ld, "force_n", NAN), "%.6g");
    sb_puts(&b, " N along ");
    vec(&b, json_get(ld, "direction"), "%.4g");
    sb_printf(&b, " (design frame), spread uniformly over the load region; self-weight %s; source %s. %s\n",
              json_get_bool(ld, "self_weight", false) ? "included" : "not included", json_get_str(ld, "source", ""), json_get_str(ld, "description", ""));
    sb_printf(&b, "- **Analysis:** %s; %s, %s formulation; solver %s; properties at ", json_get_str(an, "physics", ""), json_get_str(an, "elements", ""),
              json_get_str(an, "formulation", ""), json_get_str(an, "solver", ""));
    num(&b, json_get_num(an, "reference_temperature_c", NAN), "%.4g");
    sb_puts(&b, " degC.\n");
    sb_printf(&b, "- **Frames and units:** %s.\n\n", json_get_str(mc, "frames", ""));

    const JsonValue *dsg = json_get(mc, "designs");
    sb_puts(&b, "| Design | File | SHA-256 | Units | Size (mm) | Volume (mm3) | Thinnest wall (mm) | Mounting area (mm2) | Load area (mm2) | Lever arm (mm) |\n");
    sb_puts(&b, "|---|---|---|---|---|---|---|---|---|---|\n");
    for (size_t i = 0; i < json_len(dsg); i++) {
        const JsonValue *d = json_at(dsg, i), *g = json_get(d, "geometry"), *rg = json_get(d, "regions");
        sb_printf(&b, "| %s | ", json_get_str(d, "name", ""));
        cell(&b, json_get_str(g, "file", ""));
        sb_printf(&b, " | `%.12s` | %s | ", json_get_str(g, "sha256", ""), json_get_str(g, "units", "?"));
        vec(&b, json_get(g, "size_mm"), "%.4g");
        sb_puts(&b, " | ");
        num(&b, json_get_num(g, "volume_mm3", NAN), "%.6g");
        sb_puts(&b, " | ");
        num(&b, json_get_num(json_get(g, "wall_thickness"), "min_mm", NAN), "%.3g");
        sb_puts(&b, " | ");
        num(&b, json_get_num(json_get(rg, "mounting"), "area_mm2", NAN), "%.5g");
        sb_puts(&b, " | ");
        num(&b, json_get_num(json_get(rg, "load"), "area_mm2", NAN), "%.5g");
        sb_puts(&b, " | ");
        num(&b, json_get_num(rg, "lever_arm_length_mm", NAN), "%.4g");
        sb_puts(&b, " |\n");
    }
    const JsonValue *eq = json_get(mc, "equivalence");
    sb_printf(&b, "\nEquivalence of the conditions between designs: **%s**. %s\n\n", json_get_str(eq, "status", "not checked"), json_get_str(eq, "definition", ""));
    for (size_t i = 0; i < json_len(json_get(eq, "checks")); i++) {
        const JsonValue *c = json_at(json_get(eq, "checks"), i);
        sb_printf(&b, "- %s, design %s: difference ", json_get_str(c, "quantity", ""), json_get_str(c, "design", ""));
        num(&b, json_get_num(c, "difference", NAN), "%.3g");
        sb_printf(&b, " %s (tolerance ", json_get_str(c, "unit", ""));
        num(&b, json_get_num(c, "tolerance", NAN), "%.3g");
        sb_printf(&b, ") %s\n", json_get_bool(c, "ok", false) ? "ok" : "**differs**");
    }

    sb_puts(&b, "\n## Results by design and mesh\n\n");
    const JsonValue *designs = json_get(ev, "designs");
    for (size_t i = 0; i < json_len(designs); i++) {
        const JsonValue *d = json_at(designs, i), *levels = json_get(d, "levels");
        sb_printf(&b, "### Design %s\n\n", json_get_str(d, "name", ""));
        sb_puts(&b, "| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |\n");
        sb_puts(&b, "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
        for (size_t k = 0; k < json_len(levels); k++) {
            const JsonValue *lv = json_at(levels, k), *qq = json_get(lv, "quantities"), *ck = json_get(lv, "checks"), *ms = json_get(lv, "mesh"), *co = json_get(lv, "cost");
            num(&b, json_get_num(lv, "element_size_mm", NAN), "| %.4g | ");
            if (!json_get_bool(lv, "completed", false)) {
                sb_puts(&b, "not completed (see failures) | | | | | | | | | | | no | | | |\n");
                continue;
            }
            num(&b, json_get_num(ms, "elements", NAN), "%.0f");
            sb_puts(&b, " | ");
            num(&b, json_get_num(ms, "volume_error_percent", NAN), "%.3g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(qq, "load_region_displacement_mm", NAN), "%.6g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(qq, "stiffness_n_per_mm", NAN), "%.6g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(qq, "mass_mesh_kg", NAN), "%.5g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(ck, "equilibrium_error", NAN), "%.2g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(ck, "moment_balance_error", NAN), "%.2g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(ck, "energy_ratio", NAN) - 1, "%.2g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(json_get(ck, "load_region_mapping"), "line_of_action_shift_mm", NAN), "%.2g");
            sb_printf(&b, " | %s | %s | %s | ", json_get_str(ck, "deformation_classification", ""), json_get_bool(lv, "valid", false) ? "yes" : "**no**",
                      json_get_str(ck, "solver_method", ""));
            num(&b, json_get_num(co, "wall_seconds", NAN), "%.3g");
            sb_puts(&b, " | ");
            num(&b, json_get_num(co, "factor_mb", NAN), "%.3g");
            sb_puts(&b, " |\n");
        }
        const JsonValue *rf = json_get(json_get(d, "refinement"), "load_region_displacement_mm");
        sb_printf(&b, "\nRefinement of the load-region displacement: %s.\n", json_get_str(rf, "reading", ""));
        sb_puts(&b, "- Changes between meshes (relative to the finer mesh):");
        for (size_t k = 0; k < json_len(json_get(rf, "relative_changes")); k++) {
            const JsonValue *cv = json_at(json_get(rf, "relative_changes"), k);
            sb_puts(&b, k ? ", " : " ");
            num(&b, cv && cv->type == JSON_NUMBER ? 100 * cv->u.number : NAN, "%+.2f");
            sb_puts(&b, "%");
        }
        sb_puts(&b, "\n");
        for (size_t k = 0; k < json_len(json_get(rf, "observed_orders")); k++) {
            const JsonValue *po = json_at(json_get(rf, "observed_orders"), k);
            sb_puts(&b, "- Observed order over ");
            vec(&b, json_get(po, "element_sizes_mm"), "%.4g");
            sb_puts(&b, " mm: ");
            num(&b, json_get_num(po, "order", NAN), "%.3g");
            sb_puts(&b, "\n");
        }
        for (size_t k = 0; k < json_len(json_get(rf, "estimate_conditions")); k++) {
            const JsonValue *c = json_at(json_get(rf, "estimate_conditions"), k);
            sb_printf(&b, "- Estimate condition \"%s\": %s (%s)\n", json_get_str(c, "condition", ""), json_get_bool(c, "met", false) ? "met" : "not met",
                      json_get_str(c, "observed", ""));
        }
        for (size_t k = 0; k < json_len(levels); k++) {
            const JsonValue *lv = json_at(levels, k), *st2 = json_get(lv, "stress");
            if (!json_get_bool(lv, "completed", false)) continue;
            sb_puts(&b, "- Stress at ");
            num(&b, json_get_num(lv, "element_size_mm", NAN), "%.4g");
            sb_puts(&b, " mm (supporting information, not a strength assessment): von Mises Gauss-point maximum ");
            num(&b, json_get_num(st2, "gauss_point_max_mpa", NAN), "%.4g");
            sb_puts(&b, " MPa, nodal-average maximum ");
            num(&b, json_get_num(st2, "nodal_average_max_mpa", NAN), "%.4g");
            sb_puts(&b, " MPa, nodal 99th percentile ");
            num(&b, json_get_num(st2, "nodal_p99_mpa", NAN), "%.4g");
            sb_printf(&b, " MPa%s%s\n", json_get_bool(st2, "peak_at_support", false) ? "; peak at a support (singular: grows with refinement)" : "",
                      json_get_bool(st2, "peak_near_reentrant_corner", false) ? "; peak at a sharp inside corner (singular)" : "");
            if (json_len(json_get(lv, "invalid_reasons"))) {
                sb_puts(&b, "  - not valid: ");
                for (size_t r = 0; r < json_len(json_get(lv, "invalid_reasons")); r++)
                    sb_printf(&b, "%s%s", r ? "; " : "", json_str(json_at(json_get(lv, "invalid_reasons"), r)));
                sb_puts(&b, "\n");
            }
        }
        const JsonValue *sens = json_get(d, "sensitivity");
        if (json_len(sens)) {
            sb_puts(&b, "\n| Sensitivity | Parameter | h (mm) | Load-region displacement (mm) | Stiffness (N/mm) | Valid |\n|---|---|---|---|---|---|\n");
            for (size_t k = 0; k < json_len(sens); k++) {
                const JsonValue *s = json_at(sens, k);
                sb_printf(&b, "| %s | %s | ", json_get_str(s, "kind", ""), json_get_str(s, "parameter", ""));
                num(&b, json_get_num(s, "element_size_mm", NAN), "%.4g");
                sb_puts(&b, " | ");
                num(&b, json_get_num(json_get(s, "quantities"), "load_region_displacement_mm", NAN), "%.6g");
                sb_puts(&b, " | ");
                num(&b, json_get_num(json_get(s, "quantities"), "stiffness_n_per_mm", NAN), "%.6g");
                sb_printf(&b, " | %s |\n", json_get_bool(s, "valid", false) ? "yes" : "no");
            }
        }
        sb_puts(&b, "\n");
    }

    sb_puts(&b, "## Numerical evidence\n\n| Check | Applies to | Criterion | Observed | Outcome |\n|---|---|---|---|---|\n");
    const JsonValue *ne = json_get(ev, "numerical_evidence");
    for (size_t i = 0; i < json_len(ne); i++) {
        const JsonValue *r = json_at(ne, i);
        sb_puts(&b, "| ");
        cell(&b, json_get_str(r, "check", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(r, "applies_to", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(r, "criterion", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(r, "observed", ""));
        sb_printf(&b, " | %s |\n", strcmp(json_get_str(r, "outcome", ""), "pass") ? "**fail**" : "pass");
    }
    sb_puts(&b, "\nA small solver residual is not a discretisation-error estimate: the discretisation rows come from the refinement study.\n\n");

    sb_puts(&b, "## Interpretation\n\n");
    bullets(&b, json_get(ev, "interpretation"));
    sb_puts(&b, "\n## Important uncertainties\n\n");
    bullets(&b, json_get(ev, "important_uncertainties"));

    sb_puts(&b, "\n## Assumptions\n\n| Subject | Source | Assumption | Effect |\n|---|---|---|---|\n");
    const JsonValue *as = json_get(ev, "assumptions");
    for (size_t i = 0; i < json_len(as); i++) {
        const JsonValue *a = json_at(as, i);
        sb_puts(&b, "| ");
        cell(&b, json_get_str(a, "subject", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(a, "source", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(a, "text", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(a, "effect", ""));
        sb_puts(&b, " |\n");
    }
    const JsonValue *acc = json_get(ev, "accepted_questions");
    if (json_len(acc)) {
        sb_puts(&b, "\nQuestions the user answered by accepting an assumption:\n\n");
        for (size_t i = 0; i < json_len(acc); i++)
            sb_printf(&b, "- `%s`: %s Reason given: %s\n", json_get_str(json_at(acc, i), "id", ""), json_get_str(json_at(acc, i), "question", ""),
                      json_get_str(json_at(acc, i), "reason", ""));
    }
    sb_puts(&b, "\n## Not evaluated\n\n| Item | Reason |\n|---|---|\n");
    const JsonValue *nev = json_get(ev, "not_evaluated");
    for (size_t i = 0; i < json_len(nev); i++) {
        sb_puts(&b, "| ");
        cell(&b, json_get_str(json_at(nev, i), "item", ""));
        sb_puts(&b, " | ");
        cell(&b, json_get_str(json_at(nev, i), "reason", ""));
        sb_puts(&b, " |\n");
    }
    const JsonValue *uns = json_get(ev, "unsupported_requests");
    if (json_len(uns)) {
        sb_puts(&b, "\n## Requests this workflow does not answer\n\n");
        for (size_t i = 0; i < json_len(uns); i++) sb_printf(&b, "- %s\n", json_get_str(json_at(uns, i), "text", ""));
    }
    const JsonValue *fl = json_get(ev, "failures");
    if (json_len(fl)) {
        sb_puts(&b, "\n## Failures\n\n| Stage | Design | h (mm) | Class | Code | Message | Recovery |\n|---|---|---|---|---|---|---|\n");
        for (size_t i = 0; i < json_len(fl); i++) {
            const JsonValue *f = json_at(fl, i);
            sb_printf(&b, "| %s | %s | ", json_get_str(f, "stage", ""), json_get_str(f, "design", "all"));
            num(&b, json_get_num(f, "element_size_mm", NAN), "%.4g");
            sb_printf(&b, " | %s | %s | ", json_get_str(f, "failure_class", ""), json_get_str(f, "code", ""));
            cell(&b, json_get_str(f, "message", ""));
            sb_puts(&b, " | ");
            cell(&b, json_get_str(f, "recovery", ""));
            sb_puts(&b, " |\n");
        }
    }

    const JsonValue *rp = json_get(ev, "reproduction"), *sw = json_get(rp, "software"), *pl = json_get(rp, "platform");
    sb_puts(&b, "\n## Reproduction\n\n");
    sb_printf(&b, "- Resolved study: `%s` (SHA-256 of its content `%s`); original request `%s`; every executed operation in `%s`.\n",
              json_get_str(rp, "study_file", ""), json_get_str(rp, "study_hash", ""), json_get_str(rp, "request_file", ""), json_get_str(rp, "operations_log", ""));
    for (size_t i = 0; i < json_len(json_get(rp, "geometry")); i++) {
        const JsonValue *g = json_at(json_get(rp, "geometry"), i);
        sb_printf(&b, "- Geometry of design %s: stored copy `%s`, SHA-256 `%s` (original `%s`).\n", json_get_str(g, "design", ""),
                  json_get_str(g, "stored_copy", ""), json_get_str(g, "sha256", ""), json_get_str(g, "original_file", ""));
    }
    sb_printf(&b, "- Software: %s %s, operation contract %s, compiler %s; %s.\n", json_get_str(sw, "name", ""), json_get_str(sw, "version", ""),
              json_get_str(sw, "contract_version", ""), json_get_str(sw, "compiler", ""), json_get_str(sw, "floating_point", ""));
    if (pl)
        sb_printf(&b, "- Platform: %s %s, %s, %lld CPUs.\n", json_get_str(pl, "system", ""), json_get_str(pl, "release", ""), json_get_str(pl, "machine", ""),
                  json_get_int(pl, "online_cpus", 0));
    for (size_t i = 0; i < json_len(json_get(rp, "commands")); i++) sb_printf(&b, "- %s\n", json_str(json_at(json_get(rp, "commands"), i)));
    sb_printf(&b, "- %s\n", json_get_str(rp, "reproducibility", ""));

    sb_puts(&b, "\n## Traceability\n\n| Reported item | Location in evidence.json |\n|---|---|\n");
    sb_puts(&b, "| Outcome statement and code | `/comparison/statement`, `/comparison/outcome` |\n");
    sb_puts(&b, "| Outcome table (design i) | `/comparison/values/i/*` |\n");
    sb_puts(&b, "| Ranking robustness | `/comparison/ranking_robustness` |\n");
    sb_puts(&b, "| Modeled conditions | `/modeled_conditions/*` |\n");
    sb_puts(&b, "| Results by mesh (design i, level k) | `/designs/i/levels/k/{mesh,quantities,checks,stress,cost,run}` |\n");
    sb_puts(&b, "| Refinement readings | `/designs/i/refinement/*` |\n");
    sb_puts(&b, "| Sensitivity runs | `/designs/i/sensitivity/*` |\n");
    sb_puts(&b, "| Numerical evidence | `/numerical_evidence/*` |\n");
    sb_puts(&b, "| Assumptions, questions, exclusions, failures | `/assumptions`, `/unresolved_inputs`, `/accepted_questions`, `/not_evaluated`, `/failures` |\n");
    sb_puts(&b, "| Each analysis | `designs/<design>/runs/<job_id>/spec.json`, `summary.json`, `results.nvr` (hash in `/designs/i/levels/k/run/results_sha256`) |\n");
    return sb_steal(&b, NULL);
}
