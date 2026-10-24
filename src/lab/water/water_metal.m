/* water_metal.m - the free-surface water solver on the GPU (water_metal.h). Objective-C for Metal's interface; the
 * kernels are Metal Shading Language, compiled when the engine is made. The physics follows water.c line for line. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "water_metal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *const KERNELS = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Prm {
    float h, h4, aD, Fk, c0, B, mass, rho0, g, alpha, delta, cfl, Ly, cx, cy, cz, Lx, Lz;
    int gx, gy, gz, nc, N, periodic, chunks, pad; /* pad: the neighbour list's length per particle */
};
struct State { float t, dt; atomic_uint amax, vmax; atomic_int bad; uint steps; };
/* flags in X.w: 0 water, 1 wall, 2 water that left the region */
/* a particle is its cell (CI) and its place within the cell (X.xyz, from the cell's corner): a float there is sixteen
 * times finer than one measured from the region's corner, and forces at coarser positions stirred still water */
static inline int cell_index(int4 c, constant Prm &P) {
    if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= P.gx || c.y >= P.gy || c.z >= P.gz) return P.nc;
    return (c.z * P.gy + c.y) * P.gx + c.x;
}
static inline float3 sep(float3 a, int4 ca, float3 b, int4 cb, constant Prm &P) {
    float3 d = (a - b) + float3(ca.xyz - cb.xyz) * float3(P.cx, P.cy, P.cz);
    if (P.periodic) d.y -= P.Ly * rint(d.y / P.Ly);
    return d;
}
static inline float kf(float r, constant Prm &P) { float q = r / P.h; if (q >= 2.f) return 0.f; float u = 1.f - 0.5f * q; return P.Fk * u * u * u; }
static inline float kw(float r, constant Prm &P) { float q = r / P.h; if (q >= 2.f) return 0.f; float u = 1.f - 0.5f * q; return P.aD * u * u * u * u * (2.f * q + 1.f); }
/* Tait's pressure from the density's deviation e = (rho - rho0) / rho0: (1 + e)^7 - 1 expanded, so that a float keeps
 * its digits (the ratio itself, near 1, keeps only seven) */
static inline float tait_dev(float dev, constant Prm &P) {
    float e = dev / P.rho0;
    return P.B * e * (7.f + e * (21.f + e * (35.f + e * (35.f + e * (21.f + e * (7.f + e))))));
}

/* the cells around a point, two on each side (cells are at least h wide), each once: ys holds the y rows to visit */
static inline int rows_y(int cj, constant Prm &P, thread int *ys) {
    int n = 0;
    if (P.periodic && P.gy <= 5) { for (int j = 0; j < P.gy; j++) ys[n++] = j; return n; }
    for (int d = -2; d <= 2; d++) {
        int j = cj + d;
        if (P.periodic) j = (j % P.gy + P.gy) % P.gy;
        else if (j < 0 || j >= P.gy) continue;
        ys[n++] = j;
    }
    return n;
}

