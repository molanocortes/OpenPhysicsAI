// cadpart.cpp - exact parts from a feature list (see cadpart.h; GOALS.md G23 step 2)
#include "cadpart.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Refusal {
    std::string feature, op, message;
};

const double PI = 3.14159265358979323846;

[[noreturn]] void refuse(const std::string &msg) { throw Refusal{"", "", msg}; }

double num(const JsonValue *f, const char *key, bool need = true, double def = 0) {
    const JsonValue *v = json_get(f, key);
    if (!v) {
        if (need) refuse(std::string("\"") + key + "\" is needed");
        return def;
    }
    if (v->type != JSON_NUMBER) refuse(std::string("\"") + key + "\" must be a number");
    return v->u.number;
}

double positive(const JsonValue *f, const char *key) {
    double x = num(f, key);
    if (!(x > 0)) refuse(std::string("\"") + key + "\" must be above 0");
    return x;
}

gp_Pnt point(const JsonValue *f, const char *key, gp_Pnt def = gp_Pnt(0, 0, 0)) {
    const JsonValue *v = json_get(f, key);
    if (!v) return def;
    double x[3];
    if (!json_get_numbers(v, x, 3)) refuse(std::string("\"") + key + "\" must be three numbers [x, y, z] in mm");
    return gp_Pnt(x[0], x[1], x[2]);
}

/* "x", "-y", "+z" or [x, y, z] */
gp_Dir axis(const JsonValue *v, const char *key, gp_Dir def = gp_Dir(0, 0, 1)) {
    if (!v) return def;
    if (v->type == JSON_STRING) {
        std::string s = json_str(v);
        double sign = 1;
        if (!s.empty() && (s[0] == '+' || s[0] == '-')) sign = s[0] == '-' ? -1 : 1, s = s.substr(1);
        if (s == "x") return gp_Dir(sign, 0, 0);
        if (s == "y") return gp_Dir(0, sign, 0);
        if (s == "z") return gp_Dir(0, 0, sign);
    }
    double x[3];
    if (json_get_numbers(v, x, 3) && std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) > 1e-12) return gp_Dir(x[0], x[1], x[2]);
    refuse(std::string("\"") + key + "\" must be \"x\", \"y\", \"z\" (with + or -) or a direction [x, y, z]");
}

std::string text(const JsonValue *f, const char *key, bool need = true) {
    const char *s = json_get_str(f, key, nullptr);
    if (!s) {
        if (need) refuse(std::string("\"") + key + "\" is needed (text)");
        return "";
    }
    return s;
}

/* ---- measuring ---- */

double volume_of(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p, 1e-10);
    return p.Mass();
}

double area_of(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(s, p, 1e-10);
    return p.Mass();
}

gp_Pnt centre_of(const TopoDS_Shape &s) {
    GProp_GProps p;
    if (s.ShapeType() == TopAbs_EDGE) BRepGProp::LinearProperties(s, p);
    else BRepGProp::SurfaceProperties(s, p);
    return p.CentreOfMass();
}

int count_of(const TopoDS_Shape &s, TopAbs_ShapeEnum kind) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, kind, m);
    return m.Extent();
}

JsonValue *numbers(std::initializer_list<double> v) {
    JsonValue *a = json_array();
    for (double x : v) json_push(a, json_number(std::fabs(x) < 1e-12 ? 0 : x));
    return a;
}

JsonValue *measure(const TopoDS_Shape &s) {
    JsonValue *o = json_object();
    int solids = count_of(s, TopAbs_SOLID);
    json_set_number(o, "volume_mm3", solids ? volume_of(s) : 0);
    json_set_number(o, "area_mm2", area_of(s));
    Bnd_Box b;
    BRepBndLib::AddOptimal(s, b, Standard_False, Standard_False);
    if (!b.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        json_set(o, "box_mm", numbers({x0, y0, z0, x1, y1, z1}));
    }
    json_set_int(o, "solids", solids);
    json_set_int(o, "faces", count_of(s, TopAbs_FACE));
    json_set_bool(o, "valid", BRepCheck_Analyzer(s).IsValid() == Standard_True);
    return o;
}

