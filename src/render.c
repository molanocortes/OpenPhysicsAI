/* render.c - OpenGL 4.1 scene renderer */
#include "render.h"
#include "colormap.h"
#include "common.h"

#if defined(__aarch64__) || defined(__arm64__)
#include <arm_neon.h>
#endif

#define MAX_BLOOM 6

/* ---- shaders ------------------------------------------------------------------------------------ */

#define GLSL "#version 410 core\n"
#define GLSL_CMAP                                                                                      \
    "uniform sampler2D u_cmap;\n"                                                                      \
    "vec3 cmap(float t) { return pow(texture(u_cmap, vec2(clamp(t, 0.0, 1.0) * 0.99609375 + 0.001953125, 0.5)).rgb, vec3(2.2)); }\n"

static const char *VS_FULLSCREEN = GLSL
    "out vec2 v_uv;\n"
    "void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  v_uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *FS_BACKGROUND = GLSL
    "in vec2 v_uv; out vec4 o; uniform vec2 u_res; uniform float u_neutral; uniform vec3 u_ntop, u_nbot;\n"
    "void main() {\n"
    "  if (u_neutral > 0.5) { o = vec4(mix(u_nbot, u_ntop, v_uv.y), 1.0); return; }\n"
    "  vec3 top = vec3(0.010, 0.020, 0.036), bot = vec3(0.0015, 0.003, 0.006);\n"
    "  vec3 c = mix(bot, top, smoothstep(0.0, 1.0, v_uv.y));\n"
    "  vec2 q = (v_uv - vec2(0.5, 0.62)) * vec2(u_res.x / u_res.y, 1.0);\n"
    "  c += vec3(0.008, 0.026, 0.050) * exp(-dot(q, q) * 2.2);\n"
    "  o = vec4(c, 1.0); }\n";

static const char *VS_POS = GLSL
    "layout(location = 0) in vec3 a_pos; uniform mat4 u_vp; uniform vec3 u_offset; uniform float u_zbias; out vec3 v_world;\n"
    /* u_zbias pulls a line towards the eye by a fraction of its clip depth, so feature edges drawn on a surface do not
     * fight with it for the depth test */
    "void main() { vec3 p = a_pos + u_offset; v_world = p; gl_Position = u_vp * vec4(p, 1.0);\n"
    "  gl_Position.z -= u_zbias * gl_Position.w; }\n";

static const char *FS_FLOOR = GLSL
    "in vec3 v_world; out vec4 o; uniform vec3 u_center; uniform float u_minor, u_major, u_fade; uniform int u_result;\n"
    "float grid(vec2 p, float s) { vec2 q = p / s; vec2 g = abs(fract(q - 0.5) - 0.5) / max(fwidth(q), vec2(1e-5));\n"
    "  return 1.0 - min(min(g.x, g.y), 1.0); }\n"
    "void main() {\n"
    "  vec2 p = v_world.xz; float dist = length(p - u_center.xz);\n"
    "  float fade = exp(-dist * dist / (u_fade * u_fade));\n"
    "  vec2 w = floor(p / (u_minor * 0.25)); float weave = mod(w.x + w.y, 2.0);\n"
    "  vec3 c = vec3(0.004, 0.007, 0.011) + vec3(0.0025, 0.0035, 0.0045) * weave;\n"
    "  c += vec3(0.030, 0.075, 0.110) * grid(p, u_minor) * 0.6 + vec3(0.06, 0.16, 0.24) * grid(p, u_major);\n"
    /* A result is the subject, not the checkerboard. Keep the same reference-plane coordinates with a sparse,
     * neutral grid, no weave. This floor is a presentation aid, never a simulated material or physical light. */
    "  if (u_result == 1) c = vec3(0.007, 0.009, 0.012) + vec3(0.009, 0.011, 0.013) * grid(p, u_major);\n"
    "  o = vec4(c, fade); }\n";

static const char *FS_COLOR = GLSL
    "in vec3 v_world; out vec4 o; uniform vec4 u_color;\n"
    "void main() { o = u_color; }\n";

static const char *VS_MESH = GLSL
    "layout(location = 0) in vec3 a_pos; layout(location = 1) in vec3 a_nrm;\n"
    "uniform mat4 u_vp, u_model; uniform mat3 u_nmat; out vec3 v_world; out vec3 v_nrm;\n"
    "void main() { vec4 w = u_model * vec4(a_pos, 1.0); v_world = w.xyz; v_nrm = u_nmat * a_nrm;\n"
    "  gl_Position = u_vp * w; }\n";

static const char *FS_MESH = GLSL GLSL_CMAP
    "in vec3 v_world; in vec3 v_nrm; out vec4 o;\n"
    "uniform vec3 u_eye, u_n; uniform int u_mode, u_grid; uniform float u_spacing; uniform vec2 u_range;\n"
    "uniform sampler3D u_field;\n"
    "void main() {\n"
    "  vec3 N = normalize(v_nrm); if (!gl_FrontFacing) N = -N;\n"
    "  vec3 V = normalize(u_eye - v_world);\n"
    "  vec3 base = vec3(0.50, 0.54, 0.58);\n"
    "  if (u_mode == 2) {\n"
    "    float v = texture(u_field, (v_world + N * 1.25) / u_n).r;\n"
    "    base = cmap((v - u_range.x) / max(u_range.y - u_range.x, 1e-30)); }\n"
    "  vec3 L1 = normalize(vec3(0.35, 0.9, 0.25)), L2 = normalize(vec3(-0.6, 0.25, -0.75));\n"
    "  float d1 = max(dot(N, L1), 0.0), d2 = max(dot(N, L2), 0.0), hemi = 0.5 + 0.5 * N.y;\n"
    "  vec3 H = normalize(L1 + V); float spec = pow(max(dot(N, H), 0.0), 64.0);\n"
    "  float fres = pow(1.0 - max(dot(N, V), 0.0), 4.0);\n"
    "  vec3 c = base * (0.22 + 0.28 * hemi + 0.62 * d1 + 0.22 * d2) + vec3(spec * 0.35) + vec3(0.2, 0.5, 0.8) * fres * 0.25;\n"
    "  if (u_grid == 1) {\n"
    "    vec3 q = v_world / u_spacing; vec3 g = abs(fract(q - 0.5) - 0.5) / max(fwidth(q), vec3(1e-5));\n"
    "    vec3 l = (1.0 - min(g, 1.0)) * (1.0 - abs(N)) * (1.0 - abs(N));\n"
    "    c *= 1.0 - 0.45 * max(l.x, max(l.y, l.z)); }\n"
    "  o = vec4(c, 1.0); }\n";

static const char *FS_SLICE = GLSL GLSL_CMAP
    "in vec3 v_world; out vec4 o; uniform vec3 u_n; uniform vec2 u_range; uniform float u_opacity;\n"
    "uniform sampler3D u_field, u_solid;\n"
    "void main() {\n"
    "  vec3 tc = v_world / u_n; float s = texture(u_solid, tc).r;\n"
    "  float t = (texture(u_field, tc).r - u_range.x) / max(u_range.y - u_range.x, 1e-30);\n"
    "  vec3 c = cmap(t);\n"
    "  float b = t * 14.0; float band = abs(fract(b - 0.5) - 0.5) / max(fwidth(b), 1e-5);\n"
    "  c *= 0.82 + 0.18 * min(band, 1.0);\n"
    "  c = mix(c, vec3(0.015, 0.018, 0.022), smoothstep(0.3, 0.6, s));\n"
    "  o = vec4(c, u_opacity); }\n";

/* ribbon vertices come in pairs (side -1/+1) and are pushed apart in screen space (no geometry shader:
 * Apple's GL-on-Metal emulates those slowly) */
static const char *VS_STREAM = GLSL
    "layout(location = 0) in vec3 a_pos; layout(location = 1) in vec3 a_tan; layout(location = 2) in float a_s;\n"
    "layout(location = 3) in float a_t; layout(location = 4) in float a_side;\n"
    "uniform mat4 u_vp; uniform vec2 u_res; uniform float u_width;\n"
    "out float f_s; out float f_t; out float f_edge;\n"
    "void main() {\n"
    "  vec4 c0 = u_vp * vec4(a_pos, 1.0), c1 = u_vp * vec4(a_pos + a_tan, 1.0);\n"
    "  vec2 d = (c1.xy / max(c1.w, 1e-4) - c0.xy / max(c0.w, 1e-4)) * u_res;\n"
    "  float len = length(d); d = len > 1e-6 ? d / len : vec2(1.0, 0.0);\n"
    "  vec2 n = vec2(-d.y, d.x) * a_side * (u_width * 0.5 + 1.0) / (u_res * 0.5);\n"
    "  gl_Position = c0 + vec4(n * c0.w, 0.0, 0.0);\n"
    "  f_s = a_s; f_t = a_t; f_edge = a_side; }\n";

static const char *FS_STREAM = GLSL GLSL_CMAP
    "in float f_s; in float f_t; in float f_edge; out vec4 o;\n"
    "uniform vec2 u_range; uniform float u_width, u_phase, u_period, u_gain; uniform int u_animate;\n"
    "void main() {\n"
    "  vec3 c = cmap((f_s - u_range.x) / max(u_range.y - u_range.x, 1e-30));\n"
    "  float px = abs(f_edge) * (u_width * 0.5 + 1.0);\n"
    "  float a = clamp(u_width * 0.5 + 0.5 - px, 0.0, 1.0);\n"
    "  float pulse = 1.0;\n"
    "  if (u_animate == 1) { float ph = fract((f_t - u_phase) / u_period); pulse = 0.75 + 1.1 * pow(ph, 10.0); }\n"
    "  if (u_width >= 3.0) {\n" /* wide lines are drawn as lit tubes: the cross-section's normal from the offset */
    "    float e = clamp(f_edge * (u_width * 0.5 + 1.0) / (u_width * 0.5), -1.0, 1.0), nz = sqrt(max(1.0 - e * e, 0.0));\n"
    "    c = c * (0.38 + 0.62 * nz) + vec3(0.55) * exp(-(e + 0.35) * (e + 0.35) / 0.03) * nz; }\n"
    "  o = vec4(c * u_gain * pulse * a, a); }\n";

