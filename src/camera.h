/* camera.h - smooth orbit camera (Y up) with pan, cursor zoom, focus and presets */
#pragma once

#include <stdbool.h>
#include "math3d.h"

typedef struct Camera {
    vec3 target, target_goal;
    float yaw, pitch, dist;             /* radians / world units */
    float yaw_goal, pitch_goal, dist_goal;
    float fov_deg;
    float shift_x;                      /* horizontal lens shift in NDC (centres the view beside side panels) */
    float min_dist, max_dist;
    bool locked;                        /* pivot locked: panning disabled */
    int width, height;                  /* viewport, pixels */
    vec3 eye, forward, right, up;
    mat4 view, proj, viewproj, inv_viewproj;
    float znear, zfar;
} Camera;

void camera_init(Camera *c, vec3 target, float dist, float yaw_deg, float pitch_deg);
void camera_update(Camera *c, double dt, int width, int height);
void camera_snap(Camera *c);
void camera_orbit(Camera *c, float dx_px, float dy_px);
void camera_pan(Camera *c, float dx_px, float dy_px);
void camera_zoom(Camera *c, float factor);
void camera_zoom_at(Camera *c, float factor, float px, float py);
void camera_set_angles(Camera *c, float yaw_deg, float pitch_deg);
void camera_focus(Camera *c, vec3 point, float dist);
/* px, py: pixels from the top-left of the viewport */
void camera_ray(const Camera *c, float px, float py, vec3 *origin, vec3 *dir);
bool camera_project(const Camera *c, vec3 p, float *px, float *py);
