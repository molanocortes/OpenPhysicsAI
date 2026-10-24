/* labdiff.c - how far apart two physics-lab results are, field by field (src/lab/labio.h).
 *
 *   labdiff A.lab B.lab
 *
 * The two must have the same frames, parts and fields. For each field name it prints the largest difference between
 * the two over every frame and value, divided by the largest magnitude the field reaches; the last line is the largest
 * of these, as JSON. Exit 0 when the two have the same shape (whatever the differences), 1 when they do not or cannot
 * be read. tools/web_build.py --check uses it to say how close the WebAssembly build's results are to the native ones
 * when they are not identical byte for byte. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/lab/labio.h"

#define MAXF 64

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: labdiff A.lab B.lab\n");
        return 2;
    }
    char err[256];
    LabFile *a = lab_open(argv[1], err, sizeof err), *b = a ? lab_open(argv[2], err, sizeof err) : NULL;
    if (!a || !b) {
        fprintf(stderr, "labdiff: %s\n", err);
        return 1;
    }
    int n = lab_frame_count(a);
    if (n != lab_frame_count(b)) {
        fprintf(stderr, "labdiff: %d frames against %d\n", n, lab_frame_count(b));
        return 1;
    }
    char names[MAXF][48];
    double diff[MAXF] = {0}, mag[MAXF] = {0};
    int nf = 0;
    for (int f = 0; f < n; f++) {
        LabFrame fa, fb;
        if (!lab_read_frame(a, f, &fa, err, sizeof err) || !lab_read_frame(b, f, &fb, err, sizeof err)) {
            fprintf(stderr, "labdiff: frame %d: %s\n", f, err);
            return 1;
        }
        if (fa.nparts != fb.nparts) {
            fprintf(stderr, "labdiff: frame %d has %d parts against %d\n", f, fa.nparts, fb.nparts);
            return 1;
        }
        for (int p = 0; p < fa.nparts; p++) {
            const LabPart *pa = &fa.parts[p], *pb = &fb.parts[p];
            if (pa->nfields != pb->nfields) {
                fprintf(stderr, "labdiff: frame %d part %s: different fields\n", f, pa->name);
                return 1;
            }
            for (int k = 0; k < pa->nfields; k++) {
                const LabField *A = &pa->fields[k], *B = &pb->fields[k];
                if (strcmp(A->name, B->name) || A->count != B->count) {
                    fprintf(stderr, "labdiff: frame %d part %s: field %s against %s\n", f, pa->name, A->name, B->name);
                    return 1;
                }
                int s = 0;
                while (s < nf && strcmp(names[s], A->name)) s++;
                if (s == nf) {
                    if (nf == MAXF) continue;
                    snprintf(names[nf++], sizeof names[0], "%s", A->name);
                }
                for (size_t i = 0; i < A->count; i++) {
                    double x = A->data[i], y = B->data[i];
                    if (isfinite(x)) mag[s] = fmax(mag[s], fabs(x));
                    if (isfinite(x) && isfinite(y)) diff[s] = fmax(diff[s], fabs(x - y));
                    else if (isfinite(x) != isfinite(y)) diff[s] = INFINITY;
                }
            }
        }
        lab_frame_free(&fa);
        lab_frame_free(&fb);
    }
    double worst = 0;
    for (int s = 0; s < nf; s++) {
        double rel = mag[s] > 0 ? diff[s] / mag[s] : diff[s];
        printf("%-16s largest difference %.3g, %.3g of the field's largest magnitude\n", names[s], diff[s], rel);
        worst = fmax(worst, rel);
    }
    printf("{\"frames\":%d,\"fields\":%d,\"largest_relative_difference\":%.6g}\n", n, nf, worst);
    lab_close_file(a);
    lab_close_file(b);
    return 0;
}
