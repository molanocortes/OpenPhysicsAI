/* fembridge.h - the interface's connection to the NAVIER-AM engine
 *
 * The lattice-Boltzmann tunnel and the finite-element side of this application share one window, one camera, one set
 * of colour maps and one terminal. This module is the single place where they meet: it mirrors the engine's analysis
 * state for the panel to draw, runs interface actions as the same typed operations the control socket and the MCP
 * server call (no second code path and no hidden state), and turns a finished analysis into a triangle surface that
 * the 3D renderer draws next to the flow.
 *
 * Frames: the engine works in the build frame (metres, +Z up, origin at the centre of the plate). The viewer works in
 * lattice units with +Y up and +X along the flow. The conversion is explicit here and nowhere else:
 *     world = centre_world + scale * (x_b, z_b, -y_b)   relative to the part's bounding-box centre
 * which keeps the mapping right-handed. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math3d.h"
#include "render.h" /* ResultPart: the pieces the renderer draws */
#include "vis.h"    /* IsoMesh: the vertex layout the renderer already accepts */

enum { FEM_VON_MISES = 0, FEM_DISPLACEMENT, FEM_TEMPERATURE, FEM_FIELD_COUNT };

typedef struct FemState {
    bool have_engine;
    bool have_project;
    char project[64];
    char project_id[33];
    char project_dir[256];
    uint64_t revision;

    /* geometry */
    int nbodies;
    char body[64];
    uint32_t triangles;
    bool watertight;
    double size_mm[3];    /* placed bounding box of the first part */
    /* what the geometry check found, for the PART step's verdict */
    double volume_mm3, area_mm2, min_thickness_mm;
    int components, open_edges, nonmanifold_edges, nested_shells, patches, degenerate_removed;
    bool closed_solid;

    /* mesh */
    bool meshed, mesh_current;
    int nelems, nnodes;
    double element_size_mm;
    double mesh_volume_mm3, mesh_volume_error_pct;
    /* a conforming tetrahedral mesh (method 1): its order and quality */
    int mesh_method, tet_order, tet_regions;
    double tet_min_dihedral, tet_max_dihedral, tet_worst_aspect;
    int tet_unresolved_thin;      /* thin walls the finest cell could not resolve: the mesh is perforated there */
    double tet_finest_mm;
    /* how sure the inside test was about this mesh, and what a repair changed */
    long long uncertain_cells, abstained_rays;
    bool repaired;
    int repair_facets_dropped, repair_shells_dropped, repair_holes_filled, repair_facets_added;

    /* setup */
    int nselections, nbcs;
    char material[64];
    char material_name[64];
    char material_status[24];  /* demonstration, user_supplied, calibrated, measured */
    char material_source[640]; /* the record's own provenance sentence: where the numbers come from */
    double yield_mpa, density_kg_m3, youngs_gpa, poisson;
    /* the result drawn on the part's own surface rather than on the voxel boundary */
    bool on_surface;              /* the drawn surface is the STL, interpolated from the mesh (or a tetrahedral mesh's own faces) */
    bool visibility_mesh;         /* hidden elements require the actual FE boundary, including when SURFACE is requested */
    int tet_mesh;                 /* the result is on a tetrahedral mesh: 0, SOLID_ELEM_TET4 or SOLID_ELEM_TET10 */
    double snap_max_mm;           /* the largest distance a vertex had to move to land in an element */
    double snap_mean_mm;
    int snapped_vertices, unmapped_vertices;
    double map_ms;                /* what the mapping cost, once per geometry and mesh */
    double pick_ms;               /* what the last ray through the geometry cost */
    int nthermal_bcs;

    /* the job the interface is following */
    char job_id[64];
    char job_kind[32];
    char job_state[16];
    char job_stage[64];
    double job_progress;
    bool job_active;
    char job_error[200];
    char pending_job[64], pending_kind[32];
    bool job_adopted;     /* the job was started by another client, not by this interface */

    /* the result being displayed */
    bool have_result;
    char result_job[64];
    char result_kind[32];
    char result_body[64]; /* the body the shown result was built on (its run's spec), "" when unknown */
    int nsteps;           /* stored times of a transient result, 1 for a static one */
    double time_s;        /* time of the displayed step */
    bool has_mechanical;  /* a transient result that also carries stresses */
    bool has_displacement;/* the displayed result carries a displacement field */
    bool has_temperature, has_stress, has_groups;
    double max_displacement_mm;
    double peak;          /* peak of the displayed field, in its display unit */
    double range_lo, range_hi;
    int surface_tris;
    bool showing_geometry; /* the part itself is on screen, no result yet */
    double rebuild_ms;    /* time the last surface rebuild took, for playback pacing */
} FemState;

void fem_init(void);      /* after app.engine exists */
void fem_shutdown(void);
void fem_poll(void);      /* once per frame; refreshes the mirror when the engine or a job moved */
const FemState *fem_state(void);

