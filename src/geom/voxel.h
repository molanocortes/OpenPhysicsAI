/* voxel.h - robust triangle-mesh voxelisation onto the simulation lattice */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh.h"
#include "../threads.h"

typedef struct VoxelizeOptions {
    /* Cells whose centre lies within this Euclidean distance (in cells) of any triangle are marked solid
     * in addition to the inside test. Keeps thin plates / thin trailing edges / open meshes leak-free.
     * 0 disables the shell pass. Typical: 0.5 */
    float shell_radius;
    /* Inside test votes: parity rays are cast along X, Y and Z; a cell is inside when at least
     * `min_votes` of the 3 axes agree (2 = majority, robust to small holes; 1 = any; 3 = all). */
    int min_votes;
} VoxelizeOptions;

typedef struct VoxelizeResult {
    uint64_t solid_cells;
    int bbox_min[3], bbox_max[3]; /* inclusive cell bounds of solid cells (min > max if none) */
    uint64_t frontal_cells;       /* number of (j,k) columns containing at least one solid cell (projection on YZ) */
    double seconds;
} VoxelizeResult;

/* Lattice space: cell (i,j,k) covers [i,i+1] x [j,j+1] x [k,k+1]; its centre is (i+0.5, j+0.5, k+0.5).
 * M maps mesh coordinates to lattice coordinates.
 * out: nx*ny*nz bytes, index = i + nx*(j + ny*k). Every byte is overwritten: 1 = solid, 0 = fluid.
 * pool may be NULL (single-threaded). */
void voxelize_mesh(const Mesh *m, mat4 M, int nx, int ny, int nz, uint8_t *out, const VoxelizeOptions *opt,
                   ThreadPool *pool, VoxelizeResult *result);
