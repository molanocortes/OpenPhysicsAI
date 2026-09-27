/* lbm3d_metal.m - the 3D lattice Boltzmann step on the GPU (lbm3d_metal.h). Objective-C for Metal's interface; the
 * kernels are Metal Shading Language, compiled when the engine is made. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "lbm3d_metal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static NSString *const KERNELS = @R"MSL(
#include <metal_stdlib>
using namespace metal;
constant int CX[19] = {0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1, 0, 0, 0, 0};
constant int CY[19] = {0, 0, 0, 1, -1, 0, 0, 1, -1, -1, 1, 0, 0, 0, 0, 1, -1, 1, -1};
constant int CZ[19] = {0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, -1, -1, 1, 1, -1, -1, 1};
constant int OPP[19] = {0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15, 18, 17};
constant int MY[19] = {0, 1, 2, 4, 3, 5, 6, 9, 10, 7, 8, 11, 12, 13, 14, 18, 17, 16, 15};
constant int MZ[19] = {0, 1, 2, 3, 4, 6, 5, 7, 8, 9, 10, 13, 14, 11, 12, 17, 18, 15, 16};
constant float W[19] = {1.f/3, 1.f/18, 1.f/18, 1.f/18, 1.f/18, 1.f/18, 1.f/18, 1.f/36, 1.f/36, 1.f/36, 1.f/36, 1.f/36, 1.f/36,
                        1.f/36, 1.f/36, 1.f/36, 1.f/36, 1.f/36, 1.f/36};
struct Params {
    int nx, ny, nz, bcx, bcy, bcz, sponge, step, collision, floor;
    float nu, sponge_nu, cs2, lambda, ux, uy, uz, ax, ay, az, noise, inlet_noise;
    uint n, tick;
    int mode, ib, npts;        /* mode 1: stream and write the moments only; ib: a force per cell (fb) by Guo */
    float ibw;                 /* the direct forcing's relaxation per iteration */
    int outlet;                /* 1: a pressure outlet (lbm3d.h) */
};
static inline float feq_s(int d, float rho, float ux, float uy, float uz, float u2) { /* f_eq - w, shifted */
    float cu = CX[d] * ux + CY[d] * uy + CZ[d] * uz;
    return W[d] * (rho - 1.f) + W[d] * rho * (3.f * cu + 4.5f * cu * cu - 1.5f * u2);
}
static inline float noise(uint i, uint c) { /* the same seeded hash as lbm3d.c */
    uint h = i * 747796405u + c * 2891336453u + 12345u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float(h) / 4294967295.0f * 2.f - 1.f;
}
kernel void lbm_init(device float *f [[buffer(0)]], device const uchar *solid [[buffer(1)]], constant Params &P [[buffer(2)]],
                     uint i [[thread_position_in_grid]]) {
    if (i >= P.n) return;
    float a = P.noise * sqrt(P.ux * P.ux + P.uy * P.uy + P.uz * P.uz);
    float ux = P.ux + a * noise(i, 0), uy = P.uy + a * noise(i, 1), uz = P.uz + a * noise(i, 2), u2 = ux * ux + uy * uy + uz * uz;
    for (int d = 0; d < 19; d++) f[d * P.n + i] = solid[i] ? 0.f : feq_s(d, 1.f, ux, uy, uz, u2);
}
kernel void lbm_step(device const float *fo [[buffer(0)]], device float *fn [[buffer(1)]], device const uchar *solid [[buffer(2)]],
                     device const int *lks [[buffer(3)]], device const uchar *lkc [[buffer(4)]], device const uchar *lkd [[buffer(5)]],
                     device const float *lkq [[buffer(6)]], constant Params &P [[buffer(7)]], device atomic_float *fh [[buffer(8)]],
                     device atomic_int *bad [[buffer(9)]], device const float *fb [[buffer(10)]], device float4 *mom [[buffer(11)]],
                     uint i [[thread_position_in_grid]]) {
    if (i >= P.n) return;
    const uint n = P.n;
    const int nx = P.nx, ny = P.ny, nz = P.nz;
    const int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
    if (solid[i]) {
        if (P.mode == 1) mom[i] = float4(1, 0, 0, 0);
        else for (int d = 0; d < 19; d++) fn[d * n + i] = 0.f;
        return;
    }
    const bool inout = P.bcx == 2;
    if (inout && x == 0 && P.mode == 1) {
        mom[i] = float4(1, P.ux, P.uy, P.uz);
        return;
    }
    if (inout && x == 0) { /* inlet: equilibrium at the inlet velocity, density from the next cell */
        float rho = 1.f;
        for (int d = 0; d < 19; d++) rho += fo[d * n + i + 1];
        float ux = P.ux, uy = P.uy, uz = P.uz;
        if (P.inlet_noise > 0) {
            float a = P.inlet_noise * sqrt(ux * ux + uy * uy + uz * uz);
            uint seed = i ^ (P.tick * 2654435761u);
            ux += a * noise(seed, 3), uy += a * noise(seed, 4), uz += a * noise(seed, 5);
        }
        float u2 = ux * ux + uy * uy + uz * uz;
        for (int d = 0; d < 19; d++) fn[d * n + i] = feq_s(d, rho, ux, uy, uz, u2);
        return;
    }
    float fi[19], oux = 0, ouy = 0, ouz = 0;
    if (inout && x == nx - 1 && P.outlet == 1) { /* a pressure outlet: its own velocity of the step before */
        float r = 1.f, jx = 0, jy = 0, jz = 0;
        for (int d = 0; d < 19; d++) { float v = fo[d * n + i]; r += v, jx += CX[d] * v, jy += CY[d] * v, jz += CZ[d] * v; }
        oux = jx / r, ouy = jy / r, ouz = jz / r;
    }
    for (int d = 0; d < 19; d++) {
        int sx = x - CX[d], sy = y - CY[d], sz = z - CZ[d], dd = d;
        if (sx >= nx && inout) { /* outlet: its own from the step before, or the equilibrium of a pressure outlet */
            fi[d] = P.outlet == 1 ? feq_s(d, 1.f, oux, ouy, ouz, oux * oux + ouy * ouy + ouz * ouz) : fo[d * n + i];
            continue;
        }
        if (P.floor && sz < 0) { fi[d] = fo[OPP[d] * n + i]; continue; } /* the floor: bounce-back */
        if (sx < 0) sx += nx; else if (sx >= nx) sx -= nx;
        if (sy < 0 || sy >= ny) { if (P.bcy == 0) sy = (sy + ny) % ny; else { sy = y; dd = MY[dd]; } }
        if (sz < 0 || sz >= nz) { if (P.bcz == 0) sz = (sz + nz) % nz; else { sz = z; dd = MZ[dd]; } }
        fi[d] = fo[dd * n + (uint)(sx + nx * (sy + ny * sz))];
    }
    int ls = lks[i];
    if (ls >= 0) { /* Bouzidi's interpolated bounce-back, and the momentum the body takes */
        float Fx = 0, Fy = 0, Fz = 0;
        for (int k = ls, e = ls + lkc[i]; k < e; k++) {
            int d = lkd[k], o = OPP[d];
            float q = lkq[k], fin;
            int xn = x + CX[d], yn = y + CY[d], zn = z + CZ[d];
            if (P.bcx == 0) xn = (xn + nx) % nx;
            if (P.bcy == 0) yn = (yn + ny) % ny;
            if (P.bcz == 0) zn = (zn + nz) % nz;
            bool second = xn >= 0 && yn >= 0 && zn >= 0 && xn < nx && yn < ny && zn < nz;
            uint j = second ? (uint)(xn + nx * (yn + ny * zn)) : i;
            second = second && !solid[j];
            float fo_o = fo[o * n + i] + W[o];
            if (q < 0.5f && second) fin = 2.f * q * fo_o + (1.f - 2.f * q) * (fo[o * n + j] + W[o]);
            else if (q >= 0.5f) fin = fo_o / (2.f * q) + (2.f * q - 1.f) / (2.f * q) * (fo[d * n + i] + W[d]);
            else fin = fo_o;
            fi[d] = fin - W[d];
            float m = fo_o + fin - 2.f * W[o]; /* less the ambient part (lbm3d.c) */
            Fx += CX[o] * m, Fy += CY[o] * m, Fz += CZ[o] * m;
        }
        if (P.mode != 1) {
            atomic_fetch_add_explicit(&fh[3 * P.step], Fx, memory_order_relaxed);
            atomic_fetch_add_explicit(&fh[3 * P.step + 1], Fy, memory_order_relaxed);
            atomic_fetch_add_explicit(&fh[3 * P.step + 2], Fz, memory_order_relaxed);
        }
    }
    /* moments from the shifted populations: rho = 1 + sum, j = sum c f */
    float rho = 1.f, jx = 0, jy = 0, jz = 0;
    for (int d = 0; d < 19; d++) rho += fi[d], jx += CX[d] * fi[d], jy += CY[d] * fi[d], jz += CZ[d] * fi[d];
    if (P.mode == 1) { /* the immersed boundary's pass: the moments after streaming, before the force and the collision */
        mom[i] = float4(rho, jx, jy, jz);
        return;
    }
    if (P.ib) { /* a force per cell (the immersed boundary's, and the uniform one): Guo's forcing, split for TRT */
        float Fx = fb[3 * i] + rho * P.ax, Fy = fb[3 * i + 1] + rho * P.ay, Fz = fb[3 * i + 2] + rho * P.az;
        float ux = (jx + 0.5f * Fx) / rho, uy = (jy + 0.5f * Fy) / rho, uz = (jz + 0.5f * Fz) / rho, u2 = ux * ux + uy * uy + uz * uz;
        if (!(rho > 0.f) || !(u2 < 0.16f)) atomic_store_explicit(bad, 1, memory_order_relaxed);
        float fe[19], S[19], uF = ux * Fx + uy * Fy + uz * Fz;
        float pxx = 0, pyy = 0, pzz = 0, pxy = 0, pxz = 0, pyz = 0;
        for (int d = 0; d < 19; d++) {
            fe[d] = feq_s(d, rho, ux, uy, uz, u2);
            float cu = CX[d] * ux + CY[d] * uy + CZ[d] * uz, cF = CX[d] * Fx + CY[d] * Fy + CZ[d] * Fz;
            S[d] = W[d] * (3.f * cF + 9.f * cu * cF - 3.f * uF);
            float ne = fi[d] - fe[d] + 0.5f * S[d]; /* the non-equilibrium part with the force's half step (no first moment) */
            pxx += CX[d] * CX[d] * ne, pyy += CY[d] * CY[d] * ne, pzz += CZ[d] * CZ[d] * ne;
            pxy += CX[d] * CY[d] * ne, pxz += CX[d] * CZ[d] * ne, pyz += CY[d] * CZ[d] * ne;
        }
        float nu = P.nu;
        if (P.sponge > 0 && (x > nx - 1 - P.sponge || x < P.sponge)) {
            float r = x < P.sponge ? float(P.sponge - x) / P.sponge : float(x - (nx - 1 - P.sponge)) / P.sponge;
            nu = P.nu + (P.sponge_nu - P.nu) * r * r;
        }
        float tau = 3.f * nu + 0.5f;
        if (P.cs2 > 0) {
            float Qm = sqrt(2.f * (pxx * pxx + pyy * pyy + pzz * pzz + 2.f * (pxy * pxy + pxz * pxz + pyz * pyz)));
            tau = 0.5f * (tau + sqrt(tau * tau + 18.f * sqrt(2.f) * P.cs2 * Qm / rho));
        }
        float wp = 1.f / tau;
        if (P.collision == 1) {
            float keep = 1.f - wp;
            for (int d = 0; d < 19; d++) {
                float cx = CX[d], cy = CY[d], cz = CZ[d];
                float reg = 4.5f * W[d] * ((cx * cx - 1.f / 3) * pxx + (cy * cy - 1.f / 3) * pyy + (cz * cz - 1.f / 3) * pzz +
                                           2.f * (cx * cy * pxy + cx * cz * pxz + cy * cz * pyz));
                fn[d * n + i] = fe[d] + keep * reg + 0.5f * S[d]; /* BGK with Guo's term, rewritten: fe + (1 - w) neq + S / 2 */
            }
            return;
        }
        float wm = 1.f / (P.lambda / (tau - 0.5f) + 0.5f);
        fn[i] = fi[0] - wp * (fi[0] - fe[0]) + (1.f - 0.5f * wp) * S[0];
        for (int d = 1; d < 19; d += 2) {
            int o = d + 1;
            float sp = 0.5f * (fi[d] + fi[o]), sm = 0.5f * (fi[d] - fi[o]), ep = 0.5f * (fe[d] + fe[o]), em = 0.5f * (fe[d] - fe[o]);
            float Sp = 0.5f * (S[d] + S[o]), Sm = 0.5f * (S[d] - S[o]);
            float dp = -wp * (sp - ep) + (1.f - 0.5f * wp) * Sp, dm = -wm * (sm - em) + (1.f - 0.5f * wm) * Sm;
            fn[d * n + i] = fi[d] + dp + dm, fn[o * n + i] = fi[o] + dp - dm;
        }
        return;
    }
    float ux = jx / rho, uy = jy / rho, uz = jz / rho, u2 = ux * ux + uy * uy + uz * uz;
    if (!(rho > 0.f) || !(u2 < 0.16f)) atomic_store_explicit(bad, 1, memory_order_relaxed);
    float fe[19];
    float pxx = 0, pyy = 0, pzz = 0, pxy = 0, pxz = 0, pyz = 0;
    for (int d = 0; d < 19; d++) {
        fe[d] = feq_s(d, rho, ux, uy, uz, u2);
        if (P.cs2 > 0) {
            float ne = fi[d] - fe[d];
            pxx += CX[d] * CX[d] * ne, pyy += CY[d] * CY[d] * ne, pzz += CZ[d] * CZ[d] * ne;
            pxy += CX[d] * CY[d] * ne, pxz += CX[d] * CZ[d] * ne, pyz += CY[d] * CZ[d] * ne;
        }
    }
    float nu = P.nu;
    if (P.sponge > 0 && (x > nx - 1 - P.sponge || x < P.sponge)) { /* sponges at both ends */
        float r = x < P.sponge ? float(P.sponge - x) / P.sponge : float(x - (nx - 1 - P.sponge)) / P.sponge;
        nu = P.nu + (P.sponge_nu - P.nu) * r * r;
    }
    float tau = 3.f * nu + 0.5f;
    if (P.cs2 > 0) {
        float Qm = sqrt(2.f * (pxx * pxx + pyy * pyy + pzz * pzz + 2.f * (pxy * pxy + pxz * pxz + pyz * pyz)));
        tau = 0.5f * (tau + sqrt(tau * tau + 18.f * sqrt(2.f) * P.cs2 * Qm / rho));
    }
    float wp = 1.f / tau;
    if (P.collision == 1) { /* regularised: the non-equilibrium part projected on its second moment (lbm3d.h) */
        if (P.cs2 <= 0) {
            for (int d = 0; d < 19; d++) {
                float ne = fi[d] - fe[d];
                pxx += CX[d] * CX[d] * ne, pyy += CY[d] * CY[d] * ne, pzz += CZ[d] * CZ[d] * ne;
                pxy += CX[d] * CY[d] * ne, pxz += CX[d] * CZ[d] * ne, pyz += CY[d] * CZ[d] * ne;
            }
        }
        float keep = 1.f - wp;
        for (int d = 0; d < 19; d++) {
            float cx = CX[d], cy = CY[d], cz = CZ[d];
            float reg = 4.5f * W[d] * ((cx * cx - 1.f / 3) * pxx + (cy * cy - 1.f / 3) * pyy + (cz * cz - 1.f / 3) * pzz +
                                       2.f * (cx * cy * pxy + cx * cz * pxz + cy * cz * pyz));
            fn[d * n + i] = fe[d] + keep * reg + W[d] * rho * 3.f * (cx * P.ax + cy * P.ay + cz * P.az);
        }
        return;
    }
    float wm = 1.f / (P.lambda / (tau - 0.5f) + 0.5f);
    fn[i] = fi[0] - wp * (fi[0] - fe[0]);
    for (int d = 1; d < 19; d += 2) {
        int o = d + 1;
        float sp = 0.5f * (fi[d] + fi[o]), sm = 0.5f * (fi[d] - fi[o]), ep = 0.5f * (fe[d] + fe[o]), em = 0.5f * (fe[d] - fe[o]);
        float dp = -wp * (sp - ep), dm = -wm * (sm - em) + W[d] * rho * 3.f * (CX[d] * P.ax + CY[d] * P.ay + CZ[d] * P.az);
        fn[d * n + i] = fi[d] + dp + dm, fn[o * n + i] = fi[o] + dp - dm;
    }
}
kernel void lbm_macro(device const float *f [[buffer(0)]], device const uchar *solid [[buffer(1)]], constant Params &P [[buffer(2)]],
                      device float *out [[buffer(3)]], device const float *fb [[buffer(4)]], uint i [[thread_position_in_grid]]) {
    if (i >= P.n) return;
    if (solid[i]) { out[4 * i] = 1, out[4 * i + 1] = out[4 * i + 2] = out[4 * i + 3] = 0; return; }
    float rho = 1.f, jx = 0, jy = 0, jz = 0;
    for (int d = 0; d < 19; d++) { float v = f[d * P.n + i]; rho += v, jx += CX[d] * v, jy += CY[d] * v, jz += CZ[d] * v; }
    out[4 * i] = rho;
    if (P.ib) { /* after Guo's collision the populations carry j + F: the velocity is (j + F / 2) / rho of before */
        out[4 * i + 1] = (jx - 0.5f * fb[3 * i]) / rho - 0.5f * P.ax, out[4 * i + 2] = (jy - 0.5f * fb[3 * i + 1]) / rho - 0.5f * P.ay;
        out[4 * i + 3] = (jz - 0.5f * fb[3 * i + 2]) / rho - 0.5f * P.az;
        return;
    }
    out[4 * i + 1] = jx / rho - 0.5f * P.ax, out[4 * i + 2] = jy / rho - 0.5f * P.ay, out[4 * i + 3] = jz / rho - 0.5f * P.az;
}
/* the immersed boundary (Peskin's method with direct forcing, Uhlmann 2005, iterated: Luo et al. 2007): Lagrangian points
 * pt = (x, y, z, dV) in cell coordinates (a cell's centre at integer + 1/2), their velocities vel, and the force each
 * puts on the fluid F (lattice units); Roma, Peskin and Berger's three-point kernel (1999) */
