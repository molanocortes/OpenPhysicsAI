/* Current-free 3D magnetostatics via scalar potential and MFEM H1 elements.
 * B=-mu_r grad(psi)+Br, where psi=mu0*magnetic_scalar_potential.
 * A spherical permanent magnet, with uniform applied field. SI units. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct {
 int n;
 double dx,center[3],radius,remanence[3],applied[3],mu_r;
 bool exact_sphere_boundary; /* verification: exact isolated-sphere exterior potential */
} Magnet3DSpec;
typedef struct {double *B;int iterations;double relative_residual;} Magnet3DResult;
#ifdef __cplusplus
extern "C" {
#endif
bool magnet3d_solve(const Magnet3DSpec *,Magnet3DResult *,char *,size_t);
void magnet3d_free(Magnet3DResult *);
#ifdef __cplusplus
}
#endif
