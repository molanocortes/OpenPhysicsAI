/* cadmesh.h - a part as triangles, for openphysics-cad's mesh tools (lattice.cpp, printcheck.cpp; docs/cad.md): read from
 * STEP or STL, joined into one indexed mesh, its edges paired, its closed surfaces (shells) found and measured, and for
 * every enclosed void the shortest way out through the solid. */
#pragma once

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace cadmesh {

/* a request that cannot be done, with its reason, said to the caller as it is */
struct Refusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] inline void refuse(const std::string &m) { throw Refusal(m); }

struct V3 {
    double x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double norm(V3 a) { return std::sqrt(dot(a, a)); }

struct Tri {
    V3 a, b, c;
};

/* the part's triangles: a STEP file (.step, .stp) tessellated by Open CASCADE within deflection_mm of its surfaces, or
 * an STL (binary or ASCII); turned to face outwards if, all together, they faced inwards. Refuses with the reason. */
std::vector<Tri> load_part(const std::string &path, double deflection_mm);

/* one indexed mesh: 3 coordinates a vertex, 3 vertex indices a triangle */
struct Mesh {
    std::vector<double> v;
    std::vector<int> t;
    size_t nv() const { return v.size() / 3; }
    size_t nt() const { return t.size() / 3; }
    V3 P(int i) const { return {v[3 * (size_t)i], v[3 * (size_t)i + 1], v[3 * (size_t)i + 2]}; }
    V3 corner(size_t tri, int k) const { return P(t[3 * tri + k]); }
};

/* triangles joined where their corners are the same point exactly (a STEP tessellation shares its edges' points, an
 * STL its 32-bit floats); triangles left with two corners on one point are dropped (they have no area) */
Mesh weld(const std::vector<Tri> &tris);

/* each triangle's neighbour across each of its edges (edge k runs from corner k to corner k + 1): nb[3 t + k], -1 where
 * the surface is open; open_edges counts edges with no partner, shared_edges those claimed by more than two triangles
 * (or twice the same way). A closed, consistently oriented surface has neither. */
struct Edges {
    std::vector<int> nb;
    long open_edges = 0, shared_edges = 0;
};
Edges pair_edges(const Mesh &m);

/* the closed surfaces, edge-connected: an outer surface encloses a positive volume, the surface of a void inside the
 * solid a negative one (its triangles face into the void) */
struct Shell {
    double volume_mm3 = 0, area_mm2 = 0;
    V3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    long triangles = 0;
};
std::vector<Shell> find_shells(const Mesh &m, const Edges &e, std::vector<int> &shell_of);

/* for every void (a shell of negative volume), the shortest way out: the point of the void's surface nearest an outer
 * surface (from up to 4000 of the void's vertices), that outer point, and the depth of solid between; a point where
 * the void touches the outside (depth 0) is passed over, since nothing drains through a point */
struct WayOut {
    int shell = -1;
    V3 from{0, 0, 0}, to{0, 0, 0};
    double through_mm = 0;
};
std::vector<WayOut> ways_out(const Mesh &m, const std::vector<Shell> &shells, const std::vector<int> &shell_of);

} // namespace cadmesh