/* the outward normal of a face at the middle of its parameters */
gp_Dir normal_of(const TopoDS_Face &f) {
    BRepAdaptor_Surface s(f);
    double u = 0.5 * (s.FirstUParameter() + s.LastUParameter()), v = 0.5 * (s.FirstVParameter() + s.LastVParameter());
    gp_Pnt p;
    gp_Vec du, dv;
    s.D1(u, v, p, du, dv);
    gp_Vec n = du.Crossed(dv);
    if (n.Magnitude() < 1e-15) return gp_Dir(0, 0, 1);
    if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
    return gp_Dir(n);
}

const char *face_kind(const TopoDS_Face &f) {
    switch (BRepAdaptor_Surface(f).GetType()) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    default: return "surface";
    }
}

const char *edge_kind(const TopoDS_Edge &e) {
    switch (BRepAdaptor_Curve(e).GetType()) {
    case GeomAbs_Line: return "line";
    case GeomAbs_Circle: return "circle";
    default: return "curve";
    }
}

/* every face with its kind and parameters, F1, F2... in the order Open CASCADE maps them (the same for the same list) */
JsonValue *faces_of(const TopoDS_Shape &s) {
    JsonValue *list = json_array();
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, TopAbs_FACE, m);
    for (int i = 1; i <= m.Extent(); i++) {
        TopoDS_Face f = TopoDS::Face(m(i));
        BRepAdaptor_Surface a(f);
        JsonValue *o = json_object();
        char name[16];
        std::snprintf(name, sizeof name, "F%d", i);
        json_set_string(o, "name", name);
        json_set_string(o, "kind", face_kind(f));
        json_set_number(o, "area_mm2", area_of(f));
        gp_Pnt c = centre_of(f);
        json_set(o, "centroid_mm", numbers({c.X(), c.Y(), c.Z()}));
        switch (a.GetType()) {
        case GeomAbs_Plane: {
            gp_Dir n = normal_of(f);
            json_set(o, "normal", numbers({n.X(), n.Y(), n.Z()}));
            break;
        }
        case GeomAbs_Cylinder: {
            gp_Cylinder cy = a.Cylinder();
            gp_Dir d = cy.Axis().Direction();
            gp_Pnt p = cy.Axis().Location();
            json_set(o, "axis", numbers({d.X(), d.Y(), d.Z()}));
            json_set(o, "through_mm", numbers({p.X(), p.Y(), p.Z()}));
            json_set_number(o, "radius_mm", cy.Radius());
            break;
        }
        case GeomAbs_Cone: {
            gp_Cone co = a.Cone();
            gp_Dir d = co.Axis().Direction();
            gp_Pnt p = co.Apex();
            json_set(o, "axis", numbers({d.X(), d.Y(), d.Z()}));
            json_set(o, "apex_mm", numbers({p.X(), p.Y(), p.Z()}));
            json_set_number(o, "half_angle_deg", co.SemiAngle() * 180 / PI);
            break;
        }
        case GeomAbs_Sphere: {
            gp_Sphere sp = a.Sphere();
            gp_Pnt p = sp.Location();
            json_set(o, "centre_mm", numbers({p.X(), p.Y(), p.Z()}));
            json_set_number(o, "radius_mm", sp.Radius());
            break;
        }
        case GeomAbs_Torus: {
            gp_Torus t = a.Torus();
            gp_Dir d = t.Axis().Direction();
            gp_Pnt p = t.Location();
            json_set(o, "axis", numbers({d.X(), d.Y(), d.Z()}));
            json_set(o, "centre_mm", numbers({p.X(), p.Y(), p.Z()}));
            json_set_number(o, "major_radius_mm", t.MajorRadius());
            json_set_number(o, "minor_radius_mm", t.MinorRadius());
            break;
        }
        default: break;
        }
        json_push(list, o);
    }
    return list;
}

/* ---- choosing edges and faces by their geometry ---- */

bool parallel(const gp_Dir &a, const gp_Dir &b) { return std::fabs(a.Dot(b)) > 1 - 1e-9; }