static const char *VS_PARTICLE_UPDATE = GLSL
    "layout(location = 0) in vec4 a_state; out vec4 tf_state;\n"
    "uniform sampler3D u_vel, u_solid; uniform vec3 u_n, u_vext, u_emit_lo, u_emit_hi; uniform float u_steps, u_life, u_delay;\n"
    "uniform uint u_frame; uniform int u_reset;\n"
    "uint hashu(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }\n"
    "float rnd(inout uint s) { s = hashu(s); return float(s) / 4294967295.0; }\n"
    "void main() {\n"
    "  vec3 p = a_state.xyz; float age = a_state.w;\n"
    "  uint seed = uint(gl_VertexID) * 747796405u + u_frame * 2891336453u;\n"
    "  bool respawn = false;\n"
    "  if (u_reset == 1) {\n"
    "    p = mix(u_emit_lo, u_emit_hi, vec3(rnd(seed), rnd(seed), rnd(seed)));\n"
    "    age = -rnd(seed) * u_delay;\n" /* staggered release over one flow-through: a continuous stream */
    "  } else if (age < 0.0) {\n"
    "    age += u_steps;\n"
    "  } else if (u_steps > 0.0) {\n"
    "    int n = int(clamp(ceil(u_steps * 0.3), 1.0, 6.0)); float h = u_steps / float(n);\n"
    "    for (int i = 0; i < n; i++) {\n"
    "      vec3 v1 = textureLod(u_vel, p / u_vext, 0.0).xyz;\n"
    "      vec3 v2 = textureLod(u_vel, (p + v1 * (0.5 * h)) / u_vext, 0.0).xyz;\n"
    "      p += v2 * h; }\n"
    "    age += u_steps;\n"
    "    bool outside = any(lessThan(p, vec3(0.5))) || any(greaterThan(p, u_n - vec3(0.5)));\n"
    "    respawn = outside || age > u_life || textureLod(u_solid, p / u_n, 0.0).r > 0.5;\n"
    "  }\n"
    "  if (respawn) {\n"
    "    p = mix(u_emit_lo, u_emit_hi, vec3(rnd(seed), rnd(seed), rnd(seed)));\n"
    "    age = 0.0;\n"
    "  }\n"
    "  tf_state = vec4(p, age); }\n";

/* one instanced 4-vertex strip per particle: a motion-blurred capsule along the local velocity */
static const char *VS_PARTICLE = GLSL
    "layout(location = 0) in vec4 a_state; uniform sampler3D u_vel; uniform vec3 u_n, u_vext;\n"
    "uniform mat4 u_vp; uniform vec2 u_res; uniform float u_size, u_streak, u_life;\n"
    "out float f_speed; out vec2 f_q; flat out float f_len; flat out float f_hw; out float f_fade;\n"
    "void main() {\n"
    "  f_speed = 0.0; f_q = vec2(0.0); f_len = 0.0; f_hw = 0.0; f_fade = 0.0;\n"
    "  gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "  if (a_state.w < 0.0) return;\n"
    "  vec3 p = a_state.xyz; vec3 v = textureLod(u_vel, p / u_vext, 0.0).xyz;\n"
    "  vec4 c1 = u_vp * vec4(p, 1.0), c0 = u_vp * vec4(p - v * u_streak, 1.0);\n"
    "  if (c1.w < 1e-3 || c0.w < 1e-3) return;\n"
    "  vec2 h = u_res * 0.5; vec2 s0 = c0.xy / c0.w * h, s1 = c1.xy / c1.w * h;\n"
    "  vec2 d = s1 - s0; float dl = length(d); vec2 dir = dl > 1e-3 ? d / dl : vec2(1.0, 0.0);\n"
    "  float len = min(dl, 40.0); s0 = s1 - dir * len; vec2 nrm = vec2(-dir.y, dir.x);\n"
    "  float hw_true = u_size * 0.5, hw_draw = max(hw_true, 0.85), hw = hw_draw + 0.75;\n"
    "  float ax = (gl_VertexID == 2 || gl_VertexID == 3) ? 1.0 : 0.0;\n"
    "  float sy = (gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0;\n"
    "  vec2 pos = mix(s0 - dir * hw, s1 + dir * hw, ax) + nrm * hw * sy;\n"
    "  f_q = vec2(mix(-hw, len + hw, ax), hw * sy);\n"
    "  f_len = len; f_hw = hw_draw; f_speed = length(v);\n"
    /* fade in after release, out before the end of life and at the domain faces, so no particle pops in or out;
     * particles thinner than a pixel are drawn at pixel width with proportionally less alpha (no shimmer) */
    "  vec3 edge = min(p - 0.5, u_n - 0.5 - p);\n"
    "  f_fade = clamp(a_state.w / 40.0, 0.0, 1.0) * clamp((u_life - a_state.w) / (0.15 * u_life), 0.0, 1.0)\n"
    "         * clamp(min(edge.x, min(edge.y, edge.z)) / 4.0, 0.0, 1.0) * (hw_true / hw_draw);\n"
    "  gl_Position = vec4(pos / h, c1.z / c1.w, 1.0); }\n";

static const char *FS_PARTICLE = GLSL GLSL_CMAP
    "in float f_speed; in vec2 f_q; flat in float f_len; flat in float f_hw; in float f_fade; out vec4 o;\n"
    "uniform float u_speed_hi, u_gain;\n"
    "void main() {\n"
    "  float dx = max(max(-f_q.x, f_q.x - f_len), 0.0); float dist = length(vec2(dx, f_q.y));\n"
    "  float a = clamp(f_hw + 0.5 - dist, 0.0, 1.0) * f_fade;\n"
    "  if (a <= 0.0) discard;\n"
    "  vec3 c = cmap(f_speed / max(u_speed_hi, 1e-6));\n"
    "  o = vec4(c * u_gain * a, a); }\n";

static const char *VS_ISO = GLSL
    "layout(location = 0) in vec3 a_pos; layout(location = 1) in vec3 a_nrm; layout(location = 2) in float a_s;\n"
    "uniform mat4 u_vp; uniform vec3 u_offset; out vec3 v_world; out vec3 v_nrm; out float v_s;\n"
    "void main() { vec3 p = a_pos + u_offset; v_world = p; v_nrm = a_nrm; v_s = a_s; gl_Position = u_vp * vec4(p, 1.0); }\n";

static const char *FS_ISO = GLSL GLSL_CMAP
    "in vec3 v_world; in vec3 v_nrm; in float v_s; out vec4 o; uniform vec3 u_eye; uniform vec2 u_range;\n"
    /* u_alpha < 1 draws the piece as glass: the caller sorts the transparent pieces back to front and turns off depth
     * writes, so what lies behind them stays visible */
    "uniform float u_alpha;\n"
    /* u_flat selects matte shading whose brightness stays below 1, so a field surface keeps the colour the legend
     * promises instead of blooming towards white; the vortex isosurfaces keep the glossy look at u_flat = 0. */
    "uniform float u_flat;\n"
    "void main() {\n"
    "  vec3 N = normalize(v_nrm); if (!gl_FrontFacing) N = -N; vec3 V = normalize(u_eye - v_world);\n"
    "  vec3 base = cmap((v_s - u_range.x) / max(u_range.y - u_range.x, 1e-30));\n"
    /* below -2e30 the scalar is not a field value but a mark: plate/geometry grey, hovered, or chosen */
    "  if (u_flat > 0.5 && v_s < -2e30) base = v_s > -2.85e30 ? vec3(1.00, 0.72, 0.24)\n"
    "                                        : (v_s > -2.95e30 ? vec3(0.22, 0.88, 1.00) : vec3(0.24));\n"
    "  vec3 L = normalize(vec3(0.35, 0.9, 0.25)); float d = max(dot(N, L), 0.0), hemi = 0.5 + 0.5 * N.y;\n"
    /* Neutral fill from the other side makes low-field faces readable while keeping every hue honest. The
     * channel-common factor lies in [0.52, 0.98]; lighting is artificial, not a solved radiation field. The uniform
     * branch also avoids the two glossy power evaluations for every result pixel. */
    "  if (u_flat > 0.5) {\n"
    "    float fill = max(dot(N, normalize(vec3(-0.6, 0.25, -0.75))), 0.0);\n"
    "    vec3 matte = base * (0.52 + 0.08 * hemi + 0.26 * d + 0.12 * fill);\n"
    "    o = vec4(matte, u_alpha); return; }\n"
    "  float spec = pow(max(dot(N, normalize(L + V)), 0.0), 48.0); float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0);\n"
    "  vec3 c = base * (0.25 + 0.35 * hemi + 0.6 * d) + vec3(spec * 0.4) + base * fres * 0.9;\n"
    "  o = vec4(c, u_alpha); }\n";

static const char *FS_VOLUME = GLSL GLSL_CMAP
    "in vec3 v_world; out vec4 o; uniform vec3 u_eye, u_n; uniform vec2 u_range; uniform float u_density;\n"
    "uniform sampler3D u_field; uniform sampler2D u_depth; uniform mat4 u_inv_vp; uniform vec2 u_res; uniform int u_tf, u_has_depth;\n"
    "void main() {\n"
    "  vec3 rd = normalize(v_world - u_eye); vec3 inv = 1.0 / rd;\n"
    "  vec3 t0 = (vec3(0.0) - u_eye) * inv, t1 = (u_n - u_eye) * inv;\n"
    "  vec3 tmin = min(t0, t1), tmax = max(t0, t1);\n"
    "  float tn = max(max(tmin.x, tmin.y), max(tmin.z, 0.0)), tf = min(min(tmax.x, tmax.y), tmax.z);\n"
    /* opaque geometry already in the depth buffer ends the ray */
    "  float dz = u_has_depth == 1 ? texture(u_depth, gl_FragCoord.xy / u_res).r : 1.0;\n"
    "  if (dz < 1.0) { vec4 wp = u_inv_vp * vec4(gl_FragCoord.xy / u_res * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);\n"
    "    tf = min(tf, dot(wp.xyz / wp.w - u_eye, rd)); }\n"
    "  if (tf <= tn) discard;\n"
    "  int n = int(clamp((tf - tn) / 1.25, 8.0, 192.0)); float dt = (tf - tn) / float(n);\n"
    "  float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "  vec3 acc = vec3(0.0); float tr = 1.0;\n"
    "  for (int i = 0; i < 192; i++) {\n"
    "    if (i >= n || tr < 0.02) break;\n"
    "    vec3 p = u_eye + rd * (tn + (float(i) + jit) * dt);\n"
    "    float t = clamp((texture(u_field, p / u_n).r - u_range.x) / max(u_range.y - u_range.x, 1e-30), 0.0, 1.0);\n"
    /* opacity follows what is informative: slow fluid (wakes, stagnation) for speed and streamwise velocity,
     * departures from the middle for signed fields, high values for vorticity and Q */
    "    float wgt = u_tf == 1 ? smoothstep(0.35, 0.95, 1.0 - t) : u_tf == 2 ? smoothstep(0.15, 0.9, abs(2.0 * t - 1.0))\n"
    "              : smoothstep(0.25, 1.0, t);\n"
    "    float a = clamp(u_density * wgt * dt * 0.06, 0.0, 1.0);\n"
    "    acc += tr * a * cmap(t) * 1.6; tr *= 1.0 - a; }\n"
    "  o = vec4(acc, 1.0 - tr); }\n";

