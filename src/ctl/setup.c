/* setup.c - setup entry helpers */
#include "setup.h"

#include <stdlib.h>
#include <string.h>

static const char *BC_NAMES[BC_KIND_COUNT] = {"fixed",       "displacement", "frictionless_support", "force",     "pressure",  "traction",
                                              "gravity",     "temperature",  "heat_flux",            "convection", "radiation", "heat_source"};

const char *bc_kind_name(BcKind k) { return k >= 0 && k < BC_KIND_COUNT ? BC_NAMES[k] : "?"; }

int bc_kind_from_name(const char *s) {
    for (int i = 0; s && i < BC_KIND_COUNT; i++)
        if (!strcmp(s, BC_NAMES[i])) return i;
    return -1;
}

bool bc_is_constraint(BcKind k) { return k == BC_FIXED || k == BC_DISPLACEMENT || k == BC_FRICTIONLESS; }

bool bc_is_thermal(BcKind k) { return k >= BC_TEMPERATURE && k <= BC_HEAT_SOURCE; }

double bc_schedule_factor(const BoundaryCondition *b, double t) {
    if (b->nschedule <= 0) return 1;
    int i = 0;
    while (i + 1 < b->nschedule && b->schedule_t[i + 1] <= t) i++;
    return b->schedule_factor[i];
}

static const char *CONTACT_NAMES[CONTACT_MODEL_COUNT] = {"perfect", "conductance", "thin_layer", "insulated"};

const char *contact_model_name(ContactModel m) { return m >= 0 && m < CONTACT_MODEL_COUNT ? CONTACT_NAMES[m] : "?"; }

int contact_model_from_name(const char *s) {
    for (int i = 0; s && i < CONTACT_MODEL_COUNT; i++)
        if (!strcmp(s, CONTACT_NAMES[i])) return i;
    return -1;
}

double contact_conductance(const ThermalContact *c) {
    switch (c->model) {
    case CONTACT_CONDUCTANCE: return c->conductance;
    case CONTACT_THIN_LAYER: return c->layer_thickness > 0 ? c->layer_conductivity / c->layer_thickness : 0;
    case CONTACT_INSULATED: return 0;
    default: return 1.0 / 0.0;
    }
}

void selection_free_data(Selection *s) {
    json_free(s->query);
    free(s->tris);
    free(s->patches);
    memset(s, 0, sizeof *s);
}

void mesh_state_free(MeshState *m) {
    hexmesh_free(&m->hm);
    m->valid = false;
}
