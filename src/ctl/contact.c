/* contact.c - thermal interface topology and the thermal node split (see contact.h) */
#include "contact.h"
#include "../fem/hex8.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* neighbour offsets of the hex8 local faces, as in hexmesh.c: 0 -z, 1 +z, 2 -y, 3 +x, 4 +y, 5 -x */
static const int FACE_DIR[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};

static int face_axis(int lf) { return lf <= 1 ? 2 : (lf == 2 || lf == 4 ? 1 : 0); }

static int body_bit(const HexMesh *hm, int e) { return hm->region[e] == HEX_REGION_PLATE || hm->body[e] < 0 ? 16 : hm->body[e]; }

void contact_topology_free(ContactTopology *t) {
    free(t->elem_a), free(t->elem_b), free(t->face_a), free(t->shared);
    memset(t, 0, sizeof *t);
}

bool contact_topology(const HexMesh *hm, int ba, int bb, ContactTopology *t) {
    memset(t, 0, sizeof *t);
    t->body_a = ba, t->body_b = bb;
    int nx = hm->dims[0], ny = hm->dims[1], nz = hm->dims[2], cap = 0;
    double nsum[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) t->bmin[k] = INFINITY, t->bmax[k] = -INFINITY;
    for (int e = 0; e < hm->nelems; e++) {
        if (body_bit(hm, e) != ba) continue;
        for (int lf = 0; lf < 6; lf++) {
            int i = hm->ijk[3 * e] + FACE_DIR[lf][0], j = hm->ijk[3 * e + 1] + FACE_DIR[lf][1], k = hm->ijk[3 * e + 2] + FACE_DIR[lf][2];
            if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) continue;
            int n = hm->cell_elem[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)];
            if (n < 0 || body_bit(hm, n) != bb) continue;
            if (t->nfaces == cap) {
                cap = cap ? 2 * cap : 256;
                int *a = realloc(t->elem_a, (size_t)cap * sizeof(int)), *b = realloc(t->elem_b, (size_t)cap * sizeof(int));
                unsigned char *f = realloc(t->face_a, (size_t)cap);
                if (a) t->elem_a = a;
                if (b) t->elem_b = b;
                if (f) t->face_a = f;
                if (!a || !b || !f) return false;
            }
            t->elem_a[t->nfaces] = e, t->elem_b[t->nfaces] = n, t->face_a[t->nfaces] = (unsigned char)lf;
            t->nfaces++;
            int ax = face_axis(lf);
            double area = hm->h[(ax + 1) % 3] * hm->h[(ax + 2) % 3];
            t->area += area;
            nsum[ax] += area * FACE_DIR[lf][ax];
            for (int q = 0; q < 3; q++) {
                double lo = hm->origin[q] + hm->ijk[3 * e + q] * hm->h[q], hi = lo + hm->h[q];
                if (q == ax) lo = hi = FACE_DIR[lf][ax] > 0 ? hi : lo; /* the face plane */
                t->bmin[q] = fmin(t->bmin[q], lo), t->bmax[q] = fmax(t->bmax[q], hi);
            }
        }
    }
    double nl = sqrt(nsum[0] * nsum[0] + nsum[1] * nsum[1] + nsum[2] * nsum[2]);
    for (int q = 0; q < 3; q++) t->normal[q] = nl > 0 ? nsum[q] / nl : 0;
    /* nodes used by both bodies, and those a third body (or the plate) also uses */
    uint32_t *mask = calloc((size_t)hm->nnodes, sizeof(uint32_t));
    unsigned char *onface = calloc((size_t)hm->nnodes, 1);
    if (!mask || !onface) {
        free(mask), free(onface);
        return false;
    }
    for (int e = 0; e < hm->nelems; e++)
        for (int a = 0; a < 8; a++) mask[hm->conn[8 * (size_t)e + a]] |= 1u << body_bit(hm, e);
    for (int f = 0; f < t->nfaces; f++) {
        const int *fn = HEX8_FACE_NODES[t->face_a[f]];
        for (int q = 0; q < 4; q++) onface[hm->conn[8 * (size_t)t->elem_a[f] + fn[q]]] = 1;
    }
    uint32_t pair = (1u << ba) | (1u << bb);
    for (int n = 0; n < hm->nnodes; n++)
        if ((mask[n] & pair) == pair) t->nshared++;
    t->shared = malloc((size_t)(t->nshared ? t->nshared : 1) * sizeof(int));
    if (!t->shared) {
        free(mask), free(onface);
        return false;
    }
    int k = 0;
    for (int n = 0; n < hm->nnodes; n++)
        if ((mask[n] & pair) == pair) {
            t->shared[k++] = n;
            t->nface_nodes += onface[n];
            if (mask[n] & ~pair) t->njunction++;
        }
    free(mask), free(onface);
    if (ba < 16 && bb < 16) t->overlap_cells = hm->overlap_pairs[ba > bb ? ba : bb][ba > bb ? bb : ba];
    if (t->nfaces == 0) {
        /* separation of the two bodies' element boxes */
        double lo[2][3], hi[2][3];
        for (int s = 0; s < 2; s++)
            for (int q = 0; q < 3; q++) lo[s][q] = INFINITY, hi[s][q] = -INFINITY;
        for (int e = 0; e < hm->nelems; e++) {
            int bit = body_bit(hm, e), s = bit == ba ? 0 : (bit == bb ? 1 : -1);
            if (s < 0) continue;
            for (int q = 0; q < 3; q++) {
                double l = hm->origin[q] + hm->ijk[3 * e + q] * hm->h[q];
                lo[s][q] = fmin(lo[s][q], l), hi[s][q] = fmax(hi[s][q], l + hm->h[q]);
            }
        }
        double g = 0;
        for (int q = 0; q < 3; q++) g = fmax(g, fmax(lo[1][q] - hi[0][q], lo[0][q] - hi[1][q]));
        t->gap = isfinite(g) ? g : 0;
        for (int q = 0; q < 3; q++) t->bmin[q] = t->bmax[q] = 0;
    }
    return true;
}

