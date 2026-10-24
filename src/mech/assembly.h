/* assembly.h - assemblies of rigid parts: bodies, named frames, geometry, mass properties with provenance, joints and
 * couplings, plus material and manufacturing metadata. Loaded from the native "navier-assembly" JSON format (version 1),
 * a documented URDF subset, or STL components; compiled into a multibody model (multibody.h).
 *
 * Native format (full description in Mech Sim/docs/assembly-format.md)
 *   {"format": "navier-assembly", "version": 1, "name": ..., "units": {"length": "mm", ...},
 *    "gravity": [0, 0, -9.80665], "bodies": [...], "joints": [...], "couplings": [...]}
 *   - Plain numbers are in the unit that "units" declares for their quantity (length, angle, mass, time, force, torque,
 *     inertia, density, velocity, angular_velocity, acceleration, linear/rotational stiffness and damping); undeclared
 *     quantities are SI. Strings carry their own unit: "30 deg", "1.2 mm", "0.2 N*m/rad".
 *   - Poses: {"position": [x, y, z], "rpy": [r, p, y]} or {"position": ..., "quaternion": [w, x, y, z]}; joint frames may
 *     also name a frame of their body: "upper_arm.elbow".
 *   - Every joint states its frames on both bodies and, for revolute and prismatic joints, its axis. Nothing is placed
 *     by guesswork. Mass properties are explicit (mass, centre of mass, inertia about the centre of mass) or computed
 *     from a closed mesh with a declared fill model.
 *   - Unknown keys are reported, never silently ignored.
 * Inertia tensors use tensor components (ixy = -integral of x y dm), as URDF does. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../core/json.h"
#include "../core/paths.h"
#include "massprops.h"
#include "mechdiag.h"
#include "mmath.h"
#include "multibody.h"

typedef enum { SRC_USER = 0, SRC_MEASURED, SRC_CAD, SRC_COMPUTED, SRC_INFERRED, SRC_DEFAULT, SRC_CALIBRATED, SRC_COUNT } AsmSource;
const char *asm_source_name(AsmSource s);
int asm_source_from_name(const char *s);

typedef enum { AG_MESH = 0, AG_BOX, AG_SPHERE, AG_CYLINDER, AG_CAPSULE, AG_PLANE, AG_COUNT } AsmGeomType;
const char *asm_geom_type_name(AsmGeomType t);

typedef struct AsmGeom {
    char role[12]; /* "visual", "collision", "fem" or "environment" */
    char name[MB_NAME]; /* optional; contact results use it */
    bool has_friction, has_restitution;
    double friction, restitution;   /* collision and environment shapes: Coulomb and Newton coefficients */
    AsmSource contact_source;       /* where the coefficients come from (they are model parameters, not material data) */
    int group;                      /* shapes of the same non-zero group never touch */
    AsmGeomType type;
    char file[NV_PATH_MAX]; /* mesh: resolved path (empty when unresolved) */
    char file_as_given[512];
    char units[16];     /* mesh length unit */
    double scale[3];    /* metres per mesh unit along each axis */
    double size[3];     /* box: full lengths; sphere: radius; cylinder and capsule: radius, length (along z) */
    MPose pose;         /* in body coordinates */
} AsmGeom;

typedef struct AsmFrame {
    char name[MB_NAME];
    MPose pose; /* in body coordinates */
} AsmFrame;

/* reduced elastic model of a body (flexbody.h; produced by mech_flexible_reduce and attached with mech_flexible_attach): the body
 * becomes a flexible body of the multibody model. Interfaces are named by the child joint riding each one. Always SI and body
 * frame, whatever "units" declares. JSON:
 *   "flexible": {"method": "craig_bampton", "units": "SI", "omega2": [n], "zeta": [n], "ell": [6n], "dJ": [9n],
 *                "interfaces": [{"joint", "point": [3], "phi": [3n], "psi": [3n]}],
 *                "fe_mass_properties": {"mass", "com": [3], "inertia": [9]}, "provenance": {...}} */
typedef struct AsmFlexible {
    MbFlexDef model;                          /* body index unused */
    char (*interface_joint)[MB_NAME];         /* model.ninterfaces */
    double fe_mass, fe_com[3], fe_inertia[9]; /* of the reduced mesh (inertia about its centre of mass): the body's must equal them */
    JsonValue *provenance;                    /* owned, kept verbatim */
} AsmFlexible;
AsmFlexible *asm_flexible_from_json(const JsonValue *v, MechDiag *d, const char *subject); /* NULL with diagnostics */
JsonValue *asm_flexible_to_json(const AsmFlexible *f);
void asm_flexible_free(AsmFlexible *f); /* frees the struct too */

typedef struct AsmBody {
    char name[MB_NAME];
    char part[MB_NAME]; /* project body (STL part) this body was made from; its file frame in metres is the body frame */
    bool has_inertial;
    double mass, com[3], inertia[9]; /* kg; m, body frame; kg m^2 about the centre of mass, body axes */
    AsmSource inertial_source;
    char inertial_note[512];
    bool mass_from_mesh;
    char mass_mesh[NV_PATH_MAX];
    char mass_mesh_units[16];
    MPose mass_mesh_pose;
    FillSpec fill;
    MassProperties mp;
    AsmGeom *geoms;
    int ngeoms;
    AsmFrame *frames;
    int nframes;
    char material[64];
    AsmSource material_source;
    JsonValue *manufacturing; /* owned, kept verbatim */
    AsmFlexible *flexible;    /* owned; NULL = rigid */
} AsmBody;

