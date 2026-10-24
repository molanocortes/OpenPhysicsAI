/* bvh.h - bounding volume hierarchy over an indexed triangle set (double precision):
 * nearest ray hits, all ray hits (parity tests), closest points and box queries. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct BvhNode {
    double lo[3], hi[3];
    int first; /* leaf: start index into order[]; inner node: index of the left child (right child = first + 1) */
    int count; /* leaf: number of triangles (> 0); inner node: 0 */
} BvhNode;

typedef struct Bvh {
    const double *v; /* 3*nv positions, not owned */
    const int *tri;  /* 3*nt vertex indices, not owned */
    int nt;
    BvhNode *nodes;
    int nnodes;
    int *order; /* triangle indices grouped by leaf */
} Bvh;

bool bvh_build(Bvh *b, const double *v, const int *tri, int nt);
void bvh_free(Bvh *b);

typedef struct BvhHit {
    int tri;
    double t, u, v; /* ray parameter and barycentrics of the hit (point = (1-u-v) a + u b + v c) */
} BvhHit;

/* nearest hit with tmin < t < tmax, ignoring triangle skip_tri (-1 for none) */
bool bvh_ray_nearest(const Bvh *b, const double o[3], const double d[3], double tmin, double tmax, int skip_tri, BvhHit *hit);
/* every hit with t > tmin (unordered); fn returns false to stop */
typedef bool (*BvhHitFn)(void *ctx, const BvhHit *hit);
void bvh_ray_all(const Bvh *b, const double o[3], const double d[3], double tmin, BvhHitFn fn, void *ctx);
/* closest surface point within max_dist (false if none) */
bool bvh_closest(const Bvh *b, const double p[3], double max_dist, int *tri, double q[3], double *dist);
/* triangles whose bounding boxes overlap [lo, hi]; fn returns false to stop */
typedef bool (*BvhTriFn)(void *ctx, int tri);
void bvh_query_box(const Bvh *b, const double lo[3], const double hi[3], BvhTriFn fn, void *ctx);

bool ray_triangle(const double o[3], const double d[3], const double a[3], const double b[3], const double c[3], double *t,
                  double *u, double *v);
void closest_point_triangle(const double p[3], const double a[3], const double b[3], const double c[3], double q[3]);
/* true if the closed triangles intersect (touching counts); callers skip pairs that share vertices */
bool tri_tri_intersect(const double *a0, const double *a1, const double *a2, const double *b0, const double *b1, const double *b2);