kernel void w_clear(device uint *counts [[buffer(0)]], device uint *wet [[buffer(1)]], constant Prm &P [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    if (i <= uint(P.nc)) counts[i] = 0, wet[i] = 0;
}
kernel void w_count(device const float4 *X [[buffer(0)]], device uint *cellOf [[buffer(1)]], device atomic_uint *counts [[buffer(2)]],
                    device atomic_uint *wet [[buffer(3)]], constant Prm &P [[buffer(4)]], device const int4 *CI [[buffer(5)]],
                    uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N)) return;
    float4 x = X[i];
    int c = x.w > 1.5f ? P.nc : cell_index(CI[i], P);
    cellOf[i] = uint(c);
    atomic_fetch_add_explicit(&counts[c], 1u, memory_order_relaxed);
    if (x.w < 0.5f && c < P.nc) atomic_fetch_add_explicit(&wet[c], 1u, memory_order_relaxed);
}
kernel void w_scan_chunks(device const uint *counts [[buffer(0)]], device uint *sums [[buffer(1)]], constant Prm &P [[buffer(2)]],
                          uint k [[thread_position_in_grid]]) {
    if (k >= uint(P.chunks)) return;
    uint s = 0, a = k * 256, b = min(a + 256, uint(P.nc) + 1);
    for (uint c = a; c < b; c++) s += counts[c];
    sums[k] = s;
}
kernel void w_scan_top(device uint *sums [[buffer(0)]], constant Prm &P [[buffer(1)]], uint k [[thread_position_in_grid]]) {
    if (k != 0) return;
    uint run = 0;
    for (int q = 0; q < P.chunks; q++) { uint v = sums[q]; sums[q] = run; run += v; }
}
kernel void w_scan_final(device const uint *counts [[buffer(0)]], device const uint *sums [[buffer(1)]], device uint *cstart [[buffer(2)]],
                         device uint *fill [[buffer(3)]], constant Prm &P [[buffer(4)]], uint k [[thread_position_in_grid]]) {
    if (k >= uint(P.chunks)) return;
    uint run = sums[k], a = k * 256, b = min(a + 256, uint(P.nc) + 1);
    for (uint c = a; c < b; c++) { cstart[c] = run, fill[c] = run; run += counts[c]; }
    if (b == uint(P.nc) + 1) cstart[P.nc + 1] = run;
}
kernel void w_scatter(device const float4 *X [[buffer(0)]], device const float4 *V [[buffer(1)]], device const int *ID [[buffer(2)]],
                      device const uint *cellOf [[buffer(3)]], device atomic_uint *fill [[buffer(4)]], device float4 *X2 [[buffer(5)]],
                      device float4 *V2 [[buffer(6)]], device int *ID2 [[buffer(7)]], constant Prm &P [[buffer(8)]],
                      device const float4 *C [[buffer(9)]], device float4 *C2 [[buffer(10)]], device const int4 *CI [[buffer(11)]],
                      device int4 *CI2 [[buffer(12)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N)) return;
    uint s = atomic_fetch_add_explicit(&fill[cellOf[i]], 1u, memory_order_relaxed);
    X2[s] = X[i], V2[s] = V[i], ID2[s] = ID[i], C2[s] = C[i], CI2[s] = CI[i];
}
/* the particles of each cell into the order of their identities: the scatter's atomic slots vary from run to run,
 * and sums over neighbours in a varying order make runs that do not repeat */
