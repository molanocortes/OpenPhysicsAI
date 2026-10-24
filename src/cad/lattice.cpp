// lattice.cpp - a printed part's skin and core (see lattice.h; GOALS.md G23 step 3)
#include "lattice.h"

#include "cadmesh.h"
#include "specimen.h"

extern "C" {
#include "../mech/support_cell.h"
}

#include <manifold/manifold.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace cadmesh;

const double PI = 3.14159265358979323846;

/* ---- the part's signed distance on a grid (positive inside) ---- */

/* the distance from p to a triangle (Ericson, Real-Time Collision Detection, 5.1.5) */
double dist_tri(V3 p, const Tri &t) {
    V3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return norm(ap);
    V3 bp = p - t.b;
    double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return norm(bp);
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return norm(p - (t.a + ab * (d1 / (d1 - d3))));
    V3 cp = p - t.c;
    double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return norm(cp);
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return norm(p - (t.a + ac * (d2 / (d2 - d6))));
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return norm(p - (t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    double den = 1 / (va + vb + vc), v = vb * den, w = vc * den;
    return norm(p - (t.a + ab * v + ac * w));
}

struct Grid {
    V3 lo;
    double h = 1, band = 1;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> d;
    size_t idx(int i, int j, int k) const { return ((size_t)k * ny + j) * nx + i; }
    double sample(V3 p) const { /* trilinear; beyond the grid: outside, far */
        double fx = (p.x - lo.x) / h, fy = (p.y - lo.y) / h, fz = (p.z - lo.z) / h;
        if (fx < 0 || fy < 0 || fz < 0 || fx > nx - 1 || fy > ny - 1 || fz > nz - 1) return -band;
        int i = std::min((int)fx, nx - 2), j = std::min((int)fy, ny - 2), k = std::min((int)fz, nz - 2);
        double u = fx - i, v = fy - j, w = fz - k;
        auto D = [&](int a, int b, int c) { return (double)d[idx(i + a, j + b, k + c)]; };
        return (1 - w) * ((1 - v) * ((1 - u) * D(0, 0, 0) + u * D(1, 0, 0)) + v * ((1 - u) * D(0, 1, 0) + u * D(1, 1, 0))) +
               w * ((1 - v) * ((1 - u) * D(0, 0, 1) + u * D(1, 0, 1)) + v * ((1 - u) * D(0, 1, 1) + u * D(1, 1, 1)));
    }
};

Grid part_sdf(const std::vector<Tri> &tris, double h, double band, V3 lo, V3 hi) {
    Grid g;
    g.lo = lo, g.h = h, g.band = band;
    g.nx = (int)std::ceil((hi.x - lo.x) / h) + 1, g.ny = (int)std::ceil((hi.y - lo.y) / h) + 1, g.nz = (int)std::ceil((hi.z - lo.z) / h) + 1;
    double points = (double)g.nx * g.ny * g.nz;
    if (points > 6e7) refuse("the grid would hold " + std::to_string((long long)points) + " points: give a coarser resolution_mm");
    g.d.assign((size_t)points, (float)band);
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    /* the distance within the band under and over the surface, exact: every triangle against the points near it,
     * each thread its own slabs of z so that no two write the same point */
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < nt; w++)
        pool.emplace_back([&, w] {
            for (const Tri &t : tris) {
                double x0 = std::min({t.a.x, t.b.x, t.c.x}) - band, x1 = std::max({t.a.x, t.b.x, t.c.x}) + band;
                double y0 = std::min({t.a.y, t.b.y, t.c.y}) - band, y1 = std::max({t.a.y, t.b.y, t.c.y}) + band;
                double z0 = std::min({t.a.z, t.b.z, t.c.z}) - band, z1 = std::max({t.a.z, t.b.z, t.c.z}) + band;
                int i0 = std::max(0, (int)std::floor((x0 - lo.x) / h)), i1 = std::min(g.nx - 1, (int)std::ceil((x1 - lo.x) / h));
                int j0 = std::max(0, (int)std::floor((y0 - lo.y) / h)), j1 = std::min(g.ny - 1, (int)std::ceil((y1 - lo.y) / h));
                int k0 = std::max(0, (int)std::floor((z0 - lo.z) / h)), k1 = std::min(g.nz - 1, (int)std::ceil((z1 - lo.z) / h));
                for (int k = k0; k <= k1; k++) {
                    if ((unsigned)k % nt != w) continue;
                    for (int j = j0; j <= j1; j++)
                        for (int i = i0; i <= i1; i++) {
                            float dd = (float)dist_tri({lo.x + i * h, lo.y + j * h, lo.z + k * h}, t);
                            float &cur = g.d[g.idx(i, j, k)];
                            if (dd < cur) cur = dd;
                        }
                }
            }
        });
    for (auto &t : pool) t.join();
    /* the sign: along each column (x, y) a ray up z, its crossings counted; the ray is moved off the grid's lines by a
     * tiny irregular amount so that it never runs exactly through an edge or a vertex */
    const double ex = h * 1.37e-7 * PI, ey = h * 0.91e-7 * 2.718281828;
    double bucket = 8 * h;
    int bx = (int)std::ceil((hi.x - lo.x) / bucket) + 1, by = (int)std::ceil((hi.y - lo.y) / bucket) + 1;
    std::vector<std::vector<int>> buckets((size_t)bx * by);
    for (size_t n = 0; n < tris.size(); n++) {
        const Tri &t = tris[n];
        int a0 = std::max(0, (int)std::floor((std::min({t.a.x, t.b.x, t.c.x}) - lo.x) / bucket));
        int a1 = std::min(bx - 1, (int)std::floor((std::max({t.a.x, t.b.x, t.c.x}) - lo.x) / bucket));
        int b0 = std::max(0, (int)std::floor((std::min({t.a.y, t.b.y, t.c.y}) - lo.y) / bucket));
        int b1 = std::min(by - 1, (int)std::floor((std::max({t.a.y, t.b.y, t.c.y}) - lo.y) / bucket));
        for (int b = b0; b <= b1; b++)
            for (int a = a0; a <= a1; a++) buckets[(size_t)b * bx + a].push_back((int)n);
    }
    std::vector<std::thread> pool2;
    for (unsigned w = 0; w < nt; w++)
        pool2.emplace_back([&, w] {
            std::vector<double> zs;
            for (int j = 0; j < g.ny; j++) {
                if ((unsigned)j % nt != w) continue;
                for (int i = 0; i < g.nx; i++) {
                    double x = lo.x + i * h + ex, y = lo.y + j * h + ey;
                    const auto &cand = buckets[(size_t)std::min(by - 1, (int)((y - lo.y) / bucket)) * bx + std::min(bx - 1, (int)((x - lo.x) / bucket))];
                    zs.clear();
                    for (int n : cand) {
                        const Tri &t = tris[n];
                        double w0 = (t.b.x - t.a.x) * (y - t.a.y) - (t.b.y - t.a.y) * (x - t.a.x);
                        double w1 = (t.c.x - t.b.x) * (y - t.b.y) - (t.c.y - t.b.y) * (x - t.b.x);
                        double w2 = (t.a.x - t.c.x) * (y - t.c.y) - (t.a.y - t.c.y) * (x - t.c.x);
                        bool in = (w0 > 0 && w1 > 0 && w2 > 0) || (w0 < 0 && w1 < 0 && w2 < 0);
                        if (!in) continue;
                        double s = w0 + w1 + w2; /* barycentric weights of c, a, b */
                        zs.push_back((w1 * t.a.z + w2 * t.b.z + w0 * t.c.z) / s);
                    }
                    std::sort(zs.begin(), zs.end());
                    size_t c = 0;
                    for (int k = 0; k < g.nz; k++) {
                        double z = lo.z + k * h;
                        while (c < zs.size() && zs[c] < z) c++;
                        if (c % 2 == 1) g.d[g.idx(i, j, k)] = std::fabs(g.d[g.idx(i, j, k)]);
                        else g.d[g.idx(i, j, k)] = -std::fabs(g.d[g.idx(i, j, k)]);
                    }
                }
            }
        });
    for (auto &t : pool2) t.join();
    return g;
}

/* ---- the lattices: a signed field positive inside the solid, close to a distance near its surface ---- */

enum Kind { GYROID, SCHWARZ_P, DIAMOND, BCC, OCTET, NONE, SOLID };

struct Lattice {
    Kind kind = GYROID;
    bool sheet = true, band = false; /* band: the sheet most papers use, |G| < level (Li et al. 2019's G^2 < t^2) */
    double cell = 6, wall = 0.6, level = 0;
    V3 origin{0, 0, 0};
    std::vector<std::array<V3, 2>> struts; /* in one cell, [0, cell)^3 */

    /* the surface's function G and its gradient (per mm) */
    double tpms_g(V3 p, V3 *gv) const {
        double k = 2 * PI / cell, x = k * (p.x - origin.x), y = k * (p.y - origin.y), z = k * (p.z - origin.z);
        double sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y), sz = std::sin(z), cz = std::cos(z);
        double g, gx, gy, gz;
        if (kind == GYROID) {
            g = sx * cy + sy * cz + sz * cx;
            gx = cx * cy - sz * sx, gy = -sx * sy + cy * cz, gz = -sy * sz + cz * cx;
        } else if (kind == SCHWARZ_P) {
            g = cx + cy + cz;
            gx = -sx, gy = -sy, gz = -sz;
        } else { /* diamond */
            g = sx * sy * sz + sx * cy * cz + cx * sy * cz + cx * cy * sz;
            gx = cx * sy * sz + cx * cy * cz - sx * sy * cz - sx * cy * sz;
            gy = sx * cy * sz - sx * sy * cz + cx * cy * cz - cx * sy * sz;
            gz = sx * sy * cz - sx * cy * sz - cx * sy * sz + cx * cy * cz;
        }
        *gv = V3{k * gx, k * gy, k * gz};
        return g;
    }

    /* the distance to the surface G = level, with G - level's sign: the point projected onto the surface by two Newton
     * steps. G / |grad G| at the point alone overestimates it off the surface, which made a gyroid sheet asked for at
     * 0.6 mm come out 0.5785 mm thick, the same everywhere (measured 2026-10-08, tools/cadlatticetest.py L2) */
    double surface_distance(V3 p, double lvl) const {
        V3 gv, q = p;
        double g0 = tpms_g(p, &gv) - lvl, kk = 2 * PI / cell;
        double floor2 = (0.2 * kk) * (0.2 * kk);
        double gg = std::max(dot(gv, gv), floor2);
        if (std::fabs(g0) / std::sqrt(gg) > 0.5 * cell) return g0 / std::sqrt(gg); /* far from the surface: as before */
        for (int it = 0; it < 2; it++) {
            double g = tpms_g(q, &gv) - lvl;
            gg = std::max(dot(gv, gv), floor2);
            q = q - gv * (g / gg);
        }
        double d = norm(p - q);
        return g0 < 0 ? -d : d;
    }

    double field(V3 p) const {
        if (kind == NONE) return -1e30;
        if (kind == SOLID) return 1e30;
        if (kind == BCC || kind == OCTET) { /* the nearest strut in the point's own cell: both lattices are mirror
                                             * symmetric across the cell's faces, so it is the nearest of all */
            V3 q = p - origin;
            q = {q.x - cell * std::floor(q.x / cell), q.y - cell * std::floor(q.y / cell), q.z - cell * std::floor(q.z / cell)};
            double best = 1e30;
            for (const auto &s : struts) {
                V3 ab = s[1] - s[0];
                double t = std::clamp(dot(q - s[0], ab) / dot(ab, ab), 0.0, 1.0);
                best = std::min(best, norm(q - (s[0] + ab * t)));
            }
            return wall / 2 - best;
        }
        if (band) return std::min(-surface_distance(p, level), surface_distance(p, -level));
        if (sheet) return wall / 2 - std::fabs(surface_distance(p, 0));
        return -surface_distance(p, level); /* the network where G < level */
    }

    void make_struts() {
        struts.clear();
        double a = cell;
        if (kind == BCC) { /* the four body diagonals */
            struts.push_back({V3{0, 0, 0}, V3{a, a, a}});
            struts.push_back({V3{a, 0, 0}, V3{0, a, a}});
            struts.push_back({V3{0, a, 0}, V3{a, 0, a}});
            struts.push_back({V3{0, 0, a}, V3{a, a, 0}});
        } else if (kind == OCTET) { /* the face-centred cubic nodes joined to their nearest neighbours (a / sqrt 2) */
            std::vector<V3> nodes;
            for (int i = 0; i <= 1; i++)
                for (int j = 0; j <= 1; j++)
                    for (int k = 0; k <= 1; k++) nodes.push_back({i * a, j * a, k * a});
            double hf = a / 2;
            for (double c : {0.0, a}) {
                nodes.push_back({c, hf, hf}), nodes.push_back({hf, c, hf}), nodes.push_back({hf, hf, c});
            }
            double nn = a / std::sqrt(2.0);
            for (size_t i = 0; i < nodes.size(); i++)
                for (size_t j = i + 1; j < nodes.size(); j++)
                    if (std::fabs(norm(nodes[j] - nodes[i]) - nn) < 1e-9 * a) struts.push_back({nodes[i], nodes[j]});
        }
    }

    /* the solid fraction of one periodic cell, counted on an n^3 grid of cell centres */
    double cell_fraction(int n = 48) const {
        long in = 0;
        for (int k = 0; k < n; k++)
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    V3 p = origin + V3{(i + 0.5) * cell / n, (j + 0.5) * cell / n, (k + 0.5) * cell / n};
                    if (field(p) > 0) in++;
                }
        return (double)in / ((double)n * n * n);
    }
};

