/* gearmesh.h - a structured hexahedral mesh of a spur gear, or of a sector of its teeth, from gear.h's exact outline.
 *
 * Each tooth pitch is two blocks meshed by transfinite interpolation. The tooth block lies between the two involute
 * flanks, from the form circle to the tip, its columns graded toward both flanks so that the elements under a contact
 * are thin. The rim block lies under the root: its top edge runs from the middle of one tooth space along the root and
 * up the fillet, across the base of the tooth (the tooth block's bottom row) and down the other fillet; its bottom edge
 * is the bore. Its rows are graded toward the surface, where the bending stress of the fillet peaks. Neighbouring
 * pitches share their edges, and the 2D mesh is extruded through the face width.
 *
 * A helical gear's (gear.h) layers are its transverse section turned by twist z about the axis; its surface nodes keep
 * the transverse outward normal (turned with the layer), from which the helicoid's own normal follows (mgearpart.c).
 *
 * A sector of T teeth is cut along the radial lines through the middle of the tooth spaces at its ends; those cut
 * faces are tagged so a caller can hold them or leave them free, and say which.
 *
 * Node tags (bit flags): flank and fillet surfaces by side (the side of the tooth facing increasing polar angle is
 * "left"), the root between teeth, the tip, the bore, the two end faces, the sector's cut faces. Flank nodes also
 * carry their tooth's index and the outward normal of the exact outline. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gear.h"

enum {
    GM_LEFT = 1,     /* the side of a tooth toward increasing polar angle */
    GM_RIGHT = 2,
    GM_FLANK = 4,    /* involute (or relieved involute) */
    GM_FILLET = 8,   /* trochoidal fillet and root circle */
    GM_TIP = 16,
    GM_BORE = 32,
    GM_END0 = 64,    /* z = 0 face */
    GM_END1 = 128,   /* z = b face */
    GM_CUT = 256,    /* the sector's radial cut faces */
};

typedef struct GearMeshSpec {
    int teeth_modelled; /* teeth in the sector; the gear's own count meshes the whole gear */
    int n_across;       /* elements across a tooth */
    int n_flank;        /* along a flank, form circle to tip */
    int n_fillet;       /* along the root and the fillet of one side, space middle to form circle */
    int n_rim;          /* from the surface down to the bore */
    int n_face;         /* through the face width */
    double flank_grading;  /* the first element across a tooth relative to an even split (< 1 thinner), default 0.25 */
    double rim_grading;    /* the first element under the surface relative to an even split, default 0.25 */
    double bore_blend;     /* 0: bore nodes under the surface nodes' polar angles; 1: evenly spaced; default 0.5 */
    double junction_deg;   /* where the tooth block meets the rim block on the fillet: the outline's inclination
                              from the circumference there, default 40 degrees */
} GearMeshSpec;

typedef struct GearMesh {
    int nnodes, nelems;
    double *xyz;      /* 3 * nnodes, m; gear centre at the origin, axis +z, faces at z = 0 and z = b */
    int *conn;        /* 8 * nelems, hex8 node order of src/fem/hex8.h, positive volume */
    int *tag;         /* per node, GM_* flags */
    int *tooth;       /* per node, the tooth (0 .. T-1) of a flank or fillet node, else -1 */
    double *normal;   /* 2 * nnodes: the outline's outward normal at surface nodes (0 elsewhere) */
    double *profile;  /* per node on a flank or fillet: the radius on the outline (m); 0 elsewhere */
    int teeth;        /* T */
    double tooth_angle0; /* polar angle of tooth 0's centre line; tooth k is at tooth_angle0 + k 2 pi / z */
    /* the flank surface as quads, for contact: 4 nodes each (outward by the right-hand rule), the side, the tooth */
    int nfaces;
    int *face;        /* 4 * nfaces */
    int *face_side;   /* GM_LEFT or GM_RIGHT */
    int *face_tooth;
    int *face_elem;   /* the element behind it */
    double min_jacobian_ratio; /* smallest det J at a Gauss point over the largest in its element, over the mesh (<= 0: inverted) */
} GearMesh;

void gear_mesh_defaults(GearMeshSpec *m);
/* Tooth 0's centre line lies at polar angle `angle0`, teeth follow counter-clockwise. */
bool gear_mesh_build(const GearGeometry *g, const GearMeshSpec *m, double angle0, GearMesh *out, char *err, size_t errlen);
void gear_mesh_free(GearMesh *gm);
/* The same for an internal gear (gear_build_internal), whose rim lies outside its teeth: its outline is inverted in
 * the reference circle (r -> r_ref^2 / r, polar angles kept), which turns it into an external gear's with the rim
 * inside; that is meshed as above and mapped back. Inversion keeps angles, so the elements keep their shapes; it
 * reverses orientation, so each element's and face's nodes are reordered. The "bore" (held) is the rim's outer
 * surface, the profile radii and normals are the ring's own. */
bool gear_mesh_build_internal(const GearGeometry *g, const GearMeshSpec *m, double angle0, GearMesh *out, char *err, size_t errlen);
