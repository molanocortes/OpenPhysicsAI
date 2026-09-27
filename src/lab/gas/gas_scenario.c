/* gas_scenario.c - a gas run described in JSON (the format agents write; docs/lab/gas.md has every key).
 *
 * Every number carries its unit in its key (length_m, pressure_pa, temperature_k, ...): a key without a unit is not
 * read. A state is either a free stream (mach, pressure_pa, temperature_k, gamma, angle_deg) or primitive
 * (density_kg_m3, velocity_m_s [u, v], pressure_pa, gamma). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "gas.h"
#include "gas_scenario.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool read_state(const JsonValue *o, double R, GasPrim *w, char *err, size_t errlen, const char *what) {
    if (!o || o->type != JSON_OBJECT) {
        snprintf(err, errlen, "%s: an object is required", what);
        return false;
    }
    w->gamma = json_get_num(o, "gamma", 1.4);
    if (!(w->gamma > 1.0 && w->gamma < 3.0)) {
        snprintf(err, errlen, "%s: gamma must lie between 1 and 3", what);
        return false;
    }
    if (json_get(o, "mach")) {
        double M = json_get_num(o, "mach", 0), p = json_get_num(o, "pressure_pa", -1), T = json_get_num(o, "temperature_k", -1);
        double ang = json_get_num(o, "angle_deg", 0) * M_PI / 180;
        if (!(M >= 0) || !(p > 0) || !(T > 0) || !(R > 0)) {
            snprintf(err, errlen, "%s: a free stream needs mach, pressure_pa, temperature_k and the gas constant", what);
            return false;
        }
        w->rho = p / (R * T);
        w->p = p;
        double c = sqrt(w->gamma * p / w->rho);
        w->u = M * c * cos(ang), w->v = M * c * sin(ang);
        return true;
    }
    w->rho = json_get_num(o, "density_kg_m3", -1);
    w->p = json_get_num(o, "pressure_pa", -1);
    if (!(w->p > 0) && json_get(o, "temperature_k") && w->rho > 0) w->p = w->rho * R * json_get_num(o, "temperature_k", 0);
    if (!(w->rho > 0) && json_get(o, "temperature_k") && w->p > 0) w->rho = w->p / (R * json_get_num(o, "temperature_k", 1));
    double v[2] = {0, 0};
    const JsonValue *vel = json_get(o, "velocity_m_s");
    if (vel && !json_get_numbers(vel, v, 2)) {
        snprintf(err, errlen, "%s: velocity_m_s is [u, v]", what);
        return false;
    }
    w->u = v[0], w->v = v[1];
    if (!(w->rho > 0) || !(w->p > 0)) {
        snprintf(err, errlen, "%s: density_kg_m3 and pressure_pa (or one of them and temperature_k) are required", what);
        return false;
    }
    return true;
}

static int boundary_kind(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "inflow")) return GAS_INFLOW;
    if (!strcmp(s, "outflow")) return GAS_OUTFLOW;
    if (!strcmp(s, "wall")) return GAS_WALL;
    if (!strcmp(s, "axis")) return GAS_AXIS;
    return -1;
}

void gas_scenario_free(GasScenario *sc) {
    for (int k = 0; k < sc->spec.nbodies; k++) free(sc->spec.bodies[k].pts);
    memset(sc, 0, sizeof *sc);
}

bool gas_scenario_parse(const JsonValue *root, GasScenario *sc, char *err, size_t errlen) {
    memset(sc, 0, sizeof *sc);
    GasSpec *s = &sc->spec;
    gas_spec_defaults(s);
    snprintf(sc->title, sizeof sc->title, "%s", json_get_str(root, "title", "gas run"));
    const char *geo = json_get_str(root, "geometry", "planar");
    if (strcmp(geo, "planar") && strcmp(geo, "axisymmetric")) {
        snprintf(err, errlen, "geometry is planar or axisymmetric");
        return false;
    }
    s->axisymmetric = !strcmp(geo, "axisymmetric");
    s->gas_constant = json_get_num(root, "gas_constant_j_kgk", 287.05);
    const JsonValue *grid = json_get(root, "grid");
    if (!grid) {
        snprintf(err, errlen, "grid is required: x0_m, y0_m, length_m, root_blocks [nx, ny], max_level");
        return false;
    }
    s->x0 = json_get_num(grid, "x0_m", 0);
    s->y0 = json_get_num(grid, "y0_m", 0);
    s->length = json_get_num(grid, "length_m", -1);
    double rb[2];
    if (!(s->length > 0) || !json_get_numbers(json_get(grid, "root_blocks"), rb, 2)) {
        snprintf(err, errlen, "grid: length_m and root_blocks [nx, ny] are required");
        return false;
    }
    s->nbx = (int)rb[0], s->nby = (int)rb[1];
    s->max_level = (int)json_get_int(grid, "max_level", 3);
    s->body_level = (int)json_get_int(grid, "body_level", -1);
    s->regrid_every = (int)json_get_int(grid, "regrid_every", 4);
    s->refine_above = json_get_num(grid, "refine_above", 0.8);
    s->coarsen_below = json_get_num(grid, "coarsen_below", 0.2);
    s->refine_jump = json_get_num(grid, "refine_jump", 0.0);
    const JsonValue *bc = json_get(root, "boundaries");
    static const char *SIDES[4] = {"x_low", "x_high", "y_low", "y_high"};
    for (int k = 0; k < 4; k++) {
        int b = boundary_kind(json_get_str(bc, SIDES[k], s->axisymmetric && k == 2 ? "axis" : "outflow"));
        if (b < 0) {
            snprintf(err, errlen, "boundaries.%s is inflow, outflow, wall or axis", SIDES[k]);
            return false;
        }
        s->boundary[k] = b;
    }
    const JsonValue *fs = json_get(root, "freestream");
    if (fs) {
        if (!read_state(fs, s->gas_constant, &s->inflow, err, errlen, "freestream")) return false;
        s->initial = s->inflow;
    }
    const JsonValue *ini = json_get(root, "initial");
    if (ini && !read_state(ini, s->gas_constant, &s->initial, err, errlen, "initial")) return false;
    if (!fs && !ini) {
        snprintf(err, errlen, "freestream or initial is required");
        return false;
    }
    if (!fs) s->inflow = s->initial;
    const JsonValue *regs = json_get(root, "regions");
    for (size_t i = 0; regs && i < json_len(regs); i++) {
        if (s->nregions == GAS_MAX_REGIONS) {
            snprintf(err, errlen, "at most %d regions", GAS_MAX_REGIONS);
            return false;
        }
        const JsonValue *r = json_at(regs, i);
        GasRegion *g = &s->regions[s->nregions];
        double v[4];
        if (json_get_numbers(json_get(r, "box_m"), v, 4)) g->kind = GAS_REGION_BOX, g->x0 = v[0], g->y0 = v[1], g->x1 = v[2], g->y1 = v[3];
        else if (json_get_numbers(json_get(r, "circle_m"), v, 3)) g->kind = GAS_REGION_CIRCLE, g->cx = v[0], g->cy = v[1], g->r = v[2];
        else {
            snprintf(err, errlen, "regions[%zu]: box_m [x0, y0, x1, y1] or circle_m [cx, cy, r]", i);
            return false;
        }
        if (!read_state(json_get(r, "state"), s->gas_constant, &g->state, err, errlen, "regions[].state")) return false;
        s->nregions++;
    }
    const JsonValue *bodies = json_get(root, "bodies");
    for (size_t i = 0; bodies && i < json_len(bodies); i++) {
        if (s->nbodies == GAS_MAX_BODIES) {
            snprintf(err, errlen, "at most %d bodies", GAS_MAX_BODIES);
            return false;
        }
        const JsonValue *b = json_at(bodies, i);
        GasBody *g = &s->bodies[s->nbodies];
        double v[3];
        const JsonValue *poly = json_get(b, "polygon_m");
        if (json_get_numbers(json_get(b, "circle_m"), v, 3)) {
            g->kind = GAS_BODY_CIRCLE, g->cx = v[0], g->cy = v[1], g->r = v[2];
        } else if (poly && json_len(poly) >= 3) {
            g->kind = GAS_BODY_POLYGON;
            g->npts = (int)json_len(poly);
            g->pts = malloc((size_t)g->npts * 2 * sizeof(double));
            for (int k = 0; k < g->npts; k++)
                if (!json_get_numbers(json_at(poly, (size_t)k), &g->pts[2 * k], 2)) {
                    snprintf(err, errlen, "bodies[%zu].polygon_m[%d] is [x, y]", i, k);
                    s->nbodies++;
                    return false;
                }
        } else {
            snprintf(err, errlen, "bodies[%zu]: circle_m [cx, cy, r] or polygon_m [[x, y], ...] (counter-clockwise)", i);
            return false;
        }
        s->nbodies++;
    }
    const JsonValue *run = json_get(root, "run");
    sc->end_time = json_get_num(run, "end_time_s", -1);
    sc->frames = (int)json_get_int(run, "frames", 60);
    s->cfl = json_get_num(run, "cfl", 0.4);
    snprintf(sc->fields, sizeof sc->fields, "%s", json_get_str(run, "fields", "rho,p,mach,solid"));
    if (!(sc->end_time > 0) || sc->frames < 1) {
        snprintf(err, errlen, "run: end_time_s > 0 and frames >= 1 are required");
        return false;
    }
    return true;
}