static inline float roma(float r) {
    r = fabs(r);
    if (r <= 0.5f) return (1.f + sqrt(1.f - 3.f * r * r)) / 3.f;
    if (r <= 1.5f) return (5.f - 3.f * r - sqrt(max(0.f, 1.f - 3.f * (1.f - r) * (1.f - r)))) / 6.f;
    return 0.f;
}
static inline int wrapc(int a, int n, int periodic) { return periodic ? (a % n + n) % n : a; }
kernel void ib_zero(device float *fb [[buffer(0)]], constant Params &P [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    if (i < 3 * P.n) fb[i] = 0.f;
}
kernel void ib_reset(device float4 *F [[buffer(0)]], constant Params &P [[buffer(1)]], uint p [[thread_position_in_grid]]) {
    if (p < (uint)P.npts) F[p] = float4(0);
}
kernel void ib_spread(device const float4 *pt [[buffer(0)]], device const float4 *dF [[buffer(1)]], device atomic_float *fb [[buffer(2)]],
                      constant Params &P [[buffer(3)]], uint p [[thread_position_in_grid]]) {
    if (p >= (uint)P.npts) return;
    float4 X = pt[p], G = dF[p];
    int bx = int(floor(X.x - 0.5f)), by = int(floor(X.y - 0.5f)), bz = int(floor(X.z - 0.5f));
    for (int c = -1; c <= 2; c++)
        for (int b = -1; b <= 2; b++)
            for (int a = -1; a <= 2; a++) {
                int x = bx + a, y = by + b, z = bz + c;
                float w = roma(x + 0.5f - X.x) * roma(y + 0.5f - X.y) * roma(z + 0.5f - X.z);
                if (w == 0.f) continue;
                x = wrapc(x, P.nx, P.bcx == 0), y = wrapc(y, P.ny, P.bcy == 0), z = wrapc(z, P.nz, P.bcz == 0);
                if (x < 0 || y < 0 || z < 0 || x >= P.nx || y >= P.ny || z >= P.nz) continue;
                uint i = uint(x + P.nx * (y + P.ny * z));
                atomic_fetch_add_explicit(&fb[3 * i], w * G.x, memory_order_relaxed);
                atomic_fetch_add_explicit(&fb[3 * i + 1], w * G.y, memory_order_relaxed);
                atomic_fetch_add_explicit(&fb[3 * i + 2], w * G.z, memory_order_relaxed);
            }
}
/* the velocity at each point with the forces spread so far, (j + F / 2) / rho interpolated, and the force still needed:
 * dF = 2 rho (U - u) dV, added to the point's total */
kernel void ib_interp(device const float4 *pt [[buffer(0)]], device const float4 *vel [[buffer(1)]], device float4 *F [[buffer(2)]],
                      device float4 *dF [[buffer(3)]], device const float4 *mom [[buffer(4)]], device const float *fb [[buffer(5)]],
                      constant Params &P [[buffer(6)]], uint p [[thread_position_in_grid]]) {
    if (p >= (uint)P.npts) return;
    float4 X = pt[p];
    int bx = int(floor(X.x - 0.5f)), by = int(floor(X.y - 0.5f)), bz = int(floor(X.z - 0.5f));
    float ux = 0, uy = 0, uz = 0, rho = 0, ws = 0;
    for (int c = -1; c <= 2; c++)
        for (int b = -1; b <= 2; b++)
            for (int a = -1; a <= 2; a++) {
                int x = bx + a, y = by + b, z = bz + c;
                float w = roma(x + 0.5f - X.x) * roma(y + 0.5f - X.y) * roma(z + 0.5f - X.z);
                if (w == 0.f) continue;
                x = wrapc(x, P.nx, P.bcx == 0), y = wrapc(y, P.ny, P.bcy == 0), z = wrapc(z, P.nz, P.bcz == 0);
                if (x < 0 || y < 0 || z < 0 || x >= P.nx || y >= P.ny || z >= P.nz) continue;
                uint i = uint(x + P.nx * (y + P.ny * z));
                float4 m = mom[i];
                float r = m.x;
                ux += w * (m.y + 0.5f * fb[3 * i]) / r, uy += w * (m.z + 0.5f * fb[3 * i + 1]) / r, uz += w * (m.w + 0.5f * fb[3 * i + 2]) / r;
                rho += w * r, ws += w;
            }
    if (ws <= 0.f) { dF[p] = float4(0); return; }
    ux /= ws, uy /= ws, uz /= ws, rho /= ws;
    /* a point with a mass m (vel.w, lattice units; 0: moved as prescribed, as if infinitely heavy) answers the force too:
     * its velocity falls by F / m in the step, so the force that leaves fluid and point at one velocity is
     * (U - u) / (1 / (2 rho dV) + 1 / m) - a local implicit coupling, stable for sheets far lighter than the fluid they move */
    float4 U = vel[p];
    float m = U.w, g = P.ibw / (1.f / (2.f * rho * X.w) + (m > 0.f ? 1.f / m : 0.f));
    float4 Ft = F[p];
    if (m > 0.f) U.x -= Ft.x / m, U.y -= Ft.y / m, U.z -= Ft.z / m;
    float4 d = float4(g * (U.x - ux), g * (U.y - uy), g * (U.z - uz), 0.f);
    dF[p] = d, F[p] += d;
}
)MSL";

