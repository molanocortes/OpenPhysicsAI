/* camera.c - orbit camera */
#include "camera.h"
#include "common.h"

static void rebuild(Camera *c) {
    float cp = cosf(c->pitch), sp = sinf(c->pitch);
    vec3 dir = v3(cp * cosf(c->yaw), sp, cp * sinf(c->yaw));
    c->eye = v3_add(c->target, v3_scale(dir, c->dist));
    c->forward = v3_neg(dir);
    c->right = v3_norm(v3_cross(c->forward, v3(0, 1, 0)));
    c->up = v3_cross(c->right, c->forward);
    c->znear = MAXI(c->dist * 0.005f, 0.05f);
    c->zfar = c->dist * 20.0f + 2000.0f;
    float aspect = c->height > 0 ? (float)c->width / (float)c->height : 1.0f;
    c->view = m4_lookat(c->eye, c->target, v3(0, 1, 0));
    c->proj = m4_perspective(DEG2RAD(c->fov_deg), aspect, c->znear, c->zfar);
    if (c->shift_x != 0.0f) c->proj = m4_mul(m4_translate(v3(c->shift_x, 0, 0)), c->proj);
    c->viewproj = m4_mul(c->proj, c->view);
    if (!m4_invert(c->viewproj, &c->inv_viewproj)) c->inv_viewproj = m4_identity();
}

void camera_init(Camera *c, vec3 target, float dist, float yaw_deg, float pitch_deg) {
    memset(c, 0, sizeof *c);
    c->target = c->target_goal = target;
    c->dist = c->dist_goal = dist;
    c->yaw = c->yaw_goal = DEG2RAD(yaw_deg);
    c->pitch = c->pitch_goal = DEG2RAD(pitch_deg);
    c->fov_deg = 40.0f;
    c->min_dist = 1.0f;
    c->max_dist = 20000.0f;
    c->width = c->height = 1;
    rebuild(c);
}

void camera_update(Camera *c, double dt, int width, int height) {
    c->width = width > 0 ? width : 1;
    c->height = height > 0 ? height : 1;
    float k = 1.0f - (float)exp(-dt * 14.0);
    c->yaw += (c->yaw_goal - c->yaw) * k;
    c->pitch += (c->pitch_goal - c->pitch) * k;
    c->dist += (c->dist_goal - c->dist) * k;
    c->target = v3_lerp(c->target, c->target_goal, k);
    rebuild(c);
}

void camera_snap(Camera *c) {
    c->yaw = c->yaw_goal, c->pitch = c->pitch_goal, c->dist = c->dist_goal, c->target = c->target_goal;
    rebuild(c);
}

void camera_orbit(Camera *c, float dx, float dy) {
    c->yaw_goal += dx * 0.0065f;
    c->pitch_goal += dy * 0.0065f;
    c->pitch_goal = CLAMP(c->pitch_goal, DEG2RAD(-89.0f), DEG2RAD(89.0f));
}

static float pixel_world_size(const Camera *c) {
    return 2.0f * c->dist * tanf(DEG2RAD(c->fov_deg) * 0.5f) / (float)c->height;
}

void camera_pan(Camera *c, float dx, float dy) {
    if (c->locked) return;
    float s = pixel_world_size(c);
    c->target_goal = v3_add(c->target_goal, v3_add(v3_scale(c->right, -dx * s), v3_scale(c->up, dy * s)));
}

void camera_zoom(Camera *c, float factor) {
    c->dist_goal = CLAMP(c->dist_goal * factor, c->min_dist, c->max_dist);
}

void camera_zoom_at(Camera *c, float factor, float px, float py) {
    float before = c->dist_goal;
    camera_zoom(c, factor);
    if (c->locked) return;
    /* move the pivot toward the point under the cursor on the pivot plane */
    vec3 o, d;
    camera_ray(c, px, py, &o, &d);
    float denom = v3_dot(d, c->forward);
    if (denom < 1e-4f) return;
    float t = v3_dot(v3_sub(c->target, o), c->forward) / denom;
    vec3 hit = v3_add(o, v3_scale(d, t));
    float frac = 1.0f - c->dist_goal / before;
    c->target_goal = v3_add(c->target_goal, v3_scale(v3_sub(hit, c->target), frac));
}

void camera_set_angles(Camera *c, float yaw_deg, float pitch_deg) {
    float y = DEG2RAD(yaw_deg);
    /* take the shortest way around */
    float cur = c->yaw_goal;
    float twopi = 2.0f * (float)M_PI;
    y += twopi * roundf((cur - y) / twopi);
    c->yaw_goal = y;
    c->pitch_goal = DEG2RAD(CLAMP(pitch_deg, -89.0f, 89.0f));
}

void camera_focus(Camera *c, vec3 point, float dist) {
    c->target_goal = point;
    if (dist > 0) c->dist_goal = CLAMP(dist, c->min_dist, c->max_dist);
}

void camera_ray(const Camera *c, float px, float py, vec3 *origin, vec3 *dir) {
    float nx = 2.0f * px / (float)c->width - 1.0f;
    float ny = 1.0f - 2.0f * py / (float)c->height;
    vec4 a = m4_mul_vec4(c->inv_viewproj, (vec4){nx, ny, -1.0f, 1.0f});
    vec4 b = m4_mul_vec4(c->inv_viewproj, (vec4){nx, ny, 1.0f, 1.0f});
    vec3 pa = v3(a.x / a.w, a.y / a.w, a.z / a.w);
    vec3 pb = v3(b.x / b.w, b.y / b.w, b.z / b.w);
    *origin = c->eye;
    *dir = v3_norm(v3_sub(pb, pa));
}

bool camera_project(const Camera *c, vec3 p, float *px, float *py) {
    vec4 h = m4_mul_vec4(c->viewproj, (vec4){p.x, p.y, p.z, 1.0f});
    if (h.w <= 1e-6f) return false;
    *px = (h.x / h.w * 0.5f + 0.5f) * (float)c->width;
    *py = (0.5f - h.y / h.w * 0.5f) * (float)c->height;
    return true;
}
