/* labfilm.c - pictures and films of any physics-lab result, headless: the command line around src/lab/labview.
 *
 *   build/labfilm RESULT.lab --info
 *   build/labfilm RESULT.lab --out DIR [options]      writes DIR/frame_0000.png ...
 *
 * The options are described in src/lab/labview.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/lab/labio.h"
#include "../src/lab/labview.h"
#include "../src/render/swrender.h"

static void usage(void) {
    fprintf(stderr, "usage: labfilm RESULT.lab --info | --out DIR [--field F] [--range LO HI] [--cmap C] [--size WxH] [--view V]\n"
                    "       [--mesh] [--levels] [--mirror] [--schlieren] [--every N] [--first N] [--last N] [--title T] [--unit U]\n"
                    "       [--radius PX] [--solid F] [--lines F [N]] [--nearest] [--log] [--abs] [--light] [--zoom F] [--center X Y]\n");
}

int main(int argc, char **argv) {
    LabViewOpts o;
    labview_defaults(&o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--info")) o.info = true;
        else if (!strcmp(a, "--out") && more) o.out = argv[++i];
        else if (!strcmp(a, "--field") && more) o.field = argv[++i];
        else if (!strcmp(a, "--title") && more) o.title = argv[++i];
        else if (!strcmp(a, "--unit") && more) o.unit = argv[++i];
        else if (!strcmp(a, "--view") && more) o.view = argv[++i];
        else if (!strcmp(a, "--solid") && more) o.solid = argv[++i];
        else if (!strcmp(a, "--lines") && more) {
            o.lines = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') o.nlines = atoi(argv[++i]);
        }
        else if (!strcmp(a, "--range") && i + 2 < argc) o.lo = atof(argv[++i]), o.hi = atof(argv[++i]), o.range_given = true;
        else if (!strcmp(a, "--center") && i + 2 < argc) o.cx = atof(argv[++i]), o.cy = atof(argv[++i]), o.center_given = true;
        else if (!strcmp(a, "--cmap") && more) {
            const char *c = argv[++i];
            o.cmap = !strcmp(c, "gray") ? 100 : !strcmp(c, "ink") ? 101 : !strcmp(c, "ember") ? 102 : sw_colormap_find(c);
            if (o.cmap < 0) {
                fprintf(stderr, "unknown colour map %s\n", c);
                return 2;
            }
        } else if (!strcmp(a, "--size") && more) {
            if (sscanf(argv[++i], "%dx%d", &o.w, &o.h) != 2) return usage(), 2;
        } else if (!strcmp(a, "--every") && more) o.every = atoi(argv[++i]);
        else if (!strcmp(a, "--first") && more) o.first = atoi(argv[++i]);
        else if (!strcmp(a, "--last") && more) o.last = atoi(argv[++i]);
        else if (!strcmp(a, "--radius") && more) o.radius = atof(argv[++i]);
        else if (!strcmp(a, "--zoom") && more) o.zoom = atof(argv[++i]);
        else if (!strcmp(a, "--azim") && more) o.azim = atof(argv[++i]), o.angles = true;
        else if (!strcmp(a, "--focus") && more) o.focus = atoi(argv[++i]);
        else if (!strcmp(a, "--span") && more) o.span = atof(argv[++i]);
        else if (!strcmp(a, "--labels")) o.labels = true;
        else if (!strcmp(a, "--true-size")) o.true_size = true;
        else if (!strcmp(a, "--elev") && more) o.elev = atof(argv[++i]), o.angles = true;
        else if (!strcmp(a, "--mesh")) o.mesh = true;
        else if (!strcmp(a, "--levels")) o.levels = true;
        else if (!strcmp(a, "--mirror")) o.mirror = true;
        else if (!strcmp(a, "--mesh-below")) o.mesh_below = o.mirror = true;
        else if (!strcmp(a, "--schlieren")) o.schlieren = true;
        else if (!strcmp(a, "--nearest")) o.nearest = true;
        else if (!strcmp(a, "--log")) o.logscale = true;
        else if (!strcmp(a, "--abs")) o.absval = true;
        else if (!strcmp(a, "--light")) o.light = true;
        else if (a[0] != '-' && !o.in) o.in = a;
        else {
            usage();
            return 2;
        }
    }
    if (!o.in || (!o.info && !o.out) || o.every < 1 || o.zoom <= 0) {
        usage();
        return 2;
    }
    char err[256];
    LabFile *lf = lab_open(o.in, err, sizeof err);
    if (!lf) {
        fprintf(stderr, "labfilm: %s\n", err);
        return 1;
    }
    if (o.info) {
        int rc = labview_info(lf);
        lab_close_file(lf);
        return rc;
    }
    mkdir(o.out, 0755);
    int n = lab_frame_count(lf);
    if (o.last > n - 1) o.last = n - 1;
    double lo = o.lo, hi = o.hi;
    if (!o.range_given && o.field[0]) labview_range(lf, &o, &lo, &hi);
    int written = 0;
    for (int fi = o.first; fi <= o.last; fi += o.every) {
        LabFrame fr;
        if (!lab_read_frame(lf, fi, &fr, err, sizeof err)) {
            fprintf(stderr, "labfilm: %s\n", err);
            continue;
        }
        SwImage small;
        if (!labview_render(lf, &fr, &o, lo, hi, &small)) {
            lab_frame_free(&fr);
            continue;
        }        unsigned char *png;
        size_t len;
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%04d.png", o.out, written);
        if (sw_png(&small, &png, &len)) {
            FILE *fp = fopen(path, "wb");
            if (!fp || fwrite(png, 1, len, fp) != len) fprintf(stderr, "labfilm: cannot write %s\n", path);
            if (fp) fclose(fp);
            free(png);
        }
        sw_image_free(&small);
        lab_frame_free(&fr);
        written++;
    }
    fprintf(stderr, "labfilm: %d frames to %s, range %.6g to %.6g\n", written, o.out, lo, hi);
    lab_close_file(lf);
    return 0;
}