/* bright-pass on a 4-tap average weighted by 1/(1 + brightness) (Karis): a single very bright pixel - a thin particle
 * or line crossing pixel centres - can no longer switch the glow around it on and off from one frame to the next */
/* Screen-space ambient occlusion: a short-radius hemisphere around every pixel, sampled on the depth buffer. The
 * position and the normal are reconstructed from depth (the normal from the screen-space derivatives of the
 * reconstructed position), so nothing else has to be rendered. The result is blurred and multiplied into the scene
 * when it is composited. */
static const char *FS_SSAO = GLSL
    "out vec4 o; uniform sampler2D u_depth; uniform mat4 u_inv_vp, u_vp; uniform vec2 u_res; uniform vec3 u_eye;\n"
    "uniform float u_radius; uniform int u_samples;\n"
    "vec3 world_at(vec2 uv) {\n"
    "  float d = texture(u_depth, uv).r; vec4 c = u_inv_vp * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "  return c.xyz / c.w; }\n"
    "float rand(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy / u_res;\n"
    "  float d = texture(u_depth, uv).r;\n"
    "  if (d >= 1.0) { o = vec4(1.0); return; }\n"          /* background: no occlusion */
    "  vec3 P = world_at(uv);\n"
    "  vec3 N = normalize(cross(dFdx(P), dFdy(P)));\n"
    "  if (dot(N, normalize(u_eye - P)) < 0.0) N = -N;\n"
    "  vec3 T = normalize(abs(N.y) < 0.9 ? cross(vec3(0.0, 1.0, 0.0), N) : cross(vec3(1.0, 0.0, 0.0), N));\n"
    "  vec3 B = cross(N, T);\n"
    "  float occ = 0.0; float seed = rand(gl_FragCoord.xy);\n"
    "  for (int i = 0; i < u_samples; i++) {\n"
    "    float a = 6.2831853 * fract(seed + float(i) * 0.6180339887);\n"
    "    float r = u_radius * sqrt(fract(seed * 7.13 + float(i) * 0.2442)); \n"
    "    float z = 0.25 + 0.75 * fract(seed * 3.77 + float(i) * 0.3765);\n"
    "    vec3 dir = normalize(T * cos(a) * r + B * sin(a) * r + N * (u_radius * z));\n"
    "    vec3 S = P + dir * u_radius;\n"
    "    vec4 clip = u_vp * vec4(S, 1.0);\n"
    "    vec3 ndc = clip.xyz / clip.w; vec2 suv = ndc.xy * 0.5 + 0.5;\n"
    "    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;\n"
    "    vec3 Q = world_at(suv);\n"                          /* what the depth buffer holds in that direction */
    "    float dq = length(Q - u_eye), ds = length(S - u_eye);\n"
    "    float range = clamp(u_radius / max(abs(dq - ds), 1e-6), 0.0, 1.0);\n"
    "    if (dq < ds - 0.02 * u_radius) occ += range;\n"
    "  }\n"
    "  o = vec4(clamp(1.0 - occ / float(u_samples), 0.0, 1.0)); }\n";

static const char *FS_AOBLUR = GLSL
    "out vec4 o; uniform sampler2D u_src; uniform vec2 u_texel;\n"
    "void main() {\n"
    "  float s = 0.0;\n"
    "  for (int j = -2; j <= 2; j++)\n"
    "    for (int i = -2; i <= 2; i++) s += texture(u_src, (gl_FragCoord.xy + vec2(i, j)) * u_texel).r;\n"
    "  o = vec4(s / 25.0); }\n";

static const char *FS_PREFILTER = GLSL
    "in vec2 v_uv; out vec4 o; uniform sampler2D u_src; uniform float u_threshold; uniform vec2 u_texel;\n"
    "vec3 tap(vec2 d, inout float ws) { vec3 c = texture(u_src, v_uv + d).rgb; float w = 1.0 / (1.0 + max(c.r, max(c.g, c.b)));\n"
    "  ws += w; return c * w; }\n"
    "void main() { vec2 d = u_texel * 0.5; float ws = 0.0;\n"
    "  vec3 c = tap(vec2(-d.x, -d.y), ws) + tap(vec2(d.x, -d.y), ws) + tap(vec2(-d.x, d.y), ws) + tap(vec2(d.x, d.y), ws);\n"
    "  c /= max(ws, 1e-6); float br = max(c.r, max(c.g, c.b));\n"
    "  float soft = clamp(br - u_threshold + 0.5, 0.0, 1.0); soft = soft * soft * 0.5;\n"
    "  float k = max(soft, br - u_threshold) / max(br, 1e-4); o = vec4(c * k, 1.0); }\n";

static const char *FS_DOWN = GLSL
    "in vec2 v_uv; out vec4 o; uniform sampler2D u_src; uniform vec2 u_texel;\n"
    "void main() { vec2 d = u_texel;\n"
    "  vec3 c = texture(u_src, v_uv).rgb * 4.0 + texture(u_src, v_uv + vec2(-d.x, -d.y)).rgb + texture(u_src, v_uv + vec2(d.x, -d.y)).rgb\n"
    "         + texture(u_src, v_uv + vec2(-d.x, d.y)).rgb + texture(u_src, v_uv + vec2(d.x, d.y)).rgb;\n"
    "  o = vec4(c / 8.0, 1.0); }\n";

static const char *FS_UP = GLSL
    "in vec2 v_uv; out vec4 o; uniform sampler2D u_src; uniform vec2 u_texel;\n"
    "void main() { vec2 d = u_texel;\n"
    "  vec3 c = texture(u_src, v_uv + vec2(-d.x, 0.0)).rgb * 2.0 + texture(u_src, v_uv + vec2(d.x, 0.0)).rgb * 2.0\n"
    "         + texture(u_src, v_uv + vec2(0.0, -d.y)).rgb * 2.0 + texture(u_src, v_uv + vec2(0.0, d.y)).rgb * 2.0\n"
    "         + texture(u_src, v_uv + vec2(-d.x, -d.y)).rgb + texture(u_src, v_uv + vec2(d.x, -d.y)).rgb\n"
    "         + texture(u_src, v_uv + vec2(-d.x, d.y)).rgb + texture(u_src, v_uv + vec2(d.x, d.y)).rgb\n"
    "         + texture(u_src, v_uv).rgb * 4.0;\n"
    "  o = vec4(c / 16.0, 1.0); }\n";

static const char *FS_COMPOSITE = GLSL
    "in vec2 v_uv; out vec4 o; uniform sampler2D u_scene, u_bloom, u_ao; uniform float u_bloom_k, u_exposure, u_ao_k;\n"
    "uniform int u_has_bloom, u_has_ao;\n"
    "vec3 softclip(vec3 x) { const float k = 0.72; vec3 over = max(x - k, 0.0);\n"
    "  return min(x, vec3(k)) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k))); }\n"
    "void main() {\n"
    "  vec3 c = texture(u_scene, v_uv).rgb;\n"
    /* ambient occlusion darkens the creases before the bloom is added, so a dark crease does not glow */
    "  if (u_has_ao == 1) c *= mix(1.0, texture(u_ao, v_uv).r, u_ao_k);\n"
    "  if (u_has_bloom == 1) c += texture(u_bloom, v_uv).rgb * u_bloom_k;\n"
    "  c = softclip(c * u_exposure);\n"
    "  vec2 q = v_uv - 0.5; c *= 1.0 - dot(q, q) * 0.45;\n"
    "  c = pow(max(c, 0.0), vec3(1.0 / 2.2));\n"
    "  c += (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;\n"
    "  o = vec4(c, 1.0); }\n";

/* ---- renderer ----------------------------------------------------------------------------------- */

struct Renderer {
    int fb_w, fb_h, msaa;
    GLTarget scene, resolved;
    GLuint depth_fbo, depth_tex; /* single-sample copy of the scene depth: volume rays stop at opaque geometry */
    GLuint p_ssao, p_aoblur;
    GLTarget ao, ao_blur;        /* half-resolution ambient occlusion and its blur */
    GLTarget bloom[MAX_BLOOM];
    int bloom_levels;

    GLuint vao_empty;
    GLuint p_bg, p_floor, p_color, p_mesh, p_slice, p_stream, p_pupdate, p_particle, p_iso, p_volume;
    GLuint p_prefilter, p_down, p_up, p_composite;

    GLuint tex_cmap, tex_field, tex_vel, tex_solid;
    GLuint tex_line_cmap, tex_result_cmap;
    int result_cmap_id;   /* -1 follows the flow's colour map */
    int cmap_id, line_cmap_id;
    float field_scale; /* the field texture holds value / field_scale */
    int gnx, gny, gnz;
    int vnx, vny, vnz; /* half-resolution velocity volume */
    bool have_field, have_vel, have_solid;

    GLuint vao_pos, vbo_pos;
    GLuint vao_mesh, vbo_mesh;
    GLsizei mesh_verts;
    vec3 mesh_bmin, mesh_bmax;

    GLuint vao_stream, vbo_stream, ibo_stream;
    GLsizei stream_indices;
    uint32_t stream_lines;
    float *ribbon_v;
    uint32_t *ribbon_i;
    size_t ribbon_vcap, ribbon_icap;

    GLuint vao_iso, vbo_iso, ibo_iso;
    GLsizei iso_indices;
    GLuint vao_res, vbo_res, ibo_res;
    GLsizei res_indices;

    GLuint vao_part[2], vbo_part[2], vao_pdraw[2];
    int part_src, part_count;
    bool part_reset;
    uint32_t frame;

