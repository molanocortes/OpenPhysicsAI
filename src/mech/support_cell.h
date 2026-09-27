/* support_cell.h - support structures as geometry: typed support patterns and their homogenised properties
 *
 * A support type is a periodic pattern in x and y (one pitch, the unit cell) whose section may change with the depth
 * below the part it carries (a cone narrows toward the part, a tree branches, the tooth band touches the part only on
 * teeth). Its effective properties are not assumed: the unit cell is voxelised and solved explicitly.
 *
 *   stiffness   periodic homogenisation, hex8 on src/fem/solid.c: each of the six macro strains is applied as an
 *               initial strain, the fluctuation is periodic in x and y (and in z for a prismatic slab; a slab whose
 *               section changes with depth holds it at zero on its faces), and the volume average of the stress gives
 *               the full orthotropic matrix. stiff_z = C33_cell / C33_solid and stiff_x = C11_cell / C11_solid.
 *   conduction  a temperature difference across the cell, side faces insulated, finite volumes on the same voxels:
 *               vertical and lateral conductivity fractions.
 *   capacity    the solid fraction of the cell.
 *
 * Every voxel carries its solid volume fraction (4 x 4 x 4 samples) and its stiffness and conductance are scaled by it,
 * as Simufact scales its voxels (tutorial p. 72); an inclined strut is then not a staircase. Pieces of the cell that do
 * not carry load (not connected across the periodic cell, or not reaching a loaded face) are left out.
 * docs/contracts/supports.md holds the verification (S1 to S4). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SUPPORT_HOMOGENEOUS = 0, /* no geometry: a given stiffness fraction (the wave-4 input) */
    SUPPORT_BLOCK,           /* a square grid of thin walls */
    SUPPORT_THIN_WALL,       /* parallel thin walls */
    SUPPORT_CONE,            /* a cone per grid point, base at the plate, top at the part */
    SUPPORT_TREE,            /* a trunk per coarse grid point, branching to tips at the part */
    SUPPORT_LATTICE,         /* body-centred cubic struts */
    SUPPORT_EXPLICIT         /* meshed with the part: its elements inside a given region are the supports, solid */
} SupportType;

typedef struct SupportSpec {
    SupportType type;
    /* all lengths in m */
    double wall_thickness, spacing;                               /* block, thin_wall; spacing = tip pitch (cone, tree) */
    int direction;                                                /* thin_wall: 0 walls along x, 1 walls along y */
    double base_radius, top_radius;                               /* cone */
    double trunk_radius, branch_radius, trunk_spacing, branch_height; /* tree */
    double cell_size, strut_diameter;                             /* lattice */
    double tooth_height, tooth_pitch, contact_fraction;           /* the interface band of wall types (0: none) */
    double relative_density;                                      /* of the printed support material, 0..1 */
    double fraction;                                              /* homogeneous only */
    double height;                                                /* support height, plate to part (cone, tree) */
    double region[6];                                             /* explicit: x0 y0 z0 x1 y1 z1 (m) */
} SupportSpec;

typedef struct SupportCellProps {
    double solid_fraction;           /* volume fraction of the cell, before relative density */
    double stiff_z, stiff_x;         /* C33 and C11 of the cell over those of solid */
    double C[36];                    /* the cell's effective stiffness matrix over the solid's E (Voigt, engineering shear) */
    double cond_z, cond_x, cond_y;   /* conductivities over that of solid */
    double surface_per_volume;       /* 1/m: the pattern's surface area per unit cell volume (heat it exchanges with the air) */
    int voxels_per_pitch, layers;    /* the discretisation used */
    int elements;                    /* solid voxels in the mechanical solve */
} SupportCellProps;

const char *support_type_name(SupportType t);
bool support_type_from_name(const char *name, SupportType *t);
/* the pitch of the unit cell in x and y (m) */
double support_pitch(const SupportSpec *s);
/* true when the section does not change with depth (one solve serves the whole height, outside the tooth band) */
bool support_prismatic(const SupportSpec *s);
/* whether the point (x, y) of the unit cell ([0, pitch)^2) at depth d below the part (m) is support material */
bool support_occupied(const SupportSpec *s, double x, double y, double d);
/* checks the parameters of the type; false with a message naming the first missing or impossible one */
bool support_spec_check(const SupportSpec *s, char *err, size_t errlen);

/* Homogenised properties of the slab between depths d0 < d1 below the part. E and nu are the solid's (the fractions do
 * not depend on E; nu matters). voxels: voxels per pitch (0: four across a wall or cone, eight across a strut, 8 to 64).
 * Prismatic types ignore d0 and d1 outside the tooth band and solve a slab two pitches tall. */
bool support_cell_solve(const SupportSpec *s, double d0, double d1, double nu, int voxels, SupportCellProps *out, char *err,
                        size_t errlen);