/* Runs a typed operation on the shared engine and reports it in the terminal, exactly as the 'am' command does.
 * params_fmt is a JSON object (printf formatting); NULL means no parameters. */
bool fem_op(const char *op, const char *params_fmt, ...) __attribute__((format(printf, 2, 3)));
/* the value of the last fem_op call (owned here, valid until the next one) */
const struct JsonValue *fem_last_value(void);
/* the project's boundary conditions, read through boundary_list and cached per revision (never NULL to free) */
const struct JsonValue *fem_conditions(void);
int fem_operation_count(void); /* operations in the shared registry */
/* the last refusal from the engine, so a step can show it instead of doing nothing visible; "" when the last
 * operation went through */
const char *fem_last_error(void);
/* the summary of the followed job (checks, peaks, solver, warnings), or NULL; owned here */
const struct JsonValue *fem_job_summary(void);
/* short reason the displayed peak is not a design value (stress singularity at a support or a sharp corner), or NULL */
const char *fem_result_caveat(void);
/* why the followed job's results cannot be drawn here (an analysis kind this window does not display), or NULL */
const char *fem_unsupported_reason(void);
/* file the analysed body was imported from, so the same part can be loaded into the tunnel */
bool fem_body_source_path(char *out, size_t cap);
/* creates a project named base, or base-2, base-3 ... when that folder of the workspace already holds a project,
 * so the NEW PROJECT button never fails on an old project; the name used is written to name_out */
bool fem_new_project(const char *base, char *name_out, size_t cap);
/* lists the projects of the workspace in the terminal (newest first) and returns how many there are */
int fem_list_projects(void);
/* opens <workspace>/<name> (a bare name) or a path */
bool fem_open_project(const char *name_or_path);

/* ---- result display ---- */
int fem_field(void);
void fem_set_field(int f);
const char *fem_field_name(int f);
const char *fem_field_label(int f);
const char *fem_field_unit(int f);
bool fem_field_available(int f);

double fem_deform_scale(void);
void fem_set_deform_scale(double s); /* < 0: auto (peak displacement ~ 8% of the part) */
bool fem_deform_auto(void);
double fem_deform_applied(void);     /* the factor actually used */

/* transient results: colour by the range over every stored time (so playback shows the change) or by this step's */
bool fem_range_all(void);
void fem_set_range_all(bool on);
bool fem_range_p99(void);      /* the colour bar is clipped to the 99th percentile of what is drawn */
void fem_set_range_p99(bool on);

/* playback over the stored times of a transient result; the colour range stays on "all stored times" while it runs */
bool fem_playing(void);
void fem_set_playing(bool on);
double fem_play_speed(void);      /* stored times per second */
void fem_set_play_speed(double per_second);

int fem_step(void);
void fem_set_step(int i);
int fem_step_count(void);
double fem_step_time(int i);

bool fem_visible(void);
void fem_set_visible(bool on);

/* Debug aid until print results carry element birth times: fake them from the element centroid along an axis
 * (0 x, 1 y, 2 z), spread over the stored times; -1 switches it off. Returns false when there is no result. */
bool fem_fake_growth(int axis);
int fem_growth_axis(void); /* -1 when off */

/* follow a job (usually the one just submitted); "" clears */
void fem_follow_job(const char *id);

/* ---- everything that is queued or running, whoever started it ------------------------------------------------
 * The engine runs one job at a time, so a job the window is not looking at still decides when the next one starts.
 * The list is refreshed about once a second (job_list takes no engine lock): running first, then the queue in the
 * order it will run. */
typedef struct {
    char id[64], kind[32], label[96], state[16], stage[64];
    double progress;
    bool stopping; /* a stop was asked for; a running build ends at its next layer */
} FemJobInfo;
int fem_active_jobs(const FemJobInfo **out);
/* what a person calls it: the label it was given, else its kind in words */
const char *fem_job_words(const FemJobInfo *j);
/* stop one job, or every queued and running job (the queue first, so nothing starts behind the one that ends).
 * Returns how many were asked to stop. */
bool fem_stop_job(const char *id);
int fem_stop_all_jobs(void);
/* true between the stop of the followed job and its end */
bool fem_job_stopping(void);
/* follow a job AND put its result on screen (any job of the open project, also from an earlier session) */
void fem_show_job(const char *id);
void fem_mark_dirty(void); /* rebuild the surface on the next poll */
void fem_set_supports_neutral(bool on); /* supports grey (Simple mode) or coloured by the field */