    uint16_t *half_buf;
    size_t half_cap;
    double flow_steps;
};

void render_settings_default(RenderSettings *rs) {
    memset(rs, 0, sizeof *rs);
    rs->field = VIS_SPEED;
    rs->cmap = CMAP_TURBO;
    rs->line_cmap = -1;
    rs->auto_range = true;
    rs->surface_mode = SURFACE_FIELD;
    rs->surface_grid = true;
    rs->slice_on = false;
    rs->slice_axis = 1;
    rs->slice_pos = 0.5f;
    rs->slice_opacity = 0.9f;
    rs->stream_on = true;
    rs->stream_width = 1.5f;
    rs->stream_animate = true;
    rs->particles_on = true;
    rs->particle_count = 60000;
    rs->particle_size = 1.2f;
    rs->vortex_on = false;
    rs->vortex_level = 50.0f;
    rs->volume_on = false;
    rs->ao_on = true;      /* the result views are lit better with it; Simple mode's playback turns it off */
    rs->ao_strength = 0.75f;
    rs->volume_density = 1.0f;
    rs->floor_on = true;
    rs->box_on = true;
    rs->bloom = 0.55f;
    rs->exposure = 1.0f;
    rs->msaa = 4;
}

static void u1i(GLuint p, const char *n, int v) { glUniform1i(glGetUniformLocation(p, n), v); }
static void u1f(GLuint p, const char *n, float v) { glUniform1f(glGetUniformLocation(p, n), v); }
static void u2f(GLuint p, const char *n, float a, float b) { glUniform2f(glGetUniformLocation(p, n), a, b); }
static void u3f(GLuint p, const char *n, float a, float b, float c) { glUniform3f(glGetUniformLocation(p, n), a, b, c); }
static void u4f(GLuint p, const char *n, float a, float b, float c, float d) {
    glUniform4f(glGetUniformLocation(p, n), a, b, c, d);
}
static void um4(GLuint p, const char *n, const mat4 *m) { glUniformMatrix4fv(glGetUniformLocation(p, n), 1, GL_FALSE, m->m); }

static void bind_tex(GLuint p, const char *name, int unit, GLenum target, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(target, tex);
    u1i(p, name, unit);
}

static GLuint make_tex3d(GLenum internal, GLenum format, GLenum type, int nx, int ny, int nz) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_3D, t);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, (GLint)internal, nx, ny, nz, 0, format, type, NULL);
    return t;
}

