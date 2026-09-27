/* rendertest.c - software renderer and PNG encoder checks
 *   camera projection and pixel rays are exact inverses (perspective and orthographic)
 *   depth ordering of overlapping triangles, picking mask
 *   PNG files for an independent decoder (tools/pngcheck.py) and a labelled scene image for visual inspection
 *   make build/rendertest && ./build/rendertest build/rendertest_out && python3 tools/pngcheck.py build/rendertest_out */
#include "../src/core/png.h"
#include "../src/render/swrender.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond) g_pass++;                                                                                           \
        else {                                                                                                        \
            g_fail++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static bool write_raw(const char *path, const unsigned char *d, size_t n) {
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(d, 1, n, f) == n;
    if (f) fclose(f);
    return ok;
}

/* The result images place the colour bar near the right edge (viewrender.c: bar_x = W - 110). At 1600x900 the text
 * scale doubles, so the title and the unit have to fit in what is left: this checks that nothing is clipped at the
 * edge and that the unit does not land on the lowest tick label. */
static int ink_count(const SwImage *im, int x0, int y0, int x1, int y1, int *min_y, int *max_y) {
    int n = 0;
    if (min_y) *min_y = im->h;
    if (max_y) *max_y = -1;
    for (int y = y0; y < y1 && y < im->h; y++)
        for (int x = x0; x < x1 && x < im->w; x++) {
            const unsigned char *p = &im->rgb[3 * ((size_t)y * im->w + x)];
            if (p[0] > 200 && p[1] > 200 && p[2] > 200) { /* the legend ink is near white */
                n++;
                if (min_y && y < *min_y) *min_y = y;
                if (max_y && y > *max_y) *max_y = y;
            }
        }
    return n;
}

static void test_legend_layout(void) {
    printf("== legend layout at the size the result images use\n");
    const int W = 1600, H = 900, bar_w = 16, bar_y = 70, bar_h = H * 45 / 100, bar_x = W - 110;
    static const unsigned char BG[3] = {18, 20, 26};
    SwImage edge, room;
    CHECK(sw_image_init(&edge, W, H) && sw_image_init(&room, W, H), "images");
    sw_clear(&edge, BG, BG);
    sw_clear(&room, BG, BG);
    sw_colorbar(&edge, bar_x, bar_y, bar_w, bar_h, SW_CMAP_TURBO, 20.0, 166.0, "temperature", "degC");
    sw_colorbar(&room, 400, bar_y, bar_w, bar_h, SW_CMAP_TURBO, 20.0, 166.0, "temperature", "degC");
    int at_edge = ink_count(&edge, 0, 0, W, H, NULL, NULL);
    int with_room = ink_count(&room, 0, 0, W, H, NULL, NULL);
    CHECK(at_edge == with_room, "no legend text is lost at the right edge (%d ink pixels against %d with room)", at_edge,
          with_room);
    CHECK(ink_count(&edge, W - 2, 0, W, H, NULL, NULL) == 0, "no legend text touches the last two columns");
    /* the same bar without the unit tells which ink belongs to the unit and which to the ticks */
    SwImage plain;
    CHECK(sw_image_init(&plain, W, H), "image");
    sw_clear(&plain, BG, BG);
    sw_colorbar(&plain, bar_x, bar_y, bar_w, bar_h, SW_CMAP_TURBO, 20.0, 166.0, "temperature", NULL);
    int tick_bottom = -1, unit_top = H;
    ink_count(&plain, 0, bar_y, W, H, NULL, &tick_bottom);
    for (int yy = bar_y + bar_h; yy < H && unit_top == H; yy++)
        for (int xx = 0; xx < W; xx++) {
            const unsigned char *a = &edge.rgb[3 * ((size_t)yy * W + xx)], *b = &plain.rgb[3 * ((size_t)yy * W + xx)];
            if (a[0] > 200 && a[1] > 200 && a[2] > 200 && !(b[0] > 200 && b[1] > 200 && b[2] > 200)) {
                unit_top = yy;
                break;
            }
        }
    CHECK(tick_bottom > 0 && unit_top < H && unit_top > tick_bottom,
          "the unit sits below the lowest tick label (unit from row %d, ticks to row %d)", unit_top, tick_bottom);
    sw_image_free(&edge);
    sw_image_free(&room);
    sw_image_free(&plain);
}

static void test_png(const char *dir) {
    printf("== PNG encoder output for the independent decoder\n");
    char path[1024];
    int w = 97, h = 61;
    unsigned char *rgba = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = rgba + 4 * ((size_t)y * w + x);
            p[0] = (unsigned char)(x * 7 + y * 13 + (x * y) % 31);
            p[1] = (unsigned char)((x ^ y) * 5);
            p[2] = x < w / 2 ? 200 : (unsigned char)(y * 4); /* flat region: long matches */
            p[3] = (unsigned char)(255 - y);
        }
    snprintf(path, sizeof path, "%s/pattern_rgba.png", dir);
    CHECK(png_write_file(path, rgba, w, h, 4), "write %s", path);
    snprintf(path, sizeof path, "%s/pattern_rgba.raw", dir);
    write_raw(path, rgba, (size_t)w * h * 4);
    free(rgba);

    w = 640, h = 17;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = rgb + 3 * ((size_t)y * w + x);
            p[0] = (unsigned char)((x / 16) * 9);
            p[1] = (unsigned char)(y * 15);
            p[2] = (unsigned char)((x * 31) % 251);
        }
    snprintf(path, sizeof path, "%s/stripes_rgb.png", dir);
    CHECK(png_write_file(path, rgb, w, h, 3), "write %s", path);
    snprintf(path, sizeof path, "%s/stripes_rgb.raw", dir);
    write_raw(path, rgb, (size_t)w * h * 3);
    free(rgb);

    unsigned char *z;
    size_t zn;
    static const unsigned char TEXT[] = "abcabcabcabcabcabcabcabcabcabc-the-quick-brown-fox-the-quick-brown-fox";
    CHECK(zlib_compress(TEXT, sizeof TEXT - 1, &z, &zn) && zn < sizeof TEXT - 1, "repetitive text compresses (%zu -> %zu bytes)", sizeof TEXT - 1, zn);
    snprintf(path, sizeof path, "%s/text.zlib", dir);
    write_raw(path, z, zn);
    snprintf(path, sizeof path, "%s/text.raw", dir);
    write_raw(path, TEXT, sizeof TEXT - 1);
    free(z);
    CHECK(crc32_update(0, (const unsigned char *)"123456789", 9) == 0xCBF43926ul, "CRC-32 check value");
}