typedef struct AsmJoint {
    MbJointDef def; /* parent and child are body indices (-1 = world) */
    double effort_limit, velocity_limit; /* 0 = not stated */
} AsmJoint;

typedef struct AsmCoupling {
    char name[MB_NAME], follower[MB_NAME], driver[MB_NAME];
    double ratio, offset;
} AsmCoupling;

typedef struct AsmNote {
    char subject[128];
    char text[512];
    AsmSource source;
} AsmNote;

typedef struct Assembly {
    char name[MB_NAME];
    char description[1024];
    char source_format[24]; /* "navier-assembly", "urdf", "stl" or "api" */
    char source_path[NV_PATH_MAX];
    char source_sha256[65];
    double gravity[3];
    AsmSource gravity_source;
    AsmBody *bodies;
    int nbodies, cap_bodies;
    AsmJoint *joints;
    int njoints, cap_joints;
    AsmCoupling *couplings;
    int ncouplings, cap_couplings;
    AsmNote *assumptions;
    int nassumptions, cap_assumptions;
    AsmNote *unsupported;
    int nunsupported, cap_unsupported;
    AsmGeom *environment; /* static world shapes (planes, boxes, ...) */
    int nenvironment;
} Assembly;

typedef struct AsmLoadOptions {
    const char *root_joint; /* URDF: "fixed" or "free" connection of the root link to the world; NULL = missing input */
    int npackages;          /* URDF package://name/ resolution */
    const char *const *package_names;
    const char *const *package_dirs;
    bool require_units;       /* plain numbers of dimensional quantities need a declared unit (inline MCP definitions) */
    uint64_t max_file_bytes;  /* 0 = 64 MiB */
    uint32_t max_triangles;   /* 0 = 5 M */
} AsmLoadOptions;

Assembly *asm_new(const char *name);
void asm_free(Assembly *a);
int asm_body_index(const Assembly *a, const char *name);
int asm_joint_index(const Assembly *a, const char *name);
AsmBody *asm_add_body(Assembly *a, const char *name);       /* NULL on duplicate name or allocation failure */
AsmJoint *asm_add_joint(Assembly *a, const char *name, MbJointType type, int parent, int child);
void asm_note_assumption(Assembly *a, const char *subject, AsmSource src, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void asm_note_unsupported(Assembly *a, const char *subject, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* native format */
Assembly *asm_from_json(const JsonValue *doc, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d);
/* adds or replaces (by name, keeping its index) one body or joint given in the native format; units as in a document.
 * On failure the assembly may be partly modified: edit a copy (asm_clone) when the change must be atomic. */
bool asm_body_set_json(Assembly *a, const JsonValue *body, const JsonValue *units, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d);
/* partial update of an existing body (a new body is created as asm_body_set_json does): the fields present in patch replace
 * the body's fields (mass_properties, part, frames, material, manufacturing, and each geometry role that is named); absent
 * fields are kept */
bool asm_body_update_json(Assembly *a, const JsonValue *patch, const JsonValue *units, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d);
bool asm_joint_set_json(Assembly *a, const JsonValue *joint, const JsonValue *units, const AsmLoadOptions *opt, MechDiag *d);
Assembly *asm_clone(const Assembly *a); /* through the canonical document */
/* replaces the static environment shapes (array of geometry objects with type, pose, size, friction, restitution) */
bool asm_environment_set_json(Assembly *a, const JsonValue *shapes, const JsonValue *units, const AsmLoadOptions *opt, MechDiag *d);
Assembly *asm_load_json(const char *path, const AsmLoadOptions *opt, MechDiag *d);
JsonValue *asm_to_json(const Assembly *a); /* canonical form: SI numbers, explicit provenance */
/* URDF subset (see urdf.c for the supported elements) */
Assembly *asm_load_urdf(const char *path, const AsmLoadOptions *opt, MechDiag *d);
Assembly *asm_from_urdf_text(const char *text, size_t len, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d);
/* by extension: .json native, .urdf or .xml URDF */
Assembly *asm_load_file(const char *path, const AsmLoadOptions *opt, MechDiag *d);

/* an STL part as a body whose frame is the STL file frame (scaled to metres); mass properties from the closed mesh with the
 * given fill model. Parts exported from CAD in one common frame can then be joined with identical frames on both sides. */
AsmBody *asm_add_stl_body(Assembly *a, const char *name, const char *path, const char *units, const FillSpec *fill, AsmSource fill_source,
                          const AsmLoadOptions *opt, MechDiag *d);
/* mass properties of a closed mesh file placed in a body (pose: mesh in body coordinates) */
bool asm_mesh_mass_properties(const char *path, const char *units, const MPose *pose, const FillSpec *fill, const AsmLoadOptions *opt,
                              MassProperties *out, MechDiag *d, const char *subject);

/* multibody definition: massless links fixed to a parent become frames of that parent (reported) */
MbModelDef *asm_to_model(const Assembly *a, MechDiag *d);

JsonValue *asm_summary_json(const Assembly *a);
