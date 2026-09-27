/* impact.h - solids hit at high speed: explicit Lagrangian dynamics of metals at large strain, strain rate and
 * temperature. The Taylor anvil test, a projectile against a plate, a shield, a small body against a target.
 *
 * Model (docs/lab/impact.md):
 *   elements   eight-node hexahedra, one-point integration with the uniform gradient of Flanagan and Belytschko (Int. J.
 *              Numer. Meth. Eng. 17, 1981) and their viscous hourglass control
 *   kinematics velocity gradient at the element centre; the deviatoric stress rotates with the Jaumann rate
 *   deviator  hypoelastic, radial return to the Johnson-Cook flow stress
 *                sigma_y = (A + B ep^n) (1 + C ln(max(rate / rate0, 1))) (1 - T*^m),  T* = (T - Tr) / (Tm - Tr),
 *              with the plastic work heating the element adiabatically (Taylor-Quinney fraction chi)
 *   pressure   Mie-Gruneisen equation of state with the linear shock-velocity Hugoniot us = c0 + s up referenced to it
 *   shocks     von Neumann-Richtmyer artificial bulk viscosity, linear and quadratic in the volumetric strain rate
 *   failure    an element is eroded (removed, its mass kept at the nodes) when its equivalent plastic strain or its
 *              volumetric strain passes the material's limit
 *   contact    penalty, node against face, both ways between bodies; a rigid plane (an anvil) as a constraint;
 *              frictionless
 *   time       central difference, the step from the smallest element's wave transit with a safety factor
 *
 * Units: SI. Stresses in Pa, temperatures in K. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

#define IM_MAX_MATERIALS 8

typedef struct ImMaterial {
    char name[48];
    double rho0;          /* kg/m^3 */
    double shear_modulus; /* Pa */
    /* Johnson-Cook */
    double A, B, n, C, m; /* Pa, Pa, 1, 1, 1 */
    double rate0;         /* reference strain rate, 1/s */
    double T_room, T_melt;/* K */
    double cp;            /* J/(kg K) */
    double chi;           /* fraction of plastic work turned into heat, typically 0.9 */
    /* Mie-Gruneisen */
    double c0, s, gamma0; /* m/s, 1, 1 */
    /* erosion */
    double fail_strain;   /* equivalent plastic strain; <= 0: none */
    double fail_volumetric; /* |V/V0 - 1| above which the element is eroded (tension or crushing); <= 0: none */
    char source[1024];    /* where the numbers come from */
    char status[160];     /* what of it has been checked against the source, and when */
} ImMaterial;

typedef struct ImBody {
    int first_node, nnodes;
    int first_elem, nelems;
    int material;
    double velocity[3]; /* initial, m/s */
} ImBody;

typedef struct ImMesh {
    int nnodes, nelems;
    double *xyz; /* 3 * nnodes, m */
    int *conn;   /* 8 * nelems, the usual hexahedron order: bottom face counter-clockwise, then top */
} ImMesh;

typedef struct ImSpec {
    int nmaterials;
    ImMaterial materials[IM_MAX_MATERIALS];
    int nbodies;
    ImBody bodies[8];
    ImMesh mesh;          /* all bodies' nodes and elements, in the ranges the bodies give */
    /* symmetry planes x = 0, y = 0 (the model is a half or a quarter); the normal velocity is held there */
    bool sym_x, sym_y;
    /* a rigid anvil: the plane z = anvil_z, material below it (the Taylor test) */
    bool anvil;
    double anvil_z;
    bool uniaxial;        /* hold every node's x and y velocity: uniaxial strain, for the Hugoniot test */
    double safety;        /* of the stable step, default 0.6 */
    double hourglass;     /* viscous hourglass coefficient, default 0.1 */
    double q_linear, q_quadratic; /* bulk viscosity, default 0.06 and 1.5 */
    double contact_scale; /* penalty scale, default 0.1 */
    int threads;
} ImSpec;

typedef struct Impact Impact;

/* the material laws, shared with the particle solver (sph.h): the Johnson-Cook flow stress at plastic strain ep, rate
 * (1/s) and temperature T (K), and the Mie-Gruneisen pressure at mu = rho / rho0 - 1 and internal energy per reference
 * volume E (J/m^3) */
double im_jc_flow(const ImMaterial *M, double ep, double rate, double T);
double im_mg_pressure(const ImMaterial *M, double mu, double E);

void im_spec_defaults(ImSpec *s);
Impact *im_create(const ImSpec *s, char *err, size_t errlen);
void im_free(Impact *im);

double im_step(Impact *im); /* one step; returns the step taken */
double im_time(const Impact *im);
long im_steps(const Impact *im);
double im_dt(const Impact *im);

typedef struct ImEnergy {
    double kinetic, internal, hourglass, contact, eroded; /* J */
    double total;
} ImEnergy;
void im_energy(const Impact *im, ImEnergy *e);
/* momentum of one body, kg m/s */
void im_body_momentum(const Impact *im, int body, double p[3]);
/* the node positions and velocities now (3 * nnodes each) */
const double *im_positions(const Impact *im);
const double *im_velocities(const Impact *im);
/* per element: equivalent plastic strain, pressure (Pa), von Mises stress (Pa), temperature (K), eroded flag */
const double *im_plastic_strain(const Impact *im);
const double *im_pressure(const Impact *im);
const double *im_von_mises(const Impact *im);
const double *im_temperature(const Impact *im);
const unsigned char *im_eroded(const Impact *im);
int im_eroded_count(const Impact *im);

/* a frame: part "solid" with the living elements as hexahedra (mirrored across the symmetry planes when mirror is
 * true, so a quarter model is drawn whole), cell fields ep, p, vm, T, and node field speed */
void im_write_frame(Impact *im, LabWriter *w, bool mirror);
char *im_header_json(const ImSpec *s, const char *title);

/* meshes: a solid cylinder along z (radius r, from z0 to z0 + length), an O-grid of nr rings with nq cells per
 * quarter circumference, nz layers; quarter = true builds only x >= 0, y >= 0. A box from lo to hi with n cells. The
 * new nodes and elements are appended to the mesh (arrays grow). */
bool im_mesh_cylinder(ImMesh *m, double r, double z0, double length, int nq, int nr, int nz, bool quarter);
bool im_mesh_box(ImMesh *m, const double lo[3], const double hi[3], const int n[3]);
/* a sphere (radius r, centre c) as a cube mapped onto the sphere, nq cells per cube edge; quarter as above */
bool im_mesh_sphere(ImMesh *m, const double c[3], double r, int nq, bool quarter);
void im_mesh_free(ImMesh *m);