typedef struct {
    int nx, ny, nz, bcx, bcy, bcz, sponge, step, collision, floor;
    float nu, sponge_nu, cs2, lambda, ux, uy, uz, ax, ay, az, noise, inlet_noise;
    uint32_t n, tick;
    int mode, ib, npts;
    float ibw;
    int outlet;
} Params;

struct Lbm3DGpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> init, step, macro, ib_zero, ib_reset, ib_spread, ib_interp;
    id<MTLBuffer> f[2], solid, lks, lkc, lkd, lkq, hist, bad, out;
    id<MTLBuffer> fb, pts, vel, F, dF; /* the immersed boundary: force per cell, points, their velocities and forces */
    int ib_iters;
    Params P;
    int cur, hist_cap;
    long steps;
    char name[64];
};

Lbm3DGpu *lbm3d_gpu_create(const Lbm3D *geo, char *err, size_t errlen) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            snprintf(err, errlen, "lbm3d: no Metal GPU on this machine");
            return NULL;
        }
        NSError *e = nil;
        MTLCompileOptions *opt = [MTLCompileOptions new];
        /* IEEE single precision: the lab never builds numerics with fast-math */
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe;
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        id<MTLLibrary> lib = [dev newLibraryWithSource:KERNELS options:opt error:&e];
        if (!lib) {
            snprintf(err, errlen, "lbm3d: Metal kernels did not compile: %s", e.localizedDescription.UTF8String);
            return NULL;
        }
        Lbm3DGpu *G = calloc(1, sizeof *G);
        if (!G) return NULL;
        G->dev = dev;
        G->queue = [dev newCommandQueue];
        G->init = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lbm_init"] error:&e];
        G->step = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lbm_step"] error:&e];
        G->macro = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lbm_macro"] error:&e];
        G->ib_zero = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ib_zero"] error:&e];
        G->ib_reset = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ib_reset"] error:&e];
        G->ib_spread = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ib_spread"] error:&e];
        G->ib_interp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ib_interp"] error:&e];
        const Lbm3DSpec *s = lbm3d_spec(geo);
        size_t n = (size_t)s->nx * s->ny * s->nz;
        G->P = (Params){s->nx, s->ny, s->nz, s->bc_x, s->bc_y, s->bc_z, s->sponge, 0, s->collision, s->floor ? 1 : 0, (float)s->nu, (float)s->sponge_nu,
                        (float)(s->smagorinsky * s->smagorinsky), 3.0f / 16, (float)s->u_in[0], (float)s->u_in[1], (float)s->u_in[2],
                        (float)s->accel[0], (float)s->accel[1], (float)s->accel[2], (float)s->noise, (float)s->inlet_noise, (uint32_t)n, 0, 0, 0, 0, 1.0f, s->outlet};
        MTLResourceOptions shared = MTLResourceStorageModeShared, priv = MTLResourceStorageModePrivate;
        G->f[0] = [dev newBufferWithLength:19 * n * sizeof(float) options:priv];
        G->f[1] = [dev newBufferWithLength:19 * n * sizeof(float) options:priv];
        G->solid = [dev newBufferWithBytes:lbm3d_solid(geo) length:n options:shared];
        const int *st;
        const unsigned char *cnt, *dir;
        const double *q;
        lbm3d_link_arrays(geo, &st, &cnt, &dir, &q);
        long nl = lbm3d_links(geo);
        G->lks = [dev newBufferWithBytes:st length:n * sizeof(int) options:shared];
        G->lkc = [dev newBufferWithBytes:cnt length:n options:shared];
        G->lkd = [dev newBufferWithLength:(size_t)(nl ? nl : 1) options:shared];
        G->lkq = [dev newBufferWithLength:(size_t)(nl ? nl : 1) * sizeof(float) options:shared];
        if (nl) {
            memcpy(G->lkd.contents, dir, (size_t)nl);
            float *qf = G->lkq.contents;
            for (long k = 0; k < nl; k++) qf[k] = (float)q[k];
        }
        G->hist_cap = 256;
        G->hist = [dev newBufferWithLength:3 * sizeof(float) * (size_t)G->hist_cap options:shared];
        G->bad = [dev newBufferWithLength:sizeof(int) options:shared];
        G->out = [dev newBufferWithLength:4 * n * sizeof(float) options:shared];
        if (!G->queue || !G->init || !G->step || !G->macro || !G->f[0] || !G->f[1] || !G->out) {
            snprintf(err, errlen, "lbm3d: the GPU could not hold %zu cells (%.0f MB)", n, n * (2 * 19 * 4 + 22.0) / 1048576);
            lbm3d_gpu_free(G);
            return NULL;
        }
        snprintf(G->name, sizeof G->name, "%s", dev.name.UTF8String);
        return G;
    }
}

