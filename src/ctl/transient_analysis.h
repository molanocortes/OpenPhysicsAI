/* transient_analysis.h - transient thermal and thermomechanical analyses of a project
 *
 * thermal_case_build turns the persisted setup (mesh, materials, thermal boundary conditions) into a self-contained
 * case: owned copies of the mesh, material property tables, prescribed temperatures, boundary faces and volumetric
 * sources. The case is stepped on the job worker without touching the project; temperatures are stored at the chosen
 * output times together with the energy balance of every step. For a thermomechanical analysis the same mesh is solved
 * structurally at each stored time with the thermal strain integrated from the stress-free reference temperature and
 * with the temperature-dependent modulus, which is a sequential (one-way) coupling: the structure does not feed back
 * into the temperature field, and without plasticity or element activation the result is not a residual-stress
 * prediction (thermal stresses vanish again when the part returns to a uniform temperature). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../fem/orchestrator.h"
#include "../fem/thermal.h"
#include "../fem/thermal_integrator.h"
#include "matlib.h"
#include "static_analysis.h"

enum { TC_MAX_OUTPUT_TIMES = 256 };

/* the analyses stepped by the transient job (as job kinds and analysis names) */
static inline bool transient_analysis_kind(const char *k) {
    return k && (!strcmp(k, "transient_thermal") || !strcmp(k, "thermomechanical") || !strcmp(k, "conjugate_heat_transfer"));
}

/* analyses whose results are this time-indexed file and are read by the same result operations: the transient
 * analyses above and the layer-by-layer print (src/ctl/print_analysis.h), which fills the same case but is stepped by
 * its own driver and has no checkpoints */
static inline bool transient_results_kind(const char *k) {
    return transient_analysis_kind(k) || (k && (!strcmp(k, "fff_print") || !strcmp(k, "lpbf_build")));
}

typedef struct ThermalSettings {
    double end_time;              /* s */
    double time_step;             /* s: the fixed step, or the initial step of an adaptive run (0 = default) */
    ThermalStepping stepping;     /* fixed (default) or adaptive */
    /* adaptive stepping: step limits, controller and temporal tolerances (0 = default) */
    double dt_min, dt_max;        /* s */
    double step_safety, max_step_growth, max_step_shrink;
    int max_rejections;
    long max_steps;
    double temporal_relative;     /* dimensionless */
    double temporal_temperature;  /* K, a temperature difference */
    double temporal_enthalpy;     /* J/m^3; 0: derived from the temperature tolerance and each material's rho cp */
    bool temporal_temperature_only;
    /* stored times: explicit list, else a regular interval, else every n-th fixed step (always t = 0 and the end) */
    int noutput_times;
    double output_times[TC_MAX_OUTPUT_TIMES]; /* s */
    double output_interval;                   /* s */
    /* run control, not physics: may differ on resume */
    double checkpoint_interval;               /* s of physical time between checkpoints; 0 = none */
    double checkpoint_wall_interval;          /* s of wall-clock time between checkpoints; 0 = none */
    double theta;                 /* 1 backward Euler, 0.5 Crank-Nicolson */
    bool consistent_capacity;
    double initial_temperature;   /* K */
    double reference_temperature; /* K: stress-free temperature of the mechanical step */
    int output_every;             /* store every n-th step */
    int max_picard;
    double picard_tol;            /* K */
    ThermalPropertyEval property_eval; /* quadrature-point (default) or element-mean conductivity */
    bool element_capacity;        /* rho cp at the element mean instead of the secant enthalpy capacity */
    bool phase_change;            /* apply the latent heat of materials that state a solidus/liquidus pair */
    bool mechanical;              /* also solve the structure at every stored time */
    Hex8Formulation formulation;
    SolidSolver solver;
    double pcg_tol;
    /* conjugate heat transfer (analysis conjugate_heat_transfer): a resolved flow domain, its energy transport and the
     * solid bodies it exchanges heat with */
    bool cht;
    char fluid_body[64];
    double inlet_velocity;        /* m/s along +x, uniform over the inlet plane of the fluid body's bounding box */
    double inlet_temperature;     /* K: the temperature of entering fluid (inlet and any backflow) */
    int flow_wall[4];             /* FlowWall of the sides y-, y+, z-, z+ */
    double flow_steady_tolerance; /* relative change per 200 lattice steps; 0 = default */
    bool coupling_monolithic;     /* false (default): fluid and solids as partitioned participants iterated each step */
    double coupling_relative;     /* partitioned: relative change of the interface temperature that ends the iteration */
    int coupling_max_iterations;
    double coupling_relaxation;   /* first relaxation factor before Aitken */
} ThermalSettings;

enum { TC_MAX_OUTPUTS = 4096 };

