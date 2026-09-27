/* mechspec.h - the persisted mechanical study: actuators, sensors, controllers with their references, run settings and
 * snapshot instants ("navier-mech-study" version 1).
 *
 * Components are stored as given (validated for structure) and refer to joints, bodies, actuators and sensors by name.
 * mspec_resolve parses every quantity against the model it will run on, because the dimension of an effort, position or
 * gain depends on the joint type (revolute: N m, rad; prismatic: N, m). Plain numbers are in the unit declared under
 * "units" for their quantity (SI when undeclared); strings carry their own unit. mspec_canonical_json writes the resolved
 * study with SI numbers: that form is stored with every run, so the run can be repeated without the conversation. */
#pragma once

#include <stdbool.h>

#include "../core/json.h"
#include "mechdiag.h"
#include "mechsim.h"
#include "multibody.h"

enum { MSPEC_MAX_SNAPSHOTS = 64 };

typedef struct MechStudy {
    char name[MB_NAME];
    bool require_units; /* persisted as "plain_numbers": "require_units" (MCP) or "si" (documents) */
    JsonValue *units;       /* object (may be empty) */
    JsonValue *settings;    /* object */
    JsonValue *actuators;   /* arrays of component objects */
    JsonValue *sensors;
    JsonValue *controllers;
    JsonValue *snapshots;   /* array of times */
    /* resolved by mspec_resolve */
    bool resolved;
    StudySettings st;
    ActuatorDef *act;
    int nact;
    SensorDef *sen;
    int nsen;
    ControllerDef *ctl;
    int nctl;
    int nsnapshots;
    double snapshot_s[MSPEC_MAX_SNAPSHOTS];
} MechStudy;

MechStudy *mspec_new(const char *name);
void mspec_free(MechStudy *s);
MechStudy *mspec_from_json(const JsonValue *doc, MechDiag *d); /* stored form */
JsonValue *mspec_to_json(const MechStudy *s);                    /* stored form (units as given) */
/* kind: "actuator", "sensor" or "controller"; replaces a component with the same name */
bool mspec_set_component(MechStudy *s, const char *kind, const JsonValue *def, MechDiag *d);
bool mspec_remove_component(MechStudy *s, const char *kind, const char *name);
bool mspec_set_settings(MechStudy *s, const JsonValue *settings, const JsonValue *snapshots, MechDiag *d);
void mspec_set_units(MechStudy *s, const JsonValue *units);
/* parses quantities and resolves names against the model definition */
bool mspec_resolve(MechStudy *s, const MbModelDef *def, MechDiag *d);
/* resolved study with SI numbers and names (requires mspec_resolve) */
JsonValue *mspec_canonical_json(const MechStudy *s, const MbModelDef *def);
