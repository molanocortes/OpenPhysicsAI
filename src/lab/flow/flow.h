/* flow.h - incompressible flow past two-dimensional bodies (cylinders, aerofoils) in a tunnel: the lattice Boltzmann
 * solver of the app (src/lbm.c, D3Q19, recursive regularised collision, curved walls) on a span of four cells with
 * periodic ends, so the flow is two-dimensional; the laboratory's own measurements on top: the shedding frequency, the
 * forces, the length of a steady wake, and dye released at points upstream (streaklines, the way the classic
 * photographs were made).
 *
 * Similarity: the Reynolds number Re = U D / nu sets the flow; a scenario also names a fluid (kinematic viscosity) and a
 * size (the body's D) only to put the pictures in seconds and metres per second. The side walls are far-field
 * (equilibrium at the free stream), which keeps the blockage effect small; the inlet is a velocity, the outlet a
 * pressure, both behind absorbing layers.
 *
 * Units: lattice internally; SI in the output. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

typedef enum { FLOW_CYLINDER = 0, FLOW_NACA4 = 1 } FlowShape;

typedef struct FlowSpec {
    double Re;
    int shape;
    int D;               /* cells across the body (the cylinder's diameter, or the aerofoil's chord) */
    char naca[8];        /* four digits, e.g. "0012" */
    double aoa_deg;      /* aerofoil angle of attack */
    double width_D, length_D, upstream_D; /* tunnel height and length, and the body's distance from the inlet, in D */
    bool channel;        /* false: a tunnel with far-field side walls and uniform inflow; true: a channel with no-slip
                            walls and a parabolic inflow whose mean is the free-stream speed (Schaefer-Turek) */
    double body_y_D;     /* the body's centre above the tunnel's middle, in D */
    double u_lb;         /* lattice velocity of the free stream (0.05 to 0.1); a channel's mean speed */
    int collision;       /* 0 recursive regularised (default), 1 BGK, 2 regularised: for comparisons */
    double nu_m2_s;      /* the fluid, for the units of the pictures (water 1.0e-6) */
    double D_m;          /* the body's size, for the same */
    /* dye: rakes of release points upstream, released every `dye_every` steps */
    int ndye;
    double dye_xy[64][2]; /* in D, measured from the body's centre */
    int dye_every;
    int threads;
} FlowSpec;

typedef struct Flow Flow;

void flow_spec_defaults(FlowSpec *s);
Flow *flow_create(const FlowSpec *s, char *err, size_t errlen);
void flow_free(Flow *f);
/* advance by n lattice steps; the forces and the dye are updated every step */
bool flow_advance(Flow *f, long n);
long flow_steps(const Flow *f);
double flow_time_s(const Flow *f);          /* physical time */
double flow_convective_time(const Flow *f); /* t U / D */
/* force coefficients now and averaged since `from_step` (drag along x, lift along y, per unit span, on D) */
void flow_coefficients(const Flow *f, long from_step, double *cd, double *cl, double *cd_mean, double *cl_rms);
/* the largest drag and lift coefficients since from_step */
void flow_extrema(const Flow *f, long from_step, double *cd_max, double *cl_max);
/* the shedding frequency from the lift signal since from_step, as a Strouhal number f D / U; 0 if no shedding */
double flow_strouhal(const Flow *f, long from_step, int *periods);
/* the steady wake length behind a cylinder: from its rear to where the centreline velocity turns positive, in D */
double flow_wake_length(const Flow *f);
bool flow_unstable(const Flow *f);

/* a frame: part "flow" (the mid-span plane: fields vorticity 1/s, speed m/s, ux m/s, solid) and part "dye" (points) */
void flow_write_frame(Flow *f, LabWriter *w);
char *flow_header_json(const FlowSpec *s, const char *title);