/* what a continuation depends on: resolved solver inputs, hashed from binary data so formatting cannot matter */
typedef struct ThermalCaseHashes {
    char mesh[65], materials[65], conditions[65], settings[65];
} ThermalCaseHashes;

typedef struct ThermalCheckpoint ThermalCheckpoint;

typedef struct ThermalCase {
    ThermalSettings settings;
    /* mesh (owned; shared with the mechanical step) */
    int nnodes, nelems, nfaces;
    double *xyz;
    int *conn, *elem_mat;
    signed char *elem_body;
    int *face_elem;
    unsigned char *face_local;
    double h[3];
    int nbodies;
    char body_name[MESH_MAX_BODIES][64];
    char mesh_hash[65];
    /* materials: the records own the property tables, the thermal tables point into them */
    int nmat;
    MaterialRecord rec[SA_MAX_MATERIALS];
    ThermalMaterial tmat[SA_MAX_MATERIALS];
    char mat_id[SA_MAX_MATERIALS][64], mat_status[SA_MAX_MATERIALS][24];
    /* thermal conditions */
    unsigned char *fixed;
    double *fixed_T;    /* K */
    double *elem_source; /* W/m^3 */
    int ntfaces;
    ThermalFace *tfaces;
    int ninterface;
    ThermalInterfaceNode *interfaces; /* finite-conductance thermal interfaces (contact resistance, thin layers) */
    /* thermal interfaces between bodies: the thermal model has twin nodes where a finite-resistance or insulated
     * interface splits two bodies; the structural model keeps the mesh nodes, so tnode_mesh maps back */
    int nmesh_nodes;          /* nodes of the structural mesh (<= nnodes) */
    int *tnode_mesh;          /* nnodes: mesh node of every thermal node */
    int *interface_contact;   /* ninterface: index into contact[] */
    int ncontacts;
    struct {
        char name[64], body_a[64], body_b[64];
        int model;            /* ContactModel */
        int mesh_body_a, mesh_body_b;
        double conductance;   /* W/(m^2 K); 0 insulated; INFINITY perfect */
        double area;          /* m^2 */
        int faces;
    } contact[THERMAL_MAX_GROUPS];
    int bc_body[SA_MAX_BCS];  /* mesh body each thermal condition acts on, -1 for several or the plate */
    ThermalBudget group_budget; /* the accepted budget per body (elements), condition (faces, nodes) and interface */
    /* flat copies of the interface pairs for the results file (filled on save, read on load) */
    int *io_iface_a, *io_iface_b;
    double *io_iface_g, *io_iface_area;
    /* time-dependent conditions: base values and the condition (index into bc[]) that owns each face, node and source.
     * The job's schedule callback rebuilds tfaces[].value, elem_source and fixed from these for every trial interval. */
    bool scheduled;
    int *tface_bc;
    double *tface_base;
    int *fixed_bc;            /* nnodes, -1 when not prescribed */
    unsigned char *fixed_base;
    int nsources;
    int source_bc[SA_MAX_BCS], source_body[SA_MAX_BCS];
    double source_magnitude[SA_MAX_BCS]; /* W/m^3 */
    int nsched[SA_MAX_BCS];
    double sched_t[SA_MAX_BCS][BC_SCHEDULE_MAX], sched_f[SA_MAX_BCS][BC_SCHEDULE_MAX];
    EventSchedule events;     /* output times, schedule changes and the end */
    int nbc;
    SaBcInfo bc[SA_MAX_BCS]; /* one entry per thermal condition: faces, nodes, areas and nominal power */
    int nsets;
    SaSet set[SA_MAX_SETS];  /* selections mapped onto the mesh, for result queries */
    /* stored results */
    int noutputs, nsteps; /* nsteps: planned fixed steps before the run, accepted steps after it */
    double *times;     /* noutputs (s) */
    double *T;         /* noutputs * nnodes (K) */
    double *tmin, *tmax; /* noutputs */
    double energy_source, energy_boundary, energy_prescribed, energy_stored; /* cumulative J */
    double energy_enthalpy;    /* cumulative J from integrating H(T) - an independent measure of energy_stored */
    double worst_balance, worst_enthalpy_mismatch;
    double liquid_volume_max;  /* m^3, largest molten volume seen during the run */
    int picard_max, linear_total;
    /* time integration record (filled by the job) */
    ThermalWork work;
    StepControllerSettings control;   /* the controller actually used (adaptive) */
    ThermalAttempt recent[16];        /* the last attempts before the end or the failure */
    int nrecent;
    int nhistory;                     /* accepted steps recorded */
    double *step_t, *step_dt, *step_err; /* nhistory each: end time, size and normalised error estimate (-1 fixed) */
    char status[32];                  /* COMPLETED or the failure code */
    double last_accepted_time;
    /* checkpoint and restart */
    ThermalCaseHashes hashes;         /* computed when the case is built */
    ThermalCheckpoint *resume;        /* set when the job continues from a checkpoint (owned) */
    void *frames_file;                /* FILE* of frames.nvf while the job runs */
    long checkpoint_sequence;         /* checkpoints written in this run, including earlier sessions */
    double last_checkpoint_time;      /* s of physical time, -1 before the first */
    int nresumes;
    double resume_times[16];          /* s: the physical times this run was resumed from */
    /* element activation (the layer-by-layer print and the LPBF build): NULL when every element exists at every stored time */
    int *elem_birth; /* nelems: index of the first stored time at which the element exists; -1 never deposited */
    int *elem_death; /* nelems: first stored time at which the element no longer exists (cut away); -1 never */
    unsigned char *elem_group; /* nelems: 0 part, 1 support, 2 plate; NULL = all part */
    bool no_temperature;       /* the analysis stores no temperature field (the LPBF build): T, tmin and tmax are absent */
    /* hanging nodes of an adaptive mesh (the LPBF build): node, masters, weights and the coarse element that owns each
     * constraint; the same node may appear once per owner. Not written to the result file: the displacements written
     * there already hold the constrained values, so the file stays plain hex8. 0 for a conforming mesh. */
    int nhang;
    int *hang_node, *hang_nmaster, *hang_master, *hang_owner;
    double *hang_weight;
    /* LPBF supports: the stiffness scale of every element (the homogenised support's vertical fraction times its
     * relative density; 1 for the part), NULL without supports */
    double *elem_stiff_scale;
    /* typed supports: their homogenised orthotropic matrices (Pa, 36 each) and each element's index, -1 for the part */
    int support_nD;
    int *elem_D_of;
    double *support_D;
    /* every support band as the heat model sees it: conductivity fractions x y z, capacity, stiffness, surface per
     * volume (1/m) (6 each), and
     * each element's band, -1 for the part */
    int support_nbands;
    double *support_band_props;
    int *elem_band;
    /* mechanical results at the stored times (thermomechanical) */
    bool has_mech;
    StaticModel mech;
    double *mech_u;  /* noutputs * 3 * nnodes (m) */
    double *mech_vm; /* noutputs * nnodes (Pa), von Mises of the nodal average */
    double *mech_peak, *mech_umax; /* noutputs */
    JsonValue *build_warnings, *summary;
    JsonValue *orchestration; /* the execution plan of the last run (orch_describe): participants, couplings, synchronisation */
    /* conjugate heat transfer (settings.cht) */
    int fluid_body;            /* mesh body index of the flow domain */
    unsigned char *advect;     /* nelems: 1 for elements of the fluid body */
    unsigned char *node_part;  /* nnodes: bit 1 used by a solid element, bit 2 by a fluid element (3 = conjugate interface) */
    double *velocity;          /* 3 * nnodes (m/s): the mapped flow, zero outside the fluid; filled by the flow participant */
    int flow_n[3];             /* lattice cells of the fluid body's bounding box */
    double flow_origin[3], flow_dx;
    unsigned char *flow_solid; /* flow_n[0]*flow_n[1]*flow_n[2]: cells of the box that are not fluid */
    int *flow_corner;          /* nnodes: lattice corner index of every fluid node, -1 elsewhere */
    int nopen_in, nopen_out;   /* open faces added at the inlet and outlet planes */
    double inlet_area, interface_area; /* m^2 */
    int ninterface_nodes;
    struct {
        bool done;
        double tau, u_lattice, mach, reynolds_cell, reynolds_hydraulic, dt, physical_time, change, density_min, density_max;
        double inlet_flux, outlet_flux, scale_min, scale_max, seconds;
        long steps;
    } flow;                    /* the lattice run, for the summary */
    OrchWork orch_work;        /* coupling statistics of the whole run (checkpointed) */
    /* results of a conjugate study (filled by the job) */
    bool cht_partitioned;
    double cht_heat_into_fluid;   /* J over the run: the fluid's account of the heat it received through the interface */
    double cht_heat_into_solid;   /* J: the solids' account of the same heat (partitioned; the opposite sign when conserved) */
    double cht_continuity;        /* K: largest |T_fluid - T_solid| at the interface nodes at the end (partitioned) */
    double energy_advection, energy_inflow, energy_enthalpy_in, energy_enthalpy_out, energy_divergence; /* J over the run */
    char job_id[64];
    char run_dir[NV_PATH_MAX];
} ThermalCase;