JsonValue *contact_topology_json(const ContactTopology *t) {
    JsonValue *o = json_object();
    json_set_int(o, "faces", t->nfaces);
    json_set_number(o, "area_mm2", 1e6 * t->area);
    json_set_int(o, "shared_nodes", t->nshared);
    json_set_int(o, "face_nodes", t->nface_nodes);
    json_set_int(o, "junction_nodes", t->njunction);
    json_set_int(o, "overlapping_cells", t->overlap_cells);
    if (t->nfaces) {
        json_set(o, "bounds_min_mm", json_vec3(1e3 * t->bmin[0], 1e3 * t->bmin[1], 1e3 * t->bmin[2]));
        json_set(o, "bounds_max_mm", json_vec3(1e3 * t->bmax[0], 1e3 * t->bmax[1], 1e3 * t->bmax[2]));
        json_set(o, "normal_a_to_b", json_vec3(t->normal[0], t->normal[1], t->normal[2]));
    } else {
        json_set_number(o, "gap_mm", 1e3 * t->gap);
    }
    return o;
}

const char *contact_topology_problem(const ContactTopology *t, ContactModel model, const char *na, const char *nb, char *buf, size_t cap) {
    if (t->overlap_cells > 0) {
        snprintf(buf, cap,
                 "'%s' and '%s' overlap in %d mesh cells: the mesher gives each contested cell to one body, so the interface is not defined by "
                 "the geometry. Move the bodies apart or trim one so that they meet face to face",
                 na, nb, t->overlap_cells);
        return buf;
    }
    if (t->nfaces == 0) {
        if (t->nshared > 0)
            snprintf(buf, cap, "'%s' and '%s' touch only along edges or corners (%d shared nodes, no shared face): an interface needs a common face", na, nb,
                     t->nshared);
        else if (t->gap > 0)
            snprintf(buf, cap,
                     "'%s' and '%s' do not touch in the mesh: their meshes are %.4g mm apart. Only coincident interfaces are supported; close the gap "
                     "in the geometry or refine the mesh if the gap is below one element",
                     na, nb, 1e3 * t->gap);
        else
            snprintf(buf, cap, "'%s' and '%s' share no mesh face (the voxel mesh may separate surfaces that are closer than one element)", na, nb);
        return buf;
    }
    if (model != CONTACT_PERFECT && t->njunction > 0) {
        snprintf(buf, cap,
                 "a third body or the build plate meets the interface between '%s' and '%s' at %d nodes; a finite-resistance interface at a "
                 "junction of three bodies is not supported",
                 na, nb, t->njunction);
        return buf;
    }
    return NULL;
}

/* ---- the thermal split ---------------------------------------------------------------------------------------- */

void contact_split_free(ContactSplit *s) {
    free(s->xyz), free(s->tnode_mesh), free(s->pairs), free(s->pair_contact);
    memset(s, 0, sizeof *s);
}