/* the wall (sheet, strut) or the level (network) that gives a solid fraction on one cell, by bisection */
void solve_fraction(Lattice &L, double want, int n = 48) {
    bool level = !(L.kind == BCC || L.kind == OCTET) && (!L.sheet || L.band); /* a network or a band: its level */
    double lo = level ? (L.band ? 0 : -3.5) : 1e-6 * L.cell, hi = level ? 3.5 : L.cell;
    for (int it = 0; it < 50; it++) {
        double mid = 0.5 * (lo + hi);
        (level ? L.level : L.wall) = mid;
        double f = L.cell_fraction(n);
        if (f < want) lo = mid;
        else hi = mid;
    }
    (level ? L.level : L.wall) = 0.5 * (lo + hi);
}

/* closed and manifold as an STL reader sees it: vertices joined where their 32-bit floats agree, every edge then in
 * exactly two triangles, once each way */
bool closed_manifold(const manifold::MeshGL64 &m) {
    std::map<std::array<float, 3>, uint64_t> id;
    size_t np = m.numProp, nv = m.vertProperties.size() / np;
    std::vector<uint64_t> as(nv);
    for (size_t i = 0; i < nv; i++)
        as[i] = id.emplace(std::array<float, 3>{(float)m.vertProperties[i * np], (float)m.vertProperties[i * np + 1],
                                                (float)m.vertProperties[i * np + 2]}, id.size()).first->second;
    std::map<std::pair<uint64_t, uint64_t>, int> directed;
    size_t nt = m.triVerts.size() / 3;
    for (size_t t = 0; t < nt; t++) {
        uint64_t v[3] = {as[m.triVerts[3 * t]], as[m.triVerts[3 * t + 1]], as[m.triVerts[3 * t + 2]]};
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) return false;
        for (int k = 0; k < 3; k++) directed[{v[k], v[(k + 1) % 3]}]++;
    }
    if (directed.empty()) return false;
    for (auto &e : directed)
        if (e.second != 1 || directed.count({e.first.second, e.first.first}) != 1) return false;
    return true;
}