std::vector<TopoDS_Shape> select_one(const TopoDS_Shape &s, TopAbs_ShapeEnum kind, const JsonValue *sel) {
    if (!sel || sel->type != JSON_OBJECT) refuse("a selector is an object such as {\"parallel\": \"z\", \"between\": \"planes\"}");
    static const char *known[] = {"parallel", "between", "extreme", "kind", "near_mm", "count"};
    for (size_t i = 0; i < json_len(sel); i++) {
        const char *k = json_key_at(sel, i);
        if (std::none_of(std::begin(known), std::end(known), [&](const char *n) { return !std::strcmp(n, k); }))
            refuse(std::string("selector key \"") + k + "\" is not known: parallel, between, extreme, kind, near_mm, count");
    }
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, kind, m);
    TopTools_IndexedDataMapOfShapeListOfShape faces_of_edge;
    if (kind == TopAbs_EDGE) TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, faces_of_edge);
    std::vector<TopoDS_Shape> out;
    for (int i = 1; i <= m.Extent(); i++) {
        const TopoDS_Shape &e = m(i);
        if (const char *k = json_get_str(sel, "kind", nullptr)) {
            const char *have = kind == TopAbs_EDGE ? edge_kind(TopoDS::Edge(e)) : face_kind(TopoDS::Face(e));
            if (std::strcmp(k, have)) continue;
        }
        if (const JsonValue *p = json_get(sel, "parallel")) {
            gp_Dir d = axis(p, "parallel");
            if (kind == TopAbs_EDGE) {
                BRepAdaptor_Curve c(TopoDS::Edge(e));
                if (c.GetType() != GeomAbs_Line || !parallel(c.Line().Direction(), d)) continue;
            } else {
                TopoDS_Face f = TopoDS::Face(e);
                if (BRepAdaptor_Surface(f).GetType() != GeomAbs_Plane || !parallel(normal_of(f), d)) continue;
            }
        }
        if (const char *b = json_get_str(sel, "between", nullptr)) {
            if (kind != TopAbs_EDGE || std::strcmp(b, "planes")) refuse("\"between\": \"planes\" chooses edges between two planar faces");
            const TopTools_ListOfShape &fl = faces_of_edge.FindFromKey(e);
            std::set<const void *> distinct;
            bool planes = true;
            for (TopTools_ListOfShape::Iterator it(fl); it.More(); it.Next()) {
                distinct.insert(it.Value().TShape().get());
                if (BRepAdaptor_Surface(TopoDS::Face(it.Value())).GetType() != GeomAbs_Plane) planes = false;
            }
            if (!planes || distinct.size() < 2) continue;
        }
        out.push_back(e);
    }
    if (const JsonValue *x = json_get(sel, "extreme")) { /* the furthest along a direction, by centroid or midpoint */
        gp_Dir d = axis(x, "extreme");
        double best = -1e300, size = 0;
        Bnd_Box b;
        BRepBndLib::Add(s, b);
        if (!b.IsVoid()) size = std::sqrt(b.SquareExtent());
        std::vector<double> at;
        for (auto &e : out) {
            gp_Pnt c = centre_of(e);
            at.push_back(c.X() * d.X() + c.Y() * d.Y() + c.Z() * d.Z());
            best = std::max(best, at.back());
        }
        std::vector<TopoDS_Shape> keep;
        for (size_t i = 0; i < out.size(); i++)
            if (at[i] >= best - 1e-7 * (size + 1)) keep.push_back(out[i]);
        out.swap(keep);
    }
    if (json_get(sel, "near_mm")) { /* the nearest to a point */
        gp_Pnt p = point(sel, "near_mm");
        int n = (int)num(sel, "count", false, 1);
        if (n < 1) refuse("\"count\" must be 1 or more");
        TopoDS_Shape v = BRepBuilderAPI_MakeVertex(p).Shape();
        std::vector<std::pair<double, size_t>> d;
        for (size_t i = 0; i < out.size(); i++) {
            BRepExtrema_DistShapeShape ds(v, out[i]);
            d.push_back({ds.IsDone() ? ds.Value() : 1e300, i});
        }
        std::stable_sort(d.begin(), d.end());
        std::vector<TopoDS_Shape> keep;
        for (int i = 0; i < n && i < (int)d.size(); i++) keep.push_back(out[d[i].second]);
        out.swap(keep);
    }
    return out;
}

