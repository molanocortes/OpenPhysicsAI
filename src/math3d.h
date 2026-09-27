/* math3d.h - small header-only vector/matrix library (OpenGL column-major conventions) */
#pragma once

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((float)(d) * (float)(M_PI / 180.0))
#define RAD2DEG(r) ((float)(r) * (float)(180.0 / M_PI))

typedef struct { float x, y, z; } vec3;
typedef struct { float x, y, z, w; } vec4;
typedef struct { float m[16]; } mat4; /* column-major: element (row r, col c) = m[c*4 + r] */

static inline vec3 v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline vec3 v3_mul(vec3 a, vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline vec3 v3_neg(vec3 a) { return v3(-a.x, -a.y, -a.z); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_norm(vec3 a) {
    float l = v3_len(a);
    return l > 1e-20f ? v3_scale(a, 1.0f / l) : v3(0, 0, 0);
}
static inline vec3 v3_min(vec3 a, vec3 b) {
    return v3(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z);
}
static inline vec3 v3_max(vec3 a, vec3 b) {
    return v3(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z);
}
static inline vec3 v3_lerp(vec3 a, vec3 b, float t) {
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

static inline mat4 m4_identity(void) {
    mat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return r;
}

static inline mat4 m4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static inline mat4 m4_translate(vec3 t) {
    mat4 r = m4_identity();
    r.m[12] = t.x, r.m[13] = t.y, r.m[14] = t.z;
    return r;
}

static inline mat4 m4_scale(vec3 s) {
    mat4 r = m4_identity();
    r.m[0] = s.x, r.m[5] = s.y, r.m[10] = s.z;
    return r;
}

static inline mat4 m4_rotate(vec3 axis, float rad) {
    vec3 a = v3_norm(axis);
    float c = cosf(rad), s = sinf(rad), t = 1.0f - c;
    mat4 r = m4_identity();
    r.m[0] = t * a.x * a.x + c;
    r.m[1] = t * a.x * a.y + s * a.z;
    r.m[2] = t * a.x * a.z - s * a.y;
    r.m[4] = t * a.x * a.y - s * a.z;
    r.m[5] = t * a.y * a.y + c;
    r.m[6] = t * a.y * a.z + s * a.x;
    r.m[8] = t * a.x * a.z + s * a.y;
    r.m[9] = t * a.y * a.z - s * a.x;
    r.m[10] = t * a.z * a.z + c;
    return r;
}
static inline mat4 m4_rotate_x(float rad) { return m4_rotate(v3(1, 0, 0), rad); }
static inline mat4 m4_rotate_y(float rad) { return m4_rotate(v3(0, 1, 0), rad); }
static inline mat4 m4_rotate_z(float rad) { return m4_rotate(v3(0, 0, 1), rad); }

static inline mat4 m4_perspective(float fovy_rad, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    mat4 r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zfar * znear / (znear - zfar);
    return r;
}

static inline mat4 m4_ortho(float l, float r_, float b, float t, float n, float f) {
    mat4 r = m4_identity();
    r.m[0] = 2.0f / (r_ - l);
    r.m[5] = 2.0f / (t - b);
    r.m[10] = -2.0f / (f - n);
    r.m[12] = -(r_ + l) / (r_ - l);
    r.m[13] = -(t + b) / (t - b);
    r.m[14] = -(f + n) / (f - n);
    return r;
}

static inline mat4 m4_lookat(vec3 eye, vec3 target, vec3 up) {
    vec3 f = v3_norm(v3_sub(target, eye));
    vec3 s = v3_norm(v3_cross(f, up));
    vec3 u = v3_cross(s, f);
    mat4 r = m4_identity();
    r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z, r.m[12] = -v3_dot(s, eye);
    r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z, r.m[13] = -v3_dot(u, eye);
    r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z, r.m[14] = v3_dot(f, eye);
    return r;
}

/* Transform point (w = 1), no perspective divide. */
static inline vec3 m4_mul_point(mat4 m, vec3 p) {
    return v3(m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
              m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
              m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]);
}

/* Transform direction (w = 0). */
static inline vec3 m4_mul_dir(mat4 m, vec3 d) {
    return v3(m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
              m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
              m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z);
}

static inline vec4 m4_mul_vec4(mat4 m, vec4 v) {
    vec4 r;
    r.x = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w;
    r.y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w;
    r.z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w;
    r.w = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w;
    return r;
}

static inline mat4 m4_transpose(mat4 a) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) r.m[c * 4 + rr] = a.m[rr * 4 + c];
    return r;
}

static inline bool m4_invert(mat4 a, mat4 *out) {
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-30f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] * det;
    return true;
}