kernel void w_order(device float4 *X [[buffer(0)]], device float4 *V [[buffer(1)]], device int *ID [[buffer(2)]], device float4 *C [[buffer(3)]],
                    device int4 *CI [[buffer(4)]], device const uint *cstart [[buffer(5)]], constant Prm &P [[buffer(6)]],
                    uint c [[thread_position_in_grid]]) {
    if (c > uint(P.nc)) return;
    uint a = cstart[c], b = cstart[c + 1];
    for (uint k = a + 1; k < b; k++) {
        float4 x = X[k], v = V[k], cc = C[k];
        int id = ID[k];
        int4 ci = CI[k];
        uint j = k;
        while (j > a && ID[j - 1] > id) X[j] = X[j - 1], V[j] = V[j - 1], ID[j] = ID[j - 1], C[j] = C[j - 1], CI[j] = CI[j - 1], j--;
        X[j] = x, V[j] = v, ID[j] = id, C[j] = cc, CI[j] = ci;
    }
}
kernel void w_pressure(device const float4 *X [[buffer(0)]], device const float4 *V [[buffer(1)]], device float *Pr [[buffer(2)]],
                       constant Prm &P [[buffer(3)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N)) return;
    if (X[i].w != 1.f) Pr[i] = tait_dev(V[i].w, P);
}
/* a wall particle's pressure from the water next to it (Adami et al. 2012), clamped at zero */
kernel void w_wall(device const float4 *X [[buffer(0)]], device float4 *V [[buffer(1)]], device float *Pr [[buffer(2)]],
                   device const uint *cstart [[buffer(3)]], device const uint *wet [[buffer(4)]], constant Prm &P [[buffer(5)]],
                   device const int4 *CI [[buffer(6)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N) || X[i].w != 1.f) return;
    float3 xw = X[i].xyz;
    int4 cw = CI[i];
    int ys[8], ny = rows_y(cw.y, P, ys);
    int ci = cw.x, ck = cw.z;
    float sw = 0, sp = 0, sr = 0;
    for (int dk = -2; dk <= 2; dk++) {
        int k = ck + dk;
        if (k < 0 || k >= P.gz) continue;
        for (int y = 0; y < ny; y++)
            for (int di = -2; di <= 2; di++) {
                int ii = ci + di;
                if (ii < 0 || ii >= P.gx) continue;
                int c = (k * P.gy + ys[y]) * P.gx + ii;
                if (wet[c] == 0) continue;
                for (uint q = cstart[c]; q < cstart[c + 1]; q++) {
                    if (X[q].w != 0.f) continue;
                    float3 d = sep(xw, cw, X[q].xyz, CI[q], P);
                    float W = kw(length(d), P);
                    if (W <= 0.f) continue;
                    sw += W, sp += Pr[q] * W, sr += (P.rho0 + V[q].w) * d.z * W;
                }
            }
    }
    float p = sw > 0.f ? (sp - P.g * sr) / sw : 0.f;
    p = max(p, 0.f);
    Pr[i] = p, V[i].w = P.rho0 * (pow(p / P.B + 1.f, 1.f / 7.f) - 1.f);
}
kernel void w_rates(device const float4 *X [[buffer(0)]], device const float4 *V [[buffer(1)]], device const float *Pr [[buffer(2)]],
                    device const uint *cstart [[buffer(3)]], device float4 *A [[buffer(4)]], device State &S [[buffer(5)]],
                    constant Prm &P [[buffer(6)]], device uint *NB [[buffer(7)]], device int *NN [[buffer(8)]],
                    device const int4 *CI [[buffer(9)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N) || X[i].w != 0.f) return;
    int4 cii = CI[i];
    device uint *list = NB + (size_t)i * uint(P.pad);
    int nn = 0;
    float3 xi = X[i].xyz, vi = V[i].xyz;
    float ri = P.rho0 + V[i].w, pri = Pr[i] / (ri * ri);
    float3 acc = float3(0, 0, -P.g);
    int ys[8], ny = rows_y(cii.y, P, ys);
    int ci = cii.x, ck = cii.z;
    for (int dk = -2; dk <= 2; dk++) {
        int k = ck + dk;
        if (k < 0 || k >= P.gz) continue;
        for (int y = 0; y < ny; y++)
            for (int di = -2; di <= 2; di++) {
                int ii = ci + di;
                if (ii < 0 || ii >= P.gx) continue;
                int c = (k * P.gy + ys[y]) * P.gx + ii;
                for (uint q = cstart[c]; q < cstart[c + 1]; q++) {
                    if (q == i) continue;
                    float3 d = sep(xi, cii, X[q].xyz, CI[q], P);
                    float r2 = dot(d, d);
                    if (r2 >= P.h4 || r2 <= 0.f) continue;
                    if (nn < P.pad) list[nn] = q;
                    nn++;
                    float F = kf(sqrt(r2), P), rj = P.rho0 + V[q].w, vr = dot(vi - V[q].xyz, d), Pi = 0.f;
                    if (vr < 0.f) Pi = -P.alpha * P.c0 * (P.h * vr / (r2 + 0.01f * P.h * P.h)) / (0.5f * (ri + rj));
                    acc += -P.mass * (pri + Pr[q] / (rj * rj) + Pi) * F * d;
                }
            }
    }
    A[i] = float4(acc, 0);
    NN[i] = nn <= P.pad ? nn : -1; /* -1: the list overflowed; the density pass searches the cells again */
    atomic_fetch_max_explicit(&S.amax, as_type<uint>(length(acc)), memory_order_relaxed);
    atomic_fetch_max_explicit(&S.vmax, as_type<uint>(length(vi)), memory_order_relaxed);
}
kernel void w_dt(device State &S [[buffer(0)]], constant Prm &P [[buffer(1)]], uint k [[thread_position_in_grid]]) {
    if (k != 0) return;
    float am = as_type<float>(atomic_load_explicit(&S.amax, memory_order_relaxed)), vm = as_type<float>(atomic_load_explicit(&S.vmax, memory_order_relaxed));
    float dt = P.h / (P.c0 + vm);
    if (am > 0.f) dt = min(dt, sqrt(P.h / am));
    S.dt = P.cfl * dt, S.t += S.dt, S.steps++;
    atomic_store_explicit(&S.amax, 0u, memory_order_relaxed), atomic_store_explicit(&S.vmax, 0u, memory_order_relaxed);
}
kernel void w_kick(device const float4 *X [[buffer(0)]], device float4 *V [[buffer(1)]], device const float4 *A [[buffer(2)]],
                   device const State &S [[buffer(3)]], constant Prm &P [[buffer(4)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N) || X[i].w != 0.f) return;
    V[i].xyz += S.dt * A[i].xyz;
}
static inline float density_pair(uint q, float3 xi, int4 cii, float3 vi, float devi, device const float4 *X, device const int4 *CI,
                                 device const float4 *V, float kd, float kh, constant Prm &P) {
    float3 d = sep(xi, cii, X[q].xyz, CI[q], P);
    float F = kf(length(d), P), dr = P.mass * dot(vi - V[q].xyz, d) * F;
    if (X[q].w == 0.f) dr += kd * 2.f * ((V[q].w - devi) - kh * d.z) * (-F) / (P.rho0 + V[q].w);
    return dr;
}
kernel void w_density(device const float4 *X [[buffer(0)]], device const float4 *V [[buffer(1)]], device const uint *cstart [[buffer(2)]],
                      device float *DR [[buffer(3)]], constant Prm &P [[buffer(4)]], device const uint *NB [[buffer(5)]],
                      device const int *NN [[buffer(6)]], device const int4 *CI [[buffer(7)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N) || X[i].w != 0.f) return;
    int4 cii = CI[i];
    float3 xi = X[i].xyz, vi = V[i].xyz;
    float devi = V[i].w, dr = 0.f; /* its density less rho0 */
    const float kd = P.delta * P.h * P.c0 * P.mass, kh = P.rho0 * P.g / (P.c0 * P.c0);
    int nn = NN[i];
    if (nn >= 0) { /* the neighbours the force pass found (no particle has moved since) */
        device const uint *list = NB + (size_t)i * uint(P.pad);
        for (int k = 0; k < nn; k++) dr += density_pair(list[k], xi, cii, vi, devi, X, CI, V, kd, kh, P);
        DR[i] = dr;
        return;
    }
    int ys[8], ny = rows_y(cii.y, P, ys);
    int ci = cii.x, ck = cii.z;
    for (int dk = -2; dk <= 2; dk++) {
        int k = ck + dk;
        if (k < 0 || k >= P.gz) continue;
        for (int y = 0; y < ny; y++)
            for (int di = -2; di <= 2; di++) {
                int ii = ci + di;
                if (ii < 0 || ii >= P.gx) continue;
                int c = (k * P.gy + ys[y]) * P.gx + ii;
                for (uint q = cstart[c]; q < cstart[c + 1]; q++) {
                    if (q == i) continue;
                    float3 d = sep(xi, cii, X[q].xyz, CI[q], P);
                    float r2 = dot(d, d);
                    if (r2 >= P.h4 || r2 <= 0.f) continue;
                    float F = kf(sqrt(r2), P);
                    dr += P.mass * dot(vi - V[q].xyz, d) * F;
                    if (X[q].w == 0.f) dr += kd * 2.f * ((V[q].w - devi) - kh * d.z) * (-F) / (P.rho0 + V[q].w);
                }
            }
    }
    DR[i] = dr;
}
/* positions and density deviations are summed with Kahan's compensation (C holds the parts each sum lost): a step of
 * still water moves a particle by far less than a float's resolution at its position, and plain sums drifted */
