// cadtest.cpp - GOALS.md G23 step 1: the geometry libraries built by tools/build_cad.py compute what they must.
//
//     make build/cadtest && ./build/cadtest
//
// Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 1), amended only openly below them:
//   C1 a 40 x 30 x 20 mm box less a 5 mm radius hole through it along z: Open CASCADE's volume is 24000 - 500 pi mm^3
//      within 1e-9 relative
//   C2 a 20 mm cube with one edge rounded at 3 mm: 8000 - (1 - pi / 4) x 9 x 20 mm^3 within 1e-7 relative
//   C3 a 10 mm (radial) by 4 mm (axial) rectangle whose inner side is 20 mm from the z axis, revolved a full turn:
//      2 pi x 25 x 40 mm^3 (Pappus) within 1e-9 relative
//   C4 C1 written to STEP and read back: one valid solid of the same volume within 1e-9 relative
//   C5 C1 tessellated at 0.01 mm chordal deflection: closed and manifold (every edge in exactly two triangles, once in
//      each direction), its volume within 0.1 per cent of C1's
//   M1 Manifold's level set of the gyroid's solid network (where sin x cos y + sin y cos z + sin z cos x < 0) over
//      4 x 4 x 4 cells of 10 mm: closed and manifold, solid fraction 0.5 within 0.005 (exactly 0.5 by symmetry)
//   M2 two overlapping Manifold spheres A and B: |A u B| = |A| + |B| - |A n B| and |A - B| = |A| - |A n B| within
//      1e-9 relative
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>

#include <manifold/manifold.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

static int failures;
static void check(bool ok, const char *what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p, 1e-12); // adaptive integration to 1e-12 relative
    return p.Mass();
}

