/* Conservative 3D cell-centred conduction, SI units, double precision.
 * Six insulating outer faces. Harmonic conductivity at internal faces.
 * Explicit time step bounded by the local sum of face conductances. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    int n[3];
    size_t count;
    double dx, time;
    double *T, *capacity, *conductivity, *source, *next;
} Heat3D;
bool heat3d_create(Heat3D *h, const int n[3], double dx);
void heat3d_free(Heat3D *h);
/* Returns zero for invalid material/state or step; never advances on failure. */
double heat3d_step(Heat3D *h, double max_dt);
double heat3d_energy(const Heat3D *h);