Renderer *render_create(void) {
    Renderer *r = calloc(1, sizeof *r);
    r->msaa = -1;
    glGenVertexArrays(1, &r->vao_empty);

    r->p_bg = gl_program("background", VS_FULLSCREEN, NULL, FS_BACKGROUND);
    r->p_floor = gl_program("floor", VS_POS, NULL, FS_FLOOR);
    r->p_color = gl_program("color", VS_POS, NULL, FS_COLOR);
    r->p_mesh = gl_program("mesh", VS_MESH, NULL, FS_MESH);
    r->p_slice = gl_program("slice", VS_POS, NULL, FS_SLICE);
    r->p_stream = gl_program("streamlines", VS_STREAM, NULL, FS_STREAM);
    const char *varyings[] = {"tf_state"};
    r->p_pupdate = gl_program_tf("particle-update", VS_PARTICLE_UPDATE, varyings, 1);
    r->p_particle = gl_program("particles", VS_PARTICLE, NULL, FS_PARTICLE);
    r->p_iso = gl_program("isosurface", VS_ISO, NULL, FS_ISO);
    r->p_ssao = gl_program("ssao", VS_FULLSCREEN, NULL, FS_SSAO);
    r->p_aoblur = gl_program("ao blur", VS_FULLSCREEN, NULL, FS_AOBLUR);
    r->p_volume = gl_program("volume", VS_POS, NULL, FS_VOLUME);
    r->p_prefilter = gl_program("bloom-prefilter", VS_FULLSCREEN, NULL, FS_PREFILTER);
    r->p_down = gl_program("bloom-down", VS_FULLSCREEN, NULL, FS_DOWN);
    r->p_up = gl_program("bloom-up", VS_FULLSCREEN, NULL, FS_UP);
    r->p_composite = gl_program("composite", VS_FULLSCREEN, NULL, FS_COMPOSITE);

    glGenTextures(1, &r->tex_cmap);
    glGenTextures(1, &r->tex_result_cmap);
    glGenTextures(1, &r->tex_line_cmap);
    r->line_cmap_id = -1;
    r->result_cmap_id = -1;
    render_set_colormap(r, CMAP_TURBO);
    /* complete placeholder volumes so samplers never see an unloadable texture before the first upload */
    r->tex_field = make_tex3d(GL_R16F, GL_RED, GL_HALF_FLOAT, 1, 1, 1);
    r->field_scale = 1.0f;
    r->vnx = r->vny = r->vnz = 1;
    r->tex_vel = make_tex3d(GL_RGB16F, GL_RGB, GL_HALF_FLOAT, 1, 1, 1);
    r->tex_solid = make_tex3d(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1, 1);
    r->gnx = r->gny = r->gnz = 1;

    glGenVertexArrays(1, &r->vao_pos);
    glGenBuffers(1, &r->vbo_pos);
    glBindVertexArray(r->vao_pos);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_pos);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    glGenVertexArrays(1, &r->vao_mesh);
    glGenBuffers(1, &r->vbo_mesh);
    glBindVertexArray(r->vao_mesh);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_mesh);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glGenVertexArrays(1, &r->vao_stream);
    glGenBuffers(1, &r->vbo_stream);
    glGenBuffers(1, &r->ibo_stream);
    glBindVertexArray(r->vao_stream);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_stream);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo_stream);
    {
        const GLsizei st = 9 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void *)(3 * sizeof(float)));
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, st, (void *)(6 * sizeof(float)));
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st, (void *)(7 * sizeof(float)));
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, st, (void *)(8 * sizeof(float)));
        for (GLuint a = 0; a < 5; a++) glEnableVertexAttribArray(a);
    }

    glGenVertexArrays(1, &r->vao_iso);
    glGenBuffers(1, &r->vbo_iso);
    glGenBuffers(1, &r->ibo_iso);
    glBindVertexArray(r->vao_iso);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_iso);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo_iso);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(3 * sizeof(float)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(6 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glGenVertexArrays(1, &r->vao_res);
    glGenBuffers(1, &r->vbo_res);
    glGenBuffers(1, &r->ibo_res);
    glBindVertexArray(r->vao_res);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_res);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo_res);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(3 * sizeof(float)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(6 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glGenVertexArrays(2, r->vao_part);
    glGenBuffers(2, r->vbo_part);
    for (int i = 0; i < 2; i++) {
        glBindVertexArray(r->vao_part[i]);
        glBindBuffer(GL_ARRAY_BUFFER, r->vbo_part[i]);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(0);
    }
    /* drawing uses the same buffers as per-instance data */
    glGenVertexArrays(2, r->vao_pdraw);
    for (int i = 0; i < 2; i++) {
        glBindVertexArray(r->vao_pdraw[i]);
        glBindBuffer(GL_ARRAY_BUFFER, r->vbo_part[i]);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(0);
        glVertexAttribDivisor(0, 1);
    }
    glBindVertexArray(0);
    gl_check("render_create");
    return r;
}

static void destroy_targets(Renderer *r) {
    gl_target_destroy(&r->scene);
    gl_target_destroy(&r->resolved);
    gl_target_destroy(&r->ao);
    gl_target_destroy(&r->ao_blur);
    for (int i = 0; i < MAX_BLOOM; i++) gl_target_destroy(&r->bloom[i]);
    r->bloom_levels = 0;
}

void render_destroy(Renderer *r) {
    if (!r) return;
    destroy_targets(r);
    GLuint progs[] = {r->p_bg, r->p_floor, r->p_color, r->p_mesh, r->p_slice, r->p_stream, r->p_pupdate,
                      r->p_particle, r->p_iso, r->p_volume, r->p_prefilter, r->p_down, r->p_up, r->p_composite,
                      r->p_ssao,     r->p_aoblur};
    for (size_t i = 0; i < ARRAY_LEN(progs); i++)
        if (progs[i]) glDeleteProgram(progs[i]);
    GLuint texs[] = {r->tex_cmap, r->tex_result_cmap, r->tex_line_cmap, r->tex_field, r->tex_vel, r->tex_solid};
    for (size_t i = 0; i < ARRAY_LEN(texs); i++)
        if (texs[i]) glDeleteTextures(1, &texs[i]);
    GLuint bufs[] = {r->vbo_pos, r->vbo_mesh, r->vbo_stream, r->ibo_stream, r->vbo_iso, r->ibo_iso, r->vbo_res, r->ibo_res, r->vbo_part[0],
                     r->vbo_part[1]};
    glDeleteBuffers((GLsizei)ARRAY_LEN(bufs), bufs);
    GLuint vaos[] = {r->vao_empty, r->vao_pos,     r->vao_mesh,     r->vao_stream,   r->vao_iso,     r->vao_res,
                     r->vao_part[0], r->vao_part[1], r->vao_pdraw[0], r->vao_pdraw[1]};
    glDeleteVertexArrays((GLsizei)ARRAY_LEN(vaos), vaos);
    free(r->ribbon_v);
    free(r->ribbon_i);
    free(r->half_buf);
    free(r);
}

static void ensure_targets(Renderer *r, int w, int h, int msaa) {
    msaa = msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1;
    if (w == r->fb_w && h == r->fb_h && msaa == r->msaa && r->scene.fbo) return;
    destroy_targets(r);
    if (r->depth_fbo) glDeleteFramebuffers(1, &r->depth_fbo), r->depth_fbo = 0;
    if (r->depth_tex) glDeleteTextures(1, &r->depth_tex), r->depth_tex = 0;
    r->fb_w = w, r->fb_h = h, r->msaa = msaa;
    if (!gl_target_create(&r->scene, w, h, GL_RGBA16F, true, msaa)) {
        LOGW("MSAA x%d unavailable, falling back to no MSAA", msaa);
        r->msaa = 1;
        gl_target_create(&r->scene, w, h, GL_RGBA16F, true, 1);
    }
    gl_target_create(&r->resolved, w, h, GL_RGBA16F, false, 1);
    glGenTextures(1, &r->depth_tex);
    glBindTexture(GL_TEXTURE_2D, r->depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &r->depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->depth_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGW("depth copy target incomplete - volume rendering will not be occluded by the model");
        glDeleteFramebuffers(1, &r->depth_fbo);
        r->depth_fbo = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_target_create(&r->ao, (w + 1) / 2, (h + 1) / 2, GL_R8, false, 1);
    gl_target_create(&r->ao_blur, (w + 1) / 2, (h + 1) / 2, GL_R8, false, 1);
    int bw = w / 2, bh = h / 2;
    for (int i = 0; i < MAX_BLOOM && bw >= 16 && bh >= 16; i++) {
        if (!gl_target_create(&r->bloom[i], bw, bh, GL_RGBA16F, false, 1)) break;
        r->bloom_levels = i + 1;
        bw /= 2, bh /= 2;
    }
}

static void upload_cmap(GLuint tex, int cmap);
static void upload_result_cmap(GLuint tex, int cmap);

void render_set_colormap(Renderer *r, int cmap) {
    r->cmap_id = cmap;
    upload_cmap(r->tex_cmap, cmap);
    if (r->result_cmap_id < 0) upload_result_cmap(r->tex_result_cmap, cmap);
    if (r->line_cmap_id < 0) upload_cmap(r->tex_line_cmap, cmap);
}

/* Results keep a colour map of their own, viridis unless the user picks another: the tunnel's scenes choose theirs
 * for the flow, and a stress plot must stay readable for a colour-blind engineer whatever the tunnel last showed. */
void render_set_result_colormap(Renderer *r, int cmap) {
    r->result_cmap_id = cmap;
    upload_result_cmap(r->tex_result_cmap, cmap < 0 ? r->cmap_id : cmap);
}

int render_result_colormap(const Renderer *r) { return r->result_cmap_id >= 0 ? r->result_cmap_id : r->cmap_id; }

void render_set_line_colormap(Renderer *r, int cmap) {
    r->line_cmap_id = cmap;
    upload_cmap(r->tex_line_cmap, cmap < 0 ? r->cmap_id : cmap);
}

GLuint render_line_colormap_texture(const Renderer *r) { return r->tex_line_cmap; }

static void upload_cmap(GLuint tex, int cmap) {
    uint8_t rgba[256 * 4];
    colormap_fill_rgba(cmap, rgba, 256);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void upload_result_cmap(GLuint tex, int cmap) {
    uint8_t rgba[256 * 4];
    /* Result surface and legend share this texture: skip the nearly black bottom 12% of the map. */
    for (int i = 0; i < 256; i++) {
        float rgb[3]; colormap_sample(cmap, 0.12f + 0.88f*(float)i/255, rgb);
        for (int c = 0; c < 3; c++) rgba[4*i+c] = (uint8_t)lrintf(255*rgb[c]);
        rgba[4*i+3] = 255;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

GLuint render_result_colormap_texture(const Renderer *r) { return r->tex_result_cmap; }

GLuint render_colormap_texture(const Renderer *r) { return r->tex_cmap; }

void render_set_mesh(Renderer *r, const Mesh *m) {
    r->mesh_verts = 0;
    if (!m || !m->tri_count) return;
    size_t nv = 3 * (size_t)m->tri_count;
    float *buf = malloc(nv * 6 * sizeof(float));
    if (!buf) return;
    for (size_t t = 0; t < m->tri_count; t++) {
        vec3 a = m->pos[3 * t], b = m->pos[3 * t + 1], c = m->pos[3 * t + 2];
        vec3 fn = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
        for (int k = 0; k < 3; k++) {
            size_t i = 3 * t + (size_t)k;
            vec3 n = m->nrm ? m->nrm[i] : fn;
            float *o = buf + 6 * i;
            o[0] = m->pos[i].x, o[1] = m->pos[i].y, o[2] = m->pos[i].z;
            o[3] = n.x, o[4] = n.y, o[5] = n.z;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_mesh);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * 6 * sizeof(float)), buf, GL_STATIC_DRAW);
    free(buf);
    r->mesh_verts = (GLsizei)nv;
    r->mesh_bmin = m->bmin, r->mesh_bmax = m->bmax;
}

void render_set_grid(Renderer *r, int nx, int ny, int nz) {
    if (nx == r->gnx && ny == r->gny && nz == r->gnz && r->tex_field) return;
    if (r->tex_field) glDeleteTextures(1, &r->tex_field);
    if (r->tex_vel) glDeleteTextures(1, &r->tex_vel);
    if (r->tex_solid) glDeleteTextures(1, &r->tex_solid);
    r->gnx = nx, r->gny = ny, r->gnz = nz;
    r->tex_field = make_tex3d(GL_R16F, GL_RED, GL_HALF_FLOAT, nx, ny, nz);
    r->vnx = (nx + 1) / 2, r->vny = (ny + 1) / 2, r->vnz = (nz + 1) / 2;
    r->tex_vel = make_tex3d(GL_RGB16F, GL_RGB, GL_HALF_FLOAT, r->vnx, r->vny, r->vnz);
    r->tex_solid = make_tex3d(GL_R8, GL_RED, GL_UNSIGNED_BYTE, nx, ny, nz);
    r->have_field = r->have_vel = r->have_solid = false;
    r->part_reset = true;
    gl_check("render_set_grid");
}

void render_upload_field(Renderer *r, const float *field) {
    if (!r->tex_field) return;
    glBindTexture(GL_TEXTURE_3D, r->tex_field);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, r->gnx, r->gny, r->gnz, GL_RED, GL_FLOAT, field);
    r->field_scale = 1.0f;
    r->have_field = true;
}

void render_upload_field_half(Renderer *r, const uint16_t *half, float scale) {
    if (!r->tex_field || !half) return;
    glBindTexture(GL_TEXTURE_3D, r->tex_field);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, r->gnx, r->gny, r->gnz, GL_RED, GL_HALF_FLOAT, half);
    r->field_scale = scale > 0 ? scale : 1.0f;
    r->have_field = true;
}

typedef struct {
    const float *ux, *uy, *uz;
    uint16_t *out;
} HalfCtx;

static void half_job(void *vc, int b, int e, int tid);

static inline uint16_t half_from_float(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xFF) - 112;
    uint32_t m = x & 0x007FFFFFu;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

typedef struct {
    HalfCtx h;
    size_t n;
} HalfJob;

static void half_job(void *vc, int b, int e, int tid) {
    HalfJob *J = vc;
    (void)tid;
    const size_t chunk = 65536;
    size_t i0 = (size_t)b * chunk, i1 = MINI((size_t)e * chunk, J->n);
#if defined(__aarch64__) || defined(__arm64__)
    size_t i = i0;
    float tmp[12];
    for (; i + 4 <= i1; i += 4) {
        for (int k = 0; k < 4; k++) {
            tmp[3 * k] = J->h.ux[i + (size_t)k];
            tmp[3 * k + 1] = J->h.uy[i + (size_t)k];
            tmp[3 * k + 2] = J->h.uz[i + (size_t)k];
        }
        uint16_t *o = J->h.out + 3 * i;
        vst1_u16(o, vreinterpret_u16_f16(vcvt_f16_f32(vld1q_f32(tmp))));
        vst1_u16(o + 4, vreinterpret_u16_f16(vcvt_f16_f32(vld1q_f32(tmp + 4))));
        vst1_u16(o + 8, vreinterpret_u16_f16(vcvt_f16_f32(vld1q_f32(tmp + 8))));
    }
    for (; i < i1; i++) {
        uint16_t *o = J->h.out + 3 * i;
        o[0] = half_from_float(J->h.ux[i]), o[1] = half_from_float(J->h.uy[i]), o[2] = half_from_float(J->h.uz[i]);
    }
#else
    for (size_t i = i0; i < i1; i++) {
        uint16_t *o = J->h.out + 3 * i;
        o[0] = half_from_float(J->h.ux[i]), o[1] = half_from_float(J->h.uy[i]), o[2] = half_from_float(J->h.uz[i]);
    }
#endif
}

void render_pack_velocity(const float *ux, const float *uy, const float *uz, size_t n, uint16_t *out, ThreadPool *pool) {
    HalfJob J = {{ux, uy, uz, out}, n};
    int chunks = (int)((n + 65535) / 65536);
    if (pool)
        pool_for(pool, chunks, 1, half_job, &J);
    else
        half_job(&J, 0, chunks, 0);
}

void render_upload_velocity_half(Renderer *r, const uint16_t *half) {
    if (!r->tex_vel || !half) return;
    glBindTexture(GL_TEXTURE_3D, r->tex_vel);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, r->vnx, r->vny, r->vnz, GL_RGB, GL_HALF_FLOAT, half);
    r->have_vel = true;
}


void render_upload_solid(Renderer *r, const uint8_t *solid) {
    if (!r->tex_solid) return;
    size_t n = (size_t)r->gnx * (size_t)r->gny * (size_t)r->gnz;
    uint8_t *tmp = malloc(n);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) tmp[i] = solid[i] ? 255 : 0;
    glBindTexture(GL_TEXTURE_3D, r->tex_solid);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, r->gnx, r->gny, r->gnz, GL_RED, GL_UNSIGNED_BYTE, tmp);
    free(tmp);
    r->have_solid = true;
}