/* vertices that share one position once written as 32-bit floats (as an STL stores them): how many groups, and how
 * many of those are the same point exactly (where two pieces of the solid touch) */
std::pair<long, long> float_coincident(const manifold::MeshGL64 &m) {
    std::map<std::array<float, 3>, std::vector<size_t>> at;
    size_t np = m.numProp, nv = m.vertProperties.size() / np;
    for (size_t i = 0; i < nv; i++)
        at[{(float)m.vertProperties[i * np], (float)m.vertProperties[i * np + 1], (float)m.vertProperties[i * np + 2]}].push_back(i);
    long groups = 0, exact = 0;
    for (auto &g : at) {
        if (g.second.size() < 2) continue;
        groups++;
        size_t a0 = g.second[0], a1 = g.second[1];
        if (m.vertProperties[a0 * np] == m.vertProperties[a1 * np] && m.vertProperties[a0 * np + 1] == m.vertProperties[a1 * np + 1] &&
            m.vertProperties[a0 * np + 2] == m.vertProperties[a1 * np + 2])
            exact++;
    }
    return {groups, exact};
}

/* where two pieces of the solid touch at a point, Manifold keeps one copy of the vertex for each piece, at the same
 * position: an STL cannot say so, and any reader joins them into a defect. Each copy is moved eps into its own piece
 * (against its outward normal), so that the copies stay apart once written as floats (measured 2026-10-08: 1459 such
 * points in a plate with a gyroid core, all of them exact copies). Returns how many vertices were moved. */
long unpinch(manifold::MeshGL64 &m, double eps) {
    size_t np = m.numProp, nv = m.vertProperties.size() / np;
    std::map<std::array<float, 3>, std::vector<size_t>> at;
    for (size_t i = 0; i < nv; i++)
        at[{(float)m.vertProperties[i * np], (float)m.vertProperties[i * np + 1], (float)m.vertProperties[i * np + 2]}].push_back(i);
    std::vector<char> moved(nv, 0);
    long count = 0;
    for (auto &g : at)
        if (g.second.size() > 1)
            for (size_t v : g.second) moved[v] = 1, count++;
    if (!count) return 0;
    std::vector<V3> nrm(nv, V3{0, 0, 0});
    size_t nt = m.triVerts.size() / 3;
    for (size_t t = 0; t < nt; t++) {
        size_t a = m.triVerts[3 * t], b = m.triVerts[3 * t + 1], c = m.triVerts[3 * t + 2];
        if (!moved[a] && !moved[b] && !moved[c]) continue;
        auto P = [&](size_t i) { return V3{m.vertProperties[i * np], m.vertProperties[i * np + 1], m.vertProperties[i * np + 2]}; };
        V3 n = cross(P(b) - P(a), P(c) - P(a));
        for (size_t i : {a, b, c})
            if (moved[i]) nrm[i] = nrm[i] + n;
    }
    for (size_t i = 0; i < nv; i++) {
        if (!moved[i]) continue;
        double l = norm(nrm[i]);
        if (l == 0) continue;
        V3 d = nrm[i] * (-eps / l);
        m.vertProperties[i * np] += d.x, m.vertProperties[i * np + 1] += d.y, m.vertProperties[i * np + 2] += d.z;
    }
    return count;
}

bool write_stl(const manifold::MeshGL64 &m, const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    char head[80] = {0};
    std::snprintf(head, sizeof head, "OpenPhysicsAI openphysics-cad lattice, millimetres");
    std::fwrite(head, 1, 80, f);
    uint32_t n = (uint32_t)(m.triVerts.size() / 3);
    std::fwrite(&n, 4, 1, f);
    size_t np = m.numProp;
    for (uint32_t t = 0; t < n; t++) {
        V3 v[3];
        for (int k = 0; k < 3; k++) {
            size_t i = m.triVerts[3 * t + k] * np;
            v[k] = {m.vertProperties[i], m.vertProperties[i + 1], m.vertProperties[i + 2]};
        }
        V3 nv = cross(v[1] - v[0], v[2] - v[0]);
        double l = norm(nv);
        if (l > 0) nv = nv * (1 / l);
        float rec[12] = {(float)nv.x, (float)nv.y, (float)nv.z};
        for (int k = 0; k < 3; k++) rec[3 + 3 * k] = (float)v[k].x, rec[4 + 3 * k] = (float)v[k].y, rec[5 + 3 * k] = (float)v[k].z;
        uint16_t attr = 0;
        std::fwrite(rec, 4, 12, f);
        std::fwrite(&attr, 2, 1, f);
    }
    return std::fclose(f) == 0;
}

JsonValue *nums(std::initializer_list<double> v) {
    JsonValue *a = json_array();
    for (double x : v) json_push(a, json_number(x));
    return a;
}

/* drain holes as a part of the field (a boolean on the mesh can run a hole tangent to a flat face and leave slivers
 * that a 32-bit STL joins wrongly, as the first run of 2026-10-08 found): each a round-ended hole (a capsule) from a
 * point in a void to r past a point outside; a grid point asks only the holes listed in its bucket, where every hole
 * within `margin` of the bucket is */
