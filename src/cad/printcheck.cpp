// printcheck.cpp - can a part be printed as it is (see printcheck.h; GOALS.md G23 step 6)
#include "printcheck.h"

#include "cadmesh.h"

extern "C" {
#include "../geom/bvh.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace cadmesh;

namespace {

const double PI = 3.14159265358979323846;
const double PLATE_TOL_MM = 0.01; /* a downward face this close to the plate rests on it */
const double CREASE_DEG = 30;     /* a corner's normal is the mean of the triangles around it within this angle */

/* what an enclosed void keeps once printed, by ISO/ASTM 52900 category ("": it prints as a closed cavity) */
const std::map<std::string, std::string> KEPT = {{"powder_bed_fusion", "powder"},
                                                 {"binder_jetting", "powder"},
                                                 {"vat_photopolymerization", "resin"},
                                                 {"material_jetting", "support material"},
                                                 {"material_extrusion", ""}};

template <class F> void parallel(size_t n, F fn) {
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < nt; w++)
        pool.emplace_back([&, w] {
            for (size_t i = n * w / nt; i < n * (w + 1) / nt; i++) fn(i);
        });
    for (auto &t : pool) t.join();
}

struct Part {
    Mesh m;
    Edges e;
    std::vector<V3> fn; /* unit normals (0 where a triangle has no area) */
    std::vector<double> area;
    std::vector<V3> cn; /* the corners' normals, 3 a triangle */
    std::vector<Shell> shells;
    std::vector<int> shell_of;
    Bvh bvh{};
    V3 lo{0, 0, 0}, hi{0, 0, 0};
    double size = 0;
    ~Part() { bvh_free(&bvh); }
};

V3 centroid(const Mesh &m, size_t t) { return (m.corner(t, 0) + m.corner(t, 1) + m.corner(t, 2)) * (1.0 / 3); }

/* each corner's normal: the angle-weighted mean of the normals of the triangles around its vertex that lie within the
 * crease angle of this triangle's own; a flat face keeps its normal exactly, a curved surface gets its curvature back */
void corner_normals(Part &p) {
    const Mesh &m = p.m;
    size_t nv = m.nv(), nt = m.nt();
    std::vector<int> start(nv + 1, 0), list(3 * nt);
    for (int v : m.t) start[(size_t)v + 1]++;
    for (size_t i = 0; i < nv; i++) start[i + 1] += start[i];
    std::vector<int> fill(start.begin(), start.end() - 1);
    for (size_t t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) list[(size_t)fill[(size_t)m.t[3 * t + k]]++] = (int)(3 * t + k);
    double cosc = std::cos(CREASE_DEG * PI / 180);
    p.cn.assign(3 * nt, V3{0, 0, 0});
    parallel(nt, [&](size_t t) {
        for (int k = 0; k < 3; k++) {
            size_t v = (size_t)m.t[3 * t + k];
            V3 s{0, 0, 0};
            for (int i = start[v]; i < start[v + 1]; i++) {
                size_t u = (size_t)list[(size_t)i] / 3;
                int j = list[(size_t)i] % 3;
                if (dot(p.fn[u], p.fn[t]) < cosc) continue;
                V3 e1 = m.corner(u, (j + 1) % 3) - m.corner(u, j), e2 = m.corner(u, (j + 2) % 3) - m.corner(u, j);
                double l1 = norm(e1), l2 = norm(e2);
                if (l1 == 0 || l2 == 0) continue;
                s = s + p.fn[u] * std::acos(std::max(-1.0, std::min(1.0, dot(e1, e2) / (l1 * l2))));
            }
            double l = norm(s);
            p.cn[3 * t + k] = l > 0 ? s * (1 / l) : p.fn[t];
        }
    });
}

double plate_level(const Part &p, V3 d) {
    double plate = 1e300;
    for (size_t i = 0; i < p.m.nv(); i++) plate = std::min(plate, dot(p.m.P((int)i), d));
    return plate;
}

/* the overhang along d: where -n.d > cos(limit) (n followed linearly between a triangle's corners), the piece of each
 * triangle beyond that line, its area, and the support under it (its area projected on the plate times the drop from
 * its centroid to the part below or to the plate) */
struct Overhangs {
    double area = 0, support = 0;
    std::vector<double> tri_area, tri_support;
};

Overhangs overhangs(const Part &p, V3 d, double limit_deg, bool keep) {
    const Mesh &m = p.m;
    size_t nt = m.nt();
    double plate = plate_level(p, d), c0 = std::cos(limit_deg * PI / 180), eps = 1e-9 * std::max(1.0, p.size);
    std::vector<double> ta(nt, 0), ts(nt, 0);
    parallel(nt, [&](size_t t) {
        if (p.area[t] == 0) return;
        V3 q[3];
        double g[3];
        bool any = false, on_plate = true;
        for (int k = 0; k < 3; k++) {
            q[k] = m.corner(t, k);
            g[k] = -dot(p.cn[3 * t + k], d) - c0;
            any = any || g[k] > 0;
            if (dot(q[k], d) - plate > PLATE_TOL_MM) on_plate = false;
        }
        if (!any || on_plate) return;
        V3 poly[4];
        int np = 0;
        for (int k = 0; k < 3; k++) {
            int k2 = (k + 1) % 3;
            if (g[k] > 0) poly[np++] = q[k];
            if ((g[k] > 0) != (g[k2] > 0)) poly[np++] = q[k] + (q[k2] - q[k]) * (g[k] / (g[k] - g[k2]));
        }
        double a = 0;
        V3 c{0, 0, 0};
        for (int i = 1; i + 1 < np; i++) {
            double ai = 0.5 * norm(cross(poly[i] - poly[0], poly[i + 1] - poly[0]));
            a += ai, c = c + (poly[0] + poly[i] + poly[i + 1]) * (ai / 3);
        }
        if (!(a > 0)) return;
        c = c * (1 / a);
        double drop = dot(c, d) - plate;
        double o[3] = {c.x, c.y, c.z}, dir[3] = {-d.x, -d.y, -d.z};
        BvhHit hit;
        if (bvh_ray_nearest(&p.bvh, o, dir, eps, drop + PLATE_TOL_MM, (int)t, &hit)) drop = hit.t;
        ta[t] = a, ts[t] = a * std::max(0.0, -dot(p.fn[t], d)) * std::max(0.0, drop);
    });
    Overhangs r;
    for (size_t t = 0; t < nt; t++) r.area += ta[t], r.support += ts[t];
    if (keep) r.tri_area = std::move(ta), r.tri_support = std::move(ts);
    return r;
}

/* each triangle's depth of solid along its inward normal, to the surface it leaves the solid through; -1 where the ray
 * finds no way out (it then did not start inside) */
std::vector<double> thickness(const Part &p) {
    size_t nt = p.m.nt();
    std::vector<double> th(nt, -1);
    double eps = 1e-9 * std::max(1.0, p.size);
    parallel(nt, [&](size_t t) {
        if (p.area[t] == 0) return;
        V3 c = centroid(p.m, t), n = p.fn[t];
        double o[3] = {c.x, c.y, c.z}, dir[3] = {-n.x, -n.y, -n.z};
        BvhHit hit;
        if (bvh_ray_nearest(&p.bvh, o, dir, eps, 1e30, (int)t, &hit) && dot(p.fn[(size_t)hit.tri], n) < 0) th[t] = hit.t;
    });
    return th;
}

/* the flagged triangles in edge-connected regions */
std::vector<std::vector<int>> regions(const Part &p, const std::vector<char> &flag) {
    std::vector<std::vector<int>> out;
    std::vector<char> seen(flag.size(), 0);
    std::vector<int> stack;
    for (size_t s = 0; s < flag.size(); s++) {
        if (!flag[s] || seen[s]) continue;
        out.emplace_back();
        seen[s] = 1, stack.assign(1, (int)s);
        while (!stack.empty()) {
            int t = stack.back();
            stack.pop_back();
            out.back().push_back(t);
            for (int k = 0; k < 3; k++) {
                int n = p.e.nb[3 * (size_t)t + k];
                if (n >= 0 && flag[(size_t)n] && !seen[(size_t)n]) seen[(size_t)n] = 1, stack.push_back(n);
            }
        }
    }
    return out;
}

/* regions in the order of a key computed once for each */
template <class F> void sort_by(std::vector<std::vector<int>> &r, F key) {
    std::vector<std::pair<double, size_t>> k;
    for (size_t i = 0; i < r.size(); i++) k.push_back({key(r[i]), i});
    std::sort(k.begin(), k.end());
    std::vector<std::vector<int>> out;
    for (auto &x : k) out.push_back(std::move(r[x.second]));
    r = std::move(out);
}

JsonValue *nums(std::initializer_list<double> v) {
    JsonValue *a = json_array();
    for (double x : v) json_push(a, json_number(x));
    return a;
}
JsonValue *box_of(const Part &p, const std::vector<int> &tris) {
    V3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (int t : tris)
        for (int k = 0; k < 3; k++) {
            V3 q = p.m.corner((size_t)t, k);
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)}, hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
    return nums({lo.x, lo.y, lo.z, hi.x, hi.y, hi.z});
}

