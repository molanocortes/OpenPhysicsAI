/* vis.h - CPU visualisation kernels: derived scalar fields, streamlines and isosurfaces
 *
 * All inputs and outputs are in lattice units. Lattice space: cell (i,j,k) has its centre at
 * (i+0.5, j+0.5, k+0.5); the domain spans [0,nx] x [0,ny] x [0,nz]; flat index = i + nx*(j + ny*k).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "threads.h"

typedef struct VisGrid {
    int nx, ny, nz;
    const float *rho;     /* density (1 = reference) */
    const float *ux, *uy, *uz;
    const uint8_t *solid; /* 1 = solid cell (velocity there is 0) */
} VisGrid;

typedef enum {
    VIS_SPEED = 0,  /* |u| */
    VIS_UX,
    VIS_UY,
    VIS_UZ,
    VIS_PRESSURE,   /* (rho - 1) / 3 */
    VIS_VORTICITY,  /* |curl u| */
    VIS_QCRITERION, /* 0.5 (|Omega|^2 - |S|^2) */
    VIS_FIELD_COUNT
} VisField;

typedef struct VisRange {
    float min, max; /* over fluid cells */
    float lo, hi;   /* robust range: ~1st and ~99th percentile over fluid cells */
} VisRange;

/* out: nx*ny*nz floats. Solid cells receive a neutral value (0 for velocity-derived fields, the solver's
 * value for pressure) and are excluded from the range statistics. */
void vis_compute_field(const VisGrid *g, VisField field, float *out, VisRange *range, ThreadPool *pool);

/* Trilinear velocity sample at lattice position (x,y,z). Returns false outside the domain or when the
 * nearest cell is solid (u is still written, possibly zeros). */
bool vis_sample_velocity(const VisGrid *g, float x, float y, float z, float u[3]);
float vis_sample_scalar(const float *field, int nx, int ny, int nz, float x, float y, float z);
/* Separable 1-2-1 filter along x, y and z (a 2-cell test filter). The result goes to out; field is overwritten. */
void vis_smooth3(float *field, float *out, int nx, int ny, int nz, ThreadPool *pool);

typedef struct StreamParams {
    int max_steps;        /* per direction */
    float step;           /* arc-length step in cells, e.g. 0.5 */
    bool both_directions; /* also integrate upstream from the seed */
    float min_speed;      /* terminate below this |u| */
} StreamParams;

typedef struct StreamlineSet {
    float *verts;         /* 5 per vertex: x, y, z, scalar, flight_time (lattice time steps along the line) */
    uint32_t nverts, cap_verts;
    uint32_t *line_first; /* index of first vertex of each line */
    uint32_t *line_count; /* vertex count of each line (>= 2) */
    uint32_t nlines, cap_lines;
} StreamlineSet;

/* seeds: 3 floats per seed (lattice coords). scalar may be NULL (speed is used). Lines are stored in seed
 * order, upstream part first when both_directions (so each line is one continuous polyline with
 * monotonically increasing flight_time). Seeds inside solids/outside produce no line. */
void vis_streamlines(const VisGrid *g, const float *scalar, const float *seeds, int nseeds, const StreamParams *sp,
                     StreamlineSet *out, ThreadPool *pool);
void streamlines_free(StreamlineSet *s);

typedef struct IsoMesh {
    float *verts;      /* 7 per vertex: px, py, pz (lattice), nx, ny, nz (unit normal), scalar */
    uint32_t nverts, cap_verts;
    uint32_t *indices; /* triangles */
    uint32_t nindices, cap_indices;
} IsoMesh;

typedef struct IsoParams {
    float iso;               /* isovalue; the surface encloses the region field > iso */
    int downsample;          /* 1 = full resolution, 2 = 2x2x2 averaged (faster, smoother) */
    bool exclude_near_solid; /* ignore cells within 1 cell of a solid */
} IsoParams;

/* Naive surface nets. Normals point out of the region field > iso. color_field may be NULL (field used). */
void vis_isosurface(const VisGrid *g, const float *field, const float *color_field, const IsoParams *ip,
                    IsoMesh *out, ThreadPool *pool);
void isomesh_free(IsoMesh *m);
