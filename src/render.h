/* render.h - OpenGL 4.1 scene renderer: model surface, slices, streamlines, GPU particles, vortex
 * isosurfaces, volume rendering, HDR bloom and tone mapping. World space == lattice space. */
#pragma once

#include "camera.h"
#include "geom/mesh.h"
#include "glutil.h"
#include "threads.h"
#include "vis.h"

typedef enum { SURFACE_HIDDEN = 0, SURFACE_MATERIAL, SURFACE_FIELD } SurfaceMode;

typedef struct RenderSettings {
    int field;           /* VisField */
    int cmap;            /* ColormapId for surfaces, slices and volumes */
    int line_cmap;       /* ColormapId for streamlines and particles, -1 = same as cmap */
    bool auto_range;
    int surface_mode;
    bool surface_grid;
    bool slice_on;
    int slice_axis;      /* 0 x, 1 y, 2 z */
    float slice_pos;     /* 0..1 */
    float slice_opacity;
    bool stream_on;
    float stream_width;  /* pixels */
    bool stream_animate;
    bool particles_on;
    int particle_count;
    float particle_size; /* pixels */
    bool vortex_on;
    float vortex_level;  /* Q threshold relative to (u_lb / L)^2 */
    bool volume_on;
    float volume_density;
    bool result_on;      /* finite-element result surface (fembridge) */
    bool ao_on;          /* screen-space ambient occlusion over the depth buffer */
    float ao_strength;   /* 0 none, 1 full */
    bool floor_on;
    bool box_on;
    float bloom;
    /* the tunnel's backdrop. neutral: the dark neutral gradient of the showcase preset (docs/images/SHOWCASE.md) in
     * place of the blue vignette; the two colours are linear values before exposure and tone mapping */
    bool backdrop_neutral;
    float backdrop_top[3], backdrop_bottom[3];
    float exposure;
    int msaa;
} RenderSettings;

void render_settings_default(RenderSettings *rs);

/* One drawn piece of the result surface: a range of its index buffer, how solid it is, and where it has been moved
 * to (the exploded view). Pieces are drawn opaque first, then the transparent ones back to front. */
enum { FEM_MAX_DRAW_PARTS = 32 };

typedef struct ResultPart {
    uint32_t index_start, index_count;
    float opacity;
    float offset[3];
    float centre[3]; /* world centre of the piece as drawn, for the back-to-front order */
} ResultPart;

typedef struct RenderFrame {
    const Camera *cam;
    const RenderSettings *rs;
    double time;           /* wall clock for animation */
    float dt;              /* seconds since previous frame */
    int nx, ny, nz;        /* lattice */
    bool has_model;
    mat4 model;            /* mesh space -> lattice */
    float field_lo, field_hi;  /* lattice-unit range mapped to the colour map */
    float speed_hi;            /* lattice speed mapped to the top of the colour map for particles */
    float particle_steps;      /* lattice time steps advanced since the previous frame */
    vec3 emitter_lo, emitter_hi; /* particle emitter box (lattice coords) */
    bool probe_on;
    vec3 probe;
    const float *result_outline;
    int result_outline_vertices;
    const ResultPart *result_parts; /* NULL: draw the whole surface opaque */
    int result_nparts;
    const float *result_edges;      /* feature edges of the geometry, GL_LINES pairs in world space */
    int result_edge_vertices;
    const ResultPart *edge_parts;   /* the same pieces, indexing result_edges */
    int edge_nparts;
    bool peak_on;          /* the marker on the largest value of the drawn result */
    vec3 peak;
    float result_lo, result_hi; /* display-unit range of the result surface */
    const float *overlay;  /* GL_LINES vertex pairs in lattice space, drawn on top (streamline rake gizmo) */
    int overlay_verts;
    GLuint target_fbo;     /* 0 = window */
    int target_w, target_h;
} RenderFrame;

typedef struct Renderer Renderer;

Renderer *render_create(void);
void render_destroy(Renderer *r);
void render_set_colormap(Renderer *r, int cmap);
void render_set_line_colormap(Renderer *r, int cmap); /* -1 follows render_set_colormap */
void render_set_result_colormap(Renderer *r, int cmap); /* -1 follows render_set_colormap */
int render_result_colormap(const Renderer *r);
GLuint render_colormap_texture(const Renderer *r);
GLuint render_line_colormap_texture(const Renderer *r);
void render_set_mesh(Renderer *r, const Mesh *m);
void render_set_grid(Renderer *r, int nx, int ny, int nz);
void render_upload_field(Renderer *r, const float *field);
/* field stored as half floats of (value / scale); colour ranges stay in lattice units */
void render_upload_field_half(Renderer *r, const uint16_t *half, float scale);
/* Packs velocity into interleaved half floats (3 per cell); thread-safe, no GL calls. */
void render_pack_velocity(const float *ux, const float *uy, const float *uz, size_t n, uint16_t *out, ThreadPool *pool);
/* Velocity volume at half resolution: ((nx+1)/2) x ((ny+1)/2) x ((nz+1)/2) texels, 3 half floats each. */
void render_upload_velocity_half(Renderer *r, const uint16_t *half);
void render_upload_solid(Renderer *r, const uint8_t *solid);
void render_set_streamlines(Renderer *r, const StreamlineSet *s);
void render_set_isosurface(Renderer *r, const IsoMesh *m);
/* finite-element result surface: the same vertex layout as an isosurface, drawn with its own colour range */
void render_set_result(Renderer *r, const IsoMesh *m);
void render_reset_particles(Renderer *r);
void render_frame(Renderer *r, const RenderFrame *f);

GLuint render_result_colormap_texture(const Renderer *r);