V3 parse_direction(const JsonValue *v) {
    V3 d{0, 0, 1};
    if (const char *s = json_str(v)) {
        std::string t = s;
        static const std::map<std::string, V3> axes = {{"+x", {1, 0, 0}}, {"-x", {-1, 0, 0}}, {"+y", {0, 1, 0}}, {"-y", {0, -1, 0}},
                                                       {"+z", {0, 0, 1}}, {"-z", {0, 0, -1}}, {"x", {1, 0, 0}}, {"y", {0, 1, 0}},
                                                       {"z", {0, 0, 1}}};
        if (!axes.count(t)) refuse("\"build_direction\" must be +x, -x, +y, -y, +z, -z or [x, y, z]");
        return axes.at(t);
    }
    double x[3];
    if (!json_get_numbers(v, x, 3)) refuse("\"build_direction\" must be +x, -x, +y, -y, +z, -z or [x, y, z]");
    d = {x[0], x[1], x[2]};
    double l = norm(d);
    if (!(l > 0) || !std::isfinite(l)) refuse("\"build_direction\" must not be zero");
    return d * (1 / l);
}

std::string axis_name(V3 d) {
    const char *n[6] = {"+x", "-x", "+y", "-y", "+z", "-z"};
    V3 a[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int i = 0; i < 6; i++)
        if (dot(d, a[i]) > 1 - 1e-12) return n[i];
    char b[96];
    std::snprintf(b, sizeof b, "[%.6g, %.6g, %.6g]", d.x, d.y, d.z);
    return b;
}

