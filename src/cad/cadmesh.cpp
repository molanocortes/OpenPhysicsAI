// cadmesh.cpp - a part as triangles: read, joined, its edges paired, its shells and their ways out (see cadmesh.h)
#include "cadmesh.h"

extern "C" {
#include "../geom/bvh.h"
}

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <thread>
#include <utility>

namespace cadmesh {

namespace {

std::vector<Tri> load_step(const std::string &path, double deflection) {
    STEPControl_Reader r;
    if (r.ReadFile(path.c_str()) != IFSelect_RetDone || r.TransferRoots() <= 0) refuse("cannot read the STEP file " + path);
    TopoDS_Shape s = r.OneShape();
    BRepMesh_IncrementalMesh mesh(s, deflection, Standard_False, 0.1, Standard_True);
    std::vector<Tri> out;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(f, loc);
        if (t.IsNull()) continue;
        bool flip = f.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= t->NbTriangles(); i++) {
            int a, b, c;
            t->Triangle(i).Get(a, b, c);
            if (flip) std::swap(b, c);
            auto P = [&](int k) {
                gp_Pnt p = t->Node(k).Transformed(loc.Transformation());
                return V3{p.X(), p.Y(), p.Z()};
            };
            out.push_back({P(a), P(b), P(c)});
        }
    }
    return out;
}

std::vector<Tri> load_stl(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) refuse("cannot open the STL file " + path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> b((size_t)std::max(size, 0L));
    size_t got = std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    std::vector<Tri> out;
    uint32_t n = 0;
    if (got >= 84) std::memcpy(&n, &b[80], 4);
    if (got >= 84 && (size_t)84 + (size_t)n * 50 == got) { /* binary */
        for (uint32_t i = 0; i < n; i++) {
            float v[9];
            std::memcpy(v, &b[84 + 50 * (size_t)i + 12], 36);
            out.push_back({{v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}});
        }
        return out;
    }
    std::string text(b.begin(), b.end()); /* ASCII: "vertex x y z", three a facet */
    std::vector<V3> vs;
    for (size_t at = text.find("vertex"); at != std::string::npos; at = text.find("vertex", at + 6)) {
        V3 p;
        if (std::sscanf(text.c_str() + at + 6, "%lf %lf %lf", &p.x, &p.y, &p.z) == 3) vs.push_back(p);
    }
    if (vs.empty() || vs.size() % 3) refuse("the STL file " + path + " holds no triangles that can be read");
    for (size_t i = 0; i + 2 < vs.size(); i += 3) out.push_back({vs[i], vs[i + 1], vs[i + 2]});
    return out;
}

uint64_t edge_key(int a, int b) { return (uint64_t)(uint32_t)a << 32 | (uint32_t)b; }

} // namespace

std::vector<Tri> load_part(const std::string &path, double deflection_mm) {
    std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : "";
    for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
    std::vector<Tri> tris;
    try {
        if (ext == ".stl") tris = load_stl(path);
        else if (ext == "step" || ext == ".stp") tris = load_step(path, deflection_mm);
        else refuse("\"part\" must be a .step, .stp or .stl file");
    } catch (const Standard_Failure &e) {
        refuse(std::string("cannot read the part: ") + (e.GetMessageString() ? e.GetMessageString() : "Open CASCADE failure"));
    }
    if (tris.empty()) refuse("the part holds no triangles");
    double signed_volume = 0;
    for (const Tri &t : tris) {
        for (const V3 &p : {t.a, t.b, t.c})
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) refuse("the part has a corner that is not a number");
        signed_volume += dot(t.a, cross(t.b, t.c)) / 6;
    }
    if (signed_volume < 0) /* turned inside out: the triangles face inwards */
        for (Tri &t : tris) std::swap(t.b, t.c);
    return tris;
}

Mesh weld(const std::vector<Tri> &tris) {
    size_t nc = tris.size() * 3;
    std::vector<V3> pts(nc);
    for (size_t i = 0; i < tris.size(); i++) {
        const V3 *c[3] = {&tris[i].a, &tris[i].b, &tris[i].c};
        for (int k = 0; k < 3; k++) pts[3 * i + k] = {c[k]->x + 0.0, c[k]->y + 0.0, c[k]->z + 0.0}; /* -0 joins 0 */
    }
    std::vector<uint32_t> order(nc);
    std::iota(order.begin(), order.end(), 0u);
    auto less = [&](uint32_t a, uint32_t b) {
        const V3 &p = pts[a], &q = pts[b];
        return p.x != q.x ? p.x < q.x : p.y != q.y ? p.y < q.y : p.z < q.z;
    };
    std::sort(order.begin(), order.end(), less);
    Mesh m;
    std::vector<int> id(nc);
    for (size_t i = 0; i < nc; i++) {
        if (i == 0 || less(order[i - 1], order[i])) {
            const V3 &p = pts[order[i]];
            m.v.insert(m.v.end(), {p.x, p.y, p.z});
        }
        id[order[i]] = (int)m.nv() - 1;
    }
    m.t.reserve(nc);
    for (size_t i = 0; i < tris.size(); i++) {
        int a = id[3 * i], b = id[3 * i + 1], c = id[3 * i + 2];
        if (a == b || b == c || a == c) continue;
        m.t.insert(m.t.end(), {a, b, c});
    }
    return m;
}