kernel void w_drift(device float4 *X [[buffer(0)]], device float4 *V [[buffer(1)]], device const float *DR [[buffer(2)]],
                    device State &S [[buffer(3)]], constant Prm &P [[buffer(4)]], device float4 *C [[buffer(5)]],
                    device int4 *CI [[buffer(6)]], uint i [[thread_position_in_grid]]) {
    if (i >= uint(P.N) || X[i].w != 0.f) return;
    float4 x = X[i], v = V[i], c = C[i];
    float4 y = float4(S.dt * v.xyz, S.dt * DR[i]) - c, t = float4(x.xyz, v.w) + y;
    c = (t - float4(x.xyz, v.w)) - y;
    x.xyz = t.xyz, v.w = t.w;
    /* into the neighbouring cell when it leaves its own (subtracting a cell's width is exact there) */
    int4 cc = CI[i];
    float3 cs = float3(P.cx, P.cy, P.cz);
    for (int a = 0; a < 3; a++) {
        while (x[a] >= cs[a]) x[a] -= cs[a], cc[a]++;
        while (x[a] < 0.f) x[a] += cs[a], cc[a]--;
    }
    if (P.periodic) cc.y = (cc.y % P.gy + P.gy) % P.gy;
    C[i] = c, CI[i] = cc;
    if (cc.x < 0 || cc.x >= P.gx || cc.y < 0 || cc.y >= P.gy || cc.z < 0 || cc.z >= P.gz) x.w = 2.f, v.xyz = float3(0);
    if (!isfinite(x.x + x.y + x.z + v.w) || v.w < -0.5f * P.rho0 || v.w > P.rho0) atomic_store_explicit(&S.bad, 1, memory_order_relaxed);
    X[i] = x, V[i] = v;
}
)MSL";