struct Holes {
    struct Hole {
        V3 a, b, from, to;
        double r;
    };
    std::vector<Hole> list;
    V3 lo{0, 0, 0};
    double cell = 1;
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::vector<int>> bucket;
    void add(V3 from, V3 to, double r) {
        V3 u = to - from;
        u = u * (1 / norm(u));
        list.push_back({from, to + u * r, from, to, r});
    }
    void index(V3 glo, V3 ghi, double margin) {
        double rmax = 0;
        for (const Hole &o : list) rmax = std::max(rmax, o.r);
        cell = std::max(1.0, 2 * (rmax + margin));
        lo = glo;
        nx = (int)std::ceil((ghi.x - glo.x) / cell) + 1, ny = (int)std::ceil((ghi.y - glo.y) / cell) + 1, nz = (int)std::ceil((ghi.z - glo.z) / cell) + 1;
        bucket.assign((size_t)nx * ny * nz, {});
        for (size_t i = 0; i < list.size(); i++) {
            const Hole &o = list[i];
            double e = o.r + margin;
            int i0 = std::max(0, (int)std::floor((std::min(o.a.x, o.b.x) - e - lo.x) / cell)), i1 = std::min(nx - 1, (int)std::floor((std::max(o.a.x, o.b.x) + e - lo.x) / cell));
            int j0 = std::max(0, (int)std::floor((std::min(o.a.y, o.b.y) - e - lo.y) / cell)), j1 = std::min(ny - 1, (int)std::floor((std::max(o.a.y, o.b.y) + e - lo.y) / cell));
            int k0 = std::max(0, (int)std::floor((std::min(o.a.z, o.b.z) - e - lo.z) / cell)), k1 = std::min(nz - 1, (int)std::floor((std::max(o.a.z, o.b.z) + e - lo.z) / cell));
            for (int k = k0; k <= k1; k++)
                for (int j = j0; j <= j1; j++)
                    for (int ii = i0; ii <= i1; ii++) bucket[((size_t)k * ny + j) * nx + ii].push_back((int)i);
        }
    }
    /* the distance outside the holes, negative inside one; far (1e30) where none is near */
    double outside(V3 p) const {
        if (list.empty()) return 1e30;
        int i = (int)std::floor((p.x - lo.x) / cell), j = (int)std::floor((p.y - lo.y) / cell), k = (int)std::floor((p.z - lo.z) / cell);
        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return 1e30;
        double best = 1e30;
        for (int n : bucket[((size_t)k * ny + j) * nx + i]) {
            const Hole &o = list[(size_t)n];
            V3 ab = o.b - o.a;
            double t = std::max(0.0, std::min(1.0, dot(p - o.a, ab) / dot(ab, ab)));
            best = std::min(best, norm(p - (o.a + ab * t)) - o.r);
        }
        return best;
    }
};

/* how many enclosed voids a solid has (closed surfaces of negative volume), and each one's shortest way out */
long enclosed_voids(const manifold::Manifold &solid, std::vector<WayOut> *ways) {
    manifold::MeshGL64 g = solid.GetMeshGL64();
    Mesh m;
    size_t np = g.numProp, nv = g.vertProperties.size() / np;
    m.v.resize(3 * nv);
    for (size_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) m.v[3 * i + k] = g.vertProperties[i * np + k];
    m.t.assign(g.triVerts.begin(), g.triVerts.end());
    Edges e = pair_edges(m);
    std::vector<int> shell_of;
    std::vector<Shell> shells = find_shells(m, e, shell_of);
    long n = 0;
    for (const Shell &s : shells) n += s.volume_mm3 < 0;
    if (ways && n) *ways = ways_out(m, shells, shell_of);
    return n;
}

} // namespace

/* the core's kind, form and size from a request's "core"; the solid fraction asked for, or -1 */
double parse_core(const JsonValue *core, Lattice &L, std::string &kind) {
    if (!core || core->type != JSON_OBJECT) refuse("\"core\" is needed: {\"kind\": ..., \"cell_mm\": ..., \"wall_mm\" or \"solid_fraction\": ...}");
    kind = json_get_str(core, "kind", "");
    static const std::map<std::string, Kind> kinds = {{"gyroid", GYROID}, {"schwarz_p", SCHWARZ_P}, {"diamond", DIAMOND}, {"bcc", BCC},
                                                      {"octet", OCTET}, {"none", NONE}, {"solid", SOLID}};
    if (!kinds.count(kind)) refuse("\"core\": \"kind\" \"" + kind + "\" is not known: gyroid, schwarz_p, diamond, bcc, octet, none, solid");
    L.kind = kinds.at(kind);
    std::string form = json_get_str(core, "form", "sheet");
    if (form != "sheet" && form != "band" && form != "network") refuse("\"core\": \"form\" must be \"sheet\", \"band\" or \"network\"");
    L.sheet = form != "network", L.band = form == "band";
    bool lattice = L.kind != NONE && L.kind != SOLID, struts = L.kind == BCC || L.kind == OCTET;
    double fraction = -1;
    if (lattice) {
        L.cell = json_get_num(core, "cell_mm", 0);
        if (!(L.cell > 0)) refuse("\"core\": \"cell_mm\" must be above 0");
        L.make_struts();
        fraction = json_get_num(core, "solid_fraction", -1);
        bool has_wall = json_get(core, "wall_mm") != nullptr, has_level = json_get(core, "level") != nullptr;
        if (fraction >= 0) {
            if (fraction < 0.05 || fraction > 0.95) refuse("\"core\": \"solid_fraction\" must be 0.05 to 0.95");
        } else if ((L.sheet && !L.band) || struts) {
            if (!has_wall) refuse("\"core\": \"wall_mm\" (a sheet's thickness, a strut's diameter) or \"solid_fraction\" is needed");
            L.wall = json_get_num(core, "wall_mm", 0);
            if (!(L.wall > 0)) refuse("\"core\": \"wall_mm\" must be above 0");
        } else if (L.band) {
            if (!has_level) refuse("\"core\": a band sheet needs \"level\" (|G| < level) or \"solid_fraction\"");
            L.level = json_get_num(core, "level", 0);
            if (!(L.level > 0)) refuse("\"core\": a band sheet's \"level\" must be above 0");
        } else L.level = has_level ? json_get_num(core, "level", 0) : 0;
    }
    return fraction;
}

