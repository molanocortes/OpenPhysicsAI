/* mgearpart.h - gears as parts of a mechanism, and the contact between two gears in mesh, for assemblies of any number
 * of gears: planetary stages, multi-stage trains, compound planets.
 *
 * A model (GearModel) is one gear's geometry (external or internal, spur or helical: gear.h), its hexahedral mesh
 * (gearmesh.h) and its stiffness factorised once in its own frame (mbody.h), with the condensed compliance of its
 * involute flanks on the sides that can touch. Identical gears (the planets of a stage) share one model: one
 * factorisation, one compliance.
 *
 * A part (GearPart) is a model placed in the mechanism: its axis at a world position (possibly carried round by a
 * carrier's rotation), its teeth turned to their phase, and its rigid motions, each a degree of freedom of the contact
 * problem (mcontact_solve_q) or held: its own rotation about its axis, its translation (a floating part), and the
 * rotation of the carrier that carries its axis.
 *
 * The contact between two parts in mesh (gearpart_mesh_contacts), per layer of nodes across the face: every pair of
 * teeth that may touch is found at the closest approach of the exact transverse outlines at that layer's position along
 * the axis (a helical gear's section turned by its twist there), searched both ways so that a tip corner on a flank is
 * found. Each layer's contact is a Hertzian strip along the profile at least an element wide, its near field (narrower
 * than that) added as the half-space's line-contact compliance; its gap and force act along the helicoid's normal, which
 * leans out of the transverse plane by the base helix angle (zero for spur gears), so a helical mesh pushes its gears
 * apart along the axis too. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gear.h"
#include "gearmesh.h"
#include "mbody.h"
#include "mcontact.h"

typedef struct PartFlank {
    int *node;   /* surface nodes of one tooth's side on one layer, sorted by outline radius */
    double *r;
    int n;
} PartFlank;

typedef struct GearModel {
    GearGeometry geo;
    GearMesh mesh;       /* model frame: the middle tooth (tooth 0 of a whole gear) on +y */
    MBody body;
    double E, nu;
    int nlayer, mid;     /* layers across the face; the index of the middle tooth (0 for a whole gear) */
    double *ocum;        /* arc length along geo.right from the space middle */
    double *orad;        /* the radius of each point of geo.right */
    double step_half;    /* half the longest step between those points from the form point to the tip corner */
    int run;             /* +1: their radii grow from the form point to the tip corner (an external gear), -1: they
                            fall (an internal gear), 0: neither (the nearest point is then searched step by step) */
    PartFlank *fl[2];    /* [0] the left sides, [1] the right: [tooth * nlayer + layer] */
    bool held_all_round; /* its bore (a ring's outer rim) held all round, not at lugs */
    int refs;
} GearModel;

typedef struct GearPart {
    char name[48];
    GearModel *m;
    double ax, ay, az;   /* the axis at the reference pose, world (az: the face's start along the axis) */
    double rot0;         /* the model frame's turn that phases the teeth at the reference pose */
    int dof, dofx, dofy; /* own rotation (absolute, counter-clockwise), translation; -1: not a degree of freedom */
    int cdof;            /* the carrier whose rotation carries this axis round (kx, ky), -1: none */
    double kx, ky;
    double cx, cy;       /* the axis now (set by the mechanism before a solve) */
} GearPart;

/* How a model is held where its mesh is tagged GM_BORE (an external gear's bore, an internal gear's outer rim): all
 * round (lugs 0), or only at `lugs` evenly spaced lugs (a ring in a splined housing), each lug_deg wide, the first
 * centred at phase_deg in the model's frame; radial_free: the lugs hold the surface along the circumference and the
 * axis only (stiff springs), so the ring may move radially there, as a ring floating in its splines does. */
typedef struct GearHold {
    int lugs;
    double lug_deg, phase_deg;
    bool radial_free;
} GearHold;

/* Builds a model: the geometry (cutter non-NULL: an internal gear cut by that shaper), its mesh, its stiffness held
 * as `hold` says (NULL: all round) and, if hold_cuts, at a sector's cut faces; and the condensed compliance of the
 * involute flanks of zone_sides (GM_LEFT, GM_RIGHT or both), along the helicoid's inward normal. refs starts at 1. */
GearModel *gearmodel_build(const GearSpec *spec, const GearSpec *cutter, const GearMeshSpec *ms, double E, double nu, bool hold_cuts, int zone_sides,
                           char *err, size_t errlen);
/* The same in two steps: the model without its compliance, then the compliance of the flanks of the teeth marked in
 * `teeth` (NULL: all of them). A whole gear held all round with every tooth in its zone gets the cyclic compliance;
 * otherwise each zone node costs a solve and the compliance is stored whole. */
GearModel *gearmodel_create(const GearSpec *spec, const GearSpec *cutter, const GearMeshSpec *ms, double E, double nu, bool hold_cuts,
                            const GearHold *hold, char *err, size_t errlen);
bool gearmodel_set_zone(GearModel *m, int zone_sides, const unsigned char *teeth, char *err, size_t errlen);
void gearmodel_release(GearModel *m); /* frees it when the last part lets go */

/* a part of model m: its axis, and the polar angle of its middle tooth's centre line (tooth 0 of a whole gear) at the
 * reference pose; held until the mechanism gives it degrees of freedom */
void gearpart_init(GearPart *p, const char *name, GearModel *m, double ax, double ay, double az, double tooth_angle);
/* the polar angle (about its axis) of tooth t's centre line at the reference pose */
double gearpart_tooth_angle(const GearPart *p, int t);

/* a point of tooth t's side (GM_LEFT or GM_RIGHT) at outline radius r of the transverse section at zf along the face,
 * in the world at the part's turn phi, with the transverse outward normal */
bool gearpart_point(const GearPart *p, int side, int tooth, double phi, double zf, double r, double *wx, double *wy, double *nx, double *ny);
/* a world point (in the section at zf) against tooth t's side: the distance along the transverse outward normal
 * (positive outside the material), the foot's radius and normal; inside is false past the active flank's ends */
double gearpart_project(const GearPart *p, int side, int tooth, double phi, double zf, double wx, double wy, double *radius, double *nx, double *ny,
                        bool *inside);
/* the world position of a model node at the part's turn phi (the frame's pictures) */
void gearpart_world(const GearPart *p, double phi, int node, double out[3]);

typedef struct NearField {
    double *R, *a, *w; /* per contact: relative curvature radius, strip half-width, contact length it stands for */
    int cap;
} NearField;

/* The contacts between part A's side sideA and part B's side sideB at turns phiA, phiB: candidate tooth pairs within
 * gap_cut of their mesh's smallest gap. Each contact's row of M holds the normal gap's derivative with every rigid
 * motion of both parts. bodyA, bodyB: the parts' indices among the contact problem's bodies. q_est: the expected load
 * per unit length of contact line (N/m), for the strip's width. Appends to cs (cs->ndof set by the caller) and nf.
 * Returns the number of contacts added, or -1 with err. */
int gearpart_mesh_contacts(MContactSet *cs, NearField *nf, const GearPart *A, int bodyA, int sideA, double phiA, const GearPart *B, int bodyB, int sideB,
                           double phiB, double q_est, double gap_cut, char *err, size_t errlen);
/* the near field's compliance on every contact of cs from its load (lambda NULL: q_default per unit length) */
void gearpart_near_field(MContactSet *cs, const NearField *nf, const double *lambda, double q_default, double Estar);
void nearfield_free(NearField *nf);
