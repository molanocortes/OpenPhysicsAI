/* fire.h - fires, their plumes, and the air of rooms: low-Mach reacting flow in 3D, in the manner of NIST's Fire
 * Dynamics Simulator (FDS, McGrattan et al.), the tool of fire-safety engineering, and the same engine for buoyant air
 * with walls, obstacles, vents, openings, fans and heat sources (ventilated rooms, data centres, fires in rooms).
 *
 * The gas is ideal at the ambient pressure p0 (sound waves filtered out: the low-Mach approximation of Rehm and Baum
 * 1978). Density follows from continuity, temperature from the equation of state T = p0 W / (rho R); heat released
 * enters the flow as a divergence, div u = (1 - chi_r) q / (rho cp T) + div(k grad T) / (rho cp T), and the pressure
 * perturbation p~ comes from a projection with the density's inverse on the faces, div(grad p~ / rho) = (div u* - D) /
 * dt, solved by multigrid (mg3d.h), exactly rather than by FDS's constant-coefficient split with a lagged baroclinic
 * term.
 *
 * Species: fuel, lumped products and air (Y_A = 1 - Y_F - Y_P), transported by the flow; fuel and air burn as fast as
 * they mix, at rho min(Y_F, Y_A / s) / tau with tau the shorter of the subgrid mixing time 1 / |S| and the buoyant time
 * sqrt(2 dx / g) (FDS's eddy dissipation concept, simplified), releasing the heat of combustion; a fraction chi_r of it
 * leaves as radiation. Large-eddy simulation: Smagorinsky's viscosity, turbulent Prandtl and Schmidt numbers of 0.5.
 *
 * Grid: uniform cells of dx, velocities on the cell faces (staggered). By default the floor is a free-slip adiabatic wall
 * with a burner on it (fuel at the ambient temperature, a given heat release rate reached over a ramp) and the other five
 * faces are open: the pressure perturbation zero on them, their normal velocity carried from the face inside, air coming
 * in at the ambient state, a sponge of four cells along the sides, momentum advected upwind within two cells of them.
 * Any side may instead be a wall (no slip or free slip, adiabatic or at a temperature, with vents and openings in it);
 * solid obstacles are cells, their faces closed. A box with no open face is sealed: its background pressure rises as it
 * heats, so that div u integrates to what the vents bring in (FDS's pressure zone). Heat
 * capacities vary with temperature (NIST-JANAF). Heun's two-stage method in time at a CFL number of 0.4, the diffusion
 * limit, and no cell's density falling by more than a fifth in a step. Units: SI. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

typedef struct FireSpec {
    int n[3];            /* cells (x, y, z up); each divisible by 2 several times for the multigrid */
    double dx;           /* m */
    double origin[3];    /* the box's lower corner, m */
    double T0, p0;       /* ambient temperature K, pressure Pa */
    double g;            /* m/s^2 along -z */
    /* the fuel: molar mass kg/mol, heat of combustion J/kg, air to burn a kilogram of it (kg), radiative fraction */
    double W_fuel, dHc, s_air, chi_r;
    double W_prod;       /* molar mass of the lumped products (kg/mol) */
    double cp;           /* J/(kg K) held constant; 0 (the default): each gas's own, varying with temperature */
    double nu;           /* molecular kinematic viscosity at ambient, m^2/s */
    double cs;           /* Smagorinsky constant, default 0.2 */
    /* the burner: a rectangle on the floor [x0, y0, x1, y1] (m), its heat release rate (W; 0: no burner) */
    double burner[4], hrr;
    /* the box's sides x-, x+, y-, y+, z-, z+: FIRE_OPEN (the pressure perturbation zero, air in and out), FIRE_WALL
     * (no slip) or FIRE_SLIP (free slip); defaults open everywhere but a free-slip floor. A wall's temperature (K), 0
     * adiabatic. */
    int side[6];
    double side_T[6];
    int sponge;          /* cells of the sponge along open sides x-, x+, y-, y+ (default 4) */
    int sgs;             /* the subgrid model: FIRE_SMAGORINSKY (default), FIRE_VREMAN, FIRE_NO_SGS (resolved flow) */
    double Pr;           /* molecular Prandtl number (default 0.7) */
    double ramp;         /* s over which the burner rises linearly to its rate (default 1: switched on at once, fuel pools
                            before it mixes and burns in a burst that no grid resolves) */
    double cfl;          /* default 0.4 */
} FireSpec;