std::vector<TopoDS_Shape> select(const TopoDS_Shape &s, TopAbs_ShapeEnum kind, const JsonValue *sel, const char *key) {
    if (!sel) refuse(std::string("\"") + key + "\" is needed: a selector, or a list of them");
    std::vector<TopoDS_Shape> out;
    auto add = [&](const std::vector<TopoDS_Shape> &v) {
        for (auto &e : v)
            if (std::none_of(out.begin(), out.end(), [&](const TopoDS_Shape &o) { return o.IsSame(e); })) out.push_back(e);
    };
    if (sel->type == JSON_ARRAY)
        for (size_t i = 0; i < json_len(sel); i++) add(select_one(s, kind, json_at(sel, i)));
    else add(select_one(s, kind, sel));
    if (out.empty()) refuse(std::string("\"") + key + "\" chose no " + (kind == TopAbs_EDGE ? "edge" : "face"));
    return out;
}

/* ---- the features ---- */

struct Part {
    std::map<std::string, TopoDS_Shape> shapes;
    std::map<std::string, gp_Dir> sketch_normal;
};

const TopoDS_Shape &shape_of(Part &P, const JsonValue *f, const char *key) {
    std::string id = text(f, key);
    auto it = P.shapes.find(id);
    if (it == P.shapes.end()) refuse(std::string("\"") + key + "\": no feature \"" + id + "\" before this one");
    return it->second;
}

std::vector<TopoDS_Shape> shapes_of(Part &P, const JsonValue *f, const char *key, size_t at_least) {
    const JsonValue *l = json_get(f, key);
    if (!l || l->type != JSON_ARRAY || json_len(l) < at_least)
        refuse(std::string("\"") + key + "\" must list at least " + std::to_string(at_least) + " feature id(s)");
    std::vector<TopoDS_Shape> out;
    for (size_t i = 0; i < json_len(l); i++) {
        const char *id = json_str(json_at(l, i));
        auto it = id ? P.shapes.find(id) : P.shapes.end();
        if (it == P.shapes.end()) refuse(std::string("\"") + key + "\": no feature \"" + (id ? id : "?") + "\" before this one");
        out.push_back(it->second);
    }
    return out;
}

template <class Op> TopoDS_Shape boolean(const TopoDS_Shape &a, const TopoDS_Shape &b, const char *what) {
    Op op(a, b);
    if (!op.IsDone() || op.HasErrors()) refuse(std::string("the ") + what + " could not be made");
    return op.Shape();
}

/* the sketch plane's 3D point for (u, v), and its normal */
gp_Pnt on_plane(const std::string &plane, double off, double u, double v) {
    if (plane == "xy") return gp_Pnt(u, v, off);
    if (plane == "xz") return gp_Pnt(u, off, v);
    return gp_Pnt(off, u, v);
}

gp_Dir plane_normal(const std::string &plane) {
    if (plane == "xy") return gp_Dir(0, 0, 1);
    if (plane == "xz") return gp_Dir(0, 1, 0);
    return gp_Dir(1, 0, 0);
}

TopoDS_Shape transformed(const TopoDS_Shape &s, const gp_Trsf &t) { return BRepBuilderAPI_Transform(s, t, Standard_True).Shape(); }