std::string fmt(const char *f, double a) {
    char b[64];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

/* the part with its triangles coloured (VisCAM and SolidView: bit 15 set, then 5 bits each of red, green, blue) */
bool write_coloured_stl(const Part &p, const std::vector<uint16_t> &colour, const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    char head[80] = {0};
    std::snprintf(head, sizeof head, "OpenPhysicsAI openphysics-cad check, millimetres, VisCAM colours");
    std::fwrite(head, 1, 80, f);
    uint32_t n = (uint32_t)p.m.nt();
    std::fwrite(&n, 4, 1, f);
    for (uint32_t t = 0; t < n; t++) {
        float rec[12] = {(float)p.fn[t].x, (float)p.fn[t].y, (float)p.fn[t].z};
        for (int k = 0; k < 3; k++) {
            V3 q = p.m.corner(t, k);
            rec[3 + 3 * k] = (float)q.x, rec[4 + 3 * k] = (float)q.y, rec[5 + 3 * k] = (float)q.z;
        }
        std::fwrite(rec, 4, 12, f);
        std::fwrite(&colour[t], 2, 1, f);
    }
    return std::fclose(f) == 0;
}

uint16_t rgb15(int r, int g, int b) { return (uint16_t)(0x8000 | (r & 31) << 10 | (g & 31) << 5 | (b & 31)); }

} // namespace