JsonValue *lattice_build(const JsonValue *req, const std::string &outdir) {
    auto t0 = std::chrono::steady_clock::now();
    try {
        const char *part = json_get_str(req, "part", nullptr);
        if (!part) refuse("\"part\" is needed: a STEP or STL file");
        std::string path = part;
        const JsonValue *core = json_get(req, "core");
        if (!core || core->type != JSON_OBJECT) refuse("\"core\" is needed: {\"kind\": ..., \"cell_mm\": ..., \"wall_mm\" or \"solid_fraction\": ...}");
        Lattice L;
        std::string kind;
        double fraction = parse_core(core, L, kind);
        bool lattice = L.kind != NONE && L.kind != SOLID, struts = L.kind == BCC || L.kind == OCTET;
        double skin = json_get_num(req, "skin_mm", 1);
        if (skin < 0) refuse("\"skin_mm\" must be 0 or more");
        if (const JsonValue *o = json_get(req, "origin_mm")) {
            double x[3];
            if (!json_get_numbers(o, x, 3)) refuse("\"origin_mm\" must be [x, y, z]");
            L.origin = {x[0], x[1], x[2]};
        }
        /* drain holes: given ones, or one for each enclosed void found in the result ({"radius_mm": r}) */
        Holes drains;
        double auto_drain = 0;
        if (const JsonValue *dr = json_get(req, "drains")) {
            if (dr->type == JSON_OBJECT) {
                auto_drain = json_get_num(dr, "radius_mm", 0);
                if (!(auto_drain > 0)) refuse("\"drains\": {\"radius_mm\": r} drills one hole of radius r for each enclosed void; r must be above 0");
            } else if (dr->type == JSON_ARRAY) {
                for (size_t i = 0; i < json_len(dr); i++) {
                    const JsonValue *o = json_at(dr, i);
                    double a[3], b[3], r = json_get_num(o, "radius_mm", 0);
                    if (!json_get_numbers(json_get(o, "from_mm"), a, 3) || !json_get_numbers(json_get(o, "to_mm"), b, 3) || !(r > 0))
                        refuse("\"drains\": each hole is {\"from_mm\": [x, y, z], \"to_mm\": [x, y, z], \"radius_mm\": r}, r above 0");
                    if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) refuse("\"drains\": a hole's from_mm and to_mm must differ");
                    drains.add({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, r);
                }
            } else refuse("\"drains\" is {\"radius_mm\": r} (one for each enclosed void) or a list of holes");
        }
        /* the part */
        std::vector<Tri> tris = load_part(path, 0.01);
        V3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (const Tri &t : tris)
            for (const V3 &p : {t.a, t.b, t.c})
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        double size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        double h = json_get_num(req, "resolution_mm", 0);
        bool walled = (L.sheet && !L.band) || struts; /* a sheet of constant thickness, or struts: the wall is set */
        if (h == 0) h = std::max(0.05, std::min(lattice ? std::min(L.cell / 20, walled && fraction < 0 ? L.wall / 3 : 1e9) : 1e9, size / 200));
        if (!(h > 0)) refuse("\"resolution_mm\" must be above 0");
        if (fraction >= 0) solve_fraction(L, fraction);
        if (lattice && walled && L.wall < 2 * h)
            refuse("the wall (" + std::to_string(L.wall) + " mm) is thinner than two grid steps (" + std::to_string(2 * h) +
                   " mm): give a finer resolution_mm or a thicker wall");
        if (skin > 0 && skin < 2 * h)
            refuse("the skin (" + std::to_string(skin) + " mm) is thinner than two grid steps: give a finer resolution_mm");
        for (const auto &o : drains.list)
            if (o.r < 2 * h) refuse("a drain hole's radius (" + std::to_string(o.r) + " mm) is less than two grid steps: give a finer resolution_mm");
        if (auto_drain > 0 && auto_drain < 2 * h)
            refuse("the drain holes' radius (" + std::to_string(auto_drain) + " mm) is less than two grid steps: give a finer resolution_mm");
        double band = skin + 3 * h;
        V3 pad{2 * h, 2 * h, 2 * h};
        Grid G = part_sdf(tris, h, band, lo - pad, hi + pad);

        auto P = [&](manifold::vec3 p) { return G.sample({p.x, p.y, p.z}); };
        auto result = [&](manifold::vec3 p) {
            double d = P(p);
            double skin_region = skin > 0 ? std::min(d, skin - d) : -1e30;
            double core_region = d - skin;
            double lat = L.field({p.x, p.y, p.z});
            return std::max(skin_region, std::min(core_region, lat));
        };
        manifold::Box box(manifold::vec3(G.lo.x, G.lo.y, G.lo.z),
                          manifold::vec3(G.lo.x + (G.nx - 1) * h, G.lo.y + (G.ny - 1) * h, G.lo.z + (G.nz - 1) * h));
        manifold::Manifold out = manifold::Manifold::LevelSet(result, box, h, 0, h * 0.05);
        if (out.Status() != manifold::Manifold::Error::NoError || out.NumTri() == 0) refuse("the level set did not make a closed solid");
        /* the measures, from meshes made the same way (the part and the core's region at twice the step: smooth) */
        double v_part = manifold::Manifold::LevelSet(P, box, 2 * h, 0, h * 0.05).Volume();
        double v_region = skin > 0 ? manifold::Manifold::LevelSet([&](manifold::vec3 p) { return P(p) - skin; }, box, 2 * h, 0, h * 0.05).Volume()
                                   : v_part;
        double v_out = out.Volume(), v_skin = v_part - v_region, v_core = v_out - v_skin;
        /* the file: the same solid simplified within a quarter of a grid step (Manifold keeps it closed and manifold,
         * its vertices a subset of the level set's); the measures above are the level set's own */
        bool simplify = json_get_bool(req, "simplify", true);
        manifold::Manifold written;
        auto simplified = [&]() {
            written = simplify ? out.Simplify(h * 0.25) : out;
            if (written.Status() != manifold::Manifold::Error::NoError || written.NumTri() == 0) written = out, simplify = false;
        };
        simplified();
        /* enclosed voids (they keep powder or resin when printed: docs/cad.md#printability), and the holes that drain
         * them, made in the level set again with the holes in its field: the holes given, or one along each void's
         * shortest way out, and again while any void is left (at most three rounds) */
        std::vector<WayOut> ways;
        long voids_found = enclosed_voids(written, auto_drain > 0 ? &ways : nullptr), rounds = 0;
        auto drill = [&]() {
            drains.index(G.lo, {G.lo.x + (G.nx - 1) * h, G.lo.y + (G.ny - 1) * h, G.lo.z + (G.nz - 1) * h}, 3 * h);
            out = manifold::Manifold::LevelSet([&](manifold::vec3 p) { return std::min(result(p), drains.outside({p.x, p.y, p.z})); }, box, h, 0,
                                               h * 0.05);
            if (out.Status() != manifold::Manifold::Error::NoError || out.NumTri() == 0) refuse("the level set with the drain holes did not make a closed solid");
            simplified();
        };
        if (!drains.list.empty()) drill();
        while (auto_drain > 0 && rounds < 3 && !ways.empty()) {
            size_t before = drains.list.size();
            for (const WayOut &w : ways)
                if (w.through_mm > 0) drains.add(w.from, w.to, auto_drain);
            if (drains.list.size() == before) break;
            drill();
            rounds++;
            ways.clear();
            if (enclosed_voids(written, &ways) == 0) break;
        }
        long voids_left = drains.list.empty() ? voids_found : enclosed_voids(written, nullptr);
        manifold::MeshGL64 mesh = written.GetMeshGL64();
        auto co = float_coincident(mesh);
        long pulled = unpinch(mesh, 1e-4);
        bool own_check = closed_manifold(mesh);
        double outside = 0; /* how far any vertex lies outside the part */
        for (size_t i = 0; i + 2 < mesh.vertProperties.size(); i += mesh.numProp)
            outside = std::max(outside, -G.sample({mesh.vertProperties[i], mesh.vertProperties[i + 1], mesh.vertProperties[i + 2]}));
        double thinnest = -1; /* the skin's depth of solid under sampled points of the surface */
        V3 thinnest_at{0, 0, 0}, thinnest_n{0, 0, 0};
        double rounding = 0; /* the deepest the solid began under a sampled point of the part's surface */
        if (skin > 0) {
            std::mt19937_64 rng(7);
            std::vector<double> area(tris.size());
            for (size_t i = 0; i < tris.size(); i++) area[i] = 0.5 * norm(cross(tris[i].b - tris[i].a, tris[i].c - tris[i].a));
            std::uniform_real_distribution<double> U(0, 1);
            std::discrete_distribution<size_t> pick(area.begin(), area.end());
            thinnest = 1e30;
            V3 worst{0, 0, 0}, worst_n{0, 0, 0};
            for (int s = 0; s < 2000; s++) {
                const Tri &t = tris[pick(rng)];
                double r1 = U(rng), r2 = U(rng);
                if (r1 + r2 > 1) r1 = 1 - r1, r2 = 1 - r2;
                V3 p = t.a + (t.b - t.a) * r1 + (t.c - t.a) * r2, n = cross(t.b - t.a, t.c - t.a);
                double l = norm(n);
                if (l == 0) continue;
                n = n * (-1 / l); /* inwards */
                auto solid = [&](double s) { V3 q = p + n * s; return result(manifold::vec3(q.x, q.y, q.z)) > 0; };
                /* where the solid begins under the point: at once on a face, a little deeper within a grid step or two of
                 * a sharp edge, which the level set rounds (the first run of 2026-10-08 took that rounding, 0.037 mm,
                 * for the skin's depth); then how deep it runs from there */
                double start = h / 16;
                while (start < 2 * h && !solid(start)) start += h / 16;
                double depth = start;
                if (start < 2 * h)
                    while (depth < start + 3 * skin && solid(depth)) depth += h / 16;
                double run = start < 2 * h ? depth - start : 0;
                rounding = std::max(rounding, start < 2 * h ? start : 2 * h);
                if (run < thinnest) thinnest = run, worst = p, worst_n = n;
            }
            thinnest_at = worst, thinnest_n = worst_n;
        }
        /* cross-sections of the result at heights asked for (for pictures and for a look inside): OUTDIR/slices.json */
        std::string slices_file;
        if (const JsonValue *zs = json_get(req, "slices_mm")) {
            JsonValue *list = json_array();
            for (size_t i = 0; i < json_len(zs); i++) {
                double z = json_get_num(json_at(zs, i), "", json_at(zs, i)->type == JSON_NUMBER ? json_at(zs, i)->u.number : 0);
                if (json_at(zs, i)->type != JSON_NUMBER) continue;
                JsonValue *one = json_object(), *loops = json_set_array(one, "loops");
                json_set_number(one, "z_mm", z);
                for (const auto &poly : out.Slice(z)) {
                    JsonValue *loop = json_array();
                    for (const auto &v : poly) json_push(loop, nums({v.x, v.y}));
                    json_push(loops, loop);
                }
                json_push(list, one);
            }
            slices_file = outdir + "/slices.json";
            json_write_file(slices_file.c_str(), list, 0);
            json_free(list);
        }
        std::string stl = outdir + "/infilled.stl";
        if (!write_stl(mesh, stl + ".tmp")) {
            std::remove((stl + ".tmp").c_str());
            refuse("cannot write into " + outdir);
        }
        std::rename((stl + ".tmp").c_str(), stl.c_str());
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        JsonValue *rec = json_object(), *o;
        json_set_bool(rec, "ok", true);
        o = json_set_object(rec, "part");
        json_set_string(o, "file", path.c_str());
        json_set_int(o, "triangles", (long long)tris.size());
        json_set(o, "box_mm", nums({lo.x, lo.y, lo.z, hi.x, hi.y, hi.z}));
        json_set_number(o, "volume_mm3", v_part);
        o = json_set_object(rec, "skin");
        json_set_number(o, "thickness_mm", skin);
        json_set_number(o, "volume_mm3", v_skin);
        if (thinnest >= 0) {
            json_set_number(o, "thinnest_sampled_mm", thinnest);
            json_set(o, "thinnest_at_mm", nums({thinnest_at.x, thinnest_at.y, thinnest_at.z}));
            json_set(o, "thinnest_inwards", nums({thinnest_n.x, thinnest_n.y, thinnest_n.z}));
            json_set_number(o, "deepest_start_sampled_mm", rounding);
        }
        o = json_set_object(rec, "core");
        json_set_string(o, "kind", kind.c_str());
        if (lattice) {
            if (!struts) json_set_string(o, "form", L.band ? "band" : L.sheet ? "sheet" : "network");
            json_set_number(o, "cell_mm", L.cell);
            if ((L.sheet && !L.band) || struts) json_set_number(o, struts ? "strut_diameter_mm" : "wall_mm", L.wall);
            else json_set_number(o, "level", L.level);
            if (fraction >= 0) json_set_number(o, "solid_fraction_asked", fraction);
            json_set_number(o, "cell_solid_fraction", L.cell_fraction(96));
        }
        json_set_number(o, "region_volume_mm3", v_region);
        json_set_number(o, "solid_volume_mm3", v_core);
        json_set_number(o, "solid_fraction", v_region > 0 ? v_core / v_region : 0);
        o = json_set_object(rec, "result");
        json_set_number(o, "volume_mm3", out.Volume());
        json_set_int(o, "triangles", (long long)out.NumTri());
        json_set_int(o, "genus", out.Genus());
        json_set_bool(o, "simplified", simplify);
        json_set_int(o, "written_triangles", (long long)written.NumTri());
        json_set_number(o, "written_volume_mm3", written.Volume());
        json_set_bool(o, "closed_manifold", true);
        json_set_bool(o, "stl_closed_manifold", own_check);
        json_set_number(o, "outside_part_mm", outside);
        json_set_int(o, "touching_points", co.first);
        json_set_int(o, "touching_points_exact", co.second);
        json_set_int(o, "vertices_pulled_apart", pulled);
        json_set_int(o, "enclosed_voids", voids_left);
        if (!drains.list.empty() || auto_drain > 0) {
            JsonValue *dj = json_set_object(rec, "drains"), *list;
            json_set_int(dj, "count", (long long)drains.list.size());
            json_set_int(dj, "enclosed_voids_before", voids_found);
            json_set_int(dj, "enclosed_voids_after", voids_left);
            if (auto_drain > 0) json_set_int(dj, "rounds", rounds);
            json_set_number(dj, "removed_mm3", v_out - out.Volume());
            list = json_set_array(dj, "holes");
            for (size_t i = 0; i < drains.list.size() && i < 50; i++) {
                const auto &o = drains.list[i];
                JsonValue *one = json_object();
                json_set(one, "from_mm", nums({o.from.x, o.from.y, o.from.z}));
                json_set(one, "to_mm", nums({o.to.x, o.to.y, o.to.z}));
                json_set_number(one, "radius_mm", o.r);
                json_push(list, one);
            }
        }
        json_set_number(rec, "resolution_mm", h);
        json_set(rec, "grid", nums({(double)G.nx, (double)G.ny, (double)G.nz}));
        json_set_number(rec, "seconds", secs);
        JsonValue *files = json_set_object(rec, "files");
        json_set_string(files, "stl", stl.c_str());
        if (!slices_file.empty()) json_set_string(files, "slices", slices_file.c_str());
        return rec;
    } catch (Refusal &r) {
        JsonValue *o = json_object(), *e;
        json_set_bool(o, "ok", false);
        e = json_set_object(o, "error");
        json_set_string(e, "message", r.what());
        return o;
    }
}

