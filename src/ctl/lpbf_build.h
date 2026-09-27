/* lpbf_build.h - the inherent-strain LPBF build as an engine analysis (job kind "lpbf_build")
 *
 * lpbf_case_build turns the persisted setup (meshed body, elastic constants, inherent strain, cut) into a self-contained
 * case and refuses what it cannot answer for: a stale mesh, a strain or a cut or elastic constants without provenance, a
 * part that does not rest on the plate, several bodies or materials, a cut that removes nothing. The case is built on
 * the job worker by src/mech/lpbf.c and every stored time is written into the ordinary time-indexed result file
 * (results.nvt) with elem_birth, elem_death and elem_group, so the result operations and the application's stored-time
 * slider show the build growing and the cut. There is no temperature field.
 *
 * Scope: the inherent strain is a calibrated input, not a material property; the method is the one the commercial tools
 * use. See docs/contracts/lpbf-build.md. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "../mech/lpbf.h"
#include "../mech/support_cell.h"
#include "jobs.h"
#include "project.h"
#include "transient_analysis.h"

typedef struct LpbfSettings {
    char body[64];
    int orientation;             /* 0: the part's long axis lay along machine X; 1: along machine Y (swaps exx and eyy) */
    double layer_thickness;      /* m, the simulation layer */
    double eps[3];               /* the inherent strain as given, in the machine frame */
    char strain_source[160];     /* where the tensor comes from (a file row, or "user") */
    Provenance strain_provenance;
    char prov_text[3][24];       /* the words as given, for strain, material and cut: "assumed" has no enum value */
    double cut_height, cut_kerf; /* m; cut_height <= 0: no cut */
    double cut_from_x;           /* m: measured from the part's own minimum x, the cut acts only beyond it (the wire
                                  * enters from the free end and stops at the block that keeps the part on the plate);
                                  * 0 cuts the whole section. Measuring it from the part, not from the build frame,
                                  * keeps it independent of where the mesher centres the geometry */
    Provenance cut_provenance;
    bool has_cut;
    double E, nu;                /* Pa, - */
    double yield_stress;         /* Pa; <= 0: elastic, and the step takes the same single solve it always did */
    double hardening;            /* Pa: linear isotropic hardening modulus H; 0 is elastic-perfectly-plastic */
    int max_newton;              /* iterations allowed per step (default 25) */
    double newton_tol;           /* relative residual (default 1e-8) */
    char plastic_source[192];    /* where the yield stress comes from */
    Provenance plastic_provenance;
    char plastic_prov_text[24];
    Provenance material_provenance;
    bool whole_part;             /* all layers activated and strained at once: the cross-check, never a production answer */
    double max_element_size;     /* m: > the mesh size makes the mesh adaptive, coarse inside up to this size in x and y
                                  * (2:1 balanced, fine at the surface, one layer tall); 0 keeps the uniform mesh */
    int fine_band;               /* voxels of fine band along every surface (default 2) */
    /* supports: every voxel column below a part element down to the plate that is not part becomes support, a
     * homogenised lattice with `support_fraction` of the solid's stiffness (and, with plasticity, of its yield
     * stress). The plate is z = 0; with supports a part may stand above it. Removed after the cut. */
    bool supports;
    bool remove_supports;        /* after the cut, or after the build when there is no cut */
    int support_rule;            /* 0 overhang (faces below the critical angle, columns that reach the plate), 1 every
                                  * column under a part voxel (wave 4), 2 overhang_all (faces below the critical angle,
                                  * down to the plate or the part: a sensitivity) */
    double support_angle;        /* rad: a downward face flatter than this from the horizontal needs support */
    char support_angle_source[160];
    double support_fraction;     /* homogeneous type only (wave 4) */
    SupportSpec support;         /* the type and its geometry (docs/contracts/supports.md) */
    double support_offset;       /* m: no support column within this horizontal distance of a part wall */
    double support_height;       /* m: supports.height_above_plate as given, checked against the placement; < 0 absent */
    int support_nparam;          /* every support parameter used, with its provenance, for the result */
    struct {
        char name[24], unit[8], prov[16];
        double value;            /* in unit */
        char source[200];
    } support_param[24];
    char support_source[192];
    Provenance support_provenance;
    char support_prov_text[24];
    Hex8Formulation formulation;
    SolidSolver solver;
    double pcg_tol;
} LpbfSettings;

