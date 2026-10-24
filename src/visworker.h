/* visworker.h - background thread that turns simulation snapshots into render data
 * (display field, packed velocity texture, streamlines, vortex isosurfaces) */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sim.h"
#include "vis.h"

/* rake shapes: PLANE a smoke-wire sheet across the span, GRID a seeded plane, LINE a vertical line, WAKE a grid traced
 * both ways behind the model, RANDOM a seeded box, POINT a small ball (e.g. at a wing tip to follow its vortex) */
typedef enum { SEED_PLANE = 0, SEED_GRID, SEED_LINE, SEED_WAKE, SEED_RANDOM, SEED_POINT, SEED_COUNT } SeedMode;

typedef struct VisRequest {
    int field;              /* VisField for the display texture and line colouring */
    bool velocity;          /* pack velocity for GPU particles */
    bool stream_on;
    int seed_mode;
    int seed_count;
    int stream_steps;
    bool iso_on;
    float iso_level;        /* vortex threshold Q (L/U)^2 in lattice units (L = reference length) */
    float box_lo[3], box_hi[3]; /* model bounding box in lattice coordinates (seeding) */
    float rake_pos[3];      /* streamline rake centre, lattice coordinates */
    float rake_size[2];     /* rake extent in cells along its two in-plane axes (or ball diameter) */
    int rake_axis;          /* normal of GRID/WAKE rakes: 0 x (cross-flow plane), 1 y, 2 z */
    bool has_model;
    uint32_t version;       /* bump when any setting changes to force recomputation */
} VisRequest;

typedef struct VisResults {
    int nx, ny, nz;
    uint64_t step;
    SimUnits units;

    bool field_new;
    int field_id;
    uint16_t *field_half; /* display field divided by field_scale, as half floats (upload-ready) */
    float field_scale;
    VisRange range;

    bool vel_new;
    uint16_t *vel_half;

    bool solid_new;
    uint8_t *solid;
    uint32_t geom_version;

    bool stream_new;
    StreamlineSet stream;
    double stream_ms;

    bool iso_new;
    IsoMesh iso;
    double iso_ms;
    double field_ms;
} VisResults;

typedef struct VisWorker VisWorker;

VisWorker *visworker_create(Sim *sim, int threads);
void visworker_destroy(VisWorker *w);
void visworker_request(VisWorker *w, const VisRequest *req);
/* Main thread: exclusive access to the latest results; clear the *_new flags after consuming. */
VisResults *visworker_lock(VisWorker *w);
void visworker_unlock(VisWorker *w);