namespace {

/* a 6 x 6 matrix inverted by Gauss-Jordan with partial pivoting; false when it is singular */
bool invert6(const double A[36], double inv[36]) {
    double M[6][12];
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 12; j++) M[i][j] = j < 6 ? A[6 * i + j] : (j - 6 == i);
    double scale = 0;
    for (int i = 0; i < 36; i++) scale = std::max(scale, std::fabs(A[i]));
    for (int c = 0; c < 6; c++) {
        int piv = c;
        for (int r = c + 1; r < 6; r++)
            if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
        if (!(std::fabs(M[piv][c]) > 1e-12 * scale)) return false;
        for (int j = 0; j < 12; j++) std::swap(M[c][j], M[piv][j]);
        double d = M[c][c];
        for (int j = 0; j < 12; j++) M[c][j] /= d;
        for (int r = 0; r < 6; r++)
            if (r != c) {
                double f = M[r][c];
                for (int j = 0; j < 12; j++) M[r][j] -= f * M[c][j];
            }
    }
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) inv[6 * i + j] = M[i][j + 6];
    return true;
}

/* the least eigenvalue of a symmetric 6 x 6 matrix (cyclic Jacobi) */
double least_eigenvalue6(const double A[36]) {
    double a[6][6];
    for (int i = 0; i < 36; i++) a[i / 6][i % 6] = A[i];
    for (int sweep = 0; sweep < 60; sweep++) {
        double off = 0;
        for (int p = 0; p < 6; p++)
            for (int q = p + 1; q < 6; q++) off += a[p][q] * a[p][q];
        if (off < 1e-30) break;
        for (int p = 0; p < 6; p++)
            for (int q = p + 1; q < 6; q++) {
                if (a[p][q] == 0) continue;
                double th = 0.5 * std::atan2(2 * a[p][q], a[q][q] - a[p][p]), c = std::cos(th), sn = std::sin(th);
                for (int k = 0; k < 6; k++) {
                    double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq, a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 6; k++) {
                    double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk, a[q][k] = sn * apk + c * aqk;
                }
            }
    }
    double m = a[0][0];
    for (int i = 1; i < 6; i++) m = std::min(m, a[i][i]);
    return m;
}

} // namespace