void lbm3d_gpu_free(Lbm3DGpu *G) {
    if (!G) return;
    @autoreleasepool {
        G->dev = nil, G->queue = nil, G->init = G->step = G->macro = G->ib_zero = G->ib_reset = G->ib_spread = G->ib_interp = nil;
        G->f[0] = G->f[1] = G->solid = G->lks = G->lkc = G->lkd = G->lkq = G->hist = G->bad = G->out = nil;
        G->fb = G->pts = G->vel = G->F = G->dF = nil;
    }
    free(G);
}

static void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> ps, NSUInteger n) {
    NSUInteger tg = ps.maxTotalThreadsPerThreadgroup < 256 ? ps.maxTotalThreadsPerThreadgroup : 256;
    [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
}

void lbm3d_gpu_init(Lbm3DGpu *G) {
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [G->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:G->init];
        [enc setBuffer:G->f[G->cur] offset:0 atIndex:0];
        [enc setBuffer:G->solid offset:0 atIndex:1];
        [enc setBytes:&G->P length:sizeof G->P atIndex:2];
        dispatch(enc, G->init, G->P.n);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        G->steps = 0;
    }
}

void lbm3d_gpu_set_inlet(Lbm3DGpu *G, const double u[3]) { G->P.ux = (float)u[0], G->P.uy = (float)u[1], G->P.uz = (float)u[2]; }

