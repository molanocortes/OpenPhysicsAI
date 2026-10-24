/* repair.h - making a triangle soup into something a volume mesher can trust, with every change counted
 *
 * Exported STL is often not a closed solid: vertices repeated instead of shared, a facet stored twice, a stray sheet
 * left inside the part, a hole a few edges wide. surface_build already welds, drops degenerate and duplicate facets
 * and reports what it found. Two things it does not do, because they change the geometry rather than describe it,
 * are here:
 *
 *   - keeping only the shell the part is made of, dropping sheets and stray facets that hang off it. A sheet of zero
 *     thickness inside a solid is what a ray-parity inside test cannot survive: the ray crosses it once and every
 *     cell behind it flips. That is exactly what a customer part carried (a 10 077 facet sheet attached to a
 *     621 068 facet shell along 575 edges).
 *   - filling small holes, up to a stated number of boundary edges, by fanning the boundary loop.
 *
 * Nothing here is silent: MeshRepairReport carries the count of every change and the diagnostics before and after, so
 * the caller can print them and the user can disagree. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh.h"
#include "surface.h"

typedef struct MeshRepairOptions {
    bool keep_largest_component; /* drop every edge-connected component but the largest by area */
    int max_hole_edges;          /* fill a boundary loop of at most this many edges; 0 fills none */
    SurfaceRepairOptions surface;/* welding, degenerate and duplicate removal, orientation */
} MeshRepairOptions;

typedef struct MeshRepairReport {
    uint32_t triangles_in, triangles_out;
    uint32_t merged_vertices, degenerate_removed, duplicate_removed;
    int components_found, components_dropped;
    uint32_t triangles_dropped;
    double dropped_area, kept_area;   /* in the file's length unit, squared */
    int holes_filled, holes_too_large;
    uint32_t triangles_added;
    int largest_hole_edges_left;
    /* what the diagnostics said before and after */
    uint32_t open_edges_before, nonmanifold_edges_before;
    uint32_t open_edges_after, nonmanifold_edges_after;
    double volume_before, volume_after, area_before, area_after;
    bool closed_solid_before, closed_solid_after;
} MeshRepairReport;

void mesh_repair_defaults(MeshRepairOptions *o);

/* Builds a repaired triangle soup from `in`. `out` is initialised by the call and owned by the caller. Returns false
 * only when the input cannot be read at all or memory runs out; a repair that could not close the surface still
 * returns true, with the report saying what is left. */
bool mesh_repair(const Mesh *in, const MeshRepairOptions *o, Mesh *out, MeshRepairReport *rep, char *err,
                 size_t errlen);