JsonValue *lattice_cell(const JsonValue *req) {
    auto t0 = std::chrono::steady_clock::now();
    try {
        Lattice L;
        std::string kind;
        double fraction = parse_core(json_get(req, "core"), L, kind);
        if (L.kind == NONE) refuse("\"core\": a cell of kind none holds nothing to homogenise");
        const JsonValue *nuv = json_get(req, "poisson_ratio");
        if (!nuv || nuv->type != JSON_NUMBER)
            refuse("\"poisson_ratio\" is needed: the solid's own Poisson's ratio (the material's value; it is not guessed here)");
        double nu = nuv->u.number;
        if (!(nu > -1 && nu < 0.5)) refuse("\"poisson_ratio\" must be above -1 and below 0.5");
        long long n = json_get_int(req, "voxels", 48);
        if (n < 8 || n > 96) refuse("\"voxels\" (across the cell) must be 8 to 96");
        if (L.kind == SOLID) L.cell = json_get_num(json_get(req, "core"), "cell_mm", 1) > 0 ? json_get_num(json_get(req, "core"), "cell_mm", 1) : 1;
        if (fraction >= 0) solve_fraction(L, fraction, 96);
        /* each voxel's solid fraction from 4 x 4 x 4 samples, as the support cells are sampled */
        enum { SS = 4 };
        size_t nv = (size_t)n * n * n;
        std::vector<double> phi(nv);
        double h = L.cell / (double)n;
        unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < nt; w++)
            pool.emplace_back([&, w] {
                for (long long k = w; k < n; k += nt)
                    for (long long j = 0; j < n; j++)
                        for (long long i = 0; i < n; i++) {
                            int hit = 0;
                            for (int c = 0; c < SS; c++)
                                for (int b = 0; b < SS; b++)
                                    for (int a = 0; a < SS; a++)
                                        hit += L.field(L.origin + V3{(i + (a + 0.5) / SS) * h, (j + (b + 0.5) / SS) * h, (k + (c + 0.5) / SS) * h}) > 0;
                            phi[((size_t)k * n + j) * n + i] = hit / (double)(SS * SS * SS);
                        }
            });
        for (auto &t : pool) t.join();
        double f = 0;
        for (double x : phi) f += x;
        f /= (double)nv;
        double C[36], S[36];
        int elements = 0;
        char err[300] = "";
        if (!support_cell_stiffness(phi.data(), (int)n, (int)n, L.cell, L.cell, true, nu, C, &elements, err, sizeof err)) refuse(err);
        /* what follows from the matrix: moduli along the cube axes from its inverse, the cubic constants, bounds */
        bool stiff = invert6(C, S);
        double c11 = (C[0] + C[7] + C[14]) / 3, c12 = (C[1] + C[2] + C[8]) / 3, c44 = (C[21] + C[28] + C[35]) / 3;
        double Ks = 1 / (3 * (1 - 2 * nu)), Gs = 1 / (2 * (1 + nu)), lam = nu / ((1 + nu) * (1 - 2 * nu));
        double Cs[36] = {0};
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) Cs[6 * i + j] = lam;
            Cs[6 * i + i] = lam + 2 * Gs, Cs[6 * (i + 3) + i + 3] = Gs;
        }
        double D[36];
        for (int i = 0; i < 36; i++) D[i] = f * Cs[i] - C[i];
        double voigt_margin = least_eigenvalue6(D);
        double hs = 4 * Gs * Ks * f / (4 * Gs + 3 * Ks * (1 - f));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        JsonValue *rec = json_object(), *o, *a;
        json_set_bool(rec, "ok", true);
        o = json_set_object(rec, "core");
        json_set_string(o, "kind", kind.c_str());
        if (L.kind != SOLID) {
            if (L.kind != BCC && L.kind != OCTET) json_set_string(o, "form", L.band ? "band" : L.sheet ? "sheet" : "network");
            json_set_number(o, "cell_mm", L.cell);
            if (L.band || !L.sheet) json_set_number(o, "level", L.level);
            else json_set_number(o, L.kind == BCC || L.kind == OCTET ? "strut_diameter_mm" : "wall_mm", L.wall);
            if (fraction >= 0) json_set_number(o, "solid_fraction_asked", fraction);
        }
        json_set_number(rec, "poisson_ratio", nu);
        json_set_int(rec, "voxels", n);
        json_set_int(rec, "elements", elements);
        json_set_number(rec, "solid_fraction", f);
        a = json_set_array(rec, "stiffness_over_Es");
        for (double x : C) json_push(a, json_number(x));
        o = json_set_object(rec, "cube_axes");
        json_set(o, "modulus_over_Es", nums({stiff ? 1 / S[0] : 0, stiff ? 1 / S[7] : 0, stiff ? 1 / S[14] : 0}));
        json_set(o, "shear_modulus_over_Es", nums({stiff ? 1 / S[21] : 0, stiff ? 1 / S[28] : 0, stiff ? 1 / S[35] : 0}));
        json_set(o, "poisson_ratio", nums({stiff ? -S[1] / S[0] : 0, stiff ? -S[2] / S[0] : 0, stiff ? -S[8] / S[7] : 0}));
        o = json_set_object(rec, "cubic");
        json_set_number(o, "C11_over_Es", c11);
        json_set_number(o, "C12_over_Es", c12);
        json_set_number(o, "C44_over_Es", c44);
        json_set_number(o, "modulus_over_Es", c11 + c12 > 0 ? (c11 - c12) * (c11 + 2 * c12) / (c11 + c12) : 0);
        json_set_number(o, "zener_ratio", c11 - c12 > 0 ? 2 * c44 / (c11 - c12) : 0);
        json_set_number(o, "bulk_over_Es", (c11 + 2 * c12) / 3);
        o = json_set_object(rec, "bounds");
        json_set_number(o, "voigt_least_eigenvalue_over_Es", voigt_margin);
        json_set_number(o, "hashin_shtrikman_bulk_over_Es", hs);
        json_set_bool(o, "carries_load", stiff);
        json_set_number(rec, "seconds", secs);
        return rec;
    } catch (Refusal &r) {
        JsonValue *o = json_object(), *e;
        json_set_bool(o, "ok", false);
        e = json_set_object(o, "error");
        json_set_string(e, "message", r.what());
        return o;
    }
}