typedef struct {
    float h, h4, aD, Fk, c0, B, mass, rho0, g, alpha, delta, cfl, Ly, cx, cy, cz, Lx, Lz;
    int gx, gy, gz, nc, N, periodic, chunks, pad;
} Prm;
typedef struct {
    float t, dt;
    uint32_t amax, vmax;
    int32_t bad;
    uint32_t steps;
} State;

enum { K_CLEAR, K_COUNT, K_SCAN_CHUNKS, K_SCAN_TOP, K_SCAN_FINAL, K_SCATTER, K_ORDER, K_PRESSURE, K_WALL, K_RATES, K_DT, K_KICK, K_DENSITY, K_DRIFT, K_N };
static const char *KNAMES[K_N] = {"w_clear", "w_count", "w_scan_chunks", "w_scan_top", "w_scan_final", "w_scatter", "w_order", "w_pressure",
                                  "w_wall", "w_rates", "w_dt", "w_kick", "w_density", "w_drift"};

struct WtGpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> k[K_N];
    id<MTLBuffer> NB, NN, X[2], V[2], ID[2], C[2], CI[2], Pr, A, DR, cellOf, counts, wet, sums, cstart, fill, state;
    Prm P;
    int cur, n, nw;
    double lo[3], cs[3], t_offset, rho0;
    char name[64];
};

void wt_gpu_free(WtGpu *G) {
    if (!G) return;
    @autoreleasepool {
        for (int i = 0; i < K_N; i++) G->k[i] = nil;
        G->X[0] = G->X[1] = G->V[0] = G->V[1] = G->ID[0] = G->ID[1] = G->C[0] = G->C[1] = G->CI[0] = G->CI[1] = nil;
        G->Pr = G->A = G->DR = G->cellOf = G->counts = G->wet = G->sums = G->cstart = G->fill = G->state = G->NB = G->NN = nil;
        G->queue = nil, G->dev = nil;
    }
    free(G);
}

