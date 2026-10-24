// specimen.cpp - a voxelised test specimen compressed between rigid platens (see specimen.h; GOALS.md G23 step 5c)
#include "specimen.h"

extern "C" {
#include "../fem/solid.h"
#include "../threads.h"
}

#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace {

int find(std::vector<int> &p, int a) {
    while (p[a] != a) a = p[a] = p[p[a]];
    return a;
}

} // namespace

bool specimen_compress(const double *phi, int nx, int ny, int nz, double h, double nu, bool bonded, long max_equations,
                       SpecimenResult *out, std::string &why) {
    auto t0 = std::chrono::steady_clock::now();
    *out = SpecimenResult{};
    if (!phi || nx < 1 || ny < 1 || nz < 1 || !(h > 0) || !(nu > -1 && nu < 0.5)) {
        why = "a specimen needs voxels, a positive voxel size and -1 < nu < 0.5";
        return false;
    }
    auto V = [&](int i, int j, int k) { return ((size_t)k * ny + j) * nx + i; };
    size_t nv = (size_t)nx * ny * nz;
    /* the largest face-connected piece */
    std::vector<int> par(nv);
    std::iota(par.begin(), par.end(), 0);
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                size_t v = V(i, j, k);
                if (!(phi[v] > 0)) continue;
                size_t nb[3] = {i + 1 < nx ? V(i + 1, j, k) : (size_t)-1, j + 1 < ny ? V(i, j + 1, k) : (size_t)-1, k + 1 < nz ? V(i, j, k + 1) : (size_t)-1};
                for (size_t q : nb)
                    if (q != (size_t)-1 && phi[q] > 0) {
                        int a = find(par, (int)v), b = find(par, (int)q);
                        if (a != b) par[a] = b;
                    }
            }
    std::vector<int> size(nv, 0);
    int best = -1;
    long occupied = 0;
    for (size_t v = 0; v < nv; v++)
        if (phi[v] > 0) {
            int r = find(par, (int)v);
            occupied++;
            if (++size[r] > (best < 0 ? 0 : size[best])) best = r;
        }
    if (best < 0) {
        why = "the specimen holds no solid";
        return false;
    }
    /* elements and nodes of that piece */
    size_t N1x = nx + 1, N1y = ny + 1, ng = N1x * N1y * (size_t)(nz + 1);
    auto G = [&](int i, int j, int k) { return ((size_t)k * N1y + j) * N1x + i; };
    std::vector<int> node_of(ng, -1), conn;
    std::vector<double> xyz, scale;
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                size_t v = V(i, j, k);
                if (!(phi[v] > 0) || find(par, (int)v) != best) continue;
                for (const auto &c : corner) {
                    size_t g = G(i + c[0], j + c[1], k + c[2]);
                    if (node_of[g] < 0) {
                        node_of[g] = (int)(xyz.size() / 3);
                        xyz.insert(xyz.end(), {(i + c[0]) * h, (j + c[1]) * h, (k + c[2]) * h});
                    }
                    conn.push_back(node_of[g]);
                }
                scale.push_back(std::min(1.0, phi[v]));
            }
    int nn = (int)(xyz.size() / 3), ne = (int)scale.size();
    out->elements = ne, out->dropped = occupied - ne, out->nodes = nn;
    if ((long)3 * nn > max_equations) {
        why = "the specimen would have " + std::to_string(3L * nn) + " unknowns, more than " + std::to_string(max_equations) +
              " this machine is given: fewer voxels per cell";
        return false;
    }
    /* the platens: z held at both faces (and x, y too when bonded); a frictionless specimen is kept from sliding and
     * turning by one bottom node held in x and y, and another, furthest from it along x, held in y */
    std::vector<unsigned char> fixed(3 * (size_t)nn, 0);
    int pin = -1, turn = -1;
    double cx = 0.5 * nx * h, cy = 0.5 * ny * h, bestd = 1e300;
    for (int q = 0; q < nn; q++) {
        double z = xyz[3 * (size_t)q + 2];
        bool bottom = z < 0.5 * h, top = z > (nz - 0.5) * h;
        if (!bottom && !top) continue;
        fixed[3 * (size_t)q + 2] = 1;
        if (bonded) fixed[3 * (size_t)q] = fixed[3 * (size_t)q + 1] = 1;
        if (bottom) {
            double d = std::hypot(xyz[3 * (size_t)q] - cx, xyz[3 * (size_t)q + 1] - cy);
            if (d < bestd) bestd = d, pin = q;
        }
    }
    if (!bonded) {
        if (pin < 0) {
            why = "the kept piece does not reach the bottom platen";
            return false;
        }
        double far = -1;
        for (int q = 0; q < nn; q++) {
            if (xyz[3 * (size_t)q + 2] > 0.5 * h || q == pin) continue;
            double d = std::fabs(xyz[3 * (size_t)q] - xyz[3 * (size_t)pin]);
            if (d > far) far = d, turn = q;
        }
        fixed[3 * (size_t)pin] = fixed[3 * (size_t)pin + 1] = 1;
        if (turn >= 0) fixed[3 * (size_t)turn + 1] = 1;
    }
    /* the strain: a unit initial strain along z between the held faces */
    std::vector<double> eps0(6 * (size_t)ne, 0);
    for (int e = 0; e < ne; e++) eps0[6 * (size_t)e + 2] = 1;
    SolidMaterial mat = {1.0, nu, 0};
    HexModel model = {};
    model.nnodes = nn, model.nelems = ne, model.xyz = xyz.data(), model.conn = conn.data(), model.nmat = 1, model.mat = &mat;
    model.formulation = HEX8_INCOMPATIBLE, model.elem_scale = scale.data();
    SolidLoads loads = {};
    loads.fixed = fixed.data(), loads.eps0 = eps0.data();
    ThreadPool *pool = pool_create(cpu_perf_count());
    SolidOptions opt = {};
    opt.solver = SOLID_SOLVER_PCG, opt.pcg_tol = 1e-10, opt.pool = pool;
    SolidResult res = {};
    char err[400] = "";
    bool ok = solid_solve(&model, &loads, &opt, &res, err, sizeof err);
    if (pool) pool_destroy(pool);
    if (!ok) {
        solid_result_free(&res);
        why = err;
        return false;
    }
    /* the force the top platen holds, over the whole section, per unit strain */
    double force = 0;
    for (int q = 0; q < nn; q++)
        if (xyz[3 * (size_t)q + 2] > (nz - 0.5) * h) force += res.reaction[3 * (size_t)q + 2];
    out->modulus_over_Es = std::fabs(force) / ((double)nx * h * ny * h);
    out->equations = res.neq, out->iterations = res.stats.iterations, out->residual = res.stats.true_residual;
    solid_result_free(&res);
    out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}
