/* glutil.c - shader compilation and render targets */
#include "glutil.h"
#include "common.h"

static GLuint compile(const char *name, GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        const char *kind = type == GL_VERTEX_SHADER ? "vertex" : type == GL_FRAGMENT_SHADER ? "fragment" : "geometry";
        LOGE("shader '%s' (%s) failed:\n%s", name, kind, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint gl_program(const char *name, const char *vs, const char *gs, const char *fs) {
    GLuint v = compile(name, GL_VERTEX_SHADER, vs);
    GLuint g = gs ? compile(name, GL_GEOMETRY_SHADER, gs) : 0;
    GLuint f = compile(name, GL_FRAGMENT_SHADER, fs);
    if (!v || !f || (gs && !g)) {
        if (v) glDeleteShader(v);
        if (g) glDeleteShader(g);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    if (g) glAttachShader(p, g);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    if (g) glDeleteShader(g);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        LOGE("program '%s' link failed:\n%s", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint gl_program_tf(const char *name, const char *vs, const char **varyings, int nvaryings) {
    GLuint v = compile(name, GL_VERTEX_SHADER, vs);
    if (!v) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glTransformFeedbackVaryings(p, nvaryings, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    glDeleteShader(v);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        LOGE("program '%s' link failed:\n%s", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void gl_check(const char *where) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) LOGE("GL error 0x%04x at %s", e, where);
}

bool gl_target_create(GLTarget *t, int w, int h, GLenum internal, bool depth, int samples) {
    memset(t, 0, sizeof *t);
    t->w = w, t->h = h, t->samples = samples, t->internal = internal;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    if (samples > 1) {
        glGenRenderbuffers(1, &t->color_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, t->color_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->color_rb);
    } else {
        glGenTextures(1, &t->color_tex);
        glBindTexture(GL_TEXTURE_2D, t->color_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color_tex, 0);
    }
    if (depth) {
        glGenRenderbuffers(1, &t->depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, t->depth_rb);
        if (samples > 1)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth_rb);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("framebuffer incomplete (0x%04x) %dx%d samples=%d", st, w, h, samples);
        gl_target_destroy(t);
        return false;
    }
    return true;
}

void gl_target_destroy(GLTarget *t) {
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->color_tex) glDeleteTextures(1, &t->color_tex);
    if (t->color_rb) glDeleteRenderbuffers(1, &t->color_rb);
    if (t->depth_rb) glDeleteRenderbuffers(1, &t->depth_rb);
    memset(t, 0, sizeof *t);
}

GLuint gl_texture_2d(int w, int h, GLenum internal, GLenum format, GLenum type, const void *data, bool linear) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, data);
    GLint filt = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}
