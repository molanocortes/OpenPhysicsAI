/* labtest.c - the lab result format (src/lab/labio.h).
 *
 * Criteria, written before the first run:
 *   L1 every part kind written and read back gives the same geometry bit for bit and the same float fields
 *   L2 a file cut in the middle of its last frame opens with the complete frames only
 *   L3 a file that is not a lab result is refused with a message, not read */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lab/labio.h"

static int failures;
#define CHECK(cond, ...)                                                                                                 \
    do {                                                                                                                 \
        if (!(cond)) {                                                                                                   \
            printf("  FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                                \
            failures++;                                                                                                  \
        }                                                                                                                \
    } while (0)

int main(void) {
    const char *path = "build/labtest.lab";
    char err[256];
    printf("== L1: round trip of blocks, points and cells over three frames\n");
    LabWriter *w = lab_create(path, "{\"domain\":\"test\",\"fields\":{\"rho\":\"kg/m^3\"}}", err, sizeof err);
    CHECK(w, "%s", err);
    if (!w) return 1;
    LabBlock bl[2] = {{{4, 3, 1}, 0, LAB_PLANE_XY, {0, 0, 0}, {0.25, 0.25, 1}}, {{4, 4, 1}, 1, LAB_PLANE_XY, {0.5, 0, 0}, {0.125, 0.125, 1}}};
    double pts[6] = {1, 2, 3, -4, 5.5, 1e12};
    double nodes[8 * 3];
    for (int i = 0; i < 8; i++) nodes[3 * i] = i & 1, nodes[3 * i + 1] = (i >> 1) & 1, nodes[3 * i + 2] = (i >> 2) & 1;
    int hex[8] = {0, 1, 3, 2, 4, 5, 7, 6};
    float rho[28], m[2] = {1.5f, 2.5f}, nf[8];
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < 28; i++) rho[i] = (float)(i * 0.5 + f);
        for (int i = 0; i < 8; i++) nf[i] = (float)(i + 10 * f);
        lab_frame_begin(w, 0.1 * f);
        lab_part_blocks(w, "gas", 2, bl);
        lab_field(w, "rho", LAB_AT_CELL, 28, rho);
        lab_part_points(w, "bodies", 2, pts);
        lab_field(w, "mass", LAB_AT_NODE, 2, m);
        lab_part_cells(w, "plate", 8, nodes, 1, LAB_HEX, hex);
        lab_field(w, "u", LAB_AT_NODE, 8, nf);
        CHECK(lab_frame_end(w), "frame %d not written", f);
    }
    CHECK(lab_close(w), "close reported a write error");
    LabFile *f = lab_open(path, err, sizeof err);
    CHECK(f, "%s", err);
    if (!f) return 1;
    CHECK(lab_frame_count(f) == 3, "frames %d, expected 3", lab_frame_count(f));
    CHECK(strstr(lab_header(f), "kg/m^3") != NULL, "header lost");
    LabFrame fr;
    CHECK(lab_read_frame(f, 2, &fr, err, sizeof err), "%s", err);
    CHECK(fabs(fr.time - 0.2) < 1e-15 && fr.nparts == 3, "time %g parts %d", fr.time, fr.nparts);
    const LabPart *g = lab_find_part(&fr, "gas");
    CHECK(g && g->nblocks == 2 && memcmp(g->blocks, bl, sizeof bl) == 0, "blocks differ");
    const LabField *r = lab_find_field(g, "rho");
    CHECK(r && r->count == 28 && r->data[27] == (float)(27 * 0.5 + 2), "rho differs");
    const LabPart *b = lab_find_part(&fr, "bodies");
    CHECK(b && b->npoints == 2 && memcmp(b->xyz, pts, sizeof pts) == 0, "points differ");
    const LabPart *c = lab_find_part(&fr, "plate");
    CHECK(c && c->ncells == 1 && c->cell_type == LAB_HEX && memcmp(c->conn, hex, sizeof hex) == 0, "cells differ");
    CHECK(c && lab_find_field(c, "u") && lab_find_field(c, "u")->data[7] == 27.0f, "node field differs");
    lab_frame_free(&fr);
    lab_close_file(f);
    printf("  L1 three frames, three part kinds, 28 + 2 + 8 field values: %s\n", failures ? "differ" : "identical");

    printf("== L2: a file cut inside its last frame\n");
    FILE *fp = fopen(path, "rb");
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *all = malloc((size_t)size);
    size_t got = fread(all, 1, (size_t)size, fp);
    fclose(fp);
    fp = fopen(path, "wb");
    fwrite(all, 1, got - 40, fp);
    fclose(fp);
    free(all);
    f = lab_open(path, err, sizeof err);
    CHECK(f && lab_frame_count(f) == 2, "a cut file should open with 2 frames, got %d", f ? lab_frame_count(f) : -1);
    if (f) {
        CHECK(lab_read_frame(f, 1, &fr, err, sizeof err), "%s", err);
        lab_frame_free(&fr);
        lab_close_file(f);
    }
    printf("  L2 %ld bytes cut to %ld: 2 complete frames open\n", size, size - 40);

    printf("== L3: not a lab result\n");
    fp = fopen(path, "wb");
    fputs("hello, world\n", fp);
    fclose(fp);
    f = lab_open(path, err, sizeof err);
    CHECK(!f && strstr(err, "not a lab result"), "a text file was accepted");
    printf("  L3 refused: %s\n", err);
    remove(path);
    printf(failures ? "labtest: %d FAILED\n" : "labtest: all passed\n", failures);
    return failures ? 1 : 0;
}