bool lbm3d_gpu_steps(Lbm3DGpu *G, int nsteps, double *forces) {
    @autoreleasepool {
        if (nsteps > G->hist_cap) {
            G->hist_cap = nsteps;
            G->hist = [G->dev newBufferWithLength:3 * sizeof(float) * (size_t)nsteps options:MTLResourceStorageModeShared];
        }
        memset(G->hist.contents, 0, 3 * sizeof(float) * (size_t)nsteps);
        *(int *)G->bad.contents = 0;
        id<MTLCommandBuffer> cb = [G->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:G->step];
        [enc setBuffer:G->solid offset:0 atIndex:2];
        [enc setBuffer:G->lks offset:0 atIndex:3];
        [enc setBuffer:G->lkc offset:0 atIndex:4];
        [enc setBuffer:G->lkd offset:0 atIndex:5];
        [enc setBuffer:G->lkq offset:0 atIndex:6];
        [enc setBuffer:G->hist offset:0 atIndex:8];
        [enc setBuffer:G->bad offset:0 atIndex:9];
        [enc setBuffer:G->fb ? G->fb : G->bad offset:0 atIndex:10];
        [enc setBuffer:G->out offset:0 atIndex:11];
        for (int k = 0; k < nsteps; k++) {
            Params P = G->P;
            P.step = k, P.tick = (uint32_t)(G->steps + k);
            [enc setBuffer:G->f[G->cur] offset:0 atIndex:0];
            [enc setBuffer:G->f[1 - G->cur] offset:0 atIndex:1];
            [enc setBytes:&P length:sizeof P atIndex:7];
            dispatch(enc, G->step, G->P.n);
            [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
            G->cur = 1 - G->cur;
        }
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        G->steps += nsteps;
        if (forces) {
            const float *h = G->hist.contents;
            for (int k = 0; k < 3 * nsteps; k++) forces[k] = h[k];
        }
        return *(int *)G->bad.contents == 0 && cb.status == MTLCommandBufferStatusCompleted;
    }
}

void lbm3d_gpu_macro(Lbm3DGpu *G, double *rho, double *u) {
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [G->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:G->macro];
        [enc setBuffer:G->f[G->cur] offset:0 atIndex:0];
        [enc setBuffer:G->solid offset:0 atIndex:1];
        [enc setBytes:&G->P length:sizeof G->P atIndex:2];
        [enc setBuffer:G->out offset:0 atIndex:3];
        [enc setBuffer:G->fb ? G->fb : G->bad offset:0 atIndex:4];
        dispatch(enc, G->macro, G->P.n);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float *o = G->out.contents;
        for (size_t i = 0; i < G->P.n; i++) {
            if (rho) rho[i] = o[4 * i];
            if (u) u[3 * i] = o[4 * i + 1], u[3 * i + 1] = o[4 * i + 2], u[3 * i + 2] = o[4 * i + 3];
        }
    }
}