/* the job data; ThermalCase first, so a LpbfCase * is a ThermalCase * for the result operations */
typedef struct LpbfCase {
    ThermalCase c;
    LpbfSettings s;
    /* filled by the run */
    int layers, elements_cut, solves;
    double eps_model[3];        /* the strain actually applied, after the orientation swap */
    double tip_before, tip_after, springback; /* m, max u_z over the free-end face */
    double tip_x;               /* m, the x of the free-end face */
    double plate_reaction, equilibrium_error;
    double peak_plastic_strain;  /* largest equivalent plastic strain over the part */
    int support_elements;        /* generated below overhangs, 0 when supports are off */
    int support_faces, support_faces_on_part, support_faces_steep; /* overhang rule: supported, skipped (the column
                                  * lands on the part), and downward voxel faces steeper than the angle */
    int support_columns_offset;  /* support columns dropped because they came within the offset of a part wall */
    double support_volume;       /* m^3 of support material (element volume x solid fraction x relative density) */
    double support_contact_area; /* m^2 of support touching the part */
    double support_contact_fraction; /* of the supported faces' area */
    int support_nband;           /* the homogenised bands, for the result (the most populated first) */
    struct {
        double d0, d1, height;   /* m: depth range below the part; the support height (cones) or 0 */
        int elements;
        SupportCellProps p;
    } support_band[24];
    int support_bands_total;     /* distinct bands solved (support_nband of them are listed) */
    double tearoff_max, tearoff_sum, tearoff_resultant[3]; /* N: the supports' forces on the part before removal */
    bool held_321;
    bool cut_with_supports;      /* the cut, the stub below it and the supports went in one step */               /* the part was left free by the support removal and held 3-2-1 */
    int island_elements;         /* part elements tied to the rest only through supports, removed with them */
    int part_elements;           /* the elements of the part itself */
    int uniform_elements;        /* before coarsening (part, supports and the cut), equal to nelems when uniform */
    int coarse_levels[8];        /* elements per level after coarsening: 0 is a voxel */
    double tip_after_support_removal; /* m; equals tip_after when there are no supports */
    double umax_before_cut, umax_after_cut, umax_final; /* m: largest total displacement over the part */
    int newton_iterations;       /* extra solves the plasticity cost */
    double last_newton_residual;
    double seconds;
    double peak_vm_before, peak_vm_after; /* Pa */
} LpbfCase;

bool lpbf_case_build(Project *p, const LpbfSettings *s, LpbfCase **out, JsonValue *errors, JsonValue *warnings);
bool lpbf_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
/* One build of the case as its settings stand, filling the tip and peak fields. `store_times` writes the
 * time-indexed fields into the case (what the result file and the application need); a calibration runs the
 * same build many times and does not want them. Progress is reported into [lo, hi]. The case may be run again
 * afterwards with different settings: the run resets what it accumulates. */
bool lpbf_build_once(Job *job, LpbfCase *lc, bool store_times, double lo, double hi,
                     char *code, size_t codelen, char *err, size_t errlen);
JsonValue *lpbf_summary_json(const LpbfCase *lc);
JsonValue *lpbf_model_json(const LpbfCase *lc);
/* the supports as generated (before any run): what lpbf_supports_generate returns */
JsonValue *lpbf_supports_json(const LpbfCase *lc);
/* the supports of s applied to another voxel case (the FFF print): generated under the overhangs, homogenised;
 * stats (zeroed by the caller) receives the counts and bands. Returns the support elements, -1 on failure. */
int lpbf_supports_apply(Project *p, ThermalCase *c, const LpbfSettings *s, LpbfCase *stats, JsonValue *errors, JsonValue *warnings);