Edges pair_edges(const Mesh &m) {
    size_t nt = m.nt();
    std::vector<std::pair<uint64_t, int>> E(3 * nt);
    for (size_t t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) E[3 * t + k] = {edge_key(m.t[3 * t + k], m.t[3 * t + (k + 1) % 3]), (int)(3 * t + k)};
    std::sort(E.begin(), E.end());
    auto range = [&](uint64_t key) {
        auto lo = std::lower_bound(E.begin(), E.end(), std::make_pair(key, -1));
        auto hi = lo;
        while (hi != E.end() && hi->first == key) ++hi;
        return std::make_pair(lo, hi);
    };
    Edges e;
    e.nb.assign(3 * nt, -1);
    for (size_t t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) {
            int a = m.t[3 * t + k], b = m.t[3 * t + (k + 1) % 3];
            auto own = range(edge_key(a, b)), rev = range(edge_key(b, a));
            long n_own = own.second - own.first, n_rev = rev.second - rev.first;
            if (n_own == 1 && n_rev == 1) e.nb[3 * t + k] = rev.first->second / 3;
            else if (n_rev == 0 && n_own == 1) e.open_edges++;
            else e.shared_edges++;
        }
    return e;
}

std::vector<Shell> find_shells(const Mesh &m, const Edges &e, std::vector<int> &shell_of) {
    size_t nt = m.nt();
    shell_of.assign(nt, -1);
    std::vector<Shell> shells;
    V3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300}; /* volumes about the middle of the part, for rounding */
    for (size_t i = 0; i < m.nv(); i++) {
        V3 p = m.P((int)i);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    V3 mid = (lo + hi) * 0.5;
    std::vector<int> stack;
    for (size_t seed = 0; seed < nt; seed++) {
        if (shell_of[seed] >= 0) continue;
        int s = (int)shells.size();
        shells.push_back(Shell{});
        Shell &sh = shells.back();
        shell_of[seed] = s;
        stack.assign(1, (int)seed);
        while (!stack.empty()) {
            int t = stack.back();
            stack.pop_back();
            V3 a = m.corner((size_t)t, 0), b = m.corner((size_t)t, 1), c = m.corner((size_t)t, 2);
            sh.volume_mm3 += dot(a - mid, cross(b - mid, c - mid)) / 6;
            sh.area_mm2 += 0.5 * norm(cross(b - a, c - a));
            for (const V3 &p : {a, b, c})
                sh.lo = {std::min(sh.lo.x, p.x), std::min(sh.lo.y, p.y), std::min(sh.lo.z, p.z)},
                sh.hi = {std::max(sh.hi.x, p.x), std::max(sh.hi.y, p.y), std::max(sh.hi.z, p.z)};
            sh.triangles++;
            for (int k = 0; k < 3; k++) {
                int n = e.nb[3 * (size_t)t + k];
                if (n >= 0 && shell_of[n] < 0) shell_of[n] = s, stack.push_back(n);
            }
        }
    }
    return shells;
}

std::vector<WayOut> ways_out(const Mesh &m, const std::vector<Shell> &shells, const std::vector<int> &shell_of) {
    std::vector<WayOut> ways;
    std::vector<int> outer;
    for (size_t t = 0; t < m.nt(); t++)
        if (shells[shell_of[t]].volume_mm3 > 0) outer.insert(outer.end(), {m.t[3 * t], m.t[3 * t + 1], m.t[3 * t + 2]});
    std::vector<std::vector<int>> verts(shells.size());
    std::vector<int> seen(m.nv(), -1);
    for (size_t t = 0; t < m.nt(); t++) {
        int s = shell_of[t];
        if (shells[s].volume_mm3 >= 0) continue;
        for (int k = 0; k < 3; k++) {
            int v = m.t[3 * t + k];
            if (seen[v] != s) seen[v] = s, verts[s].push_back(v);
        }
    }
    for (size_t s = 0; s < shells.size(); s++)
        if (shells[s].volume_mm3 < 0) ways.push_back(WayOut{(int)s});
    if (ways.empty() || outer.empty()) return ways;
    Bvh b;
    if (!bvh_build(&b, m.v.data(), outer.data(), (int)(outer.size() / 3))) refuse("out of memory while looking for the voids' ways out");
    unsigned nthreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < nthreads; w++)
        pool.emplace_back([&, w] {
            for (size_t i = w; i < ways.size(); i += nthreads) {
                WayOut &way = ways[i];
                const std::vector<int> &vs = verts[(size_t)way.shell];
                size_t stride = std::max<size_t>(1, vs.size() / 4000);
                double best = 1e300;
                for (size_t j = 0; j < vs.size(); j += stride) {
                    double p[3] = {m.v[3 * (size_t)vs[j]], m.v[3 * (size_t)vs[j] + 1], m.v[3 * (size_t)vs[j] + 2]}, q[3], d;
                    int tri;
                    /* a void can touch the outside at one point (a pinch of the level set): powder does not pass a
                     * point, so the way out goes through the thinnest wall beside it, not through the point itself */
                    if (bvh_closest(&b, p, 1e30, &tri, q, &d) && d > 1e-9 && d < best)
                        best = d, way.from = {p[0], p[1], p[2]}, way.to = {q[0], q[1], q[2]};
                }
                way.through_mm = best < 1e300 ? best : 0;
            }
        });
    for (auto &t : pool) t.join();
    bvh_free(&b);
    return ways;
}

} // namespace cadmesh