WtGpu *wt_gpu_create(Wt *cpu, char *err, size_t errlen) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            snprintf(err, errlen, "water: no Metal GPU on this machine");
            return NULL;
        }
        NSError *e = nil;
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe; /* IEEE single precision, never fast-math */
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        id<MTLLibrary> lib = [dev newLibraryWithSource:KERNELS options:opt error:&e];
        if (!lib) {
            snprintf(err, errlen, "water: Metal kernels did not compile: %s", e.localizedDescription.UTF8String);
            return NULL;
        }
        WtGpu *G = calloc(1, sizeof *G);
        if (!G) return NULL;
        G->dev = dev, G->queue = [dev newCommandQueue];
        for (int i = 0; i < K_N; i++) {
            G->k[i] = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(KNAMES[i])] error:&e];
            if (!G->k[i]) {
                snprintf(err, errlen, "water: kernel %s: %s", KNAMES[i], e.localizedDescription.UTF8String);
                wt_gpu_free(G);
                return NULL;
            }
        }
        wt_prepare(cpu);
        WtParams p;
        wt_params(cpu, &p);
        G->n = wt_count(cpu), G->nw = wt_wall_count(cpu);
        int N = G->n + G->nw, nc = p.grid[0] * p.grid[1] * p.grid[2];
        memcpy(G->lo, p.lo, sizeof G->lo), G->rho0 = p.rho0;
        memcpy(G->cs, p.cell, sizeof G->cs);
        G->t_offset = wt_time(cpu);
        const double aD = 21.0 / (16 * M_PI * p.h * p.h * p.h);
        G->P = (Prm){(float)p.h, (float)(4 * p.h * p.h), (float)aD, (float)(-5 * aD / (p.h * p.h)), (float)p.c0, (float)p.B, (float)p.mass,
                     (float)p.rho0, (float)p.g, (float)p.alpha, (float)p.delta, (float)p.cfl, (float)p.Ly, (float)p.cell[0], (float)p.cell[1],
                     (float)p.cell[2], (float)(p.hi[0] - p.lo[0]), (float)(p.hi[2] - p.lo[2]), p.grid[0], p.grid[1], p.grid[2], nc, N,
                     p.periodic_y ? 1 : 0, (nc + 1 + 255) / 256, 0};
        { /* a neighbour list long enough for the support's sphere and half again */
            double rs = 2 * p.h / (p.mass > 0 ? cbrt(p.mass / p.rho0) : p.h);
            G->P.pad = (int)ceil(4.19 * rs * rs * rs * 1.5) + 16;
        }
        MTLResourceOptions sh = MTLResourceStorageModeShared, pr = MTLResourceStorageModePrivate;
        size_t Nn = (size_t)(N > 0 ? N : 1);
        for (int b = 0; b < 2; b++) {
            G->X[b] = [dev newBufferWithLength:Nn * 16 options:sh];
            G->V[b] = [dev newBufferWithLength:Nn * 16 options:sh];
            G->ID[b] = [dev newBufferWithLength:Nn * 4 options:sh];
            G->C[b] = [dev newBufferWithLength:Nn * 16 options:sh];
            G->CI[b] = [dev newBufferWithLength:Nn * 16 options:sh];
            if (G->C[b]) memset(G->C[b].contents, 0, Nn * 16);
        }
        G->Pr = [dev newBufferWithLength:Nn * 4 options:pr], G->A = [dev newBufferWithLength:Nn * 16 options:pr];
        G->DR = [dev newBufferWithLength:Nn * 4 options:pr], G->cellOf = [dev newBufferWithLength:Nn * 4 options:pr];
        G->counts = [dev newBufferWithLength:(size_t)(nc + 2) * 4 options:pr], G->wet = [dev newBufferWithLength:(size_t)(nc + 2) * 4 options:pr];
        G->sums = [dev newBufferWithLength:(size_t)G->P.chunks * 4 options:pr], G->cstart = [dev newBufferWithLength:(size_t)(nc + 2) * 4 options:pr];
        G->fill = [dev newBufferWithLength:(size_t)(nc + 2) * 4 options:pr], G->state = [dev newBufferWithLength:sizeof(State) options:sh];
        G->NB = [dev newBufferWithLength:Nn * (size_t)G->P.pad * 4 options:pr], G->NN = [dev newBufferWithLength:Nn * 4 options:pr];
        if (!G->queue || !G->X[1] || !G->V[1] || !G->ID[1] || !G->Pr || !G->A || !G->DR || !G->cellOf || !G->counts || !G->wet || !G->sums ||
            !G->cstart || !G->fill || !G->state || !G->NB || !G->NN) {
            snprintf(err, errlen, "water: the GPU could not hold %d particles and %d cells", N, nc);
            wt_gpu_free(G);
            return NULL;
        }
        float *X = G->X[0].contents, *V = G->V[0].contents;
        int *ID = G->ID[0].contents, *CIp = G->CI[0].contents;
        const double *x = wt_x(cpu), *v = wt_v(cpu), *rho = wt_rho(cpu), *wx = wt_wall_x(cpu);
        const unsigned char *out = wt_out_rw(cpu);
        for (int i = 0; i < G->n; i++) {
            for (int a = 0; a < 3; a++) {
                double r = x[3 * i + a] - p.lo[a];
                int c = (int)floor(r / p.cell[a]);
                CIp[4 * i + a] = c, X[4 * i + a] = (float)(r - c * p.cell[a]), V[4 * i + a] = (float)v[3 * i + a];
            }
            CIp[4 * i + 3] = 0;
            X[4 * i + 3] = out[i] ? 2.f : 0.f, V[4 * i + 3] = (float)(rho[i] - p.rho0), ID[i] = i;
        }
        for (int k = 0; k < G->nw; k++) {
            int i = G->n + k;
            for (int a = 0; a < 3; a++) {
                double r = wx[3 * k + a] - p.lo[a];
                int c = (int)floor(r / p.cell[a]);
                CIp[4 * i + a] = c, X[4 * i + a] = (float)(r - c * p.cell[a]), V[4 * i + a] = 0;
            }
            CIp[4 * i + 3] = 0;
            X[4 * i + 3] = 1.f, V[4 * i + 3] = 0.f, ID[i] = -1 - k;
        }
        memset(G->state.contents, 0, sizeof(State));
        snprintf(G->name, sizeof G->name, "%s", dev.name.UTF8String);
        return G;
    }
}