static double rel(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

int main() {
    const double pi = std::acos(-1.0);
    char line[400];

    // C1: a box less a through hole
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 40, 30, 20).Shape();
    TopoDS_Shape bore = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 5, 22).Shape();
    BRepAlgoAPI_Cut cut(box, bore);
    cut.Build();
    TopoDS_Shape c1 = cut.Shape();
    double v1 = volume(c1), e1 = 24000 - 500 * pi;
    std::snprintf(line, sizeof line, "C1 box less a hole: %.12g mm^3 against %.12g (relative %.2e)", v1, e1, rel(v1, e1));
    check(cut.IsDone() && rel(v1, e1) < 1e-9, line);

    // C2: one edge of a cube rounded
    TopoDS_Shape cube = BRepPrimAPI_MakeBox(20, 20, 20).Shape();
    BRepFilletAPI_MakeFillet fillet(cube);
    TopExp_Explorer edges(cube, TopAbs_EDGE);
    fillet.Add(3.0, TopoDS::Edge(edges.Current()));
    fillet.Build();
    double v2 = fillet.IsDone() ? volume(fillet.Shape()) : 0, e2 = 8000 - (1 - pi / 4) * 9 * 20;
    std::snprintf(line, sizeof line, "C2 a rounded edge: %.12g mm^3 against %.12g (relative %.2e)", v2, e2, rel(v2, e2));
    check(fillet.IsDone() && rel(v2, e2) < 1e-7, line);

    // C3: a rectangle revolved (Pappus)
    BRepBuilderAPI_MakePolygon poly(gp_Pnt(20, 0, 0), gp_Pnt(30, 0, 0), gp_Pnt(30, 0, 4), gp_Pnt(20, 0, 4), Standard_True);
    TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire()).Face();
    TopoDS_Shape ring = BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
    double v3 = volume(ring), e3 = 2 * pi * 25 * 40;
    std::snprintf(line, sizeof line, "C3 a revolved rectangle: %.12g mm^3 against %.12g (relative %.2e)", v3, e3, rel(v3, e3));
    check(rel(v3, e3) < 1e-9, line);

    // C4: STEP out and back
    const char *step = "build/cadtest.step";
    STEPControl_Writer writer;
    bool wrote = writer.Transfer(c1, STEPControl_AsIs) == IFSelect_RetDone && writer.Write(step) == IFSelect_RetDone;
    STEPControl_Reader reader;
    bool read = reader.ReadFile(step) == IFSelect_RetDone && reader.TransferRoots() > 0;
    TopoDS_Shape back = read ? reader.OneShape() : TopoDS_Shape();
    int solids = 0;
    for (TopExp_Explorer ex(back, TopAbs_SOLID); ex.More(); ex.Next()) solids++;
    bool valid = read && BRepCheck_Analyzer(back).IsValid();
    double v4 = read ? volume(back) : 0;
    std::snprintf(line, sizeof line, "C4 STEP and back: %d solid(s), valid %s, %.12g mm^3 (relative %.2e)", solids, valid ? "yes" : "no", v4,
                  rel(v4, v1));
    check(wrote && read && solids == 1 && valid && rel(v4, v1) < 1e-9, line);
    std::remove(step);

    // C5: tessellated, closed and manifold
    BRepMesh_IncrementalMesh mesher(c1, 0.01, Standard_False, 0.1, Standard_True);
    std::map<std::tuple<double, double, double>, int> index;
    std::vector<gp_Pnt> pts;
    std::vector<std::array<int, 3>> tris;
    for (TopExp_Explorer ex(c1, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(f, loc);
        if (t.IsNull()) continue;
        std::vector<int> id(t->NbNodes() + 1);
        for (int i = 1; i <= t->NbNodes(); i++) {
            gp_Pnt p = t->Node(i).Transformed(loc.Transformation());
            auto key = std::make_tuple(p.X(), p.Y(), p.Z());
            auto it = index.find(key);
            if (it == index.end()) it = index.emplace(key, (int)pts.size()).first, pts.push_back(p);
            id[i] = it->second;
        }
        bool flip = f.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= t->NbTriangles(); i++) {
            int a, b, c;
            t->Triangle(i).Get(a, b, c);
            tris.push_back(flip ? std::array<int, 3>{id[a], id[c], id[b]} : std::array<int, 3>{id[a], id[b], id[c]});
        }
    }
    std::map<std::pair<int, int>, int> directed;
    double v5 = 0;
    for (auto &t : tris) {
        for (int k = 0; k < 3; k++) directed[{t[k], t[(k + 1) % 3]}]++;
        const gp_Pnt &a = pts[t[0]], &b = pts[t[1]], &c = pts[t[2]];
        v5 += (a.X() * (b.Y() * c.Z() - b.Z() * c.Y()) - a.Y() * (b.X() * c.Z() - b.Z() * c.X()) + a.Z() * (b.X() * c.Y() - b.Y() * c.X())) / 6;
    }
    bool manifold = !tris.empty();
    for (auto &e : directed)
        if (e.second != 1 || directed.count({e.first.second, e.first.first}) != 1) manifold = false;
    std::snprintf(line, sizeof line, "C5 tessellated at 0.01 mm: %zu triangles, closed and manifold %s, %.10g mm^3 (relative %.2e)", tris.size(),
                  manifold ? "yes" : "no", v5, rel(v5, v1));
    check(manifold && rel(v5, v1) < 1e-3, line);

    // M1: the gyroid's solid network over 4 x 4 x 4 cells of 10 mm, centred so that p -> -p maps the box onto itself
    const double cell = 10, half = 20, k = 2 * pi / cell;
    auto inside = [&](manifold::vec3 p) {
        double g = std::sin(k * p.x) * std::cos(k * p.y) + std::sin(k * p.y) * std::cos(k * p.z) + std::sin(k * p.z) * std::cos(k * p.x);
        double b = std::fmin(std::fmin(half - std::fabs(p.x), half - std::fabs(p.y)), half - std::fabs(p.z));
        return std::fmin(-g, b); // positive inside: the network's side, within the box
    };
    manifold::Box bounds(manifold::vec3(-half - 1), manifold::vec3(half + 1));
    manifold::Manifold gyroid = manifold::Manifold::LevelSet(inside, bounds, 0.25, 0, 1e-6);
    double frac = gyroid.Volume() / (8 * half * half * half);
    std::snprintf(line, sizeof line, "M1 gyroid network: status %d, %zu triangles, genus %d, solid fraction %.5f", (int)gyroid.Status(),
                  gyroid.NumTri(), gyroid.Genus(), frac);
    check(gyroid.Status() == manifold::Manifold::Error::NoError && gyroid.NumTri() > 0 && std::fabs(frac - 0.5) < 0.005, line);

    // M2: inclusion and exclusion on two spheres
    manifold::Manifold A = manifold::Manifold::Sphere(10, 96), B = manifold::Manifold::Sphere(8, 96).Translate(manifold::vec3(12, 0, 0));
    double va = A.Volume(), vb = B.Volume(), vu = (A + B).Volume(), vi = (A ^ B).Volume(), vd = (A - B).Volume();
    std::snprintf(line, sizeof line, "M2 spheres: union %.12g against %.12g (%.2e), difference %.12g against %.12g (%.2e)", vu, va + vb - vi,
                  rel(vu, va + vb - vi), vd, va - vi, rel(vd, va - vi));
    check(vi > 0 && rel(vu, va + vb - vi) < 1e-9 && rel(vd, va - vi) < 1e-9, line);

    std::printf(failures ? "cadtest: %d FAILED\n" : "cadtest: all passed\n", failures);
    return failures ? 1 : 0;
}
