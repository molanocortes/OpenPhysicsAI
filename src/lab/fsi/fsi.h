/* fsi.h - a thin sheet (sheet.h) in a 3D flow (lbm3d_metal.h): fluid-structure interaction by the immersed boundary.
 *
 * Every flow step: the sheet's nodes become the immersed boundary's points (their positions in cells, their velocities
 * in lattice units, each carrying its share of the sheet's area); the flow is stepped with the direct forcing that
 * makes it follow them; the opposite of the force each point put on the fluid, in newtons, acts on its node; the sheet
 * then takes as many of its own stable steps as fit in the flow's step, the fluid's force held. Units: the sheet in SI,
 * the flow in lattice units, joined by the cell size dx (m), the flow's step dt (s) and the fluid's density (kg/m^3).
 * The flow runs on the GPU (the immersed boundary lives there). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"
#include "../flow/lbm3d_metal.h"
#include "../sheet/sheet.h"

typedef struct Fsi Fsi;

/* origin: where cell coordinate (0, 0, 0) is, m (a cell's centre is at integer + 1/2) */
Fsi *fsi_create(Lbm3DGpu *G, Sheet *S, double dx, double dt, double rho_fluid, const double origin[3], int iterations, double relax, char *err,
                size_t errlen);
void fsi_free(Fsi *C);
/* one flow step and the sheet's steps inside it; body_force (may be NULL): the force on the flow's own bodies in
 * this step, lattice units; false if the flow went unstable */
bool fsi_step(Fsi *C, ThreadPool *pool, double body_force[3]);
/* the force of the fluid on the sheet in the last step, N (sum over nodes), and the sheet's steps per flow step */
void fsi_force(const Fsi *C, double F[3]);
int fsi_substeps(const Fsi *C);