static void test_camera(void) {
    printf("== camera projection and picking rays\n");
    double bmin[3] = {-0.02, -0.01, 0}, bmax[3] = {0.02, 0.01, 0.01};
    static const char *presets[] = {"iso", "front", "top", "right", "bottom"};
    for (int fov = 0; fov < 2; fov++)
        for (int pi = 0; pi < 5; pi++) {
            SwCamera cam;
            memset(&cam, 0, sizeof cam);
            CHECK(sw_camera_preset(&cam, presets[pi], bmin, bmax, fov ? 30 : 0, 800, 600), "preset %s", presets[pi]);
            double worst = 0;
            int inside = 0;
            for (int k = 0; k < 8; k++) {
                double p[3] = {k & 1 ? bmax[0] : bmin[0], k & 2 ? bmax[1] : bmin[1], k & 4 ? bmax[2] : bmin[2]};
                double px, py, depth;
                if (!sw_project(&cam, p, &px, &py, &depth)) continue;
                inside += px >= 0 && px <= 800 && py >= 0 && py <= 600;
                double o[3], d[3];
                sw_ray(&cam, px, py, o, d);
                double v[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
                double t = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
                double e[3] = {v[0] - t * d[0], v[1] - t * d[1], v[2] - t * d[2]};
                worst = fmax(worst, sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
            }
            CHECK(worst < 1e-12 && inside == 8, "%s %s: ray reprojection error %.2e m, %d/8 corners in view", presets[pi], fov ? "perspective" : "orthographic",
                  worst, inside);
        }
}

static void test_depth(const char *dir) {
    printf("== depth ordering and scene image\n");
    SwImage im;
    sw_image_init(&im, 200, 200);
    static const unsigned char TOP[3] = {20, 24, 32}, BOT[3] = {45, 52, 66};
    sw_clear(&im, TOP, BOT);
    SwCamera cam;
    double eye[3] = {0, -1, 0}, tgt[3] = {0, 0, 0}, up[3] = {0, 0, 1};
    memset(&cam, 0, sizeof cam);
    sw_camera_look(&cam, eye, tgt, up, 40, 200, 200);
    double a0[3] = {-0.2, 0.1, -0.2}, a1[3] = {0.2, 0.1, -0.2}, a2[3] = {0, 0.1, 0.2};    /* far, blue */
    double b0[3] = {-0.2, -0.1, -0.2}, b1[3] = {0.2, -0.1, -0.2}, b2[3] = {0, -0.1, 0.2}; /* near, red */
    float blue[3] = {0, 0, 1}, red[3] = {1, 0, 0};
    sw_triangle(&im, &cam, b0, b1, b2, red, red, red, false, 2);
    sw_triangle(&im, &cam, a0, a1, a2, blue, blue, blue, false, 1);
    size_t c = 3 * (100 * 200 + 100);
    CHECK(im.rgb[c] == 255 && im.rgb[c + 2] == 0 && im.mask[100 * 200 + 100] == 3, "nearer triangle wins regardless of draw order (id mask %d)", im.mask[100 * 200 + 100]);
    sw_image_free(&im);

    /* labelled scene: a box coloured along x with its edges, axes, colour bar and title */
    SwImage hi;
    sw_image_init(&hi, 1600, 1000);
    sw_clear(&hi, TOP, BOT);
    double bmin[3] = {-0.02, -0.01, 0}, bmax[3] = {0.02, 0.01, 0.01};
    memset(&cam, 0, sizeof cam);
    sw_camera_preset(&cam, "iso", bmin, bmax, 30, 1600, 1000);
    static const int Q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    double P[8][3], S[8];
    for (int k = 0; k < 8; k++) {
        P[k][0] = k & 1 ? bmax[0] : bmin[0], P[k][1] = k & 2 ? bmax[1] : bmin[1], P[k][2] = k & 4 ? bmax[2] : bmin[2];
        S[k] = 1e3 * P[k][0]; /* field = x in mm: the full colour range must appear across each face */
    }
    for (int f = 0; f < 6; f++) {
        sw_triangle_scalar(&hi, &cam, P[Q[f][0]], P[Q[f][1]], P[Q[f][2]], S[Q[f][0]], S[Q[f][1]], S[Q[f][2]], -20, 20, SW_CMAP_TURBO, false, f);
        sw_triangle_scalar(&hi, &cam, P[Q[f][0]], P[Q[f][2]], P[Q[f][3]], S[Q[f][0]], S[Q[f][2]], S[Q[f][3]], -20, 20, SW_CMAP_TURBO, false, f);
    }
    /* the middle of the front face (x = 0) must carry the colour map's centre, not a blend of its two ends */
    double mid[3] = {0, bmin[1], 0.5 * (bmin[2] + bmax[2])}, mx, my, md;
    sw_project(&cam, mid, &mx, &my, &md);
    float expect[3];
    sw_colormap(SW_CMAP_TURBO, 0.5, expect);
    size_t mi = 3 * ((size_t)my * (size_t)hi.w + (size_t)mx);
    CHECK(fabs(hi.rgb[mi] - 255 * expect[0]) < 12 && fabs(hi.rgb[mi + 1] - 255 * expect[1]) < 12 && fabs(hi.rgb[mi + 2] - 255 * expect[2]) < 12,
          "scalar interpolation shows the colour map centre at x = 0: pixel %d %d %d, expected %.0f %.0f %.0f", hi.rgb[mi], hi.rgb[mi + 1], hi.rgb[mi + 2],
          255 * expect[0], 255 * expect[1], 255 * expect[2]);
    static const unsigned char EDGE[3] = {15, 15, 20};
    for (int f = 0; f < 6; f++)
        for (int e = 0; e < 4; e++) sw_line(&hi, &cam, P[Q[f][e]], P[Q[f][(e + 1) % 4]], EDGE, 2e-5, 3);
    SwImage out;
    CHECK(sw_downsample(&hi, &out), "downsample");
    static const unsigned char INK[3] = {235, 238, 245};
    sw_text(&out, 16, 14, 2, "Box 40 \xC3\x97 20 \xC3\x97 10 mm  (test image, 20 \xC2\xB0""C, \xC2\xB5m)", INK);
    sw_colorbar(&out, 720, 90, 18, 300, SW_CMAP_TURBO, -20.0, 20.0, "x position", "mm"); /* must match the field range */
    sw_axes(&out, &cam, 50, 440, 30);
    int covered = 0;
    for (int i = 0; i < out.w * out.h; i++) covered += out.mask[i] != 0;
    CHECK(covered > out.w * out.h / 20, "box covers %d pixels", covered);
    unsigned char *png;
    size_t n;
    CHECK(sw_png(&out, &png, &n), "encode scene");
    char path[1024];
    snprintf(path, sizeof path, "%s/scene.png", dir);
    write_raw(path, png, n);
    printf("  scene.png %d x %d, %zu bytes (%.1f%% of raw)\n", out.w, out.h, n, 100.0 * n / (out.w * out.h * 3));
    snprintf(path, sizeof path, "%s/scene.raw", dir);
    write_raw(path, out.rgb, (size_t)out.w * out.h * 3);
    free(png);
    sw_image_free(&out);
    sw_image_free(&hi);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/rendertest_out";
    mkdir(dir, 0755);
    test_legend_layout();
    test_png(dir);
    test_camera();
    test_depth(dir);
    printf("\n%s: %d passed, %d failed\n", g_fail ? "RENDER TESTS FAILED" : "ALL RENDER TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
