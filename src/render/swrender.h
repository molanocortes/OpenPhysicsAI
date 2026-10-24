/* swrender.h - software rasteriser for headless images (no OpenGL): geometry views with known cameras, selection
 * highlights and colour-mapped FEM results with legends, for MCP clients and reports.
 *
 * Camera model: right-handed world (build frame, metres). Pixel (0, 0) is the top-left corner of the image; pixel
 * centres are at (i + 0.5, j + 0.5). A perspective camera maps a point p to
 *   xc = (p - eye).right, yc = (p - eye).up, zc = (p - eye).forward
 *   px = W/2 + f xc / zc,  py = H/2 - f yc / zc,  f = (H/2) / tan(fov/2)
 * and an orthographic camera uses f' = H / ortho_height instead of f / zc. sw_ray inverts this exactly, so a pixel
 * picked in a rendered image maps back to a ray through the model. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct SwCamera {
    double eye[3], target[3], up_hint[3];
    double fov_deg;      /* > 0 perspective (vertical field of view); 0 orthographic */
    double ortho_height; /* visible world height for orthographic cameras (m) */
    int width, height;
    double right[3], up[3], forward[3]; /* orthonormal basis */
} SwCamera;

typedef struct SwImage {
    int w, h;
    unsigned char *rgb; /* 3*w*h */
    double *depth;      /* w*h: camera-space depth, +inf when empty */
    unsigned char *mask;/* w*h: object id + 1 written by triangles (0 = background), for picking tests */
} SwImage;

bool sw_image_init(SwImage *im, int w, int h);
void sw_image_free(SwImage *im);
void sw_clear(SwImage *im, const unsigned char top[3], const unsigned char bottom[3]);

bool sw_camera_look(SwCamera *c, const double eye[3], const double target[3], const double up[3], double fov_deg, int w, int h);
/* Frames a bounding box from a named direction: iso, iso_back, front (looking along +y), back, left (along +x), right,
 * top (looking down), bottom. */
bool sw_camera_preset(SwCamera *c, const char *preset, const double bmin[3], const double bmax[3], double fov_deg, int w, int h);
bool sw_project(const SwCamera *c, const double p[3], double *px, double *py, double *depth);
void sw_ray(const SwCamera *c, double px, double py, double origin[3], double dir[3]);

/* triangle with per-vertex colours (0..1); shade applies a two-sided headlight; id tags the pixels it covers */
void sw_triangle(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, const float *c0, const float *c1,
                 const float *c2, bool shade, int id);
/* LAB and LAB_SIGNED are the physics lab's shared palette (src/lab/style.h): one sequential map for magnitudes, one
 * diverging map whose zero is the dark of the room, used by every domain so that all results look like one world;
 * LAB_SOLID is the sequential map on a solid's surface: it starts at the material's own light metal */
typedef enum { SW_CMAP_TURBO = 0, SW_CMAP_VIRIDIS, SW_CMAP_COOLWARM, SW_CMAP_HEAT, SW_CMAP_LAB, SW_CMAP_LAB_SIGNED, SW_CMAP_LAB_SOLID, SW_CMAP_COUNT } SwColormap;
/* Triangle coloured by a scalar field: the value is interpolated (perspective-correct) and mapped through the colour
 * map per pixel, so a coarse element still shows the full range between its nodal values. */
void sw_triangle_scalar(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, double s0, double s1,
                        double s2, double lo, double hi, SwColormap cmap, bool shade, int id);
/* the same, drawn as glass: the pixel is blended with what is already there and the depth buffer is left alone, so the
 * caller must draw the transparent triangles after the solid ones and from the back forwards */
void sw_triangle_scalar_alpha(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, double s0, double s1,
                              double s2, double lo, double hi, SwColormap cmap, bool shade, int id, double alpha);
/* depth-tested line; depth_bias pulls it toward the camera so edges on surfaces stay visible */
void sw_line(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const unsigned char rgb[3], double depth_bias, int thickness);
void sw_rect(SwImage *im, int x, int y, int w, int h, const unsigned char rgb[3]);
void sw_text(SwImage *im, int x, int y, int scale, const char *utf8, const unsigned char rgb[3]);
int sw_text_width(const char *utf8, int scale);

void sw_colormap(SwColormap cmap, double t, float rgb[3]);
int sw_colormap_find(const char *name);
const char *sw_colormap_name(SwColormap cmap);
/* vertical colour bar with tick labels at x, y (top-left) */
void sw_colorbar(SwImage *im, int x, int y, int w, int h, SwColormap cmap, double lo, double hi, const char *title, const char *unit);
/* axis triad (x red, y green, z blue) centred at pixel (cx, cy) */
void sw_axes(SwImage *im, const SwCamera *c, int cx, int cy, int size);

/* 2x2 box-filter downsampling (render at double resolution for anti-aliasing) */
bool sw_downsample(const SwImage *src, SwImage *dst);
bool sw_png(const SwImage *im, unsigned char **png, size_t *len);