JsonValue *lattice_specimen(const JsonValue *req) {
    auto t0 = std::chrono::steady_clock::now();
    try {
        Lattice L;
        std::string kind;
        double fraction = parse_core(json_get(req, "core"), L, kind);
        if (L.kind == NONE || L.kind == SOLID) refuse("\"core\": a specimen needs a lattice (gyroid, schwarz_p, diamond, bcc, octet)");
        const JsonValue *nuv = json_get(req, "poisson_ratio");
        if (!nuv || nuv->type != JSON_NUMBER)
            refuse("\"poisson_ratio\" is needed: the solid's own Poisson's ratio (the material's value; it is not guessed here)");
        double nu = nuv->u.number;
        if (!(nu > -1 && nu < 0.5)) refuse("\"poisson_ratio\" must be above -1 and below 0.5");
        double c[3];
        if (!json_get_numbers(json_get(req, "cells"), c, 3) || c[0] < 1 || c[1] < 1 || c[2] < 1 || c[0] != std::floor(c[0]) ||
            c[1] != std::floor(c[1]) || c[2] != std::floor(c[2]))
            refuse("\"cells\" is [nx, ny, nz], whole cells along each axis (the specimen is loaded along z)");
        long long n = json_get_int(req, "voxels_per_cell", 10);
        if (n < 4 || n > 64) refuse("\"voxels_per_cell\" must be 4 to 64");
        std::string platens = json_get_str(req, "platens", "both");
        if (platens != "both" && platens != "frictionless" && platens != "bonded") refuse("\"platens\" is frictionless, bonded or both");
        long max_unknowns = (long)json_get_num(req, "max_unknowns", 1.6e6);
        if (fraction >= 0) solve_fraction(L, fraction, 96);
        int nx = (int)(c[0] * n), ny = (int)(c[1] * n), nz = (int)(c[2] * n);
        double h = L.cell / (double)n;
        /* each voxel's solid fraction from 4 x 4 x 4 samples; the specimen starts at the lattice's origin */
        auto sample = [&](int sx, int sy, int sz, std::vector<double> &phi) {
            enum { SS = 4 };
            phi.assign((size_t)sx * sy * sz, 0);
            unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
            std::vector<std::thread> pool;
            for (unsigned w = 0; w < nt; w++)
                pool.emplace_back([&, w] {
                    for (int k = (int)w; k < sz; k += (int)nt)
                        for (int j = 0; j < sy; j++)
                            for (int i = 0; i < sx; i++) {
                                int hit = 0;
                                for (int cz = 0; cz < SS; cz++)
                                    for (int cy = 0; cy < SS; cy++)
                                        for (int cx = 0; cx < SS; cx++)
                                            hit += L.field(L.origin + V3{(i + (cx + 0.5) / SS) * h, (j + (cy + 0.5) / SS) * h, (k + (cz + 0.5) / SS) * h}) > 0;
                                phi[((size_t)k * sy + j) * sx + i] = hit / (double)(SS * SS * SS);
                            }
                });
            for (auto &t : pool) t.join();
        };
        /* the periodic cell at the same voxels: the specimen's size factor is measured against it */
        std::vector<double> phi;
        sample((int)n, (int)n, (int)n, phi);
        double C[36], S[36];
        int cell_elements = 0;
        char err[300] = "";
        if (!support_cell_stiffness(phi.data(), (int)n, (int)n, L.cell, L.cell, true, nu, C, &cell_elements, err, sizeof err)) refuse(err);
        if (!invert6(C, S)) refuse("the periodic cell carries no load");
        double periodic = 1 / S[14]; /* along z, the load's axis */
        sample(nx, ny, nz, phi);
        double f = 0;
        for (double x : phi) f += x;
        f /= (double)phi.size();
        JsonValue *rec = json_object(), *o;
        json_set_bool(rec, "ok", true);
        o = json_set_object(rec, "core");
        json_set_string(o, "kind", kind.c_str());
        if (L.kind != BCC && L.kind != OCTET) json_set_string(o, "form", L.band ? "band" : L.sheet ? "sheet" : "network");
        json_set_number(o, "cell_mm", L.cell);
        if (L.band || !L.sheet) json_set_number(o, "level", L.level);
        else json_set_number(o, L.kind == BCC || L.kind == OCTET ? "strut_diameter_mm" : "wall_mm", L.wall);
        json_set_number(rec, "poisson_ratio", nu);
        json_set(rec, "cells", nums({c[0], c[1], c[2]}));
        json_set(rec, "size_mm", nums({c[0] * L.cell, c[1] * L.cell, c[2] * L.cell}));
        json_set_int(rec, "voxels_per_cell", n);
        json_set_number(rec, "solid_fraction", f);
        json_set_number(rec, "periodic_modulus_over_Es", periodic);
        for (const char *mode : {"frictionless", "bonded"}) {
            if (platens != "both" && platens != mode) continue;
            SpecimenResult r;
            std::string why;
            if (!specimen_compress(phi.data(), nx, ny, nz, h, nu, !std::strcmp(mode, "bonded"), max_unknowns, &r, why)) {
                json_free(rec);
                refuse(why);
            }
            o = json_set_object(rec, mode);
            json_set_number(o, "modulus_over_Es", r.modulus_over_Es);
            json_set_number(o, "size_factor", r.modulus_over_Es / periodic);
            json_set_int(o, "elements", r.elements);
            json_set_int(o, "voxels_dropped", r.dropped);
            json_set_int(o, "unknowns", r.equations);
            json_set_int(o, "iterations", r.iterations);
            json_set_number(o, "residual", r.residual);
            json_set_number(o, "seconds", r.seconds);
        }
        json_set_number(rec, "seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return rec;
    } catch (Refusal &r) {
        JsonValue *o = json_object(), *e;
        json_set_bool(o, "ok", false);
        e = json_set_object(o, "error");
        json_set_string(e, "message", r.what());
        return o;
    }
}
