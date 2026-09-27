/* glutil.h - OpenGL 4.1 core helpers */
#pragma once

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <stdbool.h>

/* Compiles and links; gs may be NULL. Prepends nothing: sources must start with #version. */
GLuint gl_program(const char *name, const char *vs, const char *gs, const char *fs);
/* Vertex-only program whose outputs are captured with transform feedback (interleaved). */
GLuint gl_program_tf(const char *name, const char *vs, const char **varyings, int nvaryings);
void gl_check(const char *where);

typedef struct GLTarget {
    GLuint fbo;
    GLuint color_tex; /* single-sample: texture */
    GLuint color_rb;  /* multisample: renderbuffer */
    GLuint depth_rb;
    int w, h, samples;
    GLenum internal;
} GLTarget;

bool gl_target_create(GLTarget *t, int w, int h, GLenum internal, bool depth, int samples);
void gl_target_destroy(GLTarget *t);

GLuint gl_texture_2d(int w, int h, GLenum internal, GLenum format, GLenum type, const void *data, bool linear);
