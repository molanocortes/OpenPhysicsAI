/* em.h - electromagnetic waves: Maxwell's equations in the time domain on a 3D Yee grid.
 *
 *   eps dE/dt = curl H - sigma E - J,     mu0 dH/dt = -curl E
 *
 * Scheme: Yee's staggered grid (Yee 1966) with leapfrog in time, second order in space and time, at a Courant number
 * of 0.99 / sqrt(3). Dielectrics and losses per cell (permittivity and conductivity averaged onto the edges), perfect
 * electric conductors as cells whose edges hold E = 0.
 * Open boundaries: the convolutional perfectly matched layer (Roden and Gedney, Microw. Opt. Technol. Lett. 27, 2000)
 * with polynomial grading of order 3, sigma_max = 0.8 (m + 1) / (eta0 dx), kappa 1 and a complex-frequency shift.
 * Sources: a plane wave along +x, polarised along z, entered by the total-field / scattered-field method on a box
 * (the incident field computed on an auxiliary 1D grid with the same step, which has exactly the dispersion of the 3D
 * grid along its axis); point dipoles (a current in z).
 * Measurements: point probes; the scattered power through a closed surface in the scattered-field region at one
 * frequency, from running Fourier transforms of E and H, which gives the scattering cross-section.
 *
 * Units: SI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

#define EM_MAX_OBJECTS 64

typedef enum { EM_BOX = 0, EM_SPHERE, EM_CYLINDER_Z, EM_SHAPES } EmShape; /* EM_SHAPES: bodies of labshape.h */
struct LabShapes;

typedef struct EmObject {
    int shape;
    double lo[3], hi[3];  /* box */
    double c[3], r;       /* sphere, cylinder (axis along z through c, from lo[2] to hi[2]) */
    double eps_r, sigma;  /* relative permittivity, conductivity S/m */
    bool pec;             /* perfect conductor */
    const struct LabShapes *shapes; /* EM_SHAPES: the bodies (signed distance), owned by the caller */
} EmObject;

typedef struct EmSpec {
    int n[3];            /* cells */
    double dx;           /* m, the same in all directions */
    int pml;             /* cells of absorbing layer on every side (0: the outer faces are perfect conductors) */
    int nobj;
    EmObject obj[EM_MAX_OBJECTS];
    /* plane wave: TF/SF box in cells [lo, hi), a Gaussian-modulated or Gaussian pulse */
    bool plane_wave;
    int tfsf_lo[3], tfsf_hi[3];
    double amplitude;    /* V/m */
    double f0, bandwidth;/* Hz: centre and (1/e) half-width of the spectrum; f0 0 gives a Gaussian pulse */
    /* dipoles */
    int ndip;
    double dip_pos[8][3];
    /* the frequency of the running Fourier transforms (scattering) */
    double f_dft;
    int threads;
} EmSpec;

typedef struct Em Em;

void em_spec_defaults(EmSpec *s);
Em *em_create(const EmSpec *s, char *err, size_t errlen);
void em_free(Em *e);
void em_step(Em *e);
double em_time(const Em *e);
double em_dt(const Em *e);
long em_steps(const Em *e);
/* a field component at a point (nearest edge or face): 0 Ex, 1 Ey, 2 Ez, 3 Hx, 4 Hy, 5 Hz */
double em_probe(const Em *e, int comp, const double x[3]);
/* the incident plane wave's Ez at x (from the 1D grid) */
double em_incident_ez(const Em *e, double x);
/* enable the scattered-power surface: a box in cells [lo, hi) inside the scattered-field region */
void em_scatter_surface(Em *e, const int lo[3], const int hi[3]);
/* the time-averaged scattered power through that surface at f_dft, W, and the incident intensity there, W/m^2 */
double em_scattered_power(const Em *e, double *incident_intensity);
/* the largest |Ez| in the scattered-field region (outside the TF/SF box), for checking leakage */
double em_max_scattered_ez(const Em *e, int margin);

/* a frame: part "slice" (Ez on the plane z = z_slice, cells) with fields ez and material, and part "objects" */
void em_write_frame(Em *e, LabWriter *w, double z_slice);
char *em_header_json(const EmSpec *s, const char *title);

/* All computed z planes, with the same cell-centred Ez samples as the slice writer. */
bool em_write_volume(Em *e, LabWriter *w);
/* Ez alone, on every stride-th cell (a large domain's film) */
bool em_write_volume_ez(Em *e, LabWriter *w, int stride);
/* cell-centred Ez on every stride-th cell into out (NULL: only the dimensions) */
void em_volume_ez(const Em *e, int stride, float *out, int dims[3]);
