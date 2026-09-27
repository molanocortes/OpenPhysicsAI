/* Second-order conservative 3D ideal-gas Euler foundation, SI units.
 * Periodic cube, MUSCL, SSP-RK2, HLLC flux with Rusanov fallback, double precision.
 * No AMR, body geometry or mixed gases; existing gas.c retains those cases. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct {int n;size_t count;double dx,gamma,time;double *u,*next,*stage;} Gas3D;
bool gas3d_create(Gas3D *g,int n,double dx,double gamma);
void gas3d_free(Gas3D *g);
/* State layout per cell: density, three momentum components, total energy. */
void gas3d_set(Gas3D *g,size_t q,double rho,const double v[3],double p);
bool gas3d_primitive(const Gas3D *g,size_t q,double *rho,double v[3],double *p);
double gas3d_step(Gas3D *g,double max_dt);