/* the analysed part in viewer world units; false when there is nothing placed */
bool fem_world_transform(mat4 *to_world, mat4 *to_build);
/* deformed, coloured boundary surface of the displayed result; NULL when there is none */
const IsoMesh *fem_surface(void);
/* changes whenever the surface is rebuilt or cleared, so the renderer uploads it exactly once */
uint64_t fem_surface_generation(void);
bool fem_surface_range(float *lo, float *hi);
/* world-space bounds of what is displayed (surface, else the geometry) */
bool fem_world_bounds(vec3 *lo, vec3 *hi);

/* element size the MESH step shows: the user's choice while the project has not moved on, else the project's own
 * size, else a suggestion sized for about 20 000 elements */
double fem_mesh_size_mm(void);
void fem_set_mesh_size_mm(double mm);
double fem_suggested_mesh_mm(void);
double fem_project_mesh_mm(void);
/* a file waiting for its length unit before it can be imported (drag and drop, or 'open' in the SOLID workspace) */
const char *fem_pending_import(void);
/* a body name not yet used in the project: 'bracket', then 'bracket-2', ... */
void fem_free_body_name(const char *wanted, char *out, size_t cap);
void fem_set_pending_import(const char *path);

/* ---- picking faces in the 3D view ---------------------------------------------------------------------------
 * The cursor casts a ray at the part through the geometry_pick operation; what it hits is a surface patch, which is
 * what a selection is made of. Hovering highlights, clicking collects, shift-clicking removes. */
void fem_hover_at(float px, float py);     /* window points; throttled, safe to call every frame */
bool fem_pick_at(float px, float py, bool remove);
/* drag a rectangle: every patch whose centre falls inside it is taken (or dropped); returns how many */
int fem_pick_box(float x0, float y0, float x1, float y1, bool remove, bool front_only);
void fem_clear_pick(void);
int fem_picked_count(void);
double fem_picked_area_mm2(void);
const double *fem_picked_normal(void);     /* outward normal of the picked set, build frame */
int fem_hover_patch(void);
/* turn the picked faces into a condition; direction 0 normal, 1 -normal, 2 +x, 3 +y, 4 +z */
bool fem_apply_hold(void);
bool fem_apply_load(double newtons, int direction);
bool fem_apply_gravity(void);

/* ---- the mesh-convergence check ------------------------------------------------------------------------------
 * Runs the same analysis again on a finer mesh and says how much the answer moved. The solve is a job like any
 * other: the window stays live while it runs. */
typedef enum { CONV_NONE = 0, CONV_MESHING, CONV_SOLVING, CONV_DONE, CONV_FAILED } ConvState;
typedef struct ConvResult {
    ConvState state;
    double coarse_mm, fine_mm;      /* element size */
    int coarse_elements, fine_elements;
    double coarse_disp, fine_disp;  /* max displacement, mm */
    double coarse_p99, fine_p99;    /* 99th-percentile von Mises, MPa */
    double disp_change_pct, stress_change_pct;
    bool capped, converged;
    char message[220];
} ConvResult;
void fem_repair_body(void);            /* geometry_repair on the open part, with what it changed kept for the panel */
/* Simple mode */
bool fem_pending_extent(double ext[3]);   /* a waiting file's size in its own numbers, for the unit question */
void fem_simple_after_import(void);       /* repair an almost-closed part without asking, and say what changed */
const char *fem_part_sentence(void);      /* that, as one sentence */
bool fem_place_face_down(int patch);      /* put the clicked face on the plate */
const char *fem_plate_sentence(void);
int fem_patch_at(float px, float py);
typedef struct FemStory {
    double warp_mm;              /* largest movement out of the drawn shape at the last stored time */
    char warp_direction[16];     /* upwards, downwards, sideways */
    char warp_where[64];         /* near the plate, at the top, ... */
    double stress_mpa;           /* largest residual stress (von Mises, nodal) at the last stored time */
    char stress_where[64];
    double rise_mm;              /* largest upward movement of anything printed, during the build */
    int rise_stored_time;
    double part_height_mm;
    /* supports (group 1), when the build carried them */
    int support_elements;
    double support_mm3;          /* their volume: elements times the element volume */
    bool supports_removed;
    /* plasticity, when the build let the metal yield (the run's own summary) */
    bool plastic, yielded;
    double yield_mpa;
    double peak_plastic;         /* the largest permanent stretch, as a fraction */
} FemStory;
/* a record of the material library, with the source of the values Simple mode shows and uses; each value is 0 and
 * its source "" when the record does not give it (a yield value without a source counts as absent) */