bool contact_split(const HexMesh *hm, const double *xyz_in, int *conn, int ncontacts, const ThermalContact *contacts, const int *bodies_a,
                   const int *bodies_b, ContactSplit *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    int nn0 = hm->nnodes;
    int *twin = malloc((size_t)nn0 * sizeof(int)), *twin_owner = malloc((size_t)nn0 * sizeof(int)), *pair_of = malloc((size_t)nn0 * sizeof(int));
    ContactTopology *tops = calloc((size_t)(ncontacts > 0 ? ncontacts : 1), sizeof *tops);
    bool ok = twin && twin_owner && pair_of && tops;
    if (!ok) snprintf(err, errlen, "out of memory splitting thermal interfaces");
    for (int n = 0; ok && n < nn0; n++) twin[n] = -1, twin_owner[n] = -1, pair_of[n] = -1;
    int extra = 0;
    for (int i = 0; ok && i < ncontacts; i++) {
        if (contacts[i].model == CONTACT_PERFECT) continue;
        if (i >= THERMAL_MAX_GROUPS) {
            snprintf(err, errlen, "at most %d thermal interfaces are supported", THERMAL_MAX_GROUPS);
            ok = false;
            break;
        }
        if (!contact_topology(hm, bodies_a[i], bodies_b[i], &tops[i])) {
            snprintf(err, errlen, "out of memory analysing interface '%s'", contacts[i].name);
            ok = false;
            break;
        }
        char buf[512];
        if (contact_topology_problem(&tops[i], contacts[i].model, contacts[i].body_a, contacts[i].body_b, buf, sizeof buf)) {
            snprintf(err, errlen, "interface '%s': %s", contacts[i].name, buf);
            ok = false;
            break;
        }
        for (int s = 0; s < tops[i].nshared; s++) {
            int n = tops[i].shared[s];
            if (twin[n] >= 0) {
                snprintf(err, errlen, "node %d lies on interfaces '%s' and '%s'; nodes shared by two interfaces are not supported", n,
                         contacts[twin_owner[n]].name, contacts[i].name);
                ok = false;
                break;
            }
            twin[n] = nn0 + extra++;
            twin_owner[n] = i;
        }
    }
    if (ok) {
        out->nnodes = nn0 + extra;
        out->xyz = malloc(3 * (size_t)out->nnodes * sizeof(double));
        out->tnode_mesh = malloc((size_t)out->nnodes * sizeof(int));
        if (!out->xyz || !out->tnode_mesh) {
            snprintf(err, errlen, "out of memory for %d thermal nodes", out->nnodes);
            ok = false;
        }
    }
    if (ok) {
        memcpy(out->xyz, xyz_in, 3 * (size_t)nn0 * sizeof(double));
        for (int n = 0; n < nn0; n++) {
            out->tnode_mesh[n] = n;
            if (twin[n] >= 0) {
                memcpy(out->xyz + 3 * (size_t)twin[n], xyz_in + 3 * (size_t)n, 3 * sizeof(double));
                out->tnode_mesh[twin[n]] = n;
            }
        }
        /* the elements of body b take the twins of the nodes they share with body a */
        for (int e = 0; e < hm->nelems; e++) {
            int bit = body_bit(hm, e);
            for (int a = 0; a < 8; a++) {
                int n = conn[8 * (size_t)e + a];
                if (n < nn0 && twin[n] >= 0 && bit == bodies_b[twin_owner[n]]) conn[8 * (size_t)e + a] = twin[n];
            }
        }
        /* conductance pairs on interface faces: a quarter of each face area to each of its four node pairs */
        int cap = 0;
        for (int i = 0; ok && i < ncontacts; i++) {
            if (contacts[i].model == CONTACT_PERFECT) continue;
            out->contact_area[i] = tops[i].area;
            out->contact_faces[i] = tops[i].nfaces;
            /* insulated interfaces get pairs of zero conductance: no heat crosses (the solver skips them), but the node
             * correspondence lets both sides' temperatures be reported */
            double hcond = contact_conductance(&contacts[i]);
            if (!(hcond >= 0) || !isfinite(hcond)) hcond = 0;
            for (int f = 0; ok && f < tops[i].nfaces; f++) {
                int lf = tops[i].face_a[f], ax = face_axis(lf);
                double quarter = 0.25 * hm->h[(ax + 1) % 3] * hm->h[(ax + 2) % 3];
                const int *fn = HEX8_FACE_NODES[lf];
                for (int q = 0; q < 4; q++) {
                    int n = conn[8 * (size_t)tops[i].elem_a[f] + fn[q]]; /* body a keeps the mesh node */
                    if (n >= nn0 || twin[n] < 0) {
                        snprintf(err, errlen, "interface '%s': face node %d has no twin (inconsistent split)", contacts[i].name, n);
                        ok = false;
                        break;
                    }
                    if (pair_of[n] < 0) {
                        if (out->npairs == cap) {
                            cap = cap ? 2 * cap : 256;
                            ThermalInterfaceNode *np = realloc(out->pairs, (size_t)cap * sizeof *np);
                            int *nc = realloc(out->pair_contact, (size_t)cap * sizeof(int));
                            if (np) out->pairs = np;
                            if (nc) out->pair_contact = nc;
                            if (!np || !nc) {
                                snprintf(err, errlen, "out of memory for interface pairs");
                                ok = false;
                                break;
                            }
                        }
                        out->pairs[out->npairs] = (ThermalInterfaceNode){n, twin[n], hcond, 0};
                        out->pair_contact[out->npairs] = i;
                        pair_of[n] = out->npairs++;
                    }
                    out->pairs[pair_of[n]].area += quarter;
                }
            }
        }
    }
    for (int i = 0; tops && i < ncontacts; i++) contact_topology_free(&tops[i]);
    free(tops), free(twin), free(twin_owner), free(pair_of);
    if (!ok) contact_split_free(out);
    return ok;
}