void render_set_streamlines(Renderer *r, const StreamlineSet *s) {
    r->stream_indices = 0;
    r->stream_lines = s ? s->nlines : 0;
    if (!s || !s->nlines || !s->nverts) return;
    size_t nv = 2 * (size_t)s->nverts, ni = 0;
    for (uint32_t l = 0; l < s->nlines; l++)
        if (s->line_count[l] > 1) ni += 6 * ((size_t)s->line_count[l] - 1);
    if (r->ribbon_vcap < nv) {
        free(r->ribbon_v);
        r->ribbon_v = malloc(nv * 9 * sizeof(float));
        r->ribbon_vcap = r->ribbon_v ? nv : 0;
    }
    if (r->ribbon_icap < ni) {
        free(r->ribbon_i);
        r->ribbon_i = malloc(ni * sizeof(uint32_t));
        r->ribbon_icap = r->ribbon_i ? ni : 0;
    }
    if (!r->ribbon_v || !r->ribbon_i) return;
    float *V = r->ribbon_v;
    uint32_t *I = r->ribbon_i;
    size_t k = 0;
    for (uint32_t l = 0; l < s->nlines; l++) {
        const uint32_t f = s->line_first[l], c = s->line_count[l];
        for (uint32_t j = 0; j < c; j++) {
            const float *p = s->verts + 5 * (size_t)(f + j);
            const float *pa = s->verts + 5 * (size_t)(f + (j > 0 ? j - 1 : 0));
            const float *pb = s->verts + 5 * (size_t)(f + (j + 1 < c ? j + 1 : c - 1));
            float tx = pb[0] - pa[0], ty = pb[1] - pa[1], tz = pb[2] - pa[2];
            const float tl = sqrtf(tx * tx + ty * ty + tz * tz);
            if (tl > 1e-9f) tx /= tl, ty /= tl, tz /= tl;
            else tx = 1, ty = tz = 0;
            for (int side = 0; side < 2; side++) {
                float *o = V + 9 * (2 * (size_t)(f + j) + (size_t)side);
                o[0] = p[0], o[1] = p[1], o[2] = p[2];
                o[3] = tx, o[4] = ty, o[5] = tz;
                o[6] = p[3], o[7] = p[4], o[8] = side ? 1.0f : -1.0f;
            }
        }
        for (uint32_t j = 0; j + 1 < c; j++) {
            const uint32_t a = 2 * (f + j), b = a + 2;
            I[k++] = a, I[k++] = a + 1, I[k++] = b;
            I[k++] = a + 1, I[k++] = b + 1, I[k++] = b;
        }
    }
    glBindVertexArray(r->vao_stream);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_stream);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * 9 * sizeof(float)), V, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo_stream);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(k * sizeof(uint32_t)), I, GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
    r->stream_indices = (GLsizei)k;
}

void render_set_isosurface(Renderer *r, const IsoMesh *m) {
    r->iso_indices = 0;
    if (!m || !m->nindices) return;
    glBindVertexArray(r->vao_iso);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_iso);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->nverts * 7 * sizeof(float)), m->verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo_iso);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m->nindices * sizeof(uint32_t)), m->indices, GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
    r->iso_indices = (GLsizei)m->nindices;
}

void render_set_result(Renderer *r, const IsoMesh *m) {
    r->res_indices = 0;
    if (!m || !m->nindices) return;
    glBindVertexArray(r->vao_res);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_res);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->nverts * 7 * sizeof(float)), m->verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo_res);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m->nindices * sizeof(uint32_t)), m->indices, GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
    r->res_indices = (GLsizei)m->nindices;
}

void render_reset_particles(Renderer *r) { r->part_reset = true; }

static void draw_positions(Renderer *r, const float *xyz, int nverts, GLenum mode) {
    glBindVertexArray(r->vao_pos);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * 3 * sizeof(float)), xyz, GL_STREAM_DRAW);
    glDrawArrays(mode, 0, nverts);
}

static void draw_box_quad(Renderer *r, vec3 a, vec3 b, vec3 c, vec3 d) {
    float v[18] = {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z};
    draw_positions(r, v, 6, GL_TRIANGLES);
}

static void ensure_particles(Renderer *r, int count) {
    count = CLAMP(count, 0, 4000000);
    if (count == r->part_count) return;
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, r->vbo_part[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * 4 * sizeof(float)), NULL, GL_DYNAMIC_DRAW);
    }
    r->part_count = count;
    r->part_src = 0;
    r->part_reset = true;
}