TopoDS_Shape feature(Part &P, const JsonValue *f, const std::string &op, int *chosen) {
    if (op == "box") {
        const JsonValue *size = json_get(f, "size_mm");
        double d[3];
        if (!size || !json_get_numbers(size, d, 3) || !(d[0] > 0 && d[1] > 0 && d[2] > 0))
            refuse("\"size_mm\" must be three sizes above 0, [x, y, z]");
        gp_Pnt at = point(f, "at_mm");
        if (json_get(f, "centre_mm")) {
            gp_Pnt c = point(f, "centre_mm");
            at = gp_Pnt(c.X() - d[0] / 2, c.Y() - d[1] / 2, c.Z() - d[2] / 2);
        }
        return BRepPrimAPI_MakeBox(at, d[0], d[1], d[2]).Shape();
    }
    if (op == "cylinder") {
        gp_Ax2 ax(point(f, "at_mm"), axis(json_get(f, "axis"), "axis"));
        return BRepPrimAPI_MakeCylinder(ax, positive(f, "radius_mm"), positive(f, "height_mm")).Shape();
    }
    if (op == "cone") {
        gp_Ax2 ax(point(f, "at_mm"), axis(json_get(f, "axis"), "axis"));
        double r1 = num(f, "radius1_mm"), r2 = num(f, "radius2_mm");
        if (r1 < 0 || r2 < 0 || (r1 == 0 && r2 == 0)) refuse("\"radius1_mm\" and \"radius2_mm\": 0 or more, not both 0");
        return BRepPrimAPI_MakeCone(ax, r1, r2, positive(f, "height_mm")).Shape();
    }
    if (op == "sphere") return BRepPrimAPI_MakeSphere(point(f, "centre_mm"), positive(f, "radius_mm")).Shape();
    if (op == "sketch") {
        std::string plane = text(f, "plane");
        if (plane != "xy" && plane != "xz" && plane != "yz") refuse("\"plane\" must be \"xy\", \"xz\" or \"yz\"");
        double off = num(f, "offset_mm", false, 0);
        TopoDS_Wire wire;
        if (const JsonValue *c = json_get(f, "circle")) {
            double uv[2];
            if (!json_get_numbers(json_get(c, "centre_mm"), uv, 2)) refuse("\"circle\": \"centre_mm\" must be [u, v]");
            gp_Ax2 ax(on_plane(plane, off, uv[0], uv[1]), plane_normal(plane));
            wire = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(ax, positive(c, "radius_mm"))).Edge()).Wire();
        } else {
            const JsonValue *pts = json_get(f, "points_mm");
            if (!pts || pts->type != JSON_ARRAY || json_len(pts) < 3) refuse("\"points_mm\" must list at least three [u, v] points, or give a \"circle\"");
            BRepBuilderAPI_MakePolygon poly;
            for (size_t i = 0; i < json_len(pts); i++) {
                double uv[2];
                if (!json_get_numbers(json_at(pts, i), uv, 2)) refuse("\"points_mm\": each point is [u, v] in mm");
                poly.Add(on_plane(plane, off, uv[0], uv[1]));
            }
            poly.Close();
            if (!poly.IsDone()) refuse("the points do not make a closed outline (repeated or too close)");
            wire = poly.Wire();
        }
        BRepBuilderAPI_MakeFace face(wire, Standard_True);
        if (!face.IsDone()) refuse("the outline does not make a flat face");
        P.sketch_normal[text(f, "id")] = plane_normal(plane);
        return face.Face();
    }
    if (op == "extrude") {
        const TopoDS_Shape &s = shape_of(P, f, "sketch");
        auto n = P.sketch_normal.find(text(f, "sketch"));
        if (n == P.sketch_normal.end()) refuse("\"sketch\" must name a sketch");
        double d = num(f, "distance_mm");
        if (d == 0) refuse("\"distance_mm\" must not be 0 (negative extrudes the other way)");
        return BRepPrimAPI_MakePrism(s, gp_Vec(n->second) * d, Standard_True).Shape();
    }
    if (op == "revolve") {
        const TopoDS_Shape &s = shape_of(P, f, "sketch");
        if (!P.sketch_normal.count(text(f, "sketch"))) refuse("\"sketch\" must name a sketch");
        gp_Ax1 ax(point(f, "axis_point_mm"), axis(json_get(f, "axis"), "axis"));
        double a = num(f, "angle_deg", false, 360);
        if (!(a > 0 && a <= 360)) refuse("\"angle_deg\" must be above 0 and at most 360");
        if (a == 360) return BRepPrimAPI_MakeRevol(s, ax, Standard_True).Shape();
        return BRepPrimAPI_MakeRevol(s, ax, a * PI / 180, Standard_True).Shape();
    }
    if (op == "union") {
        auto l = shapes_of(P, f, "of", 2);
        TopoDS_Shape r = l[0];
        for (size_t i = 1; i < l.size(); i++) r = boolean<BRepAlgoAPI_Fuse>(r, l[i], "union");
        return r;
    }
    if (op == "cut") {
        TopoDS_Shape r = shape_of(P, f, "from");
        for (auto &t : shapes_of(P, f, "remove", 1)) r = boolean<BRepAlgoAPI_Cut>(r, t, "cut");
        return r;
    }
    if (op == "intersect") {
        auto l = shapes_of(P, f, "of", 2);
        TopoDS_Shape r = l[0];
        for (size_t i = 1; i < l.size(); i++) r = boolean<BRepAlgoAPI_Common>(r, l[i], "intersection");
        return r;
    }
    if (op == "translate") {
        gp_Trsf t;
        gp_Pnt by = point(f, "by_mm");
        t.SetTranslation(gp_Vec(by.X(), by.Y(), by.Z()));
        return transformed(shape_of(P, f, "of"), t);
    }
    if (op == "rotate") {
        gp_Trsf t;
        t.SetRotation(gp_Ax1(point(f, "about_mm"), axis(json_get(f, "axis"), "axis")), num(f, "angle_deg") * PI / 180);
        return transformed(shape_of(P, f, "of"), t);
    }
    if (op == "pattern") { /* copies of a feature, joined: a row (step) or a ring (about an axis) */
        const TopoDS_Shape &s = shape_of(P, f, "of");
        int n = (int)num(f, "count");
        if (n < 2 || n > 1000) refuse("\"count\" must be 2 to 1000");
        TopoDS_Shape r = s;
        for (int i = 1; i < n; i++) {
            gp_Trsf t;
            if (json_get(f, "step_mm")) {
                gp_Pnt d = point(f, "step_mm");
                t.SetTranslation(gp_Vec(d.X() * i, d.Y() * i, d.Z() * i));
            } else if (json_get(f, "axis")) {
                double total = num(f, "angle_deg", false, 360);
                double step = total == 360 ? 360.0 / n : total / (n - 1);
                t.SetRotation(gp_Ax1(point(f, "about_mm"), axis(json_get(f, "axis"), "axis")), step * i * PI / 180);
            } else refuse("a pattern needs \"step_mm\" (a row) or \"axis\" (a ring)");
            r = boolean<BRepAlgoAPI_Fuse>(r, transformed(s, t), "pattern");
        }
        return r;
    }
    if (op == "fillet" || op == "chamfer") {
        const TopoDS_Shape &s = shape_of(P, f, "of");
        auto edges = select(s, TopAbs_EDGE, json_get(f, "edges"), "edges");
        *chosen = (int)edges.size();
        if (op == "fillet") {
            double r = positive(f, "radius_mm");
            BRepFilletAPI_MakeFillet mk(s);
            for (auto &e : edges) mk.Add(r, TopoDS::Edge(e));
            try {
                mk.Build();
            } catch (const Standard_Failure &) {
            }
            if (!mk.IsDone()) refuse("the rounding could not be made: the radius may be too large for the edges chosen");
            return mk.Shape();
        }
        double d = positive(f, "distance_mm");
        BRepFilletAPI_MakeChamfer mk(s);
        for (auto &e : edges) mk.Add(d, TopoDS::Edge(e));
        try {
            mk.Build();
        } catch (const Standard_Failure &) {
        }
        if (!mk.IsDone()) refuse("the chamfer could not be made: the distance may be too large for the edges chosen");
        return mk.Shape();
    }
    if (op == "shell") { /* hollowed to walls of a thickness, the chosen faces left open */
        const TopoDS_Shape &s = shape_of(P, f, "of");
        auto open = select(s, TopAbs_FACE, json_get(f, "open_faces"), "open_faces");
        *chosen = (int)open.size();
        TopTools_ListOfShape faces;
        for (auto &o : open) faces.Append(o);
        BRepOffsetAPI_MakeThickSolid mk;
        double t = positive(f, "thickness_mm");
        try {
            mk.MakeThickSolidByJoin(s, faces, -t, 1e-6);
        } catch (const Standard_Failure &) {
        }
        if (!mk.IsDone()) refuse("the walls could not be made: the thickness may be too large for the part");
        return mk.Shape();
    }
    refuse("\"op\" \"" + op + "\" is not known: box, cylinder, cone, sphere, sketch, extrude, revolve, union, cut, intersect, "
           "translate, rotate, pattern, fillet, chamfer, shell");
}

