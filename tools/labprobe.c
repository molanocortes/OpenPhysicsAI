/* labprobe.c - numbers out of a physics-lab result (src/lab/labio.h), for scripts, agents and flags.
 *
 *   build/labprobe RESULT.lab --field F [--frame N] --line X0 Y0 X1 Y1 SAMPLES     values along a line (blocks)
 *   build/labprobe RESULT.lab --field F [--frame N] --point X Y                     one value (blocks)
 *   build/labprobe RESULT.lab --field F [--frame N] --stats                         min, max, mean over a part
 *
 * Blocks parts are sampled in the finest block covering the point, bilinearly between cell centres (the same rule as
 * labfilm). --frame -1 (the default) is the last frame. Output: one "x y value" line per sample, "nan" outside the
 * mesh; with --mirror a negative y is read as |y| (axisymmetric results). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lab/labio.h"

static double sample(const LabPart *p, const float *f, double x, double y) {
    int best = -1, lev = -1;
    double bu = 0, bv = 0;
    for (int b = 0; b < p->nblocks; b++) {
        const LabBlock *bl = &p->blocks[b];
        double u = (x - bl->origin[0]) / bl->dx[0], v = (y - bl->origin[1]) / bl->dx[1];
        if (u >= 0 && v >= 0 && u < bl->n[0] && v < bl->n[1] && bl->level > lev) best = b, lev = bl->level, bu = u, bv = v;
    }
    if (best < 0) return NAN;
    size_t first = 0;
    for (int b = 0; b < best; b++) first += lab_block_cells(&p->blocks[b]);
    const LabBlock *bl = &p->blocks[best];
    const float *c = f + first;
    int nx = bl->n[0], ny = bl->n[1];
    double a = bu - 0.5, bb = bv - 0.5;
    int i0 = (int)floor(a), j0 = (int)floor(bb);
    double tx = a - i0, ty = bb - j0;
    int i1 = i0 + 1, j1 = j0 + 1;
    i0 = i0 < 0 ? 0 : i0, j0 = j0 < 0 ? 0 : j0;
    i1 = i1 > nx - 1 ? nx - 1 : i1, j1 = j1 > ny - 1 ? ny - 1 : j1;
    return (1 - ty) * ((1 - tx) * c[j0 * nx + i0] + tx * c[j0 * nx + i1]) + ty * ((1 - tx) * c[j1 * nx + i0] + tx * c[j1 * nx + i1]);
}

int main(int argc, char **argv) {
    const char *in = NULL, *field = NULL;
    int frame = -1, samples = 0;
    double line[4] = {0}, pt[2] = {0};
    bool do_line = false, do_point = false, do_stats = false, mirror = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--field") && i + 1 < argc) field = argv[++i];
        else if (!strcmp(argv[i], "--frame") && i + 1 < argc) frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--line") && i + 5 < argc) {
            for (int k = 0; k < 4; k++) line[k] = atof(argv[++i]);
            samples = atoi(argv[++i]);
            do_line = true;
        } else if (!strcmp(argv[i], "--point") && i + 2 < argc) {
            pt[0] = atof(argv[++i]), pt[1] = atof(argv[++i]);
            do_point = true;
        } else if (!strcmp(argv[i], "--stats")) do_stats = true;
        else if (!strcmp(argv[i], "--mirror")) mirror = true;
        else if (argv[i][0] != '-' && !in) in = argv[i];
        else {
            fprintf(stderr, "labprobe: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!in || !field || !(do_line || do_point || do_stats) || (do_line && samples < 1)) {
        fprintf(stderr, "usage: labprobe RESULT.lab --field F [--frame N] (--line X0 Y0 X1 Y1 N | --point X Y | --stats) [--mirror]\n");
        return 2;
    }
    char err[256];
    LabFile *lf = lab_open(in, err, sizeof err);
    if (!lf) {
        fprintf(stderr, "labprobe: %s\n", err);
        return 1;
    }
    int n = lab_frame_count(lf);
    if (frame < 0) frame = n + frame;
    LabFrame fr;
    if (!lab_read_frame(lf, frame, &fr, err, sizeof err)) {
        fprintf(stderr, "labprobe: %s\n", err);
        return 1;
    }
    const LabPart *part = NULL;
    const LabField *f = NULL;
    for (int p = 0; p < fr.nparts && !f; p++)
        if ((f = lab_find_field(&fr.parts[p], field))) part = &fr.parts[p];
    if (!f) {
        fprintf(stderr, "labprobe: no field %s in frame %d\n", field, frame);
        return 1;
    }
    printf("# frame %d, t = %.9g s, field %s\n", frame, fr.time, field);
    if (do_stats) {
        double lo = INFINITY, hi = -INFINITY, s = 0;
        for (size_t i = 0; i < f->count; i++) lo = fmin(lo, f->data[i]), hi = fmax(hi, f->data[i]), s += f->data[i];
        printf("min %.9g max %.9g mean %.9g count %zu\n", lo, hi, s / (double)f->count, f->count);
    }
    if ((do_line || do_point) && part->kind != LAB_BLOCKS) {
        fprintf(stderr, "labprobe: --line and --point read block parts\n");
        return 1;
    }
    if (do_point) printf("%.9g %.9g %.9g\n", pt[0], pt[1], sample(part, f->data, pt[0], mirror ? fabs(pt[1]) : pt[1]));
    if (do_line)
        for (int i = 0; i < samples; i++) {
            double t = samples > 1 ? (double)i / (samples - 1) : 0;
            double x = line[0] + t * (line[2] - line[0]), y = line[1] + t * (line[3] - line[1]);
            printf("%.9g %.9g %.9g\n", x, y, sample(part, f->data, x, mirror ? fabs(y) : y));
        }
    lab_frame_free(&fr);
    lab_close_file(lf);
    return 0;
}