typedef struct FemMaterialInfo {
    char id[64], name[160], status[24];
    double density_kg_m3, youngs_pa, poisson, yield_pa, hardening_pa;
    char youngs_source[400], youngs_provenance[24];
    char yield_source[400], yield_provenance[24];
    char hardening_source[400], hardening_provenance[24];
} FemMaterialInfo;
bool fem_library_material(const char *id, FemMaterialInfo *out);
const struct JsonValue *fem_material_list(void); /* material_list: every record with status, group, lacks and source line */
const struct JsonValue *fem_result_summary(void); /* the summary of the result on screen, from the job or its run directory */
bool fem_build_story(FemStory *st);        /* from the result on screen; false when it is not a build */
void fem_set_report_preface(const char *text); /* the Simple results, in words, at the top of the report */
void fem_set_build_preview(double fraction);   /* draw the part only up to this share of its height; -1 whole */
bool fem_check_mesh(void);            /* start it; false when there is nothing to check */
const ConvResult *fem_convergence(void);

/* ---- the layer study for a build -----------------------------------------------------------------------------
 * The same build run again at half the simulation layer. Halving the layer is not a finer discretisation of the
 * same model: it changes how many times the part is strained, so the answer is expected to move. The study says by
 * how much, and says what it is. */
typedef enum { LS_NONE = 0, LS_RUNNING, LS_DONE, LS_FAILED } LayerState;
typedef struct LayerStudy {
    LayerState state;
    double coarse_layer_mm, fine_layer_mm;
    double coarse_tip_mm, fine_tip_mm;          /* tip after the cut, mm */
    double coarse_spring_mm, fine_spring_mm;    /* springback, mm */
    double tip_change_pct, spring_change_pct;
    int coarse_layers, fine_layers;
    double element_mm;          /* the element height the layer has to be resolved by */
    bool limited_by_mesh;       /* the finer layer is below it, so the same elements are activated */
    char message[220];
} LayerStudy;
/* remembers this run's numbers and waits for the next build job to finish; the caller starts that job */
bool fem_layer_study_begin(double coarse_layer_mm, double fine_layer_mm);
const LayerStudy *fem_layer_study(void);

/* ---- the report ---------------------------------------------------------------------------------------------- */
/* Writes <project>/report/: the summary, the setup, the checks, the convergence and rendered images. */
bool fem_write_report(char *dir_out, size_t cap);

bool fem_surface_view(void);
void fem_set_surface_view(bool on);
/* where the largest field value sits on the drawn surface, in viewer world coordinates */
bool fem_peak_marker(float out[3]);
bool fem_section_on(void);
int fem_section_axis(void);
double fem_section_position(void);
bool fem_section_flipped(void);
void fem_section(int axis, double fraction);
void fem_section_flip(void);
bool fem_group_shown(int g);
void fem_show_group(int g, bool on);
bool fem_fake_cut(int axis, double height_mm, int stored_index);
void fem_fake_cut_clear(void);
bool fem_fake_groups(int axis, double h1, double h2);

const float *fem_outline(int *vertices);

/* The pieces of what is drawn (the shells of the STL in the surface view, the face-connected regions of the mesh in
 * the mesh views): how solid each is, and how far they are pushed apart. */
int fem_pieces(void);
const char *fem_piece_name(int i);
float fem_piece_opacity(int i);
void fem_set_piece_opacity(int i, float a);
double fem_explode(void);
void fem_set_explode(double f);
bool fem_edges_on(void);
void fem_set_edges(bool on);
bool fem_ao_on(void);
void fem_set_ao(bool on);
const ResultPart *fem_draw_parts(int *n);
const float *fem_edge_lines(int *vertices, const ResultPart **parts, int *nparts);
/* the exploded home lines: pairs from each piece's moved centre back to where it belongs */
int fem_explode_lines(float *out, int max_vertices);
/* renders the current view off screen at `scale` times the window and writes a PNG */
bool fem_export_image(const char *path, int scale); /* undeformed crease lines of the shown elements */

/* topology optimisation (docs/contracts/topology-optimisation.md): a job in the engine, its density field drawn by
 * hiding the elements the optimiser emptied, and the surviving part exported as an STL contoured at the threshold */
bool fem_optimize_start(double volume_fraction, double filter_radius_m, int max_iterations); /* 0: 60 */
bool fem_optimize_running(void);
bool fem_optimize_have(void);
bool fem_optimize_failed(void);
double fem_optimize_progress(void);
const char *fem_optimize_stage(void);
const char *fem_optimize_message(void);
bool fem_optimize_show(void);          /* the removed elements are hidden */
void fem_optimize_set_show(bool on);
void fem_optimize_set_settings(double volume_fraction, double radius_m); /* radius 0: 1.5 element widths */
void fem_optimize_clear(void);
double fem_optimize_volume_fraction(void);
double fem_optimize_radius_mm(void);
int fem_optimize_iterations(void);
double fem_optimize_compliance(void);
double fem_optimize_compliance_initial(void);
double fem_optimize_reached_fraction(void);
int fem_optimize_kept(void);
int fem_optimize_elements(void);
double fem_optimize_seconds(void);
const double *fem_optimize_history(int *n); /* compliance per iteration */
bool fem_export_optimised_stl(const char *path, char *msg, size_t cap);