/* ---- the part as triangles: STL, closed and manifold ---- */

struct Mesh {
    std::vector<gp_Pnt> pts;
    std::vector<std::array<int, 3>> tris;
};

Mesh tessellate(const TopoDS_Shape &s, double deflection) {
    BRepMesh_IncrementalMesh mesher(s, deflection, Standard_False, 0.2, Standard_True);
    Mesh m;
    std::map<std::array<double, 3>, int> index;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(f, loc);
        if (t.IsNull()) continue;
        std::vector<int> id(t->NbNodes() + 1);
        for (int i = 1; i <= t->NbNodes(); i++) {
            gp_Pnt p = t->Node(i).Transformed(loc.Transformation());
            std::array<double, 3> key = {p.X(), p.Y(), p.Z()};
            auto it = index.find(key);
            if (it == index.end()) it = index.emplace(key, (int)m.pts.size()).first, m.pts.push_back(p);
            id[i] = it->second;
        }
        bool flip = f.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= t->NbTriangles(); i++) {
            int a, b, c;
            t->Triangle(i).Get(a, b, c);
            if (id[a] == id[b] || id[b] == id[c] || id[a] == id[c]) continue;
            m.tris.push_back(flip ? std::array<int, 3>{id[a], id[c], id[b]} : std::array<int, 3>{id[a], id[b], id[c]});
        }
    }
    return m;
}