static void draw_scene(Renderer *r, const RenderFrame *f) {
    const RenderSettings *rs = f->rs;
    const Camera *cam = f->cam;
    const vec3 N = v3((float)f->nx, (float)f->ny, (float)f->nz);
    const float maxdim = MAXI(N.x, MAXI(N.y, N.z));
    const bool result_scene = f->solid_workspace;
    const float lo = f->field_lo, hi = f->field_hi > f->field_lo ? f->field_hi : f->field_lo + 1e-12f;
    /* texture-space range for everything that samples the (normalised) field texture */
    const float fscale = r->field_scale > 0 ? r->field_scale : 1.0f;
    const float tlo = lo / fscale, thi = hi / fscale;

    /* background */
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glUseProgram(r->p_bg);
    u2f(r->p_bg, "u_res", (float)f->target_w, (float)f->target_h);
    u1f(r->p_bg, "u_neutral", rs->backdrop_neutral || result_scene ? 1.0f : 0.0f);
    if (result_scene && !rs->backdrop_neutral) {
        /* Default result studio; an explicitly chosen backdrop keeps its own colours. */
        u3f(r->p_bg, "u_ntop", 0.015f, 0.018f, 0.022f);
        u3f(r->p_bg, "u_nbot", 0.006f, 0.007f, 0.009f);
    } else {
        u3f(r->p_bg, "u_ntop", rs->backdrop_top[0], rs->backdrop_top[1], rs->backdrop_top[2]);
        u3f(r->p_bg, "u_nbot", rs->backdrop_bottom[0], rs->backdrop_bottom[1], rs->backdrop_bottom[2]);
    }
    glBindVertexArray(r->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    /* floor */
    if (rs->floor_on && r->p_floor) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r->p_floor);
        um4(r->p_floor, "u_vp", &cam->viewproj);
        float y = -MAXI(3.0f, N.y * 0.06f), R = maxdim * 4.0f;
        vec3 c = v3(N.x * 0.5f, y, N.z * 0.5f);
        float major = powf(2.0f, roundf(log2f(maxdim / 6.0f)));
        u3f(r->p_floor, "u_center", c.x, c.y, c.z);
        u1f(r->p_floor, "u_minor", major / 4.0f);
        u1f(r->p_floor, "u_major", major);
        u1f(r->p_floor, "u_fade", maxdim * 1.3f);
        u1i(r->p_floor, "u_result", result_scene ? 1 : 0);
        draw_box_quad(r, v3(c.x - R, y, c.z - R), v3(c.x + R, y, c.z - R), v3(c.x + R, y, c.z + R), v3(c.x - R, y, c.z + R));
        glDisable(GL_BLEND);
    }

    /* model surface */
    if (f->has_model && rs->surface_mode != SURFACE_HIDDEN && r->mesh_verts && r->p_mesh) {
        glUseProgram(r->p_mesh);
        um4(r->p_mesh, "u_vp", &cam->viewproj);
        um4(r->p_mesh, "u_model", &f->model);
        float nm[9] = {f->model.m[0], f->model.m[1], f->model.m[2], f->model.m[4], f->model.m[5],
                       f->model.m[6], f->model.m[8], f->model.m[9], f->model.m[10]};
        glUniformMatrix3fv(glGetUniformLocation(r->p_mesh, "u_nmat"), 1, GL_FALSE, nm);
        u3f(r->p_mesh, "u_eye", cam->eye.x, cam->eye.y, cam->eye.z);
        u3f(r->p_mesh, "u_n", N.x, N.y, N.z);
        int mode = (rs->surface_mode == SURFACE_FIELD && r->have_field) ? 2 : 1;
        u1i(r->p_mesh, "u_mode", mode);
        u1i(r->p_mesh, "u_grid", rs->surface_grid ? 1 : 0);
        vec3 ext = v3_sub(r->mesh_bmax, r->mesh_bmin);
        float scale = v3_len(v3(f->model.m[0], f->model.m[1], f->model.m[2]));
        float world_ext = MAXI(ext.x, MAXI(ext.y, ext.z)) * scale;
        u1f(r->p_mesh, "u_spacing", MAXI(world_ext / 90.0f, 0.75f));
        u2f(r->p_mesh, "u_range", tlo, thi);
        bind_tex(r->p_mesh, "u_field", 0, GL_TEXTURE_3D, r->tex_field);
        bind_tex(r->p_mesh, "u_cmap", 1, GL_TEXTURE_2D, r->tex_cmap);
        glBindVertexArray(r->vao_mesh);
        glDrawArrays(GL_TRIANGLES, 0, r->mesh_verts);
    }

    /* Finite-element result surface, in the same world as the flow. Without pieces it is one opaque draw; with them,
     * the solid pieces go first and the glass ones after, back to front and without depth writes, so a body behind a
     * transparent one is still there. */
    if (rs->result_on && r->res_indices && r->p_iso) {
        glUseProgram(r->p_iso);
        um4(r->p_iso, "u_vp", &cam->viewproj);
        u3f(r->p_iso, "u_eye", cam->eye.x, cam->eye.y, cam->eye.z);
        u2f(r->p_iso, "u_range", f->result_lo, f->result_hi);
        u1f(r->p_iso, "u_flat", 1.0f);
        bind_tex(r->p_iso, "u_cmap", 1, GL_TEXTURE_2D, r->tex_result_cmap);
        glBindVertexArray(r->vao_res);
        if (!f->result_parts || f->result_nparts <= 0) {
            u3f(r->p_iso, "u_offset", 0, 0, 0);
            u1f(r->p_iso, "u_alpha", 1.0f);
            glDrawElements(GL_TRIANGLES, r->res_indices, GL_UNSIGNED_INT, (void *)0);
        } else {
            int order[FEM_MAX_DRAW_PARTS], nt = 0;
            for (int i = 0; i < f->result_nparts && i < FEM_MAX_DRAW_PARTS; i++) {
                const ResultPart *p = &f->result_parts[i];
                if (!p->index_count) continue;
                if (p->opacity < 0.999f) {
                    order[nt++] = i;
                    continue;
                }
                u3f(r->p_iso, "u_offset", p->offset[0], p->offset[1], p->offset[2]);
                u1f(r->p_iso, "u_alpha", 1.0f);
                glDrawElements(GL_TRIANGLES, (GLsizei)p->index_count, GL_UNSIGNED_INT,
                               (void *)(uintptr_t)(p->index_start * sizeof(uint32_t)));
            }
            for (int a = 0; a < nt; a++) /* farthest first: an insertion sort over a handful of pieces */
                for (int b = a + 1; b < nt; b++) {
                    const ResultPart *pa = &f->result_parts[order[a]], *pb = &f->result_parts[order[b]];
                    float da = 0, db = 0;
                    for (int k = 0; k < 3; k++) {
                        float ea = (&cam->eye.x)[k];
                        da += (pa->centre[k] - ea) * (pa->centre[k] - ea);
                        db += (pb->centre[k] - ea) * (pb->centre[k] - ea);
                    }
                    if (db > da) {
                        int t = order[a];
                        order[a] = order[b], order[b] = t;
                    }
                }
            if (nt) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                for (int a = 0; a < nt; a++) {
                    const ResultPart *p = &f->result_parts[order[a]];
                    u3f(r->p_iso, "u_offset", p->offset[0], p->offset[1], p->offset[2]);
                    u1f(r->p_iso, "u_alpha", p->opacity);
                    glDrawElements(GL_TRIANGLES, (GLsizei)p->index_count, GL_UNSIGNED_INT,
                                   (void *)(uintptr_t)(p->index_start * sizeof(uint32_t)));
                }
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }
        }
    }

    /* feature edges of the geometry, thin and dark, pulled slightly towards the eye so they sit on the surface */
    if (rs->result_on && f->result_edges && f->result_edge_vertices > 1 && r->p_color) {
        glUseProgram(r->p_color);
        um4(r->p_color, "u_vp", &cam->viewproj);
        u4f(r->p_color, "u_color", 0.06f, 0.07f, 0.09f, 0.85f);
        u1f(r->p_color, "u_zbias", 1e-4f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (!f->edge_parts || f->edge_nparts <= 0) {
            u3f(r->p_color, "u_offset", 0, 0, 0);
            draw_positions(r, f->result_edges, f->result_edge_vertices, GL_LINES);
        } else {
            for (int i = 0; i < f->edge_nparts; i++) {
                const ResultPart *p = &f->edge_parts[i];
                if (!p->index_count) continue;
                u3f(r->p_color, "u_offset", p->offset[0], p->offset[1], p->offset[2]);
                draw_positions(r, f->result_edges + 3 * (size_t)p->index_start, (int)p->index_count, GL_LINES);
            }
        }
        glDisable(GL_BLEND);
        u1f(r->p_color, "u_zbias", 0.0f);
        u3f(r->p_color, "u_offset", 0, 0, 0);
    }

    /* the undeformed shape, drawn with the depth test on: the part hides the parts of it that lie inside, so what
     * stays visible is where the deformed surface has moved away from where it started */
    if (rs->result_on && f->result_outline && f->result_outline_vertices && r->p_color) {
        glUseProgram(r->p_color);
        um4(r->p_color, "u_vp", &cam->viewproj);
        u4f(r->p_color, "u_color", 0.62f, 0.68f, 0.78f, 0.85f);
        u3f(r->p_color, "u_offset", 0, 0, 0);
        /* The first printed layer can coincide with its reference shape. Stabilise the crease overlay just like
         * feature edges, without changing its positions or the depth test against the solid. */
        u1f(r->p_color, "u_zbias", 1e-4f);
        glDepthMask(GL_FALSE);
        draw_positions(r, f->result_outline, f->result_outline_vertices, GL_LINES);
        glDepthMask(GL_TRUE);
        u1f(r->p_color, "u_zbias", 0.0f);
    }

    /* vortex isosurfaces */
    if (rs->vortex_on && r->iso_indices && r->p_iso) {
        glUseProgram(r->p_iso);
        um4(r->p_iso, "u_vp", &cam->viewproj);
        u3f(r->p_iso, "u_eye", cam->eye.x, cam->eye.y, cam->eye.z);
        u2f(r->p_iso, "u_range", 0.0f, f->speed_hi);
        u1f(r->p_iso, "u_flat", 0.0f);
        bind_tex(r->p_iso, "u_cmap", 1, GL_TEXTURE_2D, r->tex_cmap);
        glBindVertexArray(r->vao_iso);
        glDrawElements(GL_TRIANGLES, r->iso_indices, GL_UNSIGNED_INT, (void *)0);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* tunnel box */
    if (rs->box_on && r->p_color) {
        glUseProgram(r->p_color);
        um4(r->p_color, "u_vp", &cam->viewproj);
        u4f(r->p_color, "u_color", 0.16f, 0.42f, 0.62f, 0.55f);
        float X = N.x, Y = N.y, Z = N.z;
        float e[] = {0, 0, 0, X, 0, 0, 0, Y, 0, X, Y, 0, 0, 0, Z, X, 0, Z, 0, Y, Z, X, Y, Z,
                     0, 0, 0, 0, Y, 0, X, 0, 0, X, Y, 0, 0, 0, Z, 0, Y, Z, X, 0, Z, X, Y, Z,
                     0, 0, 0, 0, 0, Z, X, 0, 0, X, 0, Z, 0, Y, 0, 0, Y, Z, X, Y, 0, X, Y, Z};
        draw_positions(r, e, 24, GL_LINES);
        /* inlet / outlet planes */
        glDepthMask(GL_FALSE);
        u4f(r->p_color, "u_color", 0.10f, 0.55f, 0.90f, 0.018f);
        draw_box_quad(r, v3(0, 0, 0), v3(0, Y, 0), v3(0, Y, Z), v3(0, 0, Z));
        u4f(r->p_color, "u_color", 0.90f, 0.45f, 0.10f, 0.010f);
        draw_box_quad(r, v3(X, 0, 0), v3(X, Y, 0), v3(X, Y, Z), v3(X, 0, Z));
        glDepthMask(GL_TRUE);
    }

    /* slice */
    if (rs->slice_on && r->have_field && r->p_slice) {
        glUseProgram(r->p_slice);
        um4(r->p_slice, "u_vp", &cam->viewproj);
        u3f(r->p_slice, "u_n", N.x, N.y, N.z);
        u2f(r->p_slice, "u_range", tlo, thi);
        u1f(r->p_slice, "u_opacity", rs->slice_opacity);
        bind_tex(r->p_slice, "u_field", 0, GL_TEXTURE_3D, r->tex_field);
        bind_tex(r->p_slice, "u_solid", 2, GL_TEXTURE_3D, r->tex_solid);
        bind_tex(r->p_slice, "u_cmap", 1, GL_TEXTURE_2D, r->tex_cmap);
        glDepthMask(rs->slice_opacity > 0.99f ? GL_TRUE : GL_FALSE);
        float p = CLAMP(rs->slice_pos, 0.0f, 1.0f);
        if (rs->slice_axis == 0) {
            float x = p * N.x;
            draw_box_quad(r, v3(x, 0, 0), v3(x, N.y, 0), v3(x, N.y, N.z), v3(x, 0, N.z));
        } else if (rs->slice_axis == 1) {
            float y = p * N.y;
            draw_box_quad(r, v3(0, y, 0), v3(N.x, y, 0), v3(N.x, y, N.z), v3(0, y, N.z));
        } else {
            float z = p * N.z;
            draw_box_quad(r, v3(0, 0, z), v3(N.x, 0, z), v3(N.x, N.y, z), v3(0, N.y, z));
        }
        glDepthMask(GL_TRUE);
    }

    /* volume: before lines and particles, which stay crisp on top of the glow instead of being dimmed by it
     * every time the field texture updates */
    if (rs->volume_on && r->have_field && r->p_volume) {
        if (r->depth_fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, r->scene.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->depth_fbo);
            glBlitFramebuffer(0, 0, r->fb_w, r->fb_h, 0, 0, r->fb_w, r->fb_h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, r->scene.fbo);
        }
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(r->p_volume);
        um4(r->p_volume, "u_vp", &cam->viewproj);
        u3f(r->p_volume, "u_eye", cam->eye.x, cam->eye.y, cam->eye.z);
        u3f(r->p_volume, "u_n", N.x, N.y, N.z);
        u2f(r->p_volume, "u_range", tlo, thi);
        u1f(r->p_volume, "u_density", rs->volume_density);
        bind_tex(r->p_volume, "u_field", 0, GL_TEXTURE_3D, r->tex_field);
        bind_tex(r->p_volume, "u_cmap", 1, GL_TEXTURE_2D, r->tex_cmap);
        bind_tex(r->p_volume, "u_depth", 2, GL_TEXTURE_2D, r->depth_tex);
        um4(r->p_volume, "u_inv_vp", &cam->inv_viewproj);
        u2f(r->p_volume, "u_res", (float)r->fb_w, (float)r->fb_h);
        u1i(r->p_volume, "u_has_depth", r->depth_fbo ? 1 : 0);
        u1i(r->p_volume, "u_tf", rs->field == VIS_SPEED || rs->field == VIS_UX ? 1
                                 : rs->field == VIS_VORTICITY || rs->field == VIS_QCRITERION ? 0 : 2);
        vec3 c[8];
        for (int i = 0; i < 8; i++) c[i] = v3((i & 1) ? N.x : 0, (i & 2) ? N.y : 0, (i & 4) ? N.z : 0);
        static const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        for (int fi = 0; fi < 6; fi++)
            draw_box_quad(r, c[faces[fi][0]], c[faces[fi][1]], c[faces[fi][2]], c[faces[fi][3]]);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }

    /* streamlines (premultiplied additive glow) */
    if (rs->stream_on && r->stream_indices && r->p_stream) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(r->p_stream);
        um4(r->p_stream, "u_vp", &cam->viewproj);
        u2f(r->p_stream, "u_res", (float)f->target_w, (float)f->target_h);
        u1f(r->p_stream, "u_width", rs->stream_width * (float)f->target_h / 1100.0f + 0.6f);
        u2f(r->p_stream, "u_range", lo, hi);
        u1i(r->p_stream, "u_animate", rs->stream_animate ? 1 : 0);
        const double period = 160.0;
        r->flow_steps = fmod(r->flow_steps + f->particle_steps, period * 1000.0);
        u1f(r->p_stream, "u_phase", (float)fmod(r->flow_steps, period));
        u1f(r->p_stream, "u_period", (float)period);
        /* additive lines saturate into a white wall when dense: brightness falls with the square root of the count */
        u1f(r->p_stream, "u_gain", CLAMP(0.85f * sqrtf(400.0f / MAXI((float)r->stream_lines, 1.0f)), 0.22f, 0.85f));
        bind_tex(r->p_stream, "u_cmap", 1, GL_TEXTURE_2D, r->tex_line_cmap);
        glBindVertexArray(r->vao_stream);
        glDrawElements(GL_TRIANGLES, r->stream_indices, GL_UNSIGNED_INT, (void *)0);
        glDepthMask(GL_TRUE);
    }

    /* GPU particles: transform-feedback advection through the velocity texture */
    if (rs->particles_on && r->have_vel && r->have_solid && r->p_pupdate && r->p_particle) {
        ensure_particles(r, rs->particle_count);
        if (r->part_count > 0) {
            int src = r->part_src, dst = 1 - src;
            glUseProgram(r->p_pupdate);
            u3f(r->p_pupdate, "u_n", N.x, N.y, N.z);
            /* the velocity volume holds 2x2x2 cell averages: it spans twice its texel count in lattice units */
            u3f(r->p_pupdate, "u_vext", 2.0f * r->vnx, 2.0f * r->vny, 2.0f * r->vnz);
            u3f(r->p_pupdate, "u_emit_lo", f->emitter_lo.x, f->emitter_lo.y, f->emitter_lo.z);
            u3f(r->p_pupdate, "u_emit_hi", f->emitter_hi.x, f->emitter_hi.y, f->emitter_hi.z);
            u1f(r->p_pupdate, "u_steps", CLAMP(f->particle_steps, 0.0f, 12.0f));
            u1f(r->p_pupdate, "u_life", 2.2f * N.x / MAXI(f->speed_hi * 0.6f, 0.01f));
            u1f(r->p_pupdate, "u_delay", N.x / MAXI(f->speed_hi / 1.5f, 0.01f));
            glUniform1ui(glGetUniformLocation(r->p_pupdate, "u_frame"), r->frame++);
            u1i(r->p_pupdate, "u_reset", r->part_reset ? 1 : 0);
            bind_tex(r->p_pupdate, "u_vel", 0, GL_TEXTURE_3D, r->tex_vel);
            bind_tex(r->p_pupdate, "u_solid", 2, GL_TEXTURE_3D, r->tex_solid);
            glEnable(GL_RASTERIZER_DISCARD);
            glBindVertexArray(r->vao_part[src]);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, r->vbo_part[dst]);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, r->part_count);
            glEndTransformFeedback();
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glDisable(GL_RASTERIZER_DISCARD);
            r->part_src = dst;
            r->part_reset = false;

            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUseProgram(r->p_particle);
            um4(r->p_particle, "u_vp", &cam->viewproj);
            u2f(r->p_particle, "u_res", (float)f->target_w, (float)f->target_h);
            u3f(r->p_particle, "u_n", N.x, N.y, N.z);
            u3f(r->p_particle, "u_vext", 2.0f * r->vnx, 2.0f * r->vny, 2.0f * r->vnz);
            u1f(r->p_particle, "u_size", rs->particle_size * (float)f->target_h / 1100.0f + 0.4f);
            u1f(r->p_particle, "u_streak", 6.0f);
            u1f(r->p_particle, "u_life", 2.2f * N.x / MAXI(f->speed_hi * 0.6f, 0.01f));
            u1f(r->p_particle, "u_speed_hi", f->speed_hi);
            u1f(r->p_particle, "u_gain", CLAMP(60000.0f / MAXI((float)r->part_count, 1.0f), 0.08f, 0.8f));
            bind_tex(r->p_particle, "u_vel", 0, GL_TEXTURE_3D, r->tex_vel);
            bind_tex(r->p_particle, "u_cmap", 1, GL_TEXTURE_2D, r->tex_line_cmap);
            glBindVertexArray(r->vao_pdraw[r->part_src]);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, r->part_count);
            glDepthMask(GL_TRUE);
        }
    }

    /* where the largest value of the result sits: a cross the eye can find without hunting for the red patch */
    if (f->peak_on && r->p_color) {
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r->p_color);
        um4(r->p_color, "u_vp", &cam->viewproj);
        u4f(r->p_color, "u_color", 1.0f, 0.25f, 0.9f, 1.0f);
        float s = MAXI(1.5f, maxdim * 0.010f), g = s * 0.3f;
        vec3 p = f->peak;
        float v[] = {p.x - s, p.y, p.z, p.x - g, p.y, p.z, p.x + g, p.y, p.z, p.x + s, p.y, p.z,
                     p.x, p.y - s, p.z, p.x, p.y - g, p.z, p.x, p.y + g, p.z, p.x, p.y + s, p.z,
                     p.x, p.y, p.z - s, p.x, p.y, p.z - g, p.x, p.y, p.z + g, p.x, p.y, p.z + s};
        draw_positions(r, v, 12, GL_LINES);
        glEnable(GL_DEPTH_TEST);
    }

    /* probe marker */
    if (f->probe_on && r->p_color) {
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r->p_color);
        um4(r->p_color, "u_vp", &cam->viewproj);
        u4f(r->p_color, "u_color", 1.6f, 1.1f, 0.25f, 1.0f);
        float s = MAXI(2.0f, maxdim * 0.012f);
        vec3 p = f->probe;
        float v[] = {p.x - s, p.y, p.z, p.x + s, p.y, p.z, p.x, p.y - s, p.z, p.x, p.y + s, p.z, p.x, p.y, p.z - s, p.x, p.y, p.z + s};
        draw_positions(r, v, 6, GL_LINES);
        glEnable(GL_DEPTH_TEST);
    }
    if (f->overlay && f->overlay_verts >= 2 && r->p_color) {
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r->p_color);
        um4(r->p_color, "u_vp", &cam->viewproj);
        u4f(r->p_color, "u_color", 1.8f, 1.05f, 0.25f, 0.9f);
        draw_positions(r, f->overlay, f->overlay_verts, GL_LINES);
        glEnable(GL_DEPTH_TEST);
    }
    glDisable(GL_BLEND);
}