/* ---- checkpoint and restart (thermal_checkpoint.c) ------------------------------------------------------------------
 *
 * A checkpoint is written only at an accepted step boundary and holds everything that continuing the same physical run
 * needs: the accepted time and temperature (the phase state is the equilibrium liquid fraction of the temperature and
 * has no other history), the integrator's controller history, accepted energy budget and work counters, the number of
 * stored frames and their times, the accepted-step history and the resume history. Stored frames themselves live in
 * frames.nvf, an append-only file that is flushed to disk before every checkpoint that refers to it; resuming truncates
 * it back to the checkpoint's frame count, so a frame written after the checkpoint is neither lost nor duplicated.
 * Schedules and activation are pure functions of time and need no state. Continuation is refused when the mesh, the
 * resolved materials, the resolved thermal conditions or the physics settings hash differently from the checkpoint.
 *
 * Publication is atomic: the new file is written and synced as checkpoint.nvc.tmp, the previous checkpoint becomes
 * checkpoint.prev.nvc and the new one is renamed into place. A reader verifies the magic, the header, the declared
 * sizes and the SHA-256 of the payload, and falls back to the previous checkpoint when the newest is damaged. */
struct ThermalCheckpoint {
    char job_id[64], file[NV_PATH_MAX], created[32], software[32];
    ThermalCaseHashes hashes;
    long sequence;
    int nnodes, frames, nhistory, nresumes;
    bool has_mech;
    ThermalIntegratorState state;
    double *T;          /* nnodes: the first (or only) thermal participant's own node numbering */
    /* a partitioned study has a second thermal participant with its own accepted state */
    int nparticipants, nnodes2;
    ThermalIntegratorState state2;
    double *T2;          /* nnodes2 */
    int nvelocity;       /* 3 * nodes, 0 when the run has no flow */
    double *velocity;
    OrchWork orch_work;
    JsonValue *flow_info; /* the lattice run's diagnostics, when the checkpoint holds a flow */
    double *frame_times; /* frames */
    double *step_t, *step_dt, *step_err; /* nhistory */
    double resume_times[16];
    char fallback_reason[256]; /* why the newest checkpoint was not used, when the previous one was */
};

