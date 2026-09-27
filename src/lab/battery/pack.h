/* pack.h - a battery module: cylindrical lithium-ion cells standing in rows on a cold plate, connected in parallel,
 * each one computed by its own Doyle-Fuller-Newman model (dfn.h) at its own temperature, and the heat they make
 * conducted through the module in 3D and carried away by a coolant flowing under the plate. SI units.
 *
 * Electrical: the cells share the terminal voltage; each has its own interconnect resistance (the weld and strip to the
 * busbar), and the module's current splits between them so that every cell's voltage less its interconnect's drop is
 * the same. Solved each step by Newton's method on the split, each cell's voltage from its model taken without
 * committing (dfn_try), its slope by a secant.
 *
 * Thermal: conduction on a uniform grid of cubes, backward Euler, conjugate gradients with a Jacobi preconditioner.
 * Each cube is jelly roll (conductivity radial kr in x and y, axial kz along the cell: a wound cell is transversely
 * isotropic about its axis, which is z), plate, or gap. Each cell's heat (its model's reaction, Joule and reversible
 * heat) is spread over its cubes; its interconnect's I^2 R heat goes into its top layer. The plate's underside loses
 * heat to the coolant through a coefficient h_cool; the coolant flows along +x and warms by what it takes (m cp). Every
 * other outer face loses heat to the ambient air through h_amb. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "dfn.h"

typedef struct {
    int rows, cols;                   /* cells along y, along x */
    double pitch, radius, height;     /* m */
    double plate, margin;             /* plate thickness, and how far it reaches past the outer cells, m */
    double dx;                        /* thermal grid, m */
    double kr, kz, rc_cell;           /* the jelly roll: W/(m K) radial and axial, volumetric heat capacity J/(m^3 K) */
    double k_plate, rc_plate, k_gap, rc_gap;
    double h_cool, T_cool_in, mcp_cool; /* W/(m^2 K), K, W/K */
    double h_amb, T_amb, T0;          /* W/(m^2 K), K, the start K */
    double R_ext;                     /* each cell's interconnect resistance, ohm */
    DfnSpec cell;
} PackSpec;

typedef struct Pack Pack;

Pack *pack_create(const PackSpec *s, char *err, size_t errlen);
void pack_free(Pack *P);
int pack_cells(const Pack *P);
bool pack_set_resistance(Pack *P, int cell, double R); /* before or during the run */
/* one step of dt at module current I (A, positive on discharge); false if a cell's model failed */
bool pack_step(Pack *P, double I, double dt);
double pack_time(const Pack *P);
double pack_voltage(const Pack *P);                  /* V, the terminal */
void pack_cell_state(const Pack *P, int cell, double *I, double *T_mean, double *T_max, double *heat, double *V_cell);
double pack_coolant_out(const Pack *P);              /* K */
/* the energy check: heat made by the cells and interconnects since the start (J), heat stored in the module (J, from
 * the start's temperature), heat given to the coolant and the air (J) */
void pack_energy(const Pack *P, double *made, double *stored, double *to_coolant, double *to_air);
/* the thermal grid: cube counts, origin, spacing; the temperature (K) at a point by trilinear interpolation */
void pack_grid(const Pack *P, int n[3], double origin[3], double *dx);
double pack_T_at(const Pack *P, double x, double y, double z);
const double *pack_T(const Pack *P);
/* a cell's axis position (x, y) and the plate's top z */
void pack_cell_xy(const Pack *P, int cell, double *x, double *y);
double pack_plate_top(const Pack *P);
/* the worst balance of the last step: the largest difference between cells' terminal voltages (V), the currents'
 * sum less the module's (A) */
void pack_split_error(const Pack *P, double *dV, double *dI);
/* thermal-only access for tests: set every cube's heat source (W per cube) and step the conduction alone */
bool pack_thermal_step(Pack *P, const double *src_w, double dt);