JsonValue *mesh_record(const Mesh &m) {
    std::map<std::pair<int, int>, int> directed;
    double v = 0;
    for (auto &t : m.tris) {
        for (int k = 0; k < 3; k++) directed[{t[k], t[(k + 1) % 3]}]++;
        const gp_Pnt &a = m.pts[t[0]], &b = m.pts[t[1]], &c = m.pts[t[2]];
        v += (a.X() * (b.Y() * c.Z() - b.Z() * c.Y()) - a.Y() * (b.X() * c.Z() - b.Z() * c.X()) + a.Z() * (b.X() * c.Y() - b.Y() * c.X())) / 6;
    }
    bool closed = !m.tris.empty();
    for (auto &e : directed)
        if (e.second != 1 || directed.count({e.first.second, e.first.first}) != 1) closed = false;
    JsonValue *o = json_object();
    json_set_int(o, "triangles", (long long)m.tris.size());
    json_set_bool(o, "closed_manifold", closed);
    json_set_number(o, "volume_mm3", v);
    return o;
}

bool write_stl(const Mesh &m, const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    char head[80] = {0};
    std::snprintf(head, sizeof head, "OpenPhysicsAI openphysics-cad, millimetres");
    std::fwrite(head, 1, 80, f);
    uint32_t n = (uint32_t)m.tris.size();
    std::fwrite(&n, 4, 1, f);
    for (auto &t : m.tris) {
        const gp_Pnt &a = m.pts[t[0]], &b = m.pts[t[1]], &c = m.pts[t[2]];
        gp_Vec nv = gp_Vec(a, b).Crossed(gp_Vec(a, c));
        if (nv.Magnitude() > 0) nv.Normalize();
        float rec[12] = {(float)nv.X(), (float)nv.Y(), (float)nv.Z(), (float)a.X(), (float)a.Y(), (float)a.Z(),
                         (float)b.X(), (float)b.Y(), (float)b.Z(), (float)c.X(), (float)c.Y(), (float)c.Z()};
        uint16_t attr = 0;
        std::fwrite(rec, 4, 12, f);
        std::fwrite(&attr, 2, 1, f);
    }
    return std::fclose(f) == 0;
}

bool write_step(const TopoDS_Shape &s, const std::string &path) {
    Interface_Static::SetCVal("write.step.unit", "MM");
    STEPControl_Writer w;
    return w.Transfer(s, STEPControl_AsIs) == IFSelect_RetDone && w.Write(path.c_str()) == IFSelect_RetDone;
}

JsonValue *failure(const Refusal &r) {
    JsonValue *o = json_object(), *e;
    json_set_bool(o, "ok", false);
    e = json_set_object(o, "error");
    if (!r.feature.empty()) json_set_string(e, "feature", r.feature.c_str());
    if (!r.op.empty()) json_set_string(e, "op", r.op.c_str());
    json_set_string(e, "message", r.message.c_str());
    return o;
}

} // namespace