long lbm3d_gpu_steps_done(const Lbm3DGpu *G) { return G->steps; }
const char *lbm3d_gpu_name(const Lbm3DGpu *G) { return G->name; }

bool lbm3d_gpu_ib_enable(Lbm3DGpu *G, int npts, int iterations, double relax, char *err, size_t errlen) {
    @autoreleasepool {
        size_t np = (size_t)(npts > 0 ? npts : 1);
        MTLResourceOptions shared = MTLResourceStorageModeShared;
        G->fb = [G->dev newBufferWithLength:3 * (size_t)G->P.n * sizeof(float) options:MTLResourceStorageModePrivate];
        G->pts = [G->dev newBufferWithLength:4 * np * sizeof(float) options:shared];
        G->vel = [G->dev newBufferWithLength:4 * np * sizeof(float) options:shared];
        G->F = [G->dev newBufferWithLength:4 * np * sizeof(float) options:shared];
        G->dF = [G->dev newBufferWithLength:4 * np * sizeof(float) options:MTLResourceStorageModePrivate];
        if (!G->fb || !G->pts || !G->vel || !G->F || !G->dF || !G->ib_zero || !G->ib_reset || !G->ib_spread || !G->ib_interp) {
            snprintf(err, errlen, "lbm3d: the GPU could not hold the immersed boundary");
            return false;
        }
        memset(G->pts.contents, 0, 4 * np * sizeof(float)), memset(G->vel.contents, 0, 4 * np * sizeof(float));
        memset(G->F.contents, 0, 4 * np * sizeof(float));
        G->P.ib = 1, G->P.npts = npts, G->P.ibw = (float)(relax > 0 ? relax : 1), G->ib_iters = iterations > 0 ? iterations : 1;
        /* the force per cell starts at zero */
        id<MTLCommandBuffer> cb = [G->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:G->ib_zero];
        [enc setBuffer:G->fb offset:0 atIndex:0];
        [enc setBytes:&G->P length:sizeof G->P atIndex:1];
        dispatch(enc, G->ib_zero, 3 * (NSUInteger)G->P.n);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        return true;
    }
}