static void fullscreen(Renderer *r) {
    glBindVertexArray(r->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void render_frame(Renderer *r, const RenderFrame *f) {
    int w = MAXI(f->target_w, 16), h = MAXI(f->target_h, 16);
    ensure_targets(r, w, h, f->rs->msaa);
    if (!r->scene.fbo || !r->resolved.fbo) return;

    glBindFramebuffer(GL_FRAMEBUFFER, r->scene.fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (f->nx > 0) draw_scene(r, f);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->scene.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->resolved.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    /* ambient occlusion from the depth buffer, at half resolution and blurred */
    bool ao = f->rs->ao_on && f->rs->ao_strength > 0.001f && r->p_ssao && r->p_aoblur && r->ao.fbo && r->ao_blur.fbo && r->depth_fbo;
    if (ao) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, r->scene.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->depth_fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, r->ao.fbo);
        glViewport(0, 0, r->ao.w, r->ao.h);
        glUseProgram(r->p_ssao);
        um4(r->p_ssao, "u_inv_vp", &f->cam->inv_viewproj);
        um4(r->p_ssao, "u_vp", &f->cam->viewproj);
        u2f(r->p_ssao, "u_res", (float)r->ao.w, (float)r->ao.h);
        u3f(r->p_ssao, "u_eye", f->cam->eye.x, f->cam->eye.y, f->cam->eye.z);
        /* the radius is a fraction of the lattice, so it stays a short radius whatever the part's size */
        float scale = (float)MAXI(MAXI(f->nx, f->ny), f->nz);
        u1f(r->p_ssao, "u_radius", 0.012f * (scale > 0 ? scale : 64.0f));
        u1i(r->p_ssao, "u_samples", 12);
        bind_tex(r->p_ssao, "u_depth", 0, GL_TEXTURE_2D, r->depth_tex);
        fullscreen(r);
        glBindFramebuffer(GL_FRAMEBUFFER, r->ao_blur.fbo);
        glViewport(0, 0, r->ao_blur.w, r->ao_blur.h);
        glUseProgram(r->p_aoblur);
        u2f(r->p_aoblur, "u_texel", 1.0f / (float)r->ao.w, 1.0f / (float)r->ao.h);
        bind_tex(r->p_aoblur, "u_src", 0, GL_TEXTURE_2D, r->ao.color_tex);
        fullscreen(r);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    bool bloom = f->rs->bloom > 0.001f && r->bloom_levels >= 2 && r->p_prefilter && r->p_down && r->p_up;
    if (bloom) {
        glBindFramebuffer(GL_FRAMEBUFFER, r->bloom[0].fbo);
        glViewport(0, 0, r->bloom[0].w, r->bloom[0].h);
        glUseProgram(r->p_prefilter);
        bind_tex(r->p_prefilter, "u_src", 0, GL_TEXTURE_2D, r->resolved.color_tex);
        u1f(r->p_prefilter, "u_threshold", 0.55f);
        u2f(r->p_prefilter, "u_texel", 1.0f / (float)w, 1.0f / (float)h);
        fullscreen(r);
        glUseProgram(r->p_down);
        for (int i = 1; i < r->bloom_levels; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, r->bloom[i].fbo);
            glViewport(0, 0, r->bloom[i].w, r->bloom[i].h);
            bind_tex(r->p_down, "u_src", 0, GL_TEXTURE_2D, r->bloom[i - 1].color_tex);
            u2f(r->p_down, "u_texel", 1.0f / r->bloom[i - 1].w, 1.0f / r->bloom[i - 1].h);
            fullscreen(r);
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(r->p_up);
        for (int i = r->bloom_levels - 1; i > 0; i--) {
            glBindFramebuffer(GL_FRAMEBUFFER, r->bloom[i - 1].fbo);
            glViewport(0, 0, r->bloom[i - 1].w, r->bloom[i - 1].h);
            bind_tex(r->p_up, "u_src", 0, GL_TEXTURE_2D, r->bloom[i].color_tex);
            u2f(r->p_up, "u_texel", 1.0f / r->bloom[i].w, 1.0f / r->bloom[i].h);
            fullscreen(r);
        }
        glDisable(GL_BLEND);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, f->target_fbo);
    glViewport(0, 0, w, h);
    glUseProgram(r->p_composite);
    bind_tex(r->p_composite, "u_scene", 0, GL_TEXTURE_2D, r->resolved.color_tex);
    bind_tex(r->p_composite, "u_bloom", 1, GL_TEXTURE_2D, bloom ? r->bloom[0].color_tex : r->resolved.color_tex);
    bind_tex(r->p_composite, "u_ao", 2, GL_TEXTURE_2D, ao ? r->ao_blur.color_tex : r->resolved.color_tex);
    u1i(r->p_composite, "u_has_ao", ao ? 1 : 0);
    u1f(r->p_composite, "u_ao_k", f->rs->ao_strength);
    u1i(r->p_composite, "u_has_bloom", bloom ? 1 : 0);
    u1f(r->p_composite, "u_bloom_k", f->rs->bloom);
    u1f(r->p_composite, "u_exposure", f->rs->exposure);
    fullscreen(r);
    glActiveTexture(GL_TEXTURE0);
}