enum { FIRE_OPEN = 0, FIRE_WALL = 1, FIRE_SLIP = 2 };
enum { FIRE_SMAGORINSKY = 0, FIRE_VREMAN = 1, FIRE_NO_SGS = 2 };

typedef struct Fire Fire;

void fire_spec_defaults(FireSpec *s); /* methane in air at 20 C (sources in docs/lab/fire.md) */
Fire *fire_create(const FireSpec *s, char *err, size_t errlen);
void fire_free(Fire *F);
/* The room, added before the first step (each returns false after it, or on a bad argument). Boxes are [x0, y0, z0,
 * x1, y1, z1] in m, cells taken whole where their centre lies inside (staircase); a rectangle on a side is [a0, b0, a1,
 * b1] in the side's two other coordinates in order (y, z on the x sides, x, z on the y sides, x, y on the z sides).
 *   obstacle: solid cells, no slip at their faces, adiabatic (T 0) or held at T (K);
 *   vent: a held flow through a wall's faces, speed m/s into the box (negative: an exhaust), inflow at T (K) with a
 *         fuel mass fraction (1 for a burner; its speed then ramps up as the burner's does);
 *   opening: a patch of a wall side made open (a door, a window);
 *   heat: a heat source spread over the box, W (people, machines, radiators);
 *   fan: faces normal to an axis (0 x, 1 y, 2 z) inside the box forced to a speed (m/s, along the axis) before each
 *        projection, which may then trim them: a fan, or the air a rack of servers pulls through itself. */
bool fire_add_obstacle(Fire *F, const double box[6], double T);
bool fire_add_vent(Fire *F, int side, const double rect[4], double speed, double T, double fuel);
bool fire_add_opening(Fire *F, int side, const double rect[4]);
bool fire_add_heat(Fire *F, const double box[6], double watts);
bool fire_add_fan(Fire *F, const double box[6], int axis, double speed);
/* one step (two stages); returns the step taken, 0 if the solver failed */
double fire_step(Fire *F, ThreadPool *pool);
double fire_time(const Fire *F);
size_t fire_cells(const Fire *F);
/* per cell: temperature K, heat release rate W/m^3, speed m/s, fuel mass fraction (any may be NULL) */
void fire_fields(const Fire *F, float *T, float *q, float *speed, float *fuel);
/* diagnostics: the largest |div u - D| of the last projection relative to the larger of the largest |D| and the largest
 * divergence the projection removed, the heat released in the
 * box in the last step (W), total mass (kg), mass that came in and went out through the faces and the burner (kg) */
void fire_diagnostics(const Fire *F, double *div_err, double *hrr_now, double *mass, double *mass_in, double *mass_out);
/* the vertical velocity at a cell centre and the temperature, for probes (cell indices) */
double fire_T(const Fire *F, int i, int j, int k);
double fire_w(const Fire *F, int i, int j, int k);
double fire_q(const Fire *F, int i, int j, int k);
/* the fastest face velocity, its component (0 u, 1 v, 2 w) and face indices */
void fire_fastest(const Fire *F, double *speed, int *comp, int *i, int *j, int *k);
/* for debugging: a face velocity (comp 0 u, 1 v, 2 w) and a cell's density, pressure perturbation, D, nu_t or fuel */
double fire_face(const Fire *F, int comp, int i, int j, int k);
double fire_cell(const Fire *F, int what, int i, int j, int k);
/* heat into the gas in the last step (W): through each side's walls (0 to 5), from obstacles (6), from heat sources (7) */
void fire_heat_flows(const Fire *F, double q[8]);
/* through the box's faces since the start: mass in (kg), mass times temperature in (kg K), and the same out */
void fire_boundary_flows(const Fire *F, double f[4]);
/* the age of the air (s: how long since it came in, a passive scalar growing 1 s per s, 0 where air enters), per
 * cell; and the mass times age that has left through the box's faces since the start (kg s) */
const double *fire_age(const Fire *F);
double fire_age_out(const Fire *F);
double fire_pressure(const Fire *F);  /* the background pressure, Pa (rises in a sealed box as it heats) */
bool fire_solid(const Fire *F, int i, int j, int k);
/* the velocity at a cell centre, m/s */
void fire_velocity(const Fire *F, int i, int j, int k, double v[3]);
/* the mass the temperature cap (3000 K) has added, kg (a sign of too little resolution where fuel burns) */
double fire_mass_capped(const Fire *F);