float *lbm3d_gpu_ib_points(Lbm3DGpu *G) { return G->pts ? G->pts.contents : NULL; }
float *lbm3d_gpu_ib_velocities(Lbm3DGpu *G) { return G->vel ? G->vel.contents : NULL; }
const float *lbm3d_gpu_ib_forces(const Lbm3DGpu *G) { return G->F ? G->F.contents : NULL; }

bool lbm3d_gpu_ib_step(Lbm3DGpu *G, double force[3]) {
    @autoreleasepool {
        if (!G->fb) return false;
        float *h = G->hist.contents;
        h[0] = h[1] = h[2] = 0;
        *(int *)G->bad.contents = 0;
        Params P = G->P;
        P.step = 0, P.tick = (uint32_t)G->steps;
        id<MTLCommandBuffer> cb = [G->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        /* 1: stream, and the moments before any force */
        [enc setComputePipelineState:G->step];
        [enc setBuffer:G->f[G->cur] offset:0 atIndex:0];
        [enc setBuffer:G->f[1 - G->cur] offset:0 atIndex:1];
        [enc setBuffer:G->solid offset:0 atIndex:2];
        [enc setBuffer:G->lks offset:0 atIndex:3];
        [enc setBuffer:G->lkc offset:0 atIndex:4];
        [enc setBuffer:G->lkd offset:0 atIndex:5];
        [enc setBuffer:G->lkq offset:0 atIndex:6];
        [enc setBuffer:G->hist offset:0 atIndex:8];
        [enc setBuffer:G->bad offset:0 atIndex:9];
        [enc setBuffer:G->fb offset:0 atIndex:10];
        [enc setBuffer:G->out offset:0 atIndex:11];
        P.mode = 1;
        [enc setBytes:&P length:sizeof P atIndex:7];
        dispatch(enc, G->step, G->P.n);
        /* 2: the forces, iterated: interpolate, correct, spread */
        [enc setComputePipelineState:G->ib_zero];
        [enc setBuffer:G->fb offset:0 atIndex:0];
        [enc setBytes:&P length:sizeof P atIndex:1];
        dispatch(enc, G->ib_zero, 3 * (NSUInteger)G->P.n);
        [enc setComputePipelineState:G->ib_reset];
        [enc setBuffer:G->F offset:0 atIndex:0];
        [enc setBytes:&P length:sizeof P atIndex:1];
        dispatch(enc, G->ib_reset, (NSUInteger)(P.npts > 0 ? P.npts : 1));
        [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
        for (int it = 0; it < G->ib_iters && P.npts > 0; it++) {
            [enc setComputePipelineState:G->ib_interp];
            [enc setBuffer:G->pts offset:0 atIndex:0];
            [enc setBuffer:G->vel offset:0 atIndex:1];
            [enc setBuffer:G->F offset:0 atIndex:2];
            [enc setBuffer:G->dF offset:0 atIndex:3];
            [enc setBuffer:G->out offset:0 atIndex:4];
            [enc setBuffer:G->fb offset:0 atIndex:5];
            [enc setBytes:&P length:sizeof P atIndex:6];
            dispatch(enc, G->ib_interp, (NSUInteger)P.npts);
            [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [enc setComputePipelineState:G->ib_spread];
            [enc setBuffer:G->pts offset:0 atIndex:0];
            [enc setBuffer:G->dF offset:0 atIndex:1];
            [enc setBuffer:G->fb offset:0 atIndex:2];
            [enc setBytes:&P length:sizeof P atIndex:3];
            dispatch(enc, G->ib_spread, (NSUInteger)P.npts);
            [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        /* 3: the step with the force */
        P.mode = 0;
        [enc setComputePipelineState:G->step];
        [enc setBuffer:G->f[G->cur] offset:0 atIndex:0];
        [enc setBuffer:G->f[1 - G->cur] offset:0 atIndex:1];
        [enc setBuffer:G->solid offset:0 atIndex:2];
        [enc setBuffer:G->lks offset:0 atIndex:3];
        [enc setBuffer:G->lkc offset:0 atIndex:4];
        [enc setBuffer:G->lkd offset:0 atIndex:5];
        [enc setBuffer:G->lkq offset:0 atIndex:6];
        [enc setBuffer:G->hist offset:0 atIndex:8];
        [enc setBuffer:G->bad offset:0 atIndex:9];
        [enc setBuffer:G->fb offset:0 atIndex:10];
        [enc setBuffer:G->out offset:0 atIndex:11];
        [enc setBytes:&P length:sizeof P atIndex:7];
        dispatch(enc, G->step, G->P.n);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        G->cur = 1 - G->cur;
        G->steps++;
        if (force) force[0] = h[0], force[1] = h[1], force[2] = h[2];
        return *(int *)G->bad.contents == 0 && cb.status == MTLCommandBufferStatusCompleted;
    }
}