static double prof_t[K_N];
static int prof_on = -1;
static void disp(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> ps, NSUInteger n) {
    NSUInteger tg = ps.maxTotalThreadsPerThreadgroup < 256 ? ps.maxTotalThreadsPerThreadgroup : 256;
    [enc setComputePipelineState:ps];
    [enc dispatchThreads:MTLSizeMake(n > 0 ? n : 1, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
    [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
}
/* WATER_PROFILE=1: each kernel in a command buffer of its own, its GPU time summed and printed at the end */
#define DISP(K, n)                                                                                                     \
    do {                                                                                                               \
        disp(enc, G->k[K], n);                                                                                         \
        if (prof_on) {                                                                                                 \
            [enc endEncoding];                                                                                         \
            [cb commit];                                                                                               \
            [cb waitUntilCompleted];                                                                                   \
            prof_t[K] += cb.GPUEndTime - cb.GPUStartTime;                                                              \
            cb = [G->queue commandBuffer];                                                                             \
            enc = [cb computeCommandEncoder];                                                                          \
        }                                                                                                              \
    } while (0)
static void prof_report(void) {
    double tot = 0;
    for (int k = 0; k < K_N; k++) tot += prof_t[k];
    for (int k = 0; k < K_N; k++) fprintf(stderr, "water profile: %-14s %8.3f s  %5.1f %%\n", KNAMES[k], prof_t[k], 100 * prof_t[k] / (tot > 0 ? tot : 1));
}

bool wt_gpu_steps(WtGpu *G, int nsteps) {
    @autoreleasepool {
        if (prof_on < 0) {
            prof_on = getenv("WATER_PROFILE") != NULL;
            if (prof_on) atexit(prof_report);
        }
        const NSUInteger N = (NSUInteger)G->P.N, nc1 = (NSUInteger)G->P.nc + 1, ch = (NSUInteger)G->P.chunks;
        id<MTLCommandBuffer> cb = [G->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        for (int s = 0; s < nsteps; s++) {
            int a = G->cur, b = 1 - G->cur;
            /* sort into cell order */
            [enc setBuffer:G->counts offset:0 atIndex:0], [enc setBuffer:G->wet offset:0 atIndex:1], [enc setBytes:&G->P length:sizeof G->P atIndex:2];
            DISP(K_CLEAR, nc1);
            [enc setBuffer:G->X[a] offset:0 atIndex:0], [enc setBuffer:G->cellOf offset:0 atIndex:1], [enc setBuffer:G->counts offset:0 atIndex:2];
            [enc setBuffer:G->wet offset:0 atIndex:3], [enc setBytes:&G->P length:sizeof G->P atIndex:4];
            [enc setBuffer:G->CI[a] offset:0 atIndex:5];
            DISP(K_COUNT, N);
            [enc setBuffer:G->counts offset:0 atIndex:0], [enc setBuffer:G->sums offset:0 atIndex:1], [enc setBytes:&G->P length:sizeof G->P atIndex:2];
            DISP(K_SCAN_CHUNKS, ch);
            [enc setBuffer:G->sums offset:0 atIndex:0], [enc setBytes:&G->P length:sizeof G->P atIndex:1];
            DISP(K_SCAN_TOP, 1);
            [enc setBuffer:G->counts offset:0 atIndex:0], [enc setBuffer:G->sums offset:0 atIndex:1], [enc setBuffer:G->cstart offset:0 atIndex:2];
            [enc setBuffer:G->fill offset:0 atIndex:3], [enc setBytes:&G->P length:sizeof G->P atIndex:4];
            DISP(K_SCAN_FINAL, ch);
            [enc setBuffer:G->X[a] offset:0 atIndex:0], [enc setBuffer:G->V[a] offset:0 atIndex:1], [enc setBuffer:G->ID[a] offset:0 atIndex:2];
            [enc setBuffer:G->cellOf offset:0 atIndex:3], [enc setBuffer:G->fill offset:0 atIndex:4], [enc setBuffer:G->X[b] offset:0 atIndex:5];
            [enc setBuffer:G->V[b] offset:0 atIndex:6], [enc setBuffer:G->ID[b] offset:0 atIndex:7], [enc setBytes:&G->P length:sizeof G->P atIndex:8];
            [enc setBuffer:G->C[a] offset:0 atIndex:9], [enc setBuffer:G->C[b] offset:0 atIndex:10];
            [enc setBuffer:G->CI[a] offset:0 atIndex:11], [enc setBuffer:G->CI[b] offset:0 atIndex:12];
            DISP(K_SCATTER, N);
            G->cur = b;
            id<MTLBuffer> X = G->X[b], V = G->V[b];
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->ID[b] offset:0 atIndex:2];
            [enc setBuffer:G->C[b] offset:0 atIndex:3], [enc setBuffer:G->CI[b] offset:0 atIndex:4], [enc setBuffer:G->cstart offset:0 atIndex:5];
            [enc setBytes:&G->P length:sizeof G->P atIndex:6];
            DISP(K_ORDER, nc1);
            /* pressures, the walls', the rates, the step, the kick, the density, the drift */
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->Pr offset:0 atIndex:2];
            [enc setBytes:&G->P length:sizeof G->P atIndex:3];
            DISP(K_PRESSURE, N);
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->Pr offset:0 atIndex:2];
            [enc setBuffer:G->cstart offset:0 atIndex:3], [enc setBuffer:G->wet offset:0 atIndex:4], [enc setBytes:&G->P length:sizeof G->P atIndex:5];
            [enc setBuffer:G->CI[b] offset:0 atIndex:6];
            DISP(K_WALL, N);
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->Pr offset:0 atIndex:2];
            [enc setBuffer:G->cstart offset:0 atIndex:3], [enc setBuffer:G->A offset:0 atIndex:4], [enc setBuffer:G->state offset:0 atIndex:5];
            [enc setBytes:&G->P length:sizeof G->P atIndex:6];
            [enc setBuffer:G->NB offset:0 atIndex:7], [enc setBuffer:G->NN offset:0 atIndex:8], [enc setBuffer:G->CI[b] offset:0 atIndex:9];
            DISP(K_RATES, N);
            [enc setBuffer:G->state offset:0 atIndex:0], [enc setBytes:&G->P length:sizeof G->P atIndex:1];
            DISP(K_DT, 1);
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->A offset:0 atIndex:2];
            [enc setBuffer:G->state offset:0 atIndex:3], [enc setBytes:&G->P length:sizeof G->P atIndex:4];
            DISP(K_KICK, N);
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->cstart offset:0 atIndex:2];
            [enc setBuffer:G->DR offset:0 atIndex:3], [enc setBytes:&G->P length:sizeof G->P atIndex:4];
            [enc setBuffer:G->NB offset:0 atIndex:5], [enc setBuffer:G->NN offset:0 atIndex:6], [enc setBuffer:G->CI[b] offset:0 atIndex:7];
            DISP(K_DENSITY, N);
            [enc setBuffer:X offset:0 atIndex:0], [enc setBuffer:V offset:0 atIndex:1], [enc setBuffer:G->DR offset:0 atIndex:2];
            [enc setBuffer:G->state offset:0 atIndex:3], [enc setBytes:&G->P length:sizeof G->P atIndex:4];
            [enc setBuffer:G->C[b] offset:0 atIndex:5], [enc setBuffer:G->CI[b] offset:0 atIndex:6];
            DISP(K_DRIFT, N);
        }
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const State *S = G->state.contents;
        return S->bad == 0 && cb.status == MTLCommandBufferStatusCompleted;
    }
}

