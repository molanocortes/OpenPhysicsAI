/* specimen.h - a voxelised test specimen compressed between rigid platens (GOALS.md G23 step 5c, docs/cad.md).
 *
 * The specimen is given voxel by voxel (each voxel's solid fraction scales its stiffness, as in the periodic cell
 * solver of src/mech/support_cell.c); its largest face-connected piece is kept (the rest carries nothing). It is
 * loaded along z between platens at z = 0 and z = nz h: frictionless (only the axial displacement held at both faces;
 * one bottom node also held in x and y and another in y, so that it cannot slide or turn) or bonded (all three held at
 * both faces). The strain is applied as a uniform initial strain between the held faces, which is the same problem as
 * moving one platen by the strain times the height. Solved by src/fem/solid.c (hex8 with incompatible modes, PCG). */
#pragma once

#include <string>

struct SpecimenResult {
    double modulus_over_Es = 0; /* the platen force over the specimen's whole section over the strain, over E_s */
    long elements = 0, dropped = 0, nodes = 0, equations = 0;
    int iterations = 0;
    double residual = 0, seconds = 0;
};

/* phi: nx * ny * nz solid fractions in [0, 1], x fastest, then y, then z; h: the voxel's edge (any unit); nu: the
 * solid's Poisson's ratio. Refuses (false, with the reason) above max_equations unknowns, so that a solve fits the
 * memory it is given. */
bool specimen_compress(const double *phi, int nx, int ny, int nz, double h, double nu, bool bonded, long max_equations,
                       SpecimenResult *out, std::string &why);