JsonValue *cadpart_build(const JsonValue *model, const std::string &outdir, double deflection_mm) {
    Part P;
    JsonValue *rec = json_object(), *list = json_set_array(rec, "features");
    std::string last, id, op;
    try {
        const JsonValue *features = json_get(model, "features");
        if (!features || features->type != JSON_ARRAY || !json_len(features)) refuse("\"features\" must list the part's features");
        if (!(deflection_mm > 0)) refuse("the chordal deflection must be above 0 mm");
        for (size_t i = 0; i < json_len(features); i++) {
            const JsonValue *f = json_at(features, i);
            id = json_get_str(f, "id", ""), op = json_get_str(f, "op", "");
            if (id.empty()) refuse("every feature needs an \"id\"");
            if (P.shapes.count(id)) refuse("the id \"" + id + "\" is used twice");
            int chosen = -1;
            TopoDS_Shape s;
            try {
                s = feature(P, f, op, &chosen);
            } catch (const Standard_Failure &e) {
                refuse(std::string("Open CASCADE: ") + (e.GetMessageString() && *e.GetMessageString() ? e.GetMessageString() : e.DynamicType()->Name()));
            }
            if (s.IsNull()) refuse("the feature made nothing");
            JsonValue *m = measure(s);
            json_set_string(m, "id", id.c_str());
            json_set_string(m, "op", op.c_str());
            if (chosen >= 0) json_set_int(m, "chosen", chosen);
            bool sketch = op == "sketch";
            if (!sketch && (!json_get_bool(m, "valid", false) || !json_get_int(m, "solids", 0))) {
                const char *why = json_get_int(m, "solids", 0) ? "the result is not a valid solid" : "the result holds no solid";
                json_free(m);
                refuse(why);
            }
            json_push(list, m);
            P.shapes[id] = s;
            last = id;
        }
        id = op = "";
        std::string result = json_get_str(model, "result", last.c_str());
        auto it = P.shapes.find(result);
        if (it == P.shapes.end()) refuse("\"result\": no feature \"" + result + "\"");
        if (P.sketch_normal.count(result)) refuse("\"result\" is a sketch, not a solid");
        const TopoDS_Shape &part = it->second;
        JsonValue *pm = measure(part);
        json_set_string(pm, "id", result.c_str());
        json_set(pm, "named_faces", faces_of(part));
        json_set(rec, "part", pm);
        Mesh mesh = tessellate(part, deflection_mm);
        JsonValue *stl = mesh_record(mesh);
        json_set_number(stl, "deflection_mm", deflection_mm);
        json_set(rec, "stl", stl);
        if (!json_get_bool(stl, "closed_manifold", false)) refuse("the part's triangles are not closed and manifold");
        std::string step_path = outdir + "/part.step", stl_path = outdir + "/part.stl";
        if (!write_step(part, step_path + ".tmp") || !write_stl(mesh, stl_path + ".tmp")) {
            std::remove((step_path + ".tmp").c_str()), std::remove((stl_path + ".tmp").c_str());
            refuse("cannot write into " + outdir);
        }
        std::rename((step_path + ".tmp").c_str(), step_path.c_str());
        std::rename((stl_path + ".tmp").c_str(), stl_path.c_str());
        JsonValue *files = json_set_object(rec, "files");
        json_set_string(files, "step", step_path.c_str());
        json_set_string(files, "stl", stl_path.c_str());
        json_set_bool(rec, "ok", true);
        return rec;
    } catch (Refusal &r) {
        json_free(rec);
        r.feature = id, r.op = op;
        return failure(r);
    } catch (const Standard_Failure &e) {
        json_free(rec);
        return failure(Refusal{id, op, std::string("Open CASCADE: ") + (e.GetMessageString() ? e.GetMessageString() : "failure")});
    }
}

JsonValue *cadpart_measure_step(const std::string &path) {
    try {
        STEPControl_Reader r;
        if (r.ReadFile(path.c_str()) != IFSelect_RetDone || r.TransferRoots() <= 0) refuse("cannot read STEP file " + path);
        TopoDS_Shape s = r.OneShape();
        JsonValue *rec = json_object(), *m = measure(s);
        json_set(m, "named_faces", faces_of(s));
        json_set_bool(rec, "ok", true);
        json_set(rec, "part", m);
        return rec;
    } catch (Refusal &r) {
        return failure(r);
    } catch (const Standard_Failure &e) {
        return failure(Refusal{"", "", std::string("Open CASCADE: ") + (e.GetMessageString() ? e.GetMessageString() : "failure")});
    }
}
