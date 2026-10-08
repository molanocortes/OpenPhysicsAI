/* ops_project.c - capabilities and project lifecycle operations */
#include "ops_internal.h"
#include "../core/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mech/mech_ops.h" /* mech-integration */

static void op_capabilities_get(Engine *e, JsonValue *params, OpResult *out) {
    JsonValue *v = json_object();
    JsonValue *srv = json_set_object(v, "server");
    json_set_string(srv, "name", "navier-am");
    json_set_string(srv, "version", NAVIER_AM_VERSION);
    json_set_string(srv, "contract_version", ops_contract_version());
    json_set_string(v, "implementation_stage",
                    "projects; STL import with diagnostics, units and build placement; surface patches and named selections (also picked "
                    "on rendered views); material library and project materials; voxel hex8 volume meshes; static linear-elastic structural "
                    "analysis as background jobs with result queries, engineering quantities, probes, images and VTU/CSV export; comparison "
                    "studies of two to four designs under equivalent mounting and load with a refinement study, sensitivities and an "
                    "Engineering Evidence Record; transient thermal, one-way thermomechanical and conjugate heat transfer analyses; FDM/FFF layer-deposition "
                    "thermal stress and release (mech_print_run); calibrated inherent-strain LPBF builds (lpbf_build_run).");

    JsonValue *ops = json_set_array(v, "operations");
    for (int i = 0; i < ops_count(); i++) {
        const OpInfo *op = ops_at(i);
        JsonValue *o = json_object();
        json_set_string(o, "name", op->name);
        json_set_string(o, "title", op->title);
        json_set_string(o, "kind", op->mutating ? "mutation" : "query");
        json_push(ops, o);
    }

    JsonValue *conv = json_set_object(v, "conventions");
    json_set_string(conv, "quantities", "a number is interpreted in the unit named by the parameter's x-unit (lengths mm, "
                                        "angles deg, temperatures degC); a string may carry its own unit, e.g. \"0.25 in\"");
    json_set_string(conv, "internal_units", "SI (m, kg, s, K, N, Pa, J, W)");
    json_set_string(conv, "build_frame", "right-handed; +Z is the build direction; origin at the centre of the build plate's top surface; results in mm");
    json_set_string(conv, "file_frame", "coordinates as stored in the STL, in the body's declared unit; up_axis says which file axis is the build direction");
    json_set_string(conv, "viewer_frame", "the fluid viewer uses a Y-up world: build (x, y, z) maps to viewer (x, z, -y)");
    json_set_string(conv, "revisions", "every successful change increments the project revision; pass expected_revision to guard against conflicting edits");
    json_set_string(conv, "provenance", "inputs carry a source: user, inferred, default or calibrated; non-user values are listed as assumptions");

    JsonValue *inp = json_set_object(v, "inputs");
    JsonValue *geo = json_set_array(inp, "geometry_formats");
    json_push(geo, json_string("STL (binary)"));
    json_push(geo, json_string("STL (ASCII)"));
    JsonValue *lim = json_set_object(v, "limits");
    json_set_int(lim, "max_stl_bytes", (long long)e->cfg.max_stl_bytes);
    json_set_int(lim, "max_triangles", e->cfg.max_triangles);
    json_set_int(lim, "max_elements", (long long)e->cfg.max_elements);

    mech_capabilities_json(v); /* mech-integration */

    JsonValue *analyses = json_set_array(v, "analyses");
    JsonValue *an = json_object();
    json_set_string(an, "name", "static_structural");
    json_set_string(an, "status", "available");
    json_set_string(an, "physics", "small-strain linear elasticity; isotropic materials evaluated at a reference temperature");
    json_set_string(an, "elements", "8-node hexahedra on layer-aligned voxel meshes: Wilson-Taylor incompatible modes (default) or full integration");
    json_set_string(an, "solvers", "sparse Cholesky with nested dissection (default up to 250000 equations) or block-Jacobi conjugate gradients");
    static const char *const SUPPORTS[] = {"fixed", "displacement", "frictionless_support (flat faces normal to x, y or z)", NULL};
    static const char *const LOADS[] = {"force (total, distributed by area)", "pressure", "traction", "gravity", NULL};
    static const char *const OUTPUTS[] = {"displacement", "stress and strain tensors at Gauss points and averaged at nodes", "principal and von Mises stress",
                                          "reaction forces and moments per support",
                                          "results_quantities: area-weighted mean displacement of a selection along a direction; work-conjugate displacement and "
                                          "stiffness |F|^2/W of a force or traction load; mass of the mesh and of the STL geometry",
                                          NULL};
    static const char *const CHECKS[] = {"rigid-body modes and conflicting supports before solving", "force and moment balance of loads, body forces and reactions",
                                         "strain energy against external work", "flags for peaks at supports and sharp inside corners",
                                         "line of action of each load on the staircase mesh against the specified load",
                                         "small-deformation indicators (displacement over model size, infinitesimal rotation) with stated criteria", NULL};
    static const char *const EXCLUDED[] = {"contact", "plasticity and creep", "geometric nonlinearity", "thermal strains", "residual stresses from the build",
                                           "anisotropic printed material", NULL};
    const char *const *lists[5] = {SUPPORTS, LOADS, OUTPUTS, CHECKS, EXCLUDED};
    static const char *const KEYS[5] = {"supports", "loads", "outputs", "checks", "not_included"};
    for (int l = 0; l < 5; l++) {
        JsonValue *arr = json_set_array(an, KEYS[l]);
        for (int i = 0; lists[l][i]; i++) json_push(arr, json_string(lists[l][i]));
    }
    json_push(analyses, an);
    /* transient thermal */
    JsonValue *th = json_object();
    json_set_string(th, "name", "transient_thermal");
    json_set_string(th, "status", "available");
    json_set_string(th, "physics",
                    "enthalpy form of the energy balance d(rho h)/dt = div(K grad T) + q; conduction with temperature-dependent or "
                    "anisotropic conductivity, latent heat of melting through an equilibrium liquid fraction");
    json_set_string(th, "elements", "8-node hexahedra on layer-aligned voxel meshes; conductivity evaluated at the Gauss points (or per element on request)");
    json_set_string(th, "solvers",
                    "theta method (backward Euler or Crank-Nicolson) with fixed steps or adaptive steps (step-doubling error estimate in "
                    "temperature and enthalpy, PI step controller, rejected steps rolled back); Aitken-relaxed Picard iteration on the "
                    "nonlinearities; Jacobi-preconditioned conjugate gradients on the temperature correction of each iteration");
    JsonValue *tsx = json_set_object(th, "time_stepping");
    json_set_string(tsx, "fixed", "available: steps of time_step, landing on stored times and schedule changes");
    json_set_string(tsx, "adaptive",
                    "available: time_stepping \"adaptive\" with temporal_relative_tolerance, temporal_temperature_tolerance, optional "
                    "temporal_enthalpy_tolerance, min_time_step, max_time_step and controller limits. The estimate is per step: global error "
                    "accumulates and is not bounded by the tolerance");
    json_set_string(tsx, "events", "output_times or output_interval, and per-condition schedules (boundary_apply schedule) are landed on exactly");
    json_set_string(tsx, "failure_classes", "MIN_STEP, TOO_MANY_REJECTIONS, NONLINEAR_FAILURE, LINEAR_FAILURE, INVALID_STATE, STEP_LIMIT, INVALID_PARAMS");
    JsonValue *icx = json_set_object(th, "thermal_interfaces");
    json_set_string(icx, "status", "available: interface_define, interface_list, interface_preview, interface_remove, results_interface");
    json_set_string(icx, "models", "perfect, conductance (W/(m^2 K)), thin_layer (thickness and conductivity, no heat capacity), insulated");
    json_set_string(icx, "topology",
                    "coincident, face-matching contact of exactly two bodies in the voxel mesh; gaps, edge-only contact, overlapping geometry "
                    "and junctions of three bodies are refused with a diagnostic");
    json_set_string(icx, "results",
                    "both sides' temperatures, the jump and the heat rate at stored times; cumulative interface heat; each body's own energy "
                    "balance as an independent check");
    json_set_string(icx, "not_included", "gap-dependent or pressure-dependent conductance; non-matching or curved interfaces beyond the voxel staircase");
    JsonValue *rsx = json_set_object(th, "restart");
    json_set_string(rsx, "checkpoints",
                    "available: periodic (checkpoint_interval in physical time, checkpoint_wall_interval in wall time), on request (job_checkpoint), "
                    "on pause, cancellation and failure; atomic, SHA-256 verified, previous checkpoint kept");
    json_set_string(rsx, "resume",
                    "available: job_pause and job_resume, also in a new process after the old one was killed (job_status reports 'interrupted'). "
                    "Refused when the mesh, materials, conditions or physics settings changed. A resumed run reproduces the uninterrupted run");
    json_set_string(rsx, "partial_results", "a paused, cancelled or failed run publishes no results; job_resume can finish it");
    JsonValue *cpx = json_set_object(th, "coupling");
    json_set_string(cpx, "orchestration",
                    "available: transient_thermal and thermomechanical runs go through one shared lifecycle (trials from the accepted state, "
                    "field transfer, convergence, all participants committed together or all rolled back); the run summary's 'coupling' "
                    "section lists the participants, the couplings and when each participant is evaluated");
    json_set_string(cpx, "thermomechanical",
                    "one-way: the structure is a quasi-static participant evaluated from the temperature at the initial time and at every "
                    "stored time, which is exact for the linear elastic model because it has no history; nothing feeds back to the thermal model");
    json_set_string(cpx, "strong_coupling",
                    "available through conjugate_heat_transfer (coupling.scheme partitioned): fluid and solids as a feedback cycle iterated "
                    "to convergence each step with Aitken relaxation and a bounded number of iterations, coupling error reported separately "
                    "from temporal error, rejection and a smaller step when the cycle does not converge; verified against a monolithic model "
                    "in the core and through MCP");
    static const char *const TH_COND[] = {"time schedules on thermal conditions: piecewise-constant factors, landed on exactly",
                                          "temperature (prescribed)",
                                          "heat_flux (W/m^2 into the body)",
                                          "convection (coefficient and ambient)",
                                          "radiation (grey body to one ambient temperature)",
                                          "heat_source (volumetric, W/m^3)",
                                          "moving Gaussian source with an absorption depth (core API)",
                                          "thermal interfaces between touching bodies (interface_define): perfect, contact conductance, thin layer or "
                                          "insulated; the thermal model is split, the bodies stay bonded mechanically",
                                          NULL};
    static const char *const TH_OUT[] = {"temperature history at the stored times",
                                         "per-step energy balance: stored, sources, boundary faces, prescribed-temperature reactions",
                                         "the enthalpy change integrated independently of the capacity matrix, and their mismatch",
                                         "molten volume when a latent heat is active",
                                         "temperature range per stored time",
                                         NULL};
    static const char *const TH_CHK[] = {"the discrete energy balance of every step and its worst value over the run",
                                         "stored energy against the independently integrated enthalpy change",
                                         "steady problems without a temperature sink are rejected instead of solved",
                                         "conductivity axes must be orthonormal and every property table positive",
                                         NULL};
    static const char *const TH_NOT[] = {"surface-to-surface (enclosure) radiation: view factors and radiosity are not implemented",
                                         "advection inside solids: fluid energy transport exists only as the flow domain of a "
                                         "conjugate_heat_transfer analysis",
                                         "a global temporal error bound: adaptive stepping controls the error of each step, not of the whole run",
                                         "resuming with changed inputs: a checkpoint continues only the identical problem",
                                         "hysteresis, undercooling and non-equilibrium solidification",
                                         "boiling, participating media, compressible gas dynamics",
                                         NULL};
    const char *const *tl[4] = {TH_COND, TH_OUT, TH_CHK, TH_NOT};
    static const char *const TH_KEYS[4] = {"conditions", "outputs", "checks", "not_included"};
    for (int l = 0; l < 4; l++) {
        JsonValue *arr = json_set_array(th, TH_KEYS[l]);
        for (int i = 0; tl[l][i]; i++) json_push(arr, json_string(tl[l][i]));
    }
    json_push(analyses, th);

    JsonValue *tm = json_object();
    json_set_string(tm, "name", "thermomechanical");
    json_set_string(tm, "status", "available");
    json_set_string(tm, "physics",
                    "one-way (sequential) coupling: the temperature history drives thermal strain and temperature-dependent stiffness; "
                    "the structure does not feed back into the temperature field");
    json_set_string(tm, "elements", "the thermal mesh solved structurally at every stored time");
    static const char *const TM_NOT[] = {"two-way coupling: deformation does not change the thermal problem",
                                         "plasticity and creep, so this is not a residual-stress prediction: thermal stresses vanish when the part returns "
                                         "to a uniform temperature",
                                         "contact, so bodies interact only through shared nodes",
                                         "mechanical dissipation as a heat source",
                                         NULL};
    JsonValue *tmn = json_set_array(tm, "not_included");
    for (int i = 0; TM_NOT[i]; i++) json_push(tmn, json_string(TM_NOT[i]));
    json_push(analyses, tm);

    JsonValue *ch = json_object();
    json_set_string(ch, "name", "conjugate_heat_transfer");
    json_set_string(ch, "status", "available");
    json_set_string(ch, "physics",
                    "a steady laminar incompressible flow of a constant-property fluid through a fluid body, computed once by the lattice "
                    "Boltzmann solver without buoyancy, carries the fluid's energy (convective form, SUPG); the fluid exchanges heat with the solid "
                    "bodies it touches through their shared nodes, with no convection coefficient; the solids optionally expand thermally");
    json_set_string(ch, "coupling",
                    "flow -> fluid energy one-way (the flow does not depend on temperature); fluid <-> solids either partitioned (iterated to "
                    "convergence every step: the fluid holds the interface at the solids' temperature, the solids receive the fluid's interface "
                    "heat, Aitken relaxation, coupling error reported separately) or monolithic; solids -> structure one-way at stored times");
    json_set_string(ch, "inputs",
                    "analysis_run / setup_validate with analysis conjugate_heat_transfer, flow {fluid_body, inlet_velocity, inlet_temperature, "
                    "walls}, coupling {scheme, relative_tolerance, max_iterations, relaxation}, structural_response; a fluid material "
                    "(family fluid with dynamic_viscosity_pa_s, e.g. air_demo); cubic mesh cells");
    json_set_string(ch, "flow_limits",
                    "flow along +x through the fluid body's bounding box (inlet at x-min, outlet at x-max, sides no_slip, slip or periodic); "
                    "laminar; a lattice relaxation time below 0.52 or a density variation above 3 % is refused, which bounds the cell Reynolds "
                    "number and the channel length a mesh can carry");
    json_set_string(ch, "verification",
                    "nonsymmetric solver, SUPG advection (exponential boundary layer, moving Gaussian, uniform-state preservation, energy "
                    "closure), channel Nusselt number 5.385 to 0.01 %, lattice Poiseuille profile to 0.06 %, partitioned against monolithic "
                    "conjugate transfer to 1e-10 K: Thermal Sim/verification/stageE-evidence.md");
    static const char *const CH_NOT[] = {"turbulence, buoyancy (natural convection), compressible gas flow",
                                         "temperature-dependent fluid properties, and any effect of temperature on the flow",
                                         "a flow start-up transient: the steady flow acts from t = 0",
                                         "moving or deforming boundaries, and flow directions other than +x",
                                         "non-matching fluid and solid meshes: both are cells of the same voxel mesh",
                                         "radiation between surfaces across the fluid",
                                         NULL};
    JsonValue *chn = json_set_array(ch, "not_included");
    for (int i = 0; CH_NOT[i]; i++) json_push(chn, json_string(CH_NOT[i]));
    json_push(analyses, ch);

    /* comparison studies */
    JsonValue *cs = json_object();
    json_set_string(cs, "name", "comparison_study");
    json_set_string(cs, "status", "available");
    json_set_string(cs, "operations", "study_check, study_run, study_evidence, study_replay (CLI: navier-ctl study ...)");
    json_set_string(cs, "question",
                    "which of two to four designs has lower displacement (or higher stiffness) at a defined load region, under the same mounting "
                    "and load, how that depends on material stiffness, Poisson's ratio and the mounting idealisation, whether the ranking holds on every "
                    "tested mesh, whether each design meets the convergence criterion, and, when its conditions hold, whether the difference exceeds "
                    "the estimated discretisation errors");
    json_set_string(cs, "inputs",
                    "geometry with its unit, mounting and load regions in each design's frame, material with the source of its values, manufacturing "
                    "process, mounting idealisation (fixed or frictionless), payload mass with g or a force, element sizes, sensitivities, limits; "
                    "missing consequential inputs come back as questions, defaults as recorded assumptions");
    json_set_string(cs, "outputs",
                    "outcome (resolved, ranking_consistent_on_tested_meshes, too_small_to_distinguish, not_resolved, more_refinement_required, "
                    "cannot_establish) with a conditional statement; per design and mesh: load-region displacement, stiffness, mass, reactions, stress "
                    "peaks as supporting information; the ranking on each tested mesh; per design the convergence criterion (met or not) and a "
                    "discretisation-error estimate with the conditions checked for it (or why none is offered); ranking robustness; evidence.json "
                    "and report.md; replay comparison");
    json_set_string(cs, "not_included",
                    "strength margins or safety statements, fatigue, fasteners and contact, buckling and geometric nonlinearity, printed-material "
                    "anisotropy, dynamic loads; no validation against measurements");
    json_push(analyses, cs);
    /* Printing is implemented; expose its actual scope rather than the old planned placeholders. */
    JsonValue *fff = json_object();
    json_set_string(fff, "name", "fff_print");
    json_set_string(fff, "status", "available");
    json_set_string(fff, "operation", "mech_print_run");
    json_set_string(fff, "physics", "layer deposition, temperature-dependent conduction and cooling, incremental thermal stress, bed release and support removal");
    json_set_string(fff, "not_included", "within-layer toolpath, raster anisotropy, interlayer bond strength, crystallisation, creep below relaxation temperature, adhesion failure; inspect summary.scope and material provenance before using predictions");
    json_push(analyses, fff);
    JsonValue *lpbf = json_object();
    json_set_string(lpbf, "name", "lpbf_build");
    json_set_string(lpbf, "status", "available");
    json_set_string(lpbf, "operation", "lpbf_build_run");
    json_set_string(lpbf, "physics", "layer activation with declared inherent strain, support and plate restraint, elastic or J2 mechanical response, release and cutting");
    json_set_string(lpbf, "not_included", "laser-resolved heat input, melt-pool flow, keyholing, microstructure; strain calibration is process-specific and is not a universal material constant");
    json_push(analyses, lpbf);

    JsonValue *lims = json_set_array(v, "model_limitations");
    static const char *LIMITS[] = {
        "STL files contain triangles only: no units, face names, materials, tolerances or process data. These must be supplied or recorded as assumptions.",
        "No automatic hole filling: open or non-manifold STL surfaces are reported, not silently closed.",
        "Self-intersection checks skip faces that share a vertex; folded neighbouring faces are not detected.",
        "Wall thickness is estimated by inward ray casts from sampled faces; it is a diagnostic, not a measurement.",
        "Volume meshes are voxel-derived hexahedra: boundaries are staircases, so stresses at inclined or curved surfaces and at re-entrant steps depend on the element size. mesh_generate reports volume, area and boundary-distance errors.",
        "The static structural analysis is small-strain linear elastic and isotropic. Separate mechanical and printing operations have their own constitutive scope; inspect the selected analysis rather than assuming all operations include plasticity, contact or anisotropy.",
        "Material records carry demonstration, published or measured status and provenance; demonstration values are not calibrated data and are indicative only.",
        "Ideal supports and sharp inside corners are stress singularities: peak stresses there grow with mesh refinement and are flagged instead of being reported as design values.",
        "FFF uses a homogenised layer thermal-stress model; LPBF uses declared inherent strain. Neither resolves all printing process physics. Their summaries state omissions and calibration provenance.",
        "Thermal radiation is grey-body exchange with a single ambient temperature. There are no view factors, no occlusion and no enclosure radiosity, so surfaces do not radiate to each other.",
        "Fluid energy transport exists only in a conjugate_heat_transfer analysis, for a steady laminar constant-property flow computed once without buoyancy; elsewhere convection must be supplied as a heat-transfer coefficient.",
        "Adaptive transient stepping estimates and controls the error of each step (step doubling, temperature and enthalpy), not the global error: errors of successive steps accumulate, so establish accuracy by repeating a run with tighter tolerances. Fixed steps carry no error estimate at all.",
        "Latent heat uses an equilibrium liquid fraction that is linear between solidus and liquidus. Undercooling, hysteresis and solidification kinetics are not modelled, and the mushy interval is a model parameter that affects the answer.",
    };
    for (size_t i = 0; i < sizeof LIMITS / sizeof LIMITS[0]; i++) json_push(lims, json_string(LIMITS[i]));
    json_set(v, "access", engine_roots_json(e));
    op_succeed(out, v);
}