double wt_gpu_time(WtGpu *G) { return G->t_offset + ((const State *)G->state.contents)->t; }
long wt_gpu_steps_done(const WtGpu *G) { return (long)((const State *)G->state.contents)->steps; }
const char *wt_gpu_name(const WtGpu *G) { return G->name; }

bool wt_gpu_run_to(WtGpu *G, double until) {
    while (wt_gpu_time(G) < until) {
        const State *S = G->state.contents;
        double dt = S->dt > 0 ? S->dt : 1e-4;
        int n = (int)fmin(200, fmax(1, 0.9 * (until - wt_gpu_time(G)) / dt));
        if (!wt_gpu_steps(G, n)) return false;
    }
    return true;
}

void wt_gpu_sync(WtGpu *G, Wt *cpu) {
    const float *X = G->X[G->cur].contents, *V = G->V[G->cur].contents, *Cc = G->C[G->cur].contents;
    const int *ID = G->ID[G->cur].contents, *CIp = G->CI[G->cur].contents;
    double *x = wt_x_rw(cpu), *v = wt_v_rw(cpu), *rho = wt_rho_rw(cpu);
    unsigned char *out = wt_out_rw(cpu);
    for (int i = 0; i < G->P.N; i++) {
        int id = ID[i];
        if (id < 0) continue;
        for (int a = 0; a < 3; a++)
            x[3 * id + a] = G->lo[a] + CIp[4 * i + a] * G->cs[a] + ((double)X[4 * i + a] - Cc[4 * i + a]), v[3 * id + a] = V[4 * i + a];
        rho[id] = G->rho0 + ((double)V[4 * i + 3] - Cc[4 * i + 3]), out[id] = X[4 * i + 3] > 1.5f;
    }
    wt_set_clock(cpu, wt_gpu_time(G), wt_gpu_steps_done(G));
}