JsonValue *printcheck_run(const JsonValue *req, const std::string &outdir) {
    auto t0 = std::chrono::steady_clock::now();
    try {
        const char *part = json_get_str(req, "part", nullptr);
        if (!part) refuse("\"part\" is needed: a STEP or STL file");
        const char *process = json_get_str(req, "process", nullptr);
        if (!process)
            refuse("\"process\" is needed, as its ISO/ASTM 52900 category: powder_bed_fusion, binder_jetting, "
                   "vat_photopolymerization, material_jetting or material_extrusion");
        if (!KEPT.count(process))
            refuse(std::string("\"process\" \"") + process + "\" is not one checked here: powder_bed_fusion, binder_jetting, "
                   "vat_photopolymerization, material_jetting, material_extrusion");
        const JsonValue *ov = json_get(req, "overhang_deg");
        if (!ov || ov->type != JSON_NUMBER)
            refuse("\"overhang_deg\" is needed: the least angle between a downward surface and the plate that the process "
                   "prints without support (the process's or the machine's own value; it is not guessed here)");
        double limit = ov->u.number;
        if (!(limit > 0 && limit < 90)) refuse("\"overhang_deg\" must be above 0 and below 90");
        const JsonValue *mw = json_get(req, "min_wall_mm");
        if (!mw || mw->type != JSON_NUMBER)
            refuse("\"min_wall_mm\" is needed: the thinnest wall the process prints (the process's or the machine's own "
                   "value; it is not guessed here)");
        double min_wall = mw->u.number;
        if (!(min_wall > 0)) refuse("\"min_wall_mm\" must be above 0");
        const JsonValue *bd = json_get(req, "build_direction");
        V3 d = bd ? parse_direction(bd) : V3{0, 0, 1};
        double deflection = json_get_num(req, "deflection_mm", 0.01);
        if (!(deflection > 0)) refuse("\"deflection_mm\" must be above 0");

        /* the part */
        Part p;
        p.m = weld(load_part(part, deflection));
        Mesh &m = p.m;
        size_t nt = m.nt();
        if (!nt) refuse("the part holds no triangles with area");
        p.e = pair_edges(m);
        if (p.e.open_edges || p.e.shared_edges)
            refuse("the part's surface is not closed and manifold: " + std::to_string(p.e.open_edges) + " edges belong to one "
                   "triangle only and " + std::to_string(p.e.shared_edges) + " are shared by more than two (or twice the same "
                   "way); it must be repaired before it can be checked");
        p.shells = find_shells(m, p.e, p.shell_of);
        p.fn.resize(nt), p.area.resize(nt);
        p.lo = {1e300, 1e300, 1e300}, p.hi = {-1e300, -1e300, -1e300};
        for (size_t i = 0; i < m.nv(); i++) {
            V3 q = m.P((int)i);
            p.lo = {std::min(p.lo.x, q.x), std::min(p.lo.y, q.y), std::min(p.lo.z, q.z)};
            p.hi = {std::max(p.hi.x, q.x), std::max(p.hi.y, q.y), std::max(p.hi.z, q.z)};
        }
        p.size = std::max({p.hi.x - p.lo.x, p.hi.y - p.lo.y, p.hi.z - p.lo.z});
        double total_area = 0, volume = 0;
        for (size_t t = 0; t < nt; t++) {
            V3 c = cross(m.corner(t, 1) - m.corner(t, 0), m.corner(t, 2) - m.corner(t, 0));
            double l = norm(c);
            p.area[t] = 0.5 * l, p.fn[t] = l > 0 ? c * (1 / l) : V3{0, 0, 0};
            total_area += p.area[t];
        }
        for (const Shell &s : p.shells) volume += s.volume_mm3;
        corner_normals(p);
        if (!bvh_build(&p.bvh, m.v.data(), m.t.data(), (int)nt)) refuse("out of memory while checking the part");

        /* overhangs along the build direction, then along each axis */
        Overhangs oh = overhangs(p, d, limit, true);
        std::vector<char> flag(nt);
        for (size_t t = 0; t < nt; t++) flag[t] = oh.tri_area[t] > 0;
        auto oregs = regions(p, flag);
        sort_by(oregs, [&](const std::vector<int> &r) {
            double s = 0;
            for (int t : r) s -= oh.tri_area[(size_t)t];
            return s; /* the largest first */
        });
        const V3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        Overhangs per_axis[6];
        int best = 0;
        for (int i = 0; i < 6; i++) {
            per_axis[i] = overhangs(p, axes[i], limit, false);
            if (per_axis[i].support < per_axis[best].support - 1e-9 * std::max(1.0, per_axis[best].support) ||
                (std::fabs(per_axis[i].support - per_axis[best].support) <= 1e-9 * std::max(1.0, per_axis[best].support) &&
                 per_axis[i].area < per_axis[best].area))
                best = i;
        }

        /* walls */
        std::vector<double> th = thickness(p);
        long unmeasured = 0;
        double thinnest = -1;
        size_t thinnest_t = 0;
        std::vector<char> thin(nt, 0);
        double thin_area = 0;
        for (size_t t = 0; t < nt; t++) {
            if (p.area[t] == 0) continue;
            if (th[t] < 0) {
                unmeasured++;
                continue;
            }
            if (thinnest < 0 || th[t] < thinnest) thinnest = th[t], thinnest_t = t;
            if (th[t] < min_wall) thin[t] = 1, thin_area += p.area[t];
        }
        auto tregs = regions(p, thin);
        sort_by(tregs, [&](const std::vector<int> &r) {
            double v = 1e300;
            for (int t : r) v = std::min(v, th[(size_t)t]);
            return v; /* the thinnest first */
        });

        /* voids */
        std::vector<WayOut> ways = ways_out(m, p.shells, p.shell_of);
        std::sort(ways.begin(), ways.end(), [&](const WayOut &a, const WayOut &b) {
            return -p.shells[(size_t)a.shell].volume_mm3 > -p.shells[(size_t)b.shell].volume_mm3;
        });
        double void_volume = 0, outer_volume = 0;
        long bodies = 0;
        for (const Shell &s : p.shells)
            if (s.volume_mm3 < 0) void_volume -= s.volume_mm3;
            else outer_volume += s.volume_mm3, bodies++;
        const std::string &kept = KEPT.at(process);

        /* the verdict */
        JsonValue *issues = json_array(), *notes = json_array();
        std::string bestname = axis_name(axes[best]);
        if (oh.area > 0) {
            std::string s = fmt("%.4g", oh.area) + " mm^2 faces the plate at less than " + fmt("%.4g", limit) + " degrees and needs support (" +
                            fmt("%.4g", oh.support) + " mm^3 under it, in " + std::to_string(oregs.size()) + " region" + (oregs.size() == 1 ? "" : "s") + ")";
            if (per_axis[best].support < oh.support * (1 - 1e-9))
                s += "; built along " + bestname + " it would need " + fmt("%.4g", per_axis[best].support) + " mm^3";
            json_push(issues, json_string(s.c_str()));
        }
        if (thin_area > 0) {
            V3 at = centroid(m, thinnest_t);
            json_push(issues, json_string(("walls thinner than " + fmt("%.4g", min_wall) + " mm: " + fmt("%.4g", thin_area) + " mm^2 in " +
                                           std::to_string(tregs.size()) + " region" + (tregs.size() == 1 ? "" : "s") + ", the thinnest " +
                                           fmt("%.4g", thinnest) + " mm at (" + fmt("%.4g", at.x) + ", " + fmt("%.4g", at.y) + ", " +
                                           fmt("%.4g", at.z) + ")").c_str()));
        }
        if (!ways.empty()) {
            std::string n = std::to_string(ways.size()) + " enclosed void" + (ways.size() == 1 ? "" : "s") + " (" + fmt("%.4g", void_volume) + " mm^3)";
            if (!kept.empty())
                json_push(issues, json_string((n + " would keep the " + kept + " inside: give each a way out (cad_lattice drains, or "
                                                   "a hole cut with cad_part; each void's shortest way is listed)").c_str()));
            else json_push(notes, json_string((n + " print as closed cavities in material extrusion").c_str()));
        }
        if (bodies > 1) json_push(notes, json_string((std::to_string(bodies) + " separate solids").c_str()));
        if (unmeasured) json_push(notes, json_string((std::to_string(unmeasured) + " triangles' wall depth could not be measured").c_str()));

        /* the record: everything in OUTDIR/check.json, the largest of each list in the answer */
        auto build = [&](size_t cap) {
            JsonValue *rec = json_object(), *o, *list;
            json_set_bool(rec, "ok", true);
            o = json_set_object(rec, "part");
            json_set_string(o, "file", part);
            json_set_int(o, "triangles", (long long)nt);
            json_set(o, "box_mm", nums({p.lo.x, p.lo.y, p.lo.z, p.hi.x, p.hi.y, p.hi.z}));
            json_set_number(o, "volume_mm3", volume);
            json_set_number(o, "area_mm2", total_area);
            json_set_int(o, "solids", bodies);
            json_set_string(rec, "process", process);
            json_set(rec, "build_direction", nums({d.x, d.y, d.z}));
            json_set_bool(rec, "build_direction_given", bd != nullptr);
            o = json_set_object(rec, "overhang");
            json_set_number(o, "limit_deg", limit);
            json_set_number(o, "area_mm2", oh.area);
            json_set_number(o, "support_mm3", oh.support);
            json_set_int(o, "region_count", (long long)oregs.size());
            list = json_set_array(o, "regions");
            for (size_t i = 0; i < oregs.size() && i < cap; i++) {
                JsonValue *r = json_object();
                double a = 0, s = 0, lowest = 90;
                for (int t : oregs[i]) {
                    a += oh.tri_area[(size_t)t], s += oh.tri_support[(size_t)t];
                    lowest = std::min(lowest, std::acos(std::max(-1.0, std::min(1.0, -dot(p.fn[(size_t)t], d)))) * 180 / PI);
                }
                json_set_number(r, "area_mm2", a);
                json_set_number(r, "support_mm3", s);
                json_set_number(r, "lowest_angle_deg", lowest);
                json_set(r, "box_mm", box_of(p, oregs[i]));
                json_push(list, r);
            }
            list = json_set_array(rec, "orientations");
            for (int i = 0; i < 6; i++) {
                JsonValue *r = json_object();
                json_set_string(r, "build_direction", axis_name(axes[i]).c_str());
                json_set_number(r, "overhang_mm2", per_axis[i].area);
                json_set_number(r, "support_mm3", per_axis[i].support);
                json_push(list, r);
            }
            json_set_string(rec, "best_orientation", bestname.c_str());
            o = json_set_object(rec, "walls");
            json_set_number(o, "limit_mm", min_wall);
            json_set_number(o, "thinnest_mm", thinnest);
            if (thinnest >= 0) {
                V3 at = centroid(m, thinnest_t);
                json_set(o, "thinnest_at_mm", nums({at.x, at.y, at.z}));
            }
            json_set_number(o, "thin_area_mm2", thin_area);
            json_set_int(o, "region_count", (long long)tregs.size());
            json_set_int(o, "unmeasured_triangles", unmeasured);
            list = json_set_array(o, "regions");
            for (size_t i = 0; i < tregs.size() && i < cap; i++) {
                JsonValue *r = json_object();
                double a = 0, mn = 1e300;
                size_t mt = 0;
                for (int t : tregs[i]) {
                    a += p.area[(size_t)t];
                    if (th[(size_t)t] < mn) mn = th[(size_t)t], mt = (size_t)t;
                }
                V3 at = centroid(m, mt);
                json_set_number(r, "area_mm2", a);
                json_set_number(r, "thinnest_mm", mn);
                json_set(r, "at_mm", nums({at.x, at.y, at.z}));
                json_set(r, "box_mm", box_of(p, tregs[i]));
                json_push(list, r);
            }
            o = json_set_object(rec, "voids");
            json_set_int(o, "count", (long long)ways.size());
            json_set_number(o, "volume_mm3", void_volume);
            json_set_number(o, "outer_volume_mm3", outer_volume);
            json_set_string(o, "keeps", kept.empty() ? "nothing (closed cavities)" : kept.c_str());
            list = json_set_array(o, "list");
            for (size_t i = 0; i < ways.size() && i < cap; i++) {
                const Shell &s = p.shells[(size_t)ways[i].shell];
                JsonValue *r = json_object(), *w;
                json_set_number(r, "volume_mm3", -s.volume_mm3);
                json_set_number(r, "area_mm2", s.area_mm2);
                json_set(r, "box_mm", nums({s.lo.x, s.lo.y, s.lo.z, s.hi.x, s.hi.y, s.hi.z}));
                w = json_set_object(r, "way_out");
                json_set(w, "from_mm", nums({ways[i].from.x, ways[i].from.y, ways[i].from.z}));
                json_set(w, "to_mm", nums({ways[i].to.x, ways[i].to.y, ways[i].to.z}));
                json_set_number(w, "through_mm", ways[i].through_mm);
                json_push(list, r);
            }
            json_set_bool(rec, "needs_support", oh.area > 0);
            json_set_bool(rec, "too_thin", thin_area > 0);
            json_set_bool(rec, "traps_material", !ways.empty() && !kept.empty());
            json_set(rec, "issues", json_clone(issues));
            json_set(rec, "notes", json_clone(notes));
            return rec;
        };

        std::vector<uint16_t> colour(nt, 0);
        for (size_t t = 0; t < nt; t++) {
            bool o = oh.tri_area[t] > 0, w = thin[t] != 0, v = p.shells[(size_t)p.shell_of[t]].volume_mm3 < 0;
            colour[t] = o && w ? rgb15(31, 0, 31) : o ? rgb15(31, 0, 0) : w ? rgb15(0, 0, 31) : v ? rgb15(31, 16, 0) : 0;
        }
        std::string stl = outdir + "/checked.stl", full = outdir + "/check.json";
        if (!write_coloured_stl(p, colour, stl + ".tmp")) {
            std::remove((stl + ".tmp").c_str());
            json_free(issues), json_free(notes);
            refuse("cannot write into " + outdir);
        }
        JsonValue *all = build((size_t)-1);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        json_set_number(all, "seconds", secs);
        bool wrote = json_write_file((full + ".tmp").c_str(), all, JSON_PRETTY);
        json_free(all);
        if (!wrote) {
            std::remove((stl + ".tmp").c_str()), std::remove((full + ".tmp").c_str());
            json_free(issues), json_free(notes);
            refuse("cannot write into " + outdir);
        }
        std::rename((stl + ".tmp").c_str(), stl.c_str());
        std::rename((full + ".tmp").c_str(), full.c_str());
        JsonValue *rec = build(20);
        json_free(issues), json_free(notes);
        json_set_number(rec, "seconds", secs);
        JsonValue *files = json_set_object(rec, "files");
        json_set_string(files, "stl", stl.c_str());
        json_set_string(files, "all", full.c_str());
        return rec;
    } catch (Refusal &r) {
        JsonValue *o = json_object(), *e;
        json_set_bool(o, "ok", false);
        e = json_set_object(o, "error");
        json_set_string(e, "message", r.what());
        return o;
    }
}
