/* Double-precision periodic D3Q19 BGK flow. Lattice units internally.
 * Stationary voxel bodies use halfway bounce-back; no inlet/outlet model. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    int n;
    size_t count;
    double nu;
    double acceleration[3]; /* constant body acceleration in lattice units */
    unsigned char *solid; /* stationary halfway bounce-back cells */
    long steps;
    double *f,*next;
} Flow3D;
bool flow3d_create(Flow3D *f,int n,double nu);
void flow3d_free(Flow3D *f);
/* Set populations from density and velocity arrays, 3 interleaved components. */
bool flow3d_initialize(Flow3D *f,const double *rho,const double *velocity);
bool flow3d_step(Flow3D *f);
void flow3d_sample(const Flow3D *f,size_t cell,double *rho,double velocity[3]);