void thermal_case_hashes(const ThermalCase *c, ThermalCaseHashes *h);
/* writes checkpoint.nvc into the run directory at the accepted state of the thermal participants' integrators (one, or
 * two for a partitioned study: fluid then solid); frames = stored frames so far. The case supplies the mapped velocity
 * of a flow study and the coupling statistics. */
bool thermal_checkpoint_write(ThermalCase *c, const ThermalIntegrator *const *ti, int nti, int frames, char *err, size_t errlen);
/* reads the newest valid checkpoint of a run directory (header_only: skips the payload arrays but still verifies them) */
ThermalCheckpoint *thermal_checkpoint_read(const char *run_dir, bool header_only, char *err, size_t errlen);
void thermal_checkpoint_free(ThermalCheckpoint *ck);
/* the header of the newest valid checkpoint as JSON for job_status, or NULL */
JsonValue *thermal_checkpoint_info_json(const ThermalCheckpoint *ck);
/* frames.nvf: create (writing frame 0), append, sync, close; load restores the first `count` frames into the case,
 * checks their times against the checkpoint and truncates the file after them */
bool thermal_frames_create(ThermalCase *c, char *err, size_t errlen);
bool thermal_frames_append(ThermalCase *c, int index, char *err, size_t errlen);
bool thermal_frames_sync(ThermalCase *c, char *err, size_t errlen);
void thermal_frames_close(ThermalCase *c);
bool thermal_frames_restore(ThermalCase *c, const ThermalCheckpoint *ck, char *err, size_t errlen);

/* errors and warnings receive {code, message, hint?} objects; true when the case can be solved */
bool thermal_case_build(Project *p, const ThermalSettings *s, ThermalCase *c, JsonValue *errors, JsonValue *warnings);
void thermal_case_free(void *c); /* JobFreeFn */
JsonValue *thermal_case_model_json(const ThermalCase *c);
/* job entry point: steps the case, stores outputs and writes results.nvt and summary.json into the run directory */
bool thermal_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
JsonValue *thermal_summary_json(const ThermalCase *c);

/* results.nvt: a JSON header followed by little-endian arrays (same shape as the static results file) */
bool thermal_results_save(const ThermalCase *c, const char *path, char *err, size_t errlen);
ThermalCase *thermal_results_load(const char *path, char *err, size_t errlen);
/* one VTU per stored time plus a .pvd collection that carries the times */
bool thermal_export_vtu_series(const ThermalCase *c, const char *dir, int *files, char *err, size_t errlen);
/* time series: one row per stored time with the temperature range, energies and, when present, the mechanical peaks */
bool thermal_export_csv(const ThermalCase *c, const char *path, char *err, size_t errlen);