static JsonValue *next_steps(const char *const *steps) {
    JsonValue *a = json_array();
    for (int i = 0; steps[i]; i++) json_push(a, json_string(steps[i]));
    return a;
}

static void op_project_create(Engine *e, JsonValue *p, OpResult *out) {
    const char *name = json_get_str(p, "name", "");
    if (!body_name_valid(name)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use 1-63 letters, digits, '_', '-' or '.'", "invalid project name '%s'", name);
        return;
    }
    const char *dir_in = json_get_str(p, "directory", NULL);
    char dir[NV_PATH_MAX], err[1024];
    if (!engine_resolve_write_path(e, e->cfg.workspace, dir_in ? dir_in : name, true, false, dir, sizeof dir, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
        return;
    }
    char pj[NV_PATH_MAX];
    path_join(pj, sizeof pj, dir, "project.json");
    if (path_exists(pj) && !json_get_bool(p, "overwrite", false)) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "open it with project_open, choose another name or directory, or pass overwrite: true",
                "a project already exists in %s", dir);
        op_fail_detail(out, "directory", json_string(dir));
        return;
    }
    if (!engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, NULL, "%s", err);
        return;
    }
    Project *proj = project_new(name, dir, json_get_str(p, "description", ""));
    if (!proj) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    engine_set_project(e, proj);
    engine_touch(e, "project_create", "created project %s in %s", name, dir);
    if (!project_save_file(proj, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, "the project is active in memory but not saved", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set(v, "project", project_summary_json(proj));
    json_set_string(v, "project_file", pj);
    static const char *const STEPS[] = {"geometry_import: load the part STL with an explicit unit", NULL};
    json_set(v, "next_steps", next_steps(STEPS));
    op_succeed(out, v);
}

static void op_project_open(Engine *e, JsonValue *p, OpResult *out) {
    const char *path = json_get_str(p, "path", "");
    char real[NV_PATH_MAX], err[1024];
    if (!path_real(path, real, sizeof real)) {
        op_fail(out, NV_ERR_NOT_FOUND, NULL, "no such project: %s", path);
        return;
    }
    if (!engine_resolve_read_path(e, path, real, sizeof real, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
        return;
    }
    char pj[NV_PATH_MAX];
    if (path_is_dir(real)) path_join(pj, sizeof pj, real, "project.json");
    else snprintf(pj, sizeof pj, "%s", real);
    if (!path_is_file(pj)) {
        op_fail(out, NV_ERR_NOT_FOUND, "pass a project directory or its project.json", "no project.json at %s", pj);
        return;
    }
    Project *proj = project_load_file(pj, e->cfg.max_stl_bytes, e->cfg.max_triangles, err, sizeof err);
    if (!proj) {
        op_fail(out, strstr(err, "SHA-256") ? NV_ERR_STALE_REFERENCE : NV_ERR_IO, NULL, "cannot open %s: %s", pj, err);
        return;
    }
    char tmp[NV_PATH_MAX];
    bool writable = engine_resolve_write_path(e, NULL, proj->dir, true, false, tmp, sizeof tmp, NULL, 0);
    engine_set_project(e, proj);
    engine_touch(e, "project_open", "opened project %s from %s", proj->name, proj->dir);
    proj->saved_revision = proj->revision;
    JsonValue *v = json_object();
    json_set(v, "project", project_summary_json(proj));
    json_set_bool(v, "writable", writable);
    JsonValue *bodies = json_set_array(v, "bodies");
    for (int i = 0; i < proj->nbodies; i++) json_push(bodies, body_json(proj->bodies[i], false));
    json_set(v, "assumptions", assumptions_json(proj));
    op_succeed(out, v);
}

static void op_project_save(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    char tmp[NV_PATH_MAX], err[1024];
    if (!engine_resolve_write_path(e, NULL, e->proj->dir, true, false, tmp, sizeof tmp, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
        return;
    }
    if (!project_save_file(e->proj, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, NULL, "%s", err);
        return;
    }
    char pj[NV_PATH_MAX], hex[65];
    path_join(pj, sizeof pj, e->proj->dir, "project.json");
    JsonValue *v = json_object();
    json_set_string(v, "project_file", pj);
    json_set_int(v, "saved_revision", (long long)e->proj->saved_revision);
    if (sha256_file(pj, hex, NULL)) json_set_string(v, "sha256", hex);
    op_succeed(out, v);
}

static void op_project_inspect(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const JsonValue *sections = json_get(p, "sections");
    JsonValue *v = json_object();
    for (size_t i = 0; i < json_len(sections); i++) {
        const char *s = json_str(json_at(sections, i));
        if (!strcmp(s, "summary")) {
            json_set(v, "summary", project_summary_json(e->proj));
        } else if (!strcmp(s, "bodies")) {
            JsonValue *bodies = json_set_array(v, "bodies");
            for (int b = 0; b < e->proj->nbodies; b++) json_push(bodies, body_json(e->proj->bodies[b], true));
        } else if (!strcmp(s, "assumptions")) {
            json_set(v, "assumptions", assumptions_json(e->proj));
        } else if (!strcmp(s, "journal")) {
            json_set(v, "journal", engine_journal_json(e, 0, 64));
        }
    }
    op_succeed(out, v);
}

const OpBinding OPS_PROJECT_BINDINGS[] = {
    {"capabilities_get", op_capabilities_get},
    {"project_create", op_project_create},
    {"project_open", op_project_open},
    {"project_save", op_project_save},
    {"project_inspect", op_project_inspect},
    {NULL, NULL},
};
