/* setup.h - analysis setup stored in a project: surface selections, material assignments, boundary conditions and the
 * volume mesh state. Every entry records its provenance and the geometry revision it was resolved against, so stale
 * references are detected instead of being silently reused after the geometry, placement or mesh changes. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../core/json.h"
#include "../geom/hexmesh.h"
#include "../geom/tetmesh.h"

typedef enum { PROV_USER = 0, PROV_INFERRED, PROV_DEFAULT, PROV_CALIBRATED, PROV_COUNT } Provenance;
const char *provenance_name(Provenance p);
int provenance_from_name(const char *s); /* -1 if unknown */

typedef enum { SEL_MODE_PATCH = 0, SEL_MODE_TRIANGLE } SelectionMode;

typedef struct Selection {
    char name[64];
    char body[64];
    JsonValue *query;          /* definition as given (owned) */
    SelectionMode mode;
    double coverage;           /* patch mode: fraction of a patch's area that must match */
    int keep_largest;          /* 0 = keep every matching patch */
    char description[512];     /* the interpretation, e.g. "bottom face resting on the plate" */
    Provenance source;
    /* resolution */
    uint64_t geom_revision;    /* body geometry revision of this resolution */
    int ntri;
    int *tris;                 /* sorted analysis-surface triangle ids */
    int npatches;
    int *patches;              /* sorted patch ids with selected triangles */
    double area;               /* m^2 on the STL surface */
    double centroid[3], normal[3], bmin[3], bmax[3]; /* build frame, m; normal = area-weighted mean unit normal */
    char set_hash[65];         /* SHA-256 of the body file hash and the sorted triangle ids */
    bool stale;
    char stale_reason[256];
} Selection;

typedef struct MaterialAssignment {
    char target[64];   /* body name, or "build_plate" */
    char material[64]; /* material id */
    Provenance source;
    char note[256];
} MaterialAssignment;

typedef enum {
    BC_FIXED = 0,
    BC_DISPLACEMENT,
    BC_FRICTIONLESS,
    BC_FORCE,
    BC_PRESSURE,
    BC_TRACTION,
    BC_GRAVITY,
    /* thermal conditions (transient thermal and thermomechanical analyses) */
    BC_TEMPERATURE,
    BC_HEAT_FLUX,
    BC_CONVECTION,
    BC_RADIATION,
    BC_HEAT_SOURCE,
    BC_KIND_COUNT
} BcKind;
const char *bc_kind_name(BcKind k);
int bc_kind_from_name(const char *s);
bool bc_is_constraint(BcKind k); /* prescribed displacement */
bool bc_is_thermal(BcKind k);

enum { BC_SCHEDULE_MAX = 32 };

typedef struct BoundaryCondition {
    char name[64];
    BcKind kind;
    char selection[64];      /* empty for gravity and volumetric conditions */
    char body[64];           /* volumetric conditions (heat source) act on a body instead of a selection */
    double vec[3];           /* displacement (m), total force (N), traction (Pa) or acceleration (m/s^2) */
    bool component[3];       /* displacement: which components are prescribed */
    double magnitude;        /* pressure (Pa, positive into the surface), prescribed temperature (K), heat flux
                              * (W/m^2 into the body), convection coefficient (W/(m^2 K)), emissivity, or
                              * volumetric power (W/m^3) */
    double ambient;          /* K: ambient temperature of convection and radiation */
    /* optional load schedule of a thermal condition: from schedule_t[i] (s, strictly increasing, the first is 0) until
     * the next time the condition acts with its magnitude multiplied by schedule_factor[i] (flux, power density,
     * convection coefficient or emissivity; for a prescribed temperature 1 holds it and 0 releases the surface). The
     * times are events that transient steps land on exactly. nschedule = 0: constant in time. */
    int nschedule;
    double schedule_t[BC_SCHEDULE_MAX];
    double schedule_factor[BC_SCHEDULE_MAX];
    char description[512];
    Provenance source;
    char selection_hash[65]; /* the selection's resolved set when the condition was applied */
    uint64_t revision;
} BoundaryCondition;

/* the schedule factor of a condition at time t (1 without a schedule) */
double bc_schedule_factor(const BoundaryCondition *b, double t);

/* thermal interface between two bodies that touch in the mesh.
 *   perfect      the bodies share temperature at the interface (shared nodes): no temperature jump
 *   conductance  heat crosses as q = h (T_b - T_a) through a finite contact conductance h (W/(m^2 K))
 *   thin_layer   the same with h = layer_conductivity / layer_thickness (a layer of negligible heat capacity)
 *   insulated    no heat crosses the interface
 * Only the thermal model is split: mechanically the bodies stay bonded through their shared nodes. */
typedef enum { CONTACT_PERFECT = 0, CONTACT_CONDUCTANCE, CONTACT_THIN_LAYER, CONTACT_INSULATED, CONTACT_MODEL_COUNT } ContactModel;
const char *contact_model_name(ContactModel m);
int contact_model_from_name(const char *s);

typedef struct ThermalContact {
    char name[64];
    char body_a[64], body_b[64];
    ContactModel model;
    double conductance;        /* W/(m^2 K), conductance model */
    double layer_thickness;    /* m, thin_layer */
    double layer_conductivity; /* W/(m K), thin_layer */
    char description[512];
    Provenance source;
    uint64_t revision;
} ThermalContact;
/* the effective conductance h (W/(m^2 K)); 0 for insulated, INFINITY for perfect */
double contact_conductance(const ThermalContact *c);

enum { MESH_MAX_BODIES = 16 };

typedef struct MeshState {
    bool valid;
    bool has_settings; /* settings persist with the project; the mesh itself is regenerated on demand */
    HexMesh hm;
    double h[3];
    bool include_plate;
    double plate_thickness, plate_margin;
    int nbodies;
    char body_name[MESH_MAX_BODIES][64];
    uint64_t body_geom_revision[MESH_MAX_BODIES];
    uint64_t revision;   /* project revision when generated */
    char hash[65];       /* hash of the settings and the meshed geometry */
    /* method 1: a conforming tetrahedral mesh (docs/contracts/tet-mesh.md) carried in hm with hm.elem_type set; h[0]
     * is the surface size. The print analyses need the voxel mesh and refuse it (project_mesh_current). */
    int method;
    int tet_order;          /* 1 TET4, 2 TET10 */
    double tet_interior;    /* m */
    int tet_thin;           /* thin_wall_levels: 0, 1 or 2 */
    TetMesh tetq;           /* the tetrahedral mesher's quality report (arrays not kept) */
} MeshState;

void selection_free_data(Selection *s);
void mesh_state_free(MeshState *m);
