/* dfn.h - a lithium-ion cell by the Doyle-Fuller-Newman model (Doyle, Fuller and Newman 1993; the pseudo-two-
 * dimensional model): lithium diffusing in the electrodes' particles, ions moving through the electrolyte by diffusion
 * and migration, currents in the solid and the electrolyte, Butler-Volmer kinetics at the particles' surfaces, and the
 * heat all of that makes. SI units throughout.
 *
 * Across the cell (x): the negative electrode, the separator, the positive electrode, each divided into finite volumes;
 * in every electrode volume a representative spherical particle, divided into shells of equal thickness. Unknowns at
 * each step: the electrolyte's concentration and potential in every volume, the solid's potential and the reaction's
 * current density in every electrode volume; the particles' concentrations follow from the reaction current linearly
 * (their diffusion is linear), so they are eliminated before Newton's method and restored after. Backward Euler in
 * time; the Jacobian by finite differences, dense (a few hundred unknowns).
 *
 * Conventions: the current I (A per m^2 of electrode) is positive on discharge; the reaction current j (A per m^2 of
 * particle surface) is positive where lithium leaves the particles; the negative electrode's current collector is at
 * potential zero and the terminal voltage is the positive collector's potential. Heat (W per m^2 of electrode): the
 * reactions' overpotentials, Joule heating in the solid and in the electrolyte (each face's current times its potential
 * drop, so that the sum equals I (U_eff - V) exactly, U_eff the reaction-weighted open-circuit voltage), and the
 * reversible (entropic) heat where dU/dT is given. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef double (*DfnOcp)(double sto);  /* open-circuit potential (V) against stoichiometry c / c_max */

typedef struct {
    double L;         /* thickness, m */
    double eps_e;     /* electrolyte volume fraction (porosity) */
    double eps_s;     /* active material volume fraction */
    double R;         /* particle radius, m */
    double cmax;      /* maximum lithium concentration in the particles, mol/m^3 */
    double c0;        /* initial concentration, mol/m^3 */
    double Ds;        /* diffusivity in the particles, m^2/s */
    double sigma;     /* solid conductivity, S/m (used as is: no Bruggeman correction in the solid) */
    double k0, Ea;    /* exchange current i0 = k0 exp(Ea/R (1/Tref - 1/T)) ce^0.5 cs^0.5 (cmax - cs)^0.5, A/m^2 */
    DfnOcp U;         /* open-circuit potential */
    double dUdT;      /* V/K, entropic coefficient (0: none) */
    int n;            /* volumes across it */
} DfnElectrode;

typedef struct {
    DfnElectrode neg, pos;
    double Lsep, eps_sep; /* separator thickness m, porosity */
    int nsep;
    double brug;          /* Bruggeman exponent of the electrolyte (effective = eps^brug times bulk) */
    double ce0, tplus;    /* initial electrolyte concentration mol/m^3, cation transference number */
    double (*De)(double ce, double T);    /* electrolyte diffusivity, m^2/s */
    double (*kappa)(double ce, double T); /* electrolyte conductivity, S/m */
    double area;          /* electrode area, m^2 (current in A = I area) */
    double Tref;          /* K, reference temperature of the Arrhenius factors */
    int nr;               /* shells per particle */
} DfnSpec;

typedef struct Dfn Dfn;

/* the LG M50 21700 cell (Chen et al., J. Electrochem. Soc. 167 (2020) 080534, as tabulated by PyBaMM) */
void dfn_spec_lgm50(DfnSpec *s, int n_per_region, int nr);
Dfn *dfn_create(const DfnSpec *s, char *err, size_t errlen);
void dfn_free(Dfn *D);
/* one step of dt at cell current I (A, positive on discharge) and temperature T; false if Newton failed (the state is
 * then unchanged) */
bool dfn_step(Dfn *D, double I, double T, double dt);
/* the voltage one step at I would give, without taking it (false if Newton failed) */
bool dfn_try(Dfn *D, double I, double T, double dt, double *V);
double dfn_voltage(const Dfn *D);    /* V, after the last step (at rest before the first: the open-circuit voltage) */
double dfn_heat(const Dfn *D);       /* W, the whole cell's heat in the last step */
double dfn_ocv(const Dfn *D);        /* V, the reaction-weighted open-circuit voltage U_eff of the last step */
double dfn_time(const Dfn *D);
/* lithium in the particles and in the electrolyte, mol; charge passed, C */
void dfn_inventory(const Dfn *D, double *li_solid, double *li_electrolyte, double *li_negative, double *charge);
/* the heat's parts, W: reaction, solid Joule, electrolyte Joule, reversible */
void dfn_heat_parts(const Dfn *D, double q[4]);
/* mean stoichiometries of the two electrodes */
void dfn_stoichiometry(const Dfn *D, double *neg, double *pos);
/* particle-only test hook: the particle's surface concentration under a constant outward flux (mol/m^2/s) from a
 * uniform c0, after time t in steps of dt, on nr shells */
double dfn_particle_surface(double R, double Ds, double c0, double flux, double t, double dt, int nr);
