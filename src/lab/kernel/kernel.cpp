/* kernel.cpp - the physics kernel's composer (G24): a scenario names the mesh, the fields and their element order, the
 * equations it wants from the library, materials, boundaries chosen by geometry, loads and outputs, and this file
 * assembles them on MFEM's finite elements and solves them. Nothing about an application is programmed here: a new
 * application is a new scenario. docs/lab/kernel.md has every key.
 *
 * The library (first entries):
 *   heat         steady conduction:  -div(k grad T) = Q;  T held, a heat flux in, or convection h (T_inf - T) on
 *                boundaries; k per material, any of them a formula of x, y, z
 *   elasticity   linear, isotropic, small strain:  -div(sigma) = f,  sigma = lambda tr(e) I + 2 mu e,
 *                e = sym(grad u) - alpha (T - T_ref) I;  displacement held (all or some components, or prescribed
 *                values), tractions, a total force, a pressure on boundaries; body forces, gravity
 *   coupling     the temperature, solved or given as a formula, strains the solid (one way)
 *
 * Every system is symmetric positive definite and solved by a sparse Cholesky factorisation (Accelerate, spdirect.h),
 * double precision. Reactions come from the residual of the unconstrained system at the held degrees of freedom, so they
 * balance the loads to round-off; heat through a held boundary likewise. The run's whole record goes beside the
 * result as OUT.lab.kernel.json. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mfem.hpp"

extern "C" {
#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
}
#include "../magnet/spdirect.h"
#include "kexpr.hpp"
#include "kn_stl.h"
#include "kn_lu.h"

using namespace mfem;

namespace {

double now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* ---------------------------------------------------------------- numbers and formulas */

struct Num {
    bool set = false;
    double c = 0;
    std::shared_ptr<kn::Expr> e;
    double operator()(const double *x) const { return e ? e->eval(x) : c; }
};

/* a number, or a formula of x, y, z as a string; absent: unset */
bool get_num(const JsonValue *v, Num &n, const char *what, std::string &err) {
    n = Num();
    if (!v || v->type == JSON_NULL) return true;
    if (v->type == JSON_NUMBER) {
        n.set = true, n.c = v->u.number;
        return true;
    }
    if (v->type == JSON_STRING) {
        auto e = std::make_shared<kn::Expr>();
        std::string e2;
        if (!e->parse(v->u.string.ptr, e2)) {
            err = std::string(what) + ": " + e2;
            return false;
        }
        n.set = true, n.e = e;
        return true;
    }
    err = std::string(what) + ": a number or a formula of x, y, z is expected";
    return false;
}

bool get_vec3(const JsonValue *v, Num out[3], const char *what, std::string &err) {
    for (int k = 0; k < 3; k++) out[k] = Num();
    if (!v) return true;
    if (v->type != JSON_ARRAY || json_len(v) != 3) {
        err = std::string(what) + ": three components are expected (a number, a formula or null each)";
        return false;
    }
    for (int k = 0; k < 3; k++)
        if (!get_num(json_at(v, (size_t)k), out[k], what, err)) return false;
    return true;
}

/* ---------------------------------------------------------------- selections */

struct Sel {
    enum Kind { ALL, PLANE, BOX, POINT, ATTR, CYL } kind = ALL;
    int axis = 0, attr = 0;
    double value = 0, lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, p[3] = {0, 0, 0};
    double dir[3] = {0, 0, 1}, radius = 0; /* a cylinder: through p along dir */
    double tol = -1;         /* < 0: from the mesh's size */
    bool box_space = false;  /* in a mapped box's own coordinates, before the map ("space": "box") */
};

bool get_sel(const JsonValue *w, Sel &s, std::string &err) {
    s = Sel();
    if (!w || (w->type == JSON_STRING && !strcmp(w->u.string.ptr, "all"))) return true;
    if (w->type != JSON_OBJECT) {
        err = "where: \"all\", or {\"plane\": [axis, value]}, {\"box\": [[x0, y0, z0], [x1, y1, z1]]}, {\"point\": [x, y, z]}, "
              "{\"cylinder\": {\"point_m\", \"axis\", \"radius_m\"}} or {\"attribute\": n}";
        return false;
    }
    s.tol = json_get_num(w, "tolerance_m", -1);
    s.box_space = !strcmp(json_get_str(w, "space", "world"), "box");
    const JsonValue *v;
    if ((v = json_get(w, "plane"))) {
        const char *ax = json_str(json_at(v, 0));
        if (json_len(v) != 2 || !ax || strlen(ax) != 1 || !strchr("xyz", ax[0]) || json_at(v, 1)->type != JSON_NUMBER) {
            err = "where.plane: [\"x\" | \"y\" | \"z\", value in m]";
            return false;
        }
        s.kind = Sel::PLANE, s.axis = ax[0] - 'x', s.value = json_at(v, 1)->u.number;
        return true;
    }
    if ((v = json_get(w, "box"))) {
        if (json_len(v) != 2 || json_len(json_at(v, 0)) != 3 || json_len(json_at(v, 1)) != 3) {
            err = "where.box: [[x0, y0, z0], [x1, y1, z1]] in m";
            return false;
        }
        s.kind = Sel::BOX;
        for (int k = 0; k < 3; k++) s.lo[k] = json_at(json_at(v, 0), (size_t)k)->u.number, s.hi[k] = json_at(json_at(v, 1), (size_t)k)->u.number;
        return true;
    }
    if ((v = json_get(w, "point"))) {
        if (json_len(v) != 3) {
            err = "where.point: [x, y, z] in m";
            return false;
        }
        s.kind = Sel::POINT;
        for (int k = 0; k < 3; k++) s.p[k] = json_at(v, (size_t)k)->u.number;
        return true;
    }
    if ((v = json_get(w, "cylinder"))) { /* faces whose centres lie at radius_m from the line through point_m along axis */
        const JsonValue *pt = json_get(v, "point_m"), *ax = json_get(v, "axis");
        s.radius = json_get_num(v, "radius_m", 0);
        double n = 0;
        for (int k = 0; k < 3 && json_len(pt) == 3 && json_len(ax) == 3; k++)
            s.p[k] = json_at(pt, (size_t)k)->u.number, s.dir[k] = json_at(ax, (size_t)k)->u.number, n += s.dir[k] * s.dir[k];
        if (json_len(pt) != 3 || json_len(ax) != 3 || !(n > 0) || !(s.radius > 0)) {
            err = "where.cylinder: {\"point_m\": [x, y, z] on its axis, \"axis\": [dx, dy, dz], \"radius_m\": r}";
            return false;
        }
        for (int k = 0; k < 3; k++) s.dir[k] /= std::sqrt(n);
        s.kind = Sel::CYL;
        return true;
    }
    if ((v = json_get(w, "attribute"))) {
        s.kind = Sel::ATTR, s.attr = (int)v->u.number;
        return true;
    }
    err = "where: no plane, box, point, cylinder or attribute in it";
    return false;
}

bool sel_has(const Sel &s, const double *c, int attr, double tol) {
    double t = s.tol >= 0 ? s.tol : tol;
    switch (s.kind) {
    case Sel::ALL: return true;
    case Sel::PLANE: return std::fabs(c[s.axis] - s.value) <= t;
    case Sel::BOX:
        for (int k = 0; k < 3; k++)
            if (c[k] < s.lo[k] - t || c[k] > s.hi[k] + t) return false;
        return true;
    case Sel::ATTR: return attr == s.attr;
    case Sel::POINT: return false; /* points are vertices, not faces */
    case Sel::CYL: { /* a cylinder read from an STL is faceted: by default a thousandth of its radius either side */
        double d[3], a = 0, r2 = 0;
        for (int k = 0; k < 3; k++) d[k] = c[k] - s.p[k], a += d[k] * s.dir[k];
        for (int k = 0; k < 3; k++) r2 += (d[k] - a * s.dir[k]) * (d[k] - a * s.dir[k]);
        return std::fabs(std::sqrt(r2) - s.radius) <= (s.tol >= 0 ? s.tol : 1e-3 * s.radius);
    }
    }
    return false;
}

/* ---------------------------------------------------------------- the problem */

/* a number, or a formula that may name x, y, z, t, the scenario's fields (a value by its name, a gradient by dx( ),
 * dy( ), dz( )) and the materials' properties by their keys */
struct FNum {
    bool set = false;
    double c = 0;
    std::shared_ptr<kn::Expr> e;
};

bool get_fnum(const JsonValue *v, FNum &n, const kn::Expr::Symbols *sym, const std::string &what, std::string &err) {
    n = FNum();
    if (!v || v->type == JSON_NULL) return true;
    if (v->type == JSON_NUMBER) {
        n.set = true, n.c = v->u.number;
        return true;
    }
    if (v->type == JSON_STRING) {
        auto e = std::make_shared<kn::Expr>();
        std::string e2;
        if (!e->parse(v->u.string.ptr, e2, sym)) return err = what + ": " + e2, false;
        n.set = true;
        if (e->constant()) {
            double z[3] = {0, 0, 0};
            n.c = e->eval(z);
        } else {
            n.e = e;
        }
        return true;
    }
    return err = what + ": a number or a formula is expected", false;
}

/* whether a formula names a field (slots below 4 per field), or the time */
bool fnum_fields(const FNum &n, int nf) {
    if (n.e)
        for (int s : n.e->slots_used())
            if (s < 4 * nf) return true;
    return false;
}
bool fnum_time(const FNum &n) { return n.e && n.e->uses_time(); }

/* a field and the equation it obeys, written by the scenario:
 *   mass du/dt - div(diffusion grad u) + convection . grad u + reaction u = source */
struct FieldEq {
    std::string name, unit;
    int order = 1;
    FNum initial, mass, reaction, source;
    FNum diffusion[9];
    int ndiff = 0; /* 1 a scalar, 3 a diagonal, 9 a full tensor row by row */
    FNum convection[3];
    bool convects = false;
};

/* a boundary's condition on one field: its value held, a flux into the body, or an exchange h (ambient - u) */
struct FieldBC {
    int field = -1;
    FNum value, flux, h, ambient;
};

struct Material {
    std::string name, source;
    Num E, nu, k, alpha, rho;
    std::map<std::string, Num> props; /* every number or formula of x, y, z the material carries, by its key */
};

struct Boundary {
    std::string name;
    Sel where;
    /* elasticity */
    bool fix[3] = {false, false, false};
    Num disp[3];
    Num traction[3];
    Num pressure;
    double force[3] = {0, 0, 0};
    bool has_force = false;
    /* heat */
    Num temperature, flux, h, ambient;
    /* the scenario's fields */
    std::vector<FieldBC> fbc;
    /* contact: a rigid obstacle these faces may touch, without friction. kind 1 a plane (p a point, n its normal toward
     * the body), 2 a sphere (p its centre, r), 3 a cylinder (p a point on the axis, n the axis, r), 4 a formula of the
     * signed distance (positive on the body's side) */
    int obstacle = 0;
    double op[3] = {0, 0, 0}, on[3] = {0, 0, 1}, orad = 0, penalty = 0;
    std::shared_ptr<kn::Expr> odist;
    bool report_points = false;
    /* bookkeeping */
    int faces = 0, vertex = -1;
    double area = 0, vpos[3] = {0, 0, 0}; /* a point's vertex and where it truly is */
    Array<int> marker; /* over boundary attributes */
};

struct Problem {
    std::string title;
    bool heat = false, elastic = false;
    int order = 1;
    std::vector<Material> mats;
    std::vector<Sel> region_sel;
    std::vector<int> region_mat;
    std::vector<Boundary> bnd;
    Num body[3], gravity[3], heat_source, temperature_field;
    FNum temp_f; /* with fields: loads.temperature_k, a formula that may name them */
    double T_ref = 293.15;
    Num check_u[3], check_T;
    bool frames = true;
    double deform_scale = 0;
    struct Probe {
        std::string name;
        double p[3];
    };
    std::vector<Probe> probes;
    struct Line { /* a line sampled at evenly spaced probes, gathered back into arrays in the record */
        std::string name;
        double a[3], b[3];
        int n = 21;
        size_t first = 0;
    };
    std::vector<Line> lines;
    /* the scenario's own fields and equations */
    std::vector<FieldEq> fields;
    kn::Expr::Symbols fsym;
    std::vector<std::string> props; /* the materials' property names, in slot order after the fields' */
    bool transient = false, bdf2 = true;
    double t_end = 0, dt = 0, tol = 1e-12;
    int steps = 0, outputs = 1, max_iter = 50;
    std::vector<FNum> check_field;
    struct Crossing {
        int probe = -1, field = -1;
        double value = 0;
    };
    std::vector<Crossing> crossings; /* the first time a probe's field crosses a value */
    double contact_tol = 1e-8; /* the deepest penetration allowed, relative to the largest displacement */
    int contact_max_al = 200;
};

/* a material property of the element the point is in (its attribute is the material's index + 1) */
class MatCoef : public Coefficient {
public:
    std::function<double(const Material &, const double *)> f;
    const std::vector<Material> *mats;
    real_t Eval(ElementTransformation &T, const IntegrationPoint &ip) override {
        double x[3];
        Vector X(x, 3);
        T.Transform(ip, X);
        return f((*mats)[(size_t)(T.Attribute - 1)], x);
    }
};

class NumCoef : public Coefficient {
public:
    Num n;
    explicit NumCoef(const Num &v) : n(v) {}
    real_t Eval(ElementTransformation &T, const IntegrationPoint &ip) override {
        double x[3];
        Vector X(x, 3);
        T.Transform(ip, X);
        return n(x);
    }
};

class Num3Coef : public VectorCoefficient {
public:
    Num n[3];
    double scale[3] = {1, 1, 1};
    Num3Coef(const Num v[3]) : VectorCoefficient(3) {
        for (int k = 0; k < 3; k++) n[k] = v[k];
    }
    void Eval(Vector &V, ElementTransformation &T, const IntegrationPoint &ip) override {
        double x[3];
        Vector X(x, 3);
        T.Transform(ip, X);
        V.SetSize(3);
        for (int k = 0; k < 3; k++) V(k) = n[k].set ? scale[k] * n[k](x) : 0.0;
    }
};

/* the temperature at a point: the heat solution, or the scenario's formula */
struct TempField {
    const GridFunction *gf = nullptr;
    Num formula;
    std::function<double(ElementTransformation &, const IntegrationPoint &)> fn; /* a formula naming the fields */
    bool any() const { return gf || formula.set || fn; }
    double at(ElementTransformation &T, const IntegrationPoint &ip) const {
        if (gf) return gf->GetValue(T, ip);
        if (fn) return fn(T, ip);
        double x[3];
        Vector X(x, 3);
        T.Transform(ip, X);
        return formula(x);
    }
};

double lame_lambda(const Material &m, const double *x) {
    double E = m.E(x), nu = m.nu(x);
    return E * nu / ((1 + nu) * (1 - 2 * nu));
}
double lame_mu(const Material &m, const double *x) {
    double E = m.E(x), nu = m.nu(x);
    return E / (2 * (1 + nu));
}

/* the thermal strain's load: int (3 lambda + 2 mu) alpha (T - T_ref) div(v) dx */
class ThermalLoad : public LinearFormIntegrator {
public:
    const std::vector<Material> *mats;
    const TempField *temp;
    double T_ref;
    void AssembleRHSElementVect(const FiniteElement &el, ElementTransformation &T, Vector &elvect) override {
        int dof = el.GetDof(), dim = el.GetDim();
        DenseMatrix dsh(dof, dim);
        elvect.SetSize(dof * dim);
        elvect = 0.0;
        const IntegrationRule &ir = IntRules.Get(el.GetGeomType(), 2 * el.GetOrder() + T.OrderW());
        const Material &m = (*mats)[(size_t)(T.Attribute - 1)];
        for (int q = 0; q < ir.GetNPoints(); q++) {
            const IntegrationPoint &ip = ir.IntPoint(q);
            T.SetIntPoint(&ip);
            double x[3];
            Vector X(x, 3);
            T.Transform(ip, X);
            double E = m.E(x), nu = m.nu(x), c = E / (1 - 2 * nu) * m.alpha(x) * (temp->at(T, ip) - T_ref);
            el.CalcPhysDShape(T, dsh);
            double w = ip.weight * T.Weight() * c;
            for (int d = 0; d < dim; d++)
                for (int j = 0; j < dof; j++) elvect(j + d * dof) += w * dsh(j, d);
        }
    }
};

/* ---------------------------------------------------------------- reading the scenario */

bool read_problem(const JsonValue *root, Problem &P, std::string &err) {
    P.title = json_get_str(root, "title", "a kernel scenario");
    P.order = (int)json_get_int(root, "order", 1);
    if (P.order < 1 || P.order > 4) return err = "order: 1 to 4 (the elements' polynomial degree)", false;
    const JsonValue *ph = json_get(root, "physics");
    for (size_t i = 0; i < json_len(ph); i++) {
        const char *p = json_str(json_at(ph, i));
        if (p && !strcmp(p, "heat")) P.heat = true;
        else if (p && !strcmp(p, "elasticity")) P.elastic = true;
        else return err = std::string("physics: \"") + (p ? p : "?") + "\" is not in the library (heat, elasticity)", false;
    }
    const JsonValue *fj = json_get(root, "fields");
    bool has_fields = fj && fj->type == JSON_OBJECT && fj->u.object.len > 0;
    if (!P.heat && !P.elastic && !has_fields)
        return err = "physics: name what to solve, [\"heat\"], [\"elasticity\"] or both; or write the equations in \"fields\"", false;
    if (has_fields && P.heat)
        return err = "fields and the library's heat are not combined: write heat as a field (mass rho c_p, diffusion k); the library's elasticity may follow it (loads.temperature_k naming the field)", false;
    /* materials */
    const JsonValue *mj = json_get(root, "materials");
    bool no_mats = !mj || mj->type != JSON_OBJECT || mj->u.object.len == 0;
    if (no_mats && (!has_fields || P.elastic)) return err = "materials: {name: {properties with units in their keys, source}}", false;
    if (no_mats) { /* the fields' equations carry their own coefficients */
        Material m;
        m.name = "default", m.source = "none needed: the fields' equations carry their own coefficients";
        m.alpha.set = true;
        P.mats.push_back(m);
    }
    for (size_t i = 0; !no_mats && i < mj->u.object.len; i++) {
        Material m;
        const JsonValue *v = mj->u.object.values[i];
        m.name = mj->u.object.keys[i];
        m.source = json_get_str(v, "source", "");
        if (m.source.empty()) return err = "material " + m.name + ": its source is needed (where the values come from)", false;
        std::string w = "material " + m.name;
        if (!get_num(json_get(v, "youngs_modulus_pa"), m.E, w.c_str(), err) || !get_num(json_get(v, "poisson_ratio"), m.nu, w.c_str(), err) ||
            !get_num(json_get(v, "conductivity_w_mk"), m.k, w.c_str(), err) || !get_num(json_get(v, "expansion_per_k"), m.alpha, w.c_str(), err) ||
            !get_num(json_get(v, "density_kg_m3"), m.rho, w.c_str(), err))
            return false;
        if (P.elastic && (!m.E.set || !m.nu.set)) return err = w + ": elasticity needs youngs_modulus_pa and poisson_ratio", false;
        if (P.heat && !m.k.set) return err = w + ": heat needs conductivity_w_mk", false;
        if (!m.alpha.set) m.alpha.set = true, m.alpha.c = 0;
        for (size_t j = 0; j < v->u.object.len; j++) { /* every property, for the fields' formulas */
            const JsonValue *pv = v->u.object.values[j];
            Num pn;
            std::string e2;
            if (strcmp(v->u.object.keys[j], "source") && (pv->type == JSON_NUMBER || pv->type == JSON_STRING) && get_num(pv, pn, "", e2))
                m.props[v->u.object.keys[j]] = pn;
        }
        P.mats.push_back(m);
    }
    /* fields: the scenario's own equations */
    if (has_fields) {
        static const char *reserved[] = {"x",    "y",     "z",     "t",    "pi",   "e",   "sin",  "cos",  "tan",  "asin",  "acos",
                                         "atan", "sinh",  "cosh",  "tanh", "exp",  "log", "log10", "sqrt", "abs", "floor", "ceil",
                                         "sign", "step",  "min",   "max",  "pow",  "atan2", "if",  "dx",   "dy",  "dz"};
        if (fj->u.object.len > 16) return err = "fields: at most 16", false;
        for (size_t i = 0; i < fj->u.object.len; i++) {
            std::string nm = fj->u.object.keys[i];
            bool ok = !nm.empty() && (isalpha((unsigned char)nm[0]) || nm[0] == '_');
            for (char c : nm) ok = ok && (isalnum((unsigned char)c) || c == '_');
            for (const char *r : reserved) ok = ok && nm != r;
            if (!ok) return err = "fields: \"" + nm + "\" cannot name a field (letters, digits and _; not x, y, z, t, pi, e or a function's name)", false;
            FieldEq q;
            q.name = nm;
            P.fields.push_back(q);
        }
        int nf = (int)P.fields.size();
        for (int k = 0; k < nf; k++) {
            const std::string &nm = P.fields[(size_t)k].name;
            P.fsym.slot[nm] = 4 * k, P.fsym.slot["dx:" + nm] = 4 * k + 1, P.fsym.slot["dy:" + nm] = 4 * k + 2, P.fsym.slot["dz:" + nm] = 4 * k + 3;
        }
        for (const Material &m : P.mats)
            for (const auto &kv : m.props) {
                if (P.fsym.slot.count(kv.first)) return err = "material " + m.name + ": its property \"" + kv.first + "\" has a field's name", false;
                if (std::find(P.props.begin(), P.props.end(), kv.first) == P.props.end()) P.props.push_back(kv.first);
            }
        if (4 * nf + (int)P.props.size() > 128) return err = "fields: too many fields and material properties for a formula (128 names)", false;
        for (size_t j = 0; j < P.props.size(); j++) P.fsym.slot[P.props[j]] = 4 * nf + (int)j;
        static const char *known[] = {"order", "unit", "initial", "mass", "diffusion", "convection", "reaction", "source", "note"};
        for (int k = 0; k < nf; k++) {
            FieldEq &q = P.fields[(size_t)k];
            const JsonValue *v = fj->u.object.values[k];
            std::string w = "fields." + q.name;
            if (v->type != JSON_OBJECT) return err = w + ": the coefficients of its equation, {\"diffusion\": ..., \"source\": ...}", false;
            for (size_t j = 0; j < v->u.object.len; j++) {
                bool ok = false;
                for (const char *kk : known) ok = ok || !strcmp(v->u.object.keys[j], kk);
                if (!ok)
                    return err = w + ": \"" + v->u.object.keys[j] + "\" is not a field's key (order, unit, initial, mass, diffusion, convection, reaction, source)", false;
            }
            q.order = (int)json_get_int(v, "order", P.order);
            if (q.order < 1 || q.order > 4) return err = w + ".order: 1 to 4", false;
            q.unit = json_get_str(v, "unit", "");
            if (!get_fnum(json_get(v, "initial"), q.initial, nullptr, w + ".initial", err) || !get_fnum(json_get(v, "mass"), q.mass, &P.fsym, w + ".mass", err) ||
                !get_fnum(json_get(v, "reaction"), q.reaction, &P.fsym, w + ".reaction", err) ||
                !get_fnum(json_get(v, "source"), q.source, &P.fsym, w + ".source", err))
                return false;
            const JsonValue *dj = json_get(v, "diffusion");
            if (dj && dj->type == JSON_ARRAY) {
                q.ndiff = (int)json_len(dj);
                if (q.ndiff != 3 && q.ndiff != 9) return err = w + ".diffusion: one value, three (a diagonal) or nine (a full tensor, row by row)", false;
                for (int c = 0; c < q.ndiff; c++)
                    if (!get_fnum(json_at(dj, (size_t)c), q.diffusion[c], &P.fsym, w + ".diffusion", err)) return false;
            } else if (dj) {
                q.ndiff = 1;
                if (!get_fnum(dj, q.diffusion[0], &P.fsym, w + ".diffusion", err)) return false;
            }
            const JsonValue *cj = json_get(v, "convection");
            if (cj) {
                if (json_len(cj) != 3) return err = w + ".convection: [vx, vy, vz], a velocity, each a number or a formula", false;
                for (int c = 0; c < 3; c++)
                    if (!get_fnum(json_at(cj, (size_t)c), q.convection[c], &P.fsym, w + ".convection", err)) return false;
                q.convects = true;
            }
            if (!q.ndiff && !q.reaction.set && !q.mass.set && !q.convects)
                return err = w + ": its equation needs at least one of mass, diffusion, convection, reaction", false;
        }
    }
    /* regions: which material where (by element centres); one material needs none */
    const JsonValue *rj = json_get(root, "regions");
    for (size_t i = 0; i < json_len(rj); i++) {
        const JsonValue *v = json_at(rj, i);
        Sel s;
        if (!get_sel(json_get(v, "where"), s, err)) return err = "regions: " + err, false;
        const char *mn = json_get_str(v, "material", "");
        int k = -1;
        for (size_t m = 0; m < P.mats.size(); m++)
            if (P.mats[m].name == mn) k = (int)m;
        if (k < 0) return err = std::string("regions: no material named \"") + mn + "\"", false;
        P.region_sel.push_back(s), P.region_mat.push_back(k);
    }
    if (P.mats.size() > 1 && P.region_sel.empty()) return err = "regions: several materials need regions saying where each is", false;
    /* loads on the body */
    const JsonValue *lj = json_get(root, "loads");
    P.T_ref = json_get_num(root, "reference_temperature_k", 293.15);
    if (!get_vec3(json_get(lj, "body_force_n_m3"), P.body, "loads.body_force_n_m3", err) || !get_vec3(json_get(lj, "gravity_m_s2"), P.gravity, "loads.gravity_m_s2", err) ||
        !get_num(json_get(lj, "heat_source_w_m3"), P.heat_source, "loads.heat_source_w_m3", err) ||
        (!has_fields && !get_num(json_get(lj, "temperature_k"), P.temperature_field, "loads.temperature_k", err)) ||
        (has_fields && !get_fnum(json_get(lj, "temperature_k"), P.temp_f, &P.fsym, "loads.temperature_k", err)))
        return false;
    if (has_fields && P.temp_f.set && !P.elastic) return err = "loads.temperature_k strains the solid: it needs \"physics\": [\"elasticity\"]", false;
    if (P.heat && P.temperature_field.set) return err = "loads.temperature_k gives the temperature; with heat in physics it is solved instead", false;
    /* boundaries */
    const JsonValue *bj = json_get(root, "boundaries");
    if (json_len(bj) > 60) return err = "boundaries: at most 60", false;
    for (size_t i = 0; i < json_len(bj); i++) {
        const JsonValue *v = json_at(bj, i);
        Boundary b;
        b.name = json_get_str(v, "name", "");
        if (b.name.empty()) b.name = "boundary " + std::to_string(i + 1);
        std::string w = "boundary " + b.name;
        if (!get_sel(json_get(v, "where"), b.where, err)) return err = w + ": " + err, false;
        if (json_get_bool(v, "fixed", false)) b.fix[0] = b.fix[1] = b.fix[2] = true;
        const JsonValue *fx = json_get(v, "fix");
        for (size_t k = 0; k < json_len(fx); k++) {
            const char *c = json_str(json_at(fx, k));
            if (!c || strlen(c) != 1 || !strchr("xyz", c[0])) return err = w + ": fix lists \"x\", \"y\", \"z\"", false;
            b.fix[c[0] - 'x'] = true;
        }
        if (!get_vec3(json_get(v, "displacement_m"), b.disp, (w + ".displacement_m").c_str(), err)) return false;
        for (int k = 0; k < 3; k++)
            if (b.disp[k].set) b.fix[k] = true;
        if (!get_vec3(json_get(v, "traction_pa"), b.traction, (w + ".traction_pa").c_str(), err)) return false;
        if (!get_num(json_get(v, "pressure_pa"), b.pressure, (w + ".pressure_pa").c_str(), err)) return false;
        const JsonValue *f = json_get(v, "force_n");
        if (f) {
            if (json_len(f) != 3) return err = w + ": force_n is [Fx, Fy, Fz] in N, spread evenly over the boundary", false;
            for (int k = 0; k < 3; k++) b.force[k] = json_at(f, (size_t)k)->u.number;
            b.has_force = true;
        }
        if (!get_num(json_get(v, "temperature_k"), b.temperature, (w + ".temperature_k").c_str(), err) ||
            !get_num(json_get(v, "heat_flux_w_m2"), b.flux, (w + ".heat_flux_w_m2").c_str(), err))
            return false;
        const JsonValue *cv = json_get(v, "convection");
        if (cv && (!get_num(json_get(cv, "h_w_m2k"), b.h, (w + ".convection.h_w_m2k").c_str(), err) ||
                   !get_num(json_get(cv, "ambient_k"), b.ambient, (w + ".convection.ambient_k").c_str(), err)))
            return false;
        if (cv && (!b.h.set || !b.ambient.set)) return err = w + ": convection needs h_w_m2k and ambient_k", false;
        if (b.where.kind == Sel::POINT && (b.has_force || b.pressure.set || b.traction[0].set || b.traction[1].set || b.traction[2].set || b.flux.set || cv))
            return err = w + ": a point can only be held (fix, displacement_m, temperature_k)", false;
        const JsonValue *ct = json_get(v, "contact");
        if (ct) { /* a rigid obstacle these faces may touch */
            if (b.where.kind == Sel::POINT) return err = w + ": contact needs faces, not a point", false;
            if (!P.fields.empty()) return err = w + ": contact is solved for one state; with fields it is not followed in time yet", false;
            auto vec3 = [&](const JsonValue *a, double *out, const char *what) {
                if (json_len(a) != 3) return err = w + ".contact: " + what + " [x, y, z]", false;
                for (int k = 0; k < 3; k++) out[k] = json_at(a, (size_t)k)->u.number;
                return true;
            };
            const JsonValue *o;
            if ((o = json_get(ct, "plane"))) {
                b.obstacle = 1;
                if (!vec3(json_get(o, "point_m"), b.op, "plane.point_m") || !vec3(json_get(o, "normal"), b.on, "plane.normal (toward the body)")) return false;
            } else if ((o = json_get(ct, "sphere"))) {
                b.obstacle = 2, b.orad = json_get_num(o, "radius_m", 0);
                if (!vec3(json_get(o, "center_m"), b.op, "sphere.center_m")) return false;
            } else if ((o = json_get(ct, "cylinder"))) {
                b.obstacle = 3, b.orad = json_get_num(o, "radius_m", 0);
                if (!vec3(json_get(o, "point_m"), b.op, "cylinder.point_m") || !vec3(json_get(o, "axis"), b.on, "cylinder.axis")) return false;
            } else if (json_str(json_get(ct, "distance"))) {
                b.obstacle = 4, b.odist = std::make_shared<kn::Expr>();
                std::string e2;
                if (!b.odist->parse(json_str(json_get(ct, "distance")), e2)) return err = w + ".contact.distance: " + e2, false;
            } else {
                return err = w + ".contact: {\"plane\": {point_m, normal}}, {\"sphere\": {center_m, radius_m}}, {\"cylinder\": {point_m, axis, radius_m}} or {\"distance\": a formula, positive on the body's side}", false;
            }
            if ((b.obstacle == 2 || b.obstacle == 3) && !(b.orad > 0)) return err = w + ".contact: a positive radius_m", false;
            if (b.obstacle == 1 || b.obstacle == 3) {
                double nn = std::sqrt(b.on[0] * b.on[0] + b.on[1] * b.on[1] + b.on[2] * b.on[2]);
                if (!(nn > 0)) return err = w + ".contact: a nonzero direction", false;
                for (int k = 0; k < 3; k++) b.on[k] /= nn;
            }
            b.penalty = json_get_num(ct, "penalty_pa_m", 0);
            b.report_points = json_get_bool(ct, "report_points", false);
        }
        static const char *fkind[3] = {"value", "flux", "exchange"};
        for (int kind = 0; kind < 3; kind++) { /* conditions on the scenario's fields, {field: ...} */
            const JsonValue *o = json_get(v, fkind[kind]);
            if (!o) continue;
            if (o->type != JSON_OBJECT || P.fields.empty()) return err = w + "." + fkind[kind] + ": {field's name: ...} for the scenario's fields", false;
            for (size_t j = 0; j < o->u.object.len; j++) {
                std::string fn = o->u.object.keys[j], ww = w + "." + fkind[kind] + "." + fn;
                int fi = -1;
                for (size_t k = 0; k < P.fields.size(); k++)
                    if (P.fields[k].name == fn) fi = (int)k;
                if (fi < 0) return err = w + "." + fkind[kind] + ": no field named \"" + fn + "\"", false;
                size_t c = 0;
                while (c < b.fbc.size() && b.fbc[c].field != fi) c++;
                if (c == b.fbc.size()) b.fbc.push_back(FieldBC()), b.fbc.back().field = fi;
                FieldBC &bc = b.fbc[c];
                const JsonValue *x = o->u.object.values[j];
                if (kind == 0 && !get_fnum(x, bc.value, &P.fsym, ww, err)) return false;
                if (kind == 1 && !get_fnum(x, bc.flux, &P.fsym, ww, err)) return false;
                if (kind == 2) {
                    if (!get_fnum(json_get(x, "h"), bc.h, &P.fsym, ww + ".h", err) || !get_fnum(json_get(x, "ambient"), bc.ambient, &P.fsym, ww + ".ambient", err))
                        return false;
                    if (!bc.h.set || !bc.ambient.set) return err = ww + ": {\"h\": the exchange coefficient, \"ambient\": the outside value}", false;
                }
            }
        }
        for (const FieldBC &c : b.fbc)
            if (b.where.kind == Sel::POINT && (c.flux.set || c.h.set || fnum_fields(c.value, (int)P.fields.size())))
                return err = w + ": a point can only hold a field's value, a number or a formula of x, y, z, t", false;
        P.bnd.push_back(b);
    }
    /* checks against exact solutions, probes, pictures */
    const JsonValue *cj = json_get(root, "check");
    if (!get_vec3(json_get(cj, "displacement_m"), P.check_u, "check.displacement_m", err) || !get_num(json_get(cj, "temperature_k"), P.check_T, "check.temperature_k", err))
        return false;
    const JsonValue *pj = json_get(json_get(root, "outputs"), "probes");
    for (size_t i = 0; i < json_len(pj); i++) {
        const JsonValue *v = json_at(pj, i), *pt = json_get(v, "point_m");
        if (json_len(pt) != 3) return err = "outputs.probes: each {\"name\", \"point_m\": [x, y, z]}", false;
        Problem::Probe pr;
        pr.name = json_get_str(v, "name", ("probe " + std::to_string(i + 1)).c_str());
        for (int k = 0; k < 3; k++) pr.p[k] = json_at(pt, (size_t)k)->u.number;
        P.probes.push_back(pr);
    }
    const JsonValue *xj = json_get(json_get(root, "outputs"), "crossings");
    for (size_t i = 0; i < json_len(xj); i++) {
        const JsonValue *v = json_at(xj, i);
        Problem::Crossing c;
        std::string pn = json_get_str(v, "probe", ""), fn = json_get_str(v, "field", "");
        for (size_t k = 0; k < P.probes.size(); k++)
            if (P.probes[k].name == pn) c.probe = (int)k;
        for (size_t k = 0; k < P.fields.size(); k++)
            if (P.fields[k].name == fn) c.field = (int)k;
        const JsonValue *val = json_get(v, "value");
        if (c.probe < 0 || c.field < 0 || !val || val->type != JSON_NUMBER)
            return err = "outputs.crossings: each {\"probe\": a probe's name, \"field\": a field's name, \"value\": a number}", false;
        c.value = val->u.number;
        P.crossings.push_back(c);
    }
    P.check_field.assign(P.fields.size(), FNum());
    const JsonValue *cf = json_get(cj, "fields");
    for (size_t j = 0; cf && cf->type == JSON_OBJECT && j < cf->u.object.len; j++) {
        int fi = -1;
        for (size_t k = 0; k < P.fields.size(); k++)
            if (P.fields[k].name == cf->u.object.keys[j]) fi = (int)k;
        if (fi < 0) return err = std::string("check.fields: no field named \"") + cf->u.object.keys[j] + "\"", false;
        if (!get_fnum(cf->u.object.values[j], P.check_field[(size_t)fi], nullptr, std::string("check.fields.") + cf->u.object.keys[j], err)) return false;
    }
    const JsonValue *tj = json_get(root, "time");
    if (tj) {
        if (P.fields.empty()) return err = "time: for the fields' equations; the library's heat and elasticity are steady", false;
        P.t_end = json_get_num(tj, "end_s", 0), P.dt = json_get_num(tj, "step_s", 0);
        if (!(P.t_end > 0) || !(P.dt > 0) || P.dt > P.t_end) return err = "time: {\"end_s\": how long, \"step_s\": the step, no longer than end_s}", false;
        double n = std::ceil(P.t_end / P.dt - 1e-9);
        if (n > 1e6) return err = "time: more than a million steps", false;
        P.steps = (int)n, P.dt = P.t_end / P.steps;
        std::string m = json_get_str(tj, "method", "bdf2");
        if (m == "bdf2") P.bdf2 = true;
        else if (m == "backward_euler") P.bdf2 = false;
        else return err = "time.method: \"bdf2\" (second order, the default) or \"backward_euler\" (first order)", false;
        P.outputs = (int)json_get_int(tj, "outputs", std::min(P.steps, 20));
        P.outputs = std::max(1, std::min(P.outputs, P.steps));
        P.transient = true;
    }
    const JsonValue *sj = json_get(root, "solver");
    P.tol = json_get_num(sj, "tolerance", 1e-12);
    P.contact_tol = json_get_num(sj, "contact_tolerance", 1e-8);
    P.contact_max_al = (int)json_get_int(sj, "contact_iterations", 200);
    P.max_iter = (int)json_get_int(sj, "max_iterations", 50);
    if (!(P.tol > 0) || P.max_iter < 1) return err = "solver: {\"tolerance\": above 0, \"max_iterations\": at least 1}", false;
    const JsonValue *lj2 = json_get(json_get(root, "outputs"), "lines");
    for (size_t i = 0; i < json_len(lj2); i++) {
        const JsonValue *v = json_at(lj2, i), *fa = json_get(v, "from_m"), *ta = json_get(v, "to_m");
        Problem::Line l;
        l.name = json_get_str(v, "name", ("line " + std::to_string(i + 1)).c_str());
        l.n = (int)json_get_int(v, "points", 21);
        if (json_len(fa) != 3 || json_len(ta) != 3 || l.n < 2 || l.n > 10000)
            return err = "outputs.lines: each {\"name\", \"from_m\": [x, y, z], \"to_m\": [x, y, z], \"points\": 2 to 10000}", false;
        for (int k = 0; k < 3; k++) l.a[k] = json_at(fa, (size_t)k)->u.number, l.b[k] = json_at(ta, (size_t)k)->u.number;
        l.first = P.probes.size();
        for (int j = 0; j < l.n; j++) {
            Problem::Probe pr;
            pr.name = l.name + " " + std::to_string(j);
            for (int k = 0; k < 3; k++) pr.p[k] = l.a[k] + (l.b[k] - l.a[k]) * j / (l.n - 1);
            P.probes.push_back(pr);
        }
        P.lines.push_back(l);
    }
    P.frames = json_get_bool(json_get(root, "run"), "frames", true);
    P.deform_scale = json_get_num(json_get(root, "picture"), "deformation_scale", 0);
    return true;
}

/* where each element and boundary face was in a mapped box's own coordinates (empty: no map) */
struct RefCentres {
    std::vector<std::array<double, 3>> elem, bdr;
};

std::array<double, 3> centre_of(ElementTransformation *T, Geometry::Type g) {
    double c[3] = {0, 0, 0};
    Vector C(c, 3);
    T->Transform(Geometries.GetCenter(g), C);
    return {c[0], c[1], c[2]};
}

/* what a part read from an STL became (the record's "mesh") */
struct PartMeshInfo {
    bool stl = false;
    KnTetMesh m = {};
};

/* a part from an STL file: the repository's tetrahedral mesher (kn_stl.c), then MFEM's mesh; quadratic tetrahedra
 * carry their mid-edge nodes, which the mesher put on the surface, as the mesh's curved geometry */
bool stl_mesh(const JsonValue *mj, const char *stl, int order, Mesh &mesh, PartMeshInfo &info, std::string &err) {
    double sc = json_get_num(mj, "scale", 1), o[3] = {0, 0, 0}, h = json_get_num(mj, "element_size_m", 0);
    const JsonValue *org = json_get(mj, "origin_m");
    for (int k = 0; k < 3 && json_len(org) == 3; k++) o[k] = json_at(org, (size_t)k)->u.number;
    if (!(sc > 0)) return err = "mesh.scale: a positive factor (the file's unit in metres)", false;
    if (!(h > 0)) return err = "mesh.element_size_m: the tetrahedra's size at the part's surface, in metres", false;
    KnTetMesh &tm = info.m;
    char e2[512] = "";
    if (!kn_stl_mesh(stl, sc, o, h, json_get_num(mj, "interior_size_m", 0), order >= 2 ? 2 : 1, json_get_int(mj, "max_elements", 300000), &tm, e2,
                     sizeof e2))
        return err = std::string("mesh.stl: ") + e2, false;
    info.stl = true;
    std::vector<int> vid((size_t)tm.nnodes, -1), node_of;
    for (int i = 0; i < 4 * tm.nelems; i++)
        if (vid[(size_t)tm.corner[i]] < 0) vid[(size_t)tm.corner[i]] = (int)node_of.size(), node_of.push_back(tm.corner[i]);
    Mesh m(3, (int)node_of.size(), tm.nelems, tm.nfaces, 3);
    for (int n : node_of) m.AddVertex(tm.xyz + 3 * (size_t)n);
    for (int e = 0; e < tm.nelems; e++) {
        int c[4];
        for (int a = 0; a < 4; a++) c[a] = vid[(size_t)tm.corner[4 * (size_t)e + a]];
        m.AddTet(c, 1);
    }
    for (int f = 0; f < tm.nfaces; f++) {
        int c[3];
        for (int a = 0; a < 3; a++) c[a] = vid[(size_t)tm.face[3 * (size_t)f + a]];
        m.AddBdrTriangle(c, 1);
    }
    m.FinalizeTetMesh(1, 1, true);
    if (tm.order == 2) {
        std::map<std::pair<int, int>, int> mid;
        for (int i = 0; i < 6 * tm.nelems; i++) {
            const int *q = tm.edge + 3 * (size_t)i;
            int a = vid[(size_t)q[0]], b = vid[(size_t)q[1]];
            mid[{std::min(a, b), std::max(a, b)}] = q[2];
        }
        m.SetCurvature(2);
        GridFunction *nodes = m.GetNodes();
        const FiniteElementSpace *fes = nodes->FESpace();
        for (int ed = 0; ed < m.GetNEdges(); ed++) {
            Array<int> ev, dofs;
            m.GetEdgeVertices(ed, ev);
            auto it = mid.find({std::min(ev[0], ev[1]), std::max(ev[0], ev[1])});
            fes->GetEdgeInteriorDofs(ed, dofs);
            if (it == mid.end() || dofs.Size() != 1) return err = "mesh.stl: an edge without its middle node", false;
            for (int k = 0; k < 3; k++) (*nodes)(fes->DofToVDof(dofs[0], k)) = tm.xyz[3 * (size_t)it->second + k];
        }
    }
    mesh = std::move(m);
    return true;
}

/* refuses a mesh that folds over itself: an element whose Jacobian is not positive inside it (at the points of a rule
 * exact for its shape's degree) would add a negative stiffness and give answers that look like any other; at its corners
 * the Jacobian may be zero (a degenerate corner, as a square mapped onto a disc has) but not negative. A map whose
 * derivative vanishes on a face (x^3 at 0) folds curved elements: their middle nodes are the map's values, and the
 * parabola through them turns back near the face */
bool check_mesh_folds(Mesh &mesh, std::string &err) {
    int co = mesh.GetNodes() ? mesh.GetNodes()->FESpace()->GetMaxElementOrder() : 1;
    for (int e = 0; e < mesh.GetNE(); e++) {
        Geometry::Type g = mesh.GetElementBaseGeometry(e);
        ElementTransformation *T = mesh.GetElementTransformation(e);
        const IntegrationRule &in = IntRules.Get(g, 2 * co + 2), &corners = *Geometries.GetVertices(g);
        double big = 0;
        for (int pass = 0; pass < 2; pass++) {
            const IntegrationRule &ir = pass == 0 ? in : corners;
            for (int q = 0; q < ir.GetNPoints(); q++) {
                T->SetIntPoint(&ir.IntPoint(q));
                double det = T->Jacobian().Det();
                if (pass == 0) big = std::fmax(big, det);
                if (pass == 0 ? det > 0 : det >= -1e-9 * big) continue;
                double c[3];
                Vector C(c, 3);
                T->Transform(ir.IntPoint(q), C);
                char b[400];
                snprintf(b, sizeof b, "mesh: element %d folds over itself near (%.4g, %.4g, %.4g) m (its Jacobian is %.3g there: it must be positive inside an element and not negative at its corners). "
                         "A map_m whose derivative vanishes on a face (like x^3 at x = 0) does this to curved elements: give it a slope there (0.1*x + 0.9*x^3)", e, c[0], c[1], c[2], det);
                return err = b, false;
            }
        }
    }
    return true;
}

bool make_mesh(const JsonValue *root, int order, Mesh &mesh, RefCentres &ref, PartMeshInfo &info, std::string &err) {
    const JsonValue *mj = json_get(root, "mesh"), *box = json_get(mj, "box"), *map = json_get(mj, "map_m");
    const char *file = json_get_str(mj, "file", nullptr), *stl = json_get_str(mj, "stl", nullptr);
    if (box) {
        const JsonValue *sz = json_get(box, "size_m"), *nc = json_get(box, "cells"), *org = json_get(box, "origin_m");
        if (json_len(sz) != 3 || json_len(nc) != 3) return err = "mesh.box: size_m [Lx, Ly, Lz] and cells [nx, ny, nz]", false;
        int n[3];
        double L[3], o[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            n[k] = (int)json_at(nc, (size_t)k)->u.number, L[k] = json_at(sz, (size_t)k)->u.number;
            if (json_len(org) == 3) o[k] = json_at(org, (size_t)k)->u.number;
            if (n[k] < 1 || !(L[k] > 0)) return err = "mesh.box: positive sizes and at least one cell a side", false;
        }
        bool tets = json_get_bool(box, "tetrahedra", false);
        mesh = Mesh::MakeCartesian3D(n[0], n[1], n[2], tets ? Element::TETRAHEDRON : Element::HEXAHEDRON, L[0], L[1], L[2], false);
        for (int i = 0; i < mesh.GetNV(); i++) {
            real_t *v = mesh.GetVertex(i);
            for (int k = 0; k < 3; k++) v[k] += o[k];
        }
        for (int r = 0, nr = (int)json_get_int(mj, "refine", 0); r < nr; r++) mesh.UniformRefinement();
        if (map) { /* the box's points carried by formulas: a ring, a tube, a twisted bar; curved to the elements' order */
            Num f[3];
            if (!get_vec3(map, f, "mesh.map_m", err)) return false;
            for (int k = 0; k < 3; k++)
                if (!f[k].set) return err = "mesh.map_m: three formulas of x, y, z (the box's coordinates)", false;
            for (int e = 0; e < mesh.GetNE(); e++) ref.elem.push_back(centre_of(mesh.GetElementTransformation(e), mesh.GetElementBaseGeometry(e)));
            for (int i = 0; i < mesh.GetNBE(); i++) ref.bdr.push_back(centre_of(mesh.GetBdrElementTransformation(i), mesh.GetBdrElementGeometry(i)));
            int co = (int)json_get_int(mj, "curved_order", order);
            if (co > 1) mesh.SetCurvature(co);
            VectorFunctionCoefficient tr(3, [f](const Vector &x, Vector &y) {
                y.SetSize(3);
                for (int k = 0; k < 3; k++) y(k) = f[k](x.GetData());
            });
            mesh.Transform(tr);
        }
        return check_mesh_folds(mesh, err);
    } else if (stl) {
        if (!stl_mesh(mj, stl, order, mesh, info, err)) return false;
    } else if (file) {
        FILE *f = fopen(file, "r");
        if (!f) return err = std::string("mesh.file: cannot open ") + file + " (paths are from the working directory)", false;
        fclose(f);
        mesh = Mesh(file, 1, 1);
        if (mesh.Dimension() != 3) return err = "mesh.file: a 3D mesh is needed", false;
        /* a file's own units: scale to metres, then move (a curved mesh's nodes with it) */
        double sc = json_get_num(mj, "scale", 1), o[3] = {0, 0, 0};
        const JsonValue *org = json_get(mj, "origin_m");
        for (int k = 0; k < 3 && json_len(org) == 3; k++) o[k] = json_at(org, (size_t)k)->u.number;
        if (!(sc > 0)) return err = "mesh.scale: a positive factor (the file's unit in metres)", false;
        if (sc != 1 || o[0] != 0 || o[1] != 0 || o[2] != 0) {
            VectorFunctionCoefficient tr(3, [sc, o](const Vector &x, Vector &y) {
                y.SetSize(3);
                for (int k = 0; k < 3; k++) y(k) = sc * x(k) + o[k];
            });
            mesh.Transform(tr);
        }
    } else {
        return err = "mesh: {\"box\": {\"size_m\", \"cells\"}}, {\"stl\": a part, \"element_size_m\"} or {\"file\": an MFEM, Gmsh or VTK mesh}", false;
    }
    for (int r = 0, nr = (int)json_get_int(mj, "refine", 0); r < nr; r++) mesh.UniformRefinement();
    if (map) return err = "mesh.map_m: only for a box", false;
    return check_mesh_folds(mesh, err);
}

/* ---------------------------------------------------------------- assigning materials and boundaries */

double mesh_size(Mesh &mesh) {
    Vector lo, hi;
    mesh.GetBoundingBox(lo, hi, 1);
    double d = 0;
    for (int k = 0; k < 3; k++) d = std::fmax(d, hi(k) - lo(k));
    return d;
}

/* every vertex's true position (a curved, mapped mesh moves its nodes, not its vertex list) */
std::vector<std::array<double, 3>> vertex_positions(Mesh &mesh) {
    std::vector<std::array<double, 3>> pos((size_t)mesh.GetNV());
    for (int e = 0; e < mesh.GetNE(); e++) {
        Array<int> vv;
        mesh.GetElementVertices(e, vv);
        ElementTransformation *T = mesh.GetElementTransformation(e);
        const IntegrationRule *ir = Geometries.GetVertices(mesh.GetElementBaseGeometry(e));
        for (int j = 0; j < vv.Size(); j++) {
            Vector X(3);
            T->Transform(ir->IntPoint(j), X);
            pos[(size_t)vv[j]] = {X(0), X(1), X(2)};
        }
    }
    return pos;
}

bool assign(Problem &P, Mesh &mesh, const RefCentres &ref, std::string &err) {
    double tol = 1e-9 * mesh_size(mesh);
    for (const Sel *sp : [&] {
             std::vector<const Sel *> v;
             for (const Sel &q : P.region_sel) v.push_back(&q);
             for (const Boundary &b : P.bnd) v.push_back(&b.where);
             return v;
         }())
        if (sp->box_space && ref.bdr.empty()) return err = "a selection in \"space\": \"box\" needs a box mesh with map_m", false;
    /* elements: the last region that holds an element's centre gives its material; one material: all of them */
    for (int e = 0; e < mesh.GetNE(); e++) {
        std::array<double, 3> cw = centre_of(mesh.GetElementTransformation(e), mesh.GetElementBaseGeometry(e));
        int m = P.region_sel.empty() ? 0 : -1;
        for (size_t r = 0; r < P.region_sel.size(); r++) {
            const double *c = P.region_sel[r].box_space ? ref.elem[(size_t)e].data() : cw.data();
            if (sel_has(P.region_sel[r], c, mesh.GetAttribute(e), tol)) m = P.region_mat[r];
        }
        const double *c = cw.data();
        if (m < 0) return err = "regions: an element at (" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) + ") is in none", false;
        mesh.SetAttribute(e, m + 1);
    }
    /* boundary faces: each face's set of boundaries (a face may be in several: a symmetry plane under a pressure) is
     * its attribute; a boundary's marker holds every attribute whose set includes it */
    std::map<unsigned long long, int> sig_attr;
    std::vector<unsigned long long> attr_sig;
    std::vector<int> old_attr(mesh.GetNBE());
    for (int i = 0; i < mesh.GetNBE(); i++) old_attr[i] = mesh.GetBdrAttribute(i);
    for (int i = 0; i < mesh.GetNBE(); i++) {
        ElementTransformation *T = mesh.GetBdrElementTransformation(i);
        std::array<double, 3> cw = centre_of(T, mesh.GetBdrElementGeometry(i));
        unsigned long long sig = 0;
        for (size_t b = 0; b < P.bnd.size(); b++) {
            const double *c = P.bnd[b].where.box_space ? ref.bdr[(size_t)i].data() : cw.data();
            if (P.bnd[b].where.kind != Sel::POINT && sel_has(P.bnd[b].where, c, old_attr[i], tol)) sig |= 1ULL << b;
        }
        auto it = sig_attr.find(sig);
        int a;
        if (it == sig_attr.end()) {
            attr_sig.push_back(sig);
            a = (int)attr_sig.size();
            sig_attr[sig] = a;
        } else {
            a = it->second;
        }
        mesh.SetBdrAttribute(i, a);
        for (size_t b = 0; b < P.bnd.size(); b++)
            if (sig >> b & 1ULL) {
                P.bnd[b].faces++;
                const IntegrationRule &ir = IntRules.Get(mesh.GetBdrElementGeometry(i), 2);
                for (int q = 0; q < ir.GetNPoints(); q++) {
                    T->SetIntPoint(&ir.IntPoint(q));
                    P.bnd[b].area += ir.IntPoint(q).weight * T->Weight();
                }
            }
    }
    mesh.SetAttributes();
    int na = mesh.bdr_attributes.Size() ? mesh.bdr_attributes.Max() : 0;
    std::vector<std::array<double, 3>> pos;
    for (size_t b = 0; b < P.bnd.size(); b++) {
        Boundary &B = P.bnd[b];
        B.marker.SetSize(na);
        B.marker = 0;
        for (int a = 1; a <= na; a++)
            if (attr_sig[(size_t)(a - 1)] >> b & 1ULL) B.marker[a - 1] = 1;
        if (B.where.kind == Sel::POINT) { /* the nearest vertex */
            if (pos.empty()) pos = vertex_positions(mesh);
            double best = INFINITY;
            for (int v = 0; v < mesh.GetNV(); v++) {
                const double *x = pos[(size_t)v].data();
                double d = std::hypot(std::hypot(x[0] - B.where.p[0], x[1] - B.where.p[1]), x[2] - B.where.p[2]);
                if (d < best) best = d, B.vertex = v;
            }
            for (int k = 0; k < 3; k++) B.vpos[k] = pos[(size_t)B.vertex][(size_t)k];
        } else if (B.faces == 0) {
            return err = "boundary " + B.name + ": its selection holds no face of the mesh", false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- solving */

bool solve_spd(BilinearForm &a, LinearForm &b, Array<int> &ess, GridFunction &x, double &factor_mb, std::string &err) {
    SparseMatrix A;
    Vector X, B;
    a.FormLinearSystem(ess, x, b, A, X, B);
    char e2[256] = "";
    SpDirect *s = spd_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), e2, sizeof e2);
    if (!s) return err = std::string("the system is not positive definite (is the body held against every rigid motion?): ") + e2, false;
    factor_mb = std::fmax(factor_mb, spd_factor_bytes(s) / 1048576.0);
    X = B;
    bool ok = spd_solve(s, X.GetData());
    spd_free(s);
    if (!ok) return err = "the solve failed", false;
    a.RecoverFEMSolution(X, b, x);
    return true;
}

/* ---------------------------------------------------------------- fields: solving the scenario's own equations */

/* what a field's formula sees at a point: the position, the time, every field's value and gradient, the material's
 * properties (on a boundary face, those of the element behind it) */
struct FieldEnv {
    Mesh *mesh = nullptr;
    const Problem *P = nullptr;
    std::vector<GridFunction *> gf;
    double t = 0;
    std::vector<std::vector<const Num *>> props; /* per material, per property slot: its value, or none */
    /* each field's values on one element, fetched once for all the element's points (invalidate after a solve) */
    mutable int cache_el = -1;
    mutable std::vector<Vector> loc;
    mutable std::vector<char> have;
    mutable Array<int> dofs;
    mutable Vector shape;
    mutable DenseMatrix dshape;
    void setup() {
        loc.resize(gf.size()), have.assign(gf.size(), 0);
        props.assign(P->mats.size(), std::vector<const Num *>(P->props.size(), nullptr));
        for (size_t m = 0; m < P->mats.size(); m++)
            for (size_t j = 0; j < P->props.size(); j++) {
                auto it = P->mats[m].props.find(P->props[j]);
                if (it != P->mats[m].props.end()) props[m][j] = &it->second;
            }
    }
    void invalidate() const { cache_el = -1; }
    double eval(const FNum &n, ElementTransformation &T, const IntegrationPoint &ip) const {
        if (!n.e) return n.c;
        double x[3] = {0, 0, 0}, slots[128];
        bool have_x = false;
        auto pos = [&]() {
            if (!have_x) {
                Vector X(x, 3);
                T.Transform(ip, X);
                have_x = true;
            }
        };
        T.SetIntPoint(&ip);
        if (n.e->uses_position()) pos();
        int nf = (int)gf.size(), gk = -1;
        double g[3] = {0, 0, 0};
        bool elem = T.ElementType == ElementTransformation::ELEMENT;
        if (elem && T.ElementNo != cache_el) cache_el = T.ElementNo, std::fill(have.begin(), have.end(), 0);
        for (int sl : n.e->slots_used()) {
            if (sl < 4 * nf) {
                int k = sl / 4, c = sl % 4;
                const GridFunction &u = *gf[(size_t)k];
                if (!elem) { /* a boundary face: MFEM's own evaluation */
                    if (c == 0) {
                        slots[sl] = u.GetValue(T, ip);
                    } else {
                        Vector gv(3);
                        if (gk != k) u.GetGradient(T, gv), g[0] = gv(0), g[1] = gv(1), g[2] = gv(2), gk = k;
                        slots[sl] = g[c - 1];
                    }
                    continue;
                }
                const FiniteElement *fe = u.FESpace()->GetFE(T.ElementNo);
                if (!have[(size_t)k]) {
                    u.FESpace()->GetElementDofs(T.ElementNo, dofs);
                    u.GetSubVector(dofs, loc[(size_t)k]);
                    have[(size_t)k] = 1;
                }
                if (c == 0) {
                    shape.SetSize(fe->GetDof());
                    fe->CalcShape(ip, shape);
                    slots[sl] = shape * loc[(size_t)k];
                } else {
                    if (gk != k) {
                        dshape.SetSize(fe->GetDof(), 3);
                        fe->CalcPhysDShape(T, dshape);
                        Vector gv(g, 3);
                        dshape.MultTranspose(loc[(size_t)k], gv);
                        gk = k;
                    }
                    slots[sl] = g[c - 1];
                }
            } else {
                int attr = T.Attribute;
                if (!elem) {
                    int el = -1, info = 0;
                    mesh->GetBdrElementAdjacentElement(T.ElementNo, el, info);
                    attr = el >= 0 ? mesh->GetAttribute(el) : 1;
                }
                const Num *v = props[(size_t)(attr - 1)][(size_t)(sl - 4 * nf)];
                if (!v) {
                    slots[sl] = NAN;
                } else if (v->e) {
                    pos();
                    slots[sl] = (*v)(x);
                } else {
                    slots[sl] = v->c;
                }
            }
        }
        return n.e->eval(x, t, slots);
    }
};

/* a field's coefficient (times scale; times a second formula when n2 is set: an exchange's h times its ambient) */
class FCoef : public Coefficient {
public:
    const FieldEnv *env;
    FNum n, n2;
    double scale;
    FCoef(const FieldEnv *e, const FNum &a, double s = 1) : env(e), n(a), scale(s) {}
    real_t Eval(ElementTransformation &T, const IntegrationPoint &ip) override {
        double v = env->eval(n, T, ip);
        if (n2.set) v *= env->eval(n2, T, ip);
        return scale * v;
    }
};

class FVecCoef : public VectorCoefficient {
public:
    const FieldEnv *env;
    FNum n[3];
    FVecCoef(const FieldEnv *e, const FNum *v) : VectorCoefficient(3), env(e) {
        for (int k = 0; k < 3; k++) n[k] = v[k];
    }
    void Eval(Vector &V, ElementTransformation &T, const IntegrationPoint &ip) override {
        V.SetSize(3);
        for (int k = 0; k < 3; k++) V(k) = n[k].set ? env->eval(n[k], T, ip) : 0.0;
    }
};

/* a diffusion tensor: a diagonal (3) or full, row by row (9) */
class FMatCoef : public MatrixCoefficient {
public:
    const FieldEnv *env;
    FNum n[9];
    int count;
    FMatCoef(const FieldEnv *e, const FNum *v, int c) : MatrixCoefficient(3), env(e), count(c) {
        for (int k = 0; k < c; k++) n[k] = v[k];
    }
    void Eval(DenseMatrix &K, ElementTransformation &T, const IntegrationPoint &ip) override {
        K.SetSize(3);
        K = 0.0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                if (count == 9) K(i, j) = env->eval(n[3 * i + j], T, ip);
                else if (i == j) K(i, i) = env->eval(n[i], T, ip);
    }
};

struct Stress {
    double vm, s[6], p[3]; /* p: the principal stresses, largest first */
};

/* the eigenvalues of a symmetric 3 x 3 stress (xx yy zz xy yz xz), largest first: the trigonometric solution of the
 * characteristic cubic (Smith, 1961) */
void principal(const double *s, double *p) {
    double a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5];
    double off = d * d + e * e + f * f, q = (a + b + c) / 3;
    double p2 = (a - q) * (a - q) + (b - q) * (b - q) + (c - q) * (c - q) + 2 * off;
    if (off == 0 || p2 == 0) {
        p[0] = a, p[1] = b, p[2] = c;
        std::sort(p, p + 3, [](double x, double y) { return x > y; });
        return;
    }
    double pp = std::sqrt(p2 / 6), B[6] = {(a - q) / pp, (b - q) / pp, (c - q) / pp, d / pp, e / pp, f / pp};
    double det = B[0] * (B[1] * B[2] - B[4] * B[4]) - B[3] * (B[3] * B[2] - B[4] * B[5]) + B[5] * (B[3] * B[4] - B[1] * B[5]);
    double r = det / 2, phi = r <= -1 ? M_PI / 3 : r >= 1 ? 0 : std::acos(r) / 3;
    p[0] = q + 2 * pp * std::cos(phi), p[2] = q + 2 * pp * std::cos(phi + 2 * M_PI / 3), p[1] = 3 * q - p[0] - p[2];
}

/* the stress at a point of an element, the thermal strain taken out */
Stress stress_at(const GridFunction &u, const TempField &temp, double T_ref, const std::vector<Material> &mats, ElementTransformation &T,
                 const IntegrationPoint &ip) {
    T.SetIntPoint(&ip);
    DenseMatrix g(3, 3);
    u.GetVectorGradient(T, g);
    double x[3];
    Vector X(x, 3);
    T.Transform(ip, X);
    const Material &m = mats[(size_t)(T.Attribute - 1)];
    double lam = lame_lambda(m, x), mu = lame_mu(m, x), th = temp.any() ? m.alpha(x) * (temp.at(T, ip) - T_ref) : 0;
    double e[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) e[i][j] = 0.5 * (g(i, j) + g(j, i)) - (i == j ? th : 0);
    double tr = e[0][0] + e[1][1] + e[2][2], s[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) s[i][j] = 2 * mu * e[i][j] + (i == j ? lam * tr : 0);
    Stress r;
    r.s[0] = s[0][0], r.s[1] = s[1][1], r.s[2] = s[2][2], r.s[3] = s[0][1], r.s[4] = s[1][2], r.s[5] = s[0][2];
    r.vm = std::sqrt(0.5 * ((s[0][0] - s[1][1]) * (s[0][0] - s[1][1]) + (s[1][1] - s[2][2]) * (s[1][1] - s[2][2]) + (s[2][2] - s[0][0]) * (s[2][2] - s[0][0])) +
                     3 * (s[0][1] * s[0][1] + s[1][2] * s[1][2] + s[0][2] * s[0][2]));
    principal(r.s, r.p);
    return r;
}

/* the body's surface for the pictures: true vertex positions, boundary quadrilaterals and triangles */
struct SurfacePic {
    int nv = 0;
    std::vector<double> xyz;
    std::vector<int> quads, tris;
};

SurfacePic surface_pic(Mesh &mesh);
std::vector<double> vertex_values(Mesh &mesh, const GridFunction &u);
JsonValue *jvec(const double *v, int n);
double bnd_integral(Mesh &mesh, const Boundary &B, int order, const std::function<double(ElementTransformation &, const IntegrationPoint &)> &f);

/* gravity on the body: rho g, with each material's density */
class GravCoef : public VectorCoefficient {
public:
    const std::vector<Material> *mats = nullptr;
    Num g[3];
    GravCoef() : VectorCoefficient(3) {}
    void Eval(Vector &V, ElementTransformation &T, const IntegrationPoint &ip) override {
        double x[3];
        Vector X(x, 3);
        T.Transform(ip, X);
        V.SetSize(3);
        double rho = (*mats)[(size_t)(T.Attribute - 1)].rho(x);
        for (int k = 0; k < 3; k++) V(k) = g[k].set ? rho * g[k](x) : 0;
    }
};

/* the library's elasticity loads other than the thermal strain's: body force, gravity, tractions, forces, pressures */
bool mech_loads(Problem &P, LinearForm &b, std::vector<std::unique_ptr<Coefficient>> &keep, std::vector<std::unique_ptr<VectorCoefficient>> &keepv,
                std::string &err) {
    if (P.body[0].set || P.body[1].set || P.body[2].set) {
        keepv.emplace_back(new Num3Coef(P.body));
        b.AddDomainIntegrator(new VectorDomainLFIntegrator(*keepv.back()));
    }
    if (P.gravity[0].set || P.gravity[1].set || P.gravity[2].set) {
        for (const Material &m : P.mats)
            if (!m.rho.set) return err = "gravity needs density_kg_m3 in material " + m.name, false;
        GravCoef *gc = new GravCoef();
        gc->mats = &P.mats;
        for (int k = 0; k < 3; k++) gc->g[k] = P.gravity[k];
        keepv.emplace_back(gc);
        b.AddDomainIntegrator(new VectorDomainLFIntegrator(*gc));
    }
    for (Boundary &B : P.bnd) {
        if (B.where.kind == Sel::POINT) continue;
        if (B.traction[0].set || B.traction[1].set || B.traction[2].set) {
            keepv.emplace_back(new Num3Coef(B.traction));
            b.AddBoundaryIntegrator(new VectorBoundaryLFIntegrator(*keepv.back()), B.marker);
        }
        if (B.has_force) {
            Num t[3];
            for (int k = 0; k < 3; k++) t[k].set = true, t[k].c = B.force[k] / B.area;
            keepv.emplace_back(new Num3Coef(t));
            b.AddBoundaryIntegrator(new VectorBoundaryLFIntegrator(*keepv.back()), B.marker);
        }
        if (B.pressure.set) { /* a pressure pushes into the body: a traction -p n */
            Num p = B.pressure;
            keep.emplace_back(new FunctionCoefficient([p](const Vector &x) { return -p(x.GetData()); }));
            b.AddBoundaryIntegrator(new VectorBoundaryFluxLFIntegrator(*keep.back()), B.marker);
        }
    }
    return true;
}

/* the held components: faces by marker, points by their vertex; their values projected into U first */
void held_dofs(Problem &P, FiniteElementSpace &V, GridFunction &U, Array<int> &ess) {
    for (Boundary &B : P.bnd) {
        if (!(B.fix[0] || B.fix[1] || B.fix[2])) continue;
        if (B.where.kind == Sel::POINT) {
            Array<int> vd;
            V.GetVertexVDofs(B.vertex, vd);
            const double *xx = B.vpos;
            for (int k = 0; k < 3; k++)
                if (B.fix[k]) U(vd[k]) = B.disp[k].set ? B.disp[k](xx) : 0.0, ess.Append(vd[k]);
            continue;
        }
        /* only the held components are projected: a free one keeps what another boundary gave it */
        NumCoef c0(B.disp[0]), c1(B.disp[1]), c2(B.disp[2]);
        ConstantCoefficient zero(0.0);
        Coefficient *cc[3] = {B.fix[0] ? (B.disp[0].set ? (Coefficient *)&c0 : &zero) : nullptr, B.fix[1] ? (B.disp[1].set ? (Coefficient *)&c1 : &zero) : nullptr,
                              B.fix[2] ? (B.disp[2].set ? (Coefficient *)&c2 : &zero) : nullptr};
        U.ProjectBdrCoefficient(cc, B.marker);
        for (int k = 0; k < 3; k++) {
            if (!B.fix[k]) continue;
            Array<int> c;
            V.GetEssentialTrueDofs(B.marker, c, k);
            ess.Append(c);
        }
    }
    ess.Sort(), ess.Unique();
}

/* the stress's peaks over every element's quadrature points and nodes, each element's own value at a node, not averaged
 * with its neighbours' (a peak on a surface sits at nodes, which quadrature points never reach: on the C-ring's inner
 * fibre they read 3.5 % under): the largest von Mises, the largest principal stress (the most tension) and the smallest
 * (the most compression), each with its place */
struct StressPeaks {
    double vm = 0, p1 = -INFINITY, p3 = INFINITY, vm_at[3] = {0, 0, 0}, p1_at[3] = {0, 0, 0}, p3_at[3] = {0, 0, 0};
};
StressPeaks stress_peaks(Mesh &mesh, FiniteElementSpace &V, const GridFunction &U, const TempField &temp, const Problem &P) {
    StressPeaks k;
    for (int e = 0; e < mesh.GetNE(); e++) {
        ElementTransformation *T = mesh.GetElementTransformation(e);
        const IntegrationRule *rules[2] = {&IntRules.Get(mesh.GetElementBaseGeometry(e), 2 * P.order), &V.GetFE(e)->GetNodes()};
        for (const IntegrationRule *ir : rules)
            for (int q = 0; q < ir->GetNPoints(); q++) {
                Stress st = stress_at(U, temp, P.T_ref, P.mats, *T, ir->IntPoint(q));
                double *at = st.vm > k.vm ? k.vm_at : nullptr, *a1 = st.p[0] > k.p1 ? k.p1_at : nullptr, *a3 = st.p[2] < k.p3 ? k.p3_at : nullptr;
                if (!at && !a1 && !a3) continue;
                double x[3];
                Vector X(x, 3);
                T->Transform(ir->IntPoint(q), X);
                if (at) k.vm = st.vm, std::copy(x, x + 3, at);
                if (a1) k.p1 = st.p[0], std::copy(x, x + 3, a1);
                if (a3) k.p3 = st.p[2], std::copy(x, x + 3, a3);
            }
    }
    return k;
}

/* each boundary's reaction (the residual r = K u - f summed over its held degrees of freedom) and mean displacement */
void record_reactions(Mesh &mesh, Problem &P, FiniteElementSpace &V, const GridFunction &U, const Vector &r, JsonValue *bres) {
    for (Boundary &B : P.bnd) {
        JsonValue *o = json_get(bres, B.name.c_str()) ? (JsonValue *)json_get(bres, B.name.c_str()) : json_set_object(bres, B.name.c_str());
        double R[3] = {0, 0, 0}, mean[3] = {0, 0, 0};
        bool any = B.fix[0] || B.fix[1] || B.fix[2];
        if (B.where.kind == Sel::POINT) {
            Array<int> vd;
            V.GetVertexVDofs(B.vertex, vd);
            for (int k = 0; k < 3; k++) R[k] = B.fix[k] ? r(vd[k]) : 0, mean[k] = U(vd[k]);
        } else {
            for (int k = 0; k < 3 && any; k++) {
                if (!B.fix[k]) continue;
                Array<int> c;
                V.GetEssentialTrueDofs(B.marker, c, k);
                for (int i = 0; i < c.Size(); i++) R[k] += r(c[i]);
            }
            for (int k = 0; k < 3; k++)
                mean[k] = bnd_integral(mesh, B, P.order, [&](ElementTransformation &Tr, const IntegrationPoint &ip) {
                              Vector v(3);
                              U.GetVectorValue(Tr, ip, v);
                              return v(k);
                          }) / B.area;
        }
        if (any) json_set(o, "reaction_n", jvec(R, 3));
        json_set(o, "mean_displacement_m", jvec(mean, 3));
    }
}

/* one field's state: its space, its values now and at the two steps before, its kept system */
struct FieldState {
    std::unique_ptr<H1_FECollection> fec;
    std::unique_ptr<FiniteElementSpace> fes;
    std::unique_ptr<GridFunction> u, u1, u2;
    Array<int> held, ess;
    std::vector<int> pdof;
    std::vector<FNum> pval;
    std::vector<std::array<double, 3>> ppos;
    bool matrix_varies = true, mass_varies = true, rhs_varies = true, symmetric = true, holds = false;
    std::unique_ptr<Vector> b0;           /* the loads, kept while nothing in them varies */
    std::unique_ptr<SparseMatrix> afull;  /* the system before its held values are taken out, and its last right-hand */
    Vector bfull;                         /* side: their residual is what flows in where the field is held */
    std::vector<std::unique_ptr<Coefficient>> acoef, mcoef;
    std::vector<std::unique_ptr<VectorCoefficient>> vcoef;
    std::vector<std::unique_ptr<MatrixCoefficient>> kcoef;
    std::unique_ptr<BilinearForm> a, m; /* after the coefficients they use, so that they go first */
    double a_sigma = -1;
    SpDirect *chol = nullptr;
    KnLU *lu = nullptr;
    int factorizations = 0;
    ~FieldState() {
        spd_free(chol);
        kn_lu_free(lu);
    }
};

/* solves the scenario's fields, steady or in time, writing a frame per output time to w and the results into rec */
bool run_fields(Problem &P, Mesh &mesh, LabWriter *w, JsonValue *rec, std::string &err, int &frames, double &factor_mb, double &t_asm, double &t_solve) {
    int nf = (int)P.fields.size();
    std::vector<std::unique_ptr<FieldState>> fs;
    FieldEnv env;
    env.mesh = &mesh, env.P = &P;
    for (int i = 0; i < nf; i++) {
        auto f = std::make_unique<FieldState>();
        f->fec.reset(new H1_FECollection(P.fields[(size_t)i].order, 3));
        f->fes.reset(new FiniteElementSpace(&mesh, f->fec.get()));
        f->u.reset(new GridFunction(f->fes.get())), f->u1.reset(new GridFunction(f->fes.get())), f->u2.reset(new GridFunction(f->fes.get()));
        *f->u = 0.0;
        env.gf.push_back(f->u.get());
        fs.push_back(std::move(f));
    }
    env.setup();
    int na = mesh.bdr_attributes.Size() ? mesh.bdr_attributes.Max() : 0;
    bool coupled = false;
    for (int i = 0; i < nf; i++) {
        FieldState &f = *fs[(size_t)i];
        const FieldEq &q = P.fields[(size_t)i];
        if (q.initial.set) {
            FCoef c(&env, q.initial);
            f.u->ProjectCoefficient(c);
            env.invalidate();
        }
        f.held.SetSize(na);
        f.held = 0;
        auto varies = [&](const FNum &n) { return fnum_fields(n, nf) || fnum_time(n); };
        bool mv = varies(q.mass) || varies(q.reaction), uses = fnum_fields(q.mass, nf) || fnum_fields(q.reaction, nf) || fnum_fields(q.source, nf);
        for (int c = 0; c < q.ndiff; c++) mv = mv || varies(q.diffusion[c]), uses = uses || fnum_fields(q.diffusion[c], nf);
        for (int c = 0; c < 3; c++) mv = mv || varies(q.convection[c]), uses = uses || fnum_fields(q.convection[c], nf);
        for (Boundary &B : P.bnd)
            for (const FieldBC &c : B.fbc) {
                if (c.field != i) continue;
                mv = mv || varies(c.h);
                uses = uses || fnum_fields(c.h, nf) || fnum_fields(c.ambient, nf) || fnum_fields(c.flux, nf) || fnum_fields(c.value, nf);
                if (!c.value.set) continue;
                if (B.where.kind == Sel::POINT) {
                    Array<int> d;
                    f.fes->GetVertexDofs(B.vertex, d);
                    for (int k = 0; k < d.Size(); k++) f.pdof.push_back(d[k]), f.pval.push_back(c.value), f.ppos.push_back({B.vpos[0], B.vpos[1], B.vpos[2]});
                } else {
                    for (int k = 0; k < na; k++) f.held[k] |= B.marker[k];
                }
            }
        f.fes->GetEssentialTrueDofs(f.held, f.ess);
        for (int d : f.pdof) f.ess.Append(d);
        bool rv = varies(q.source);
        for (Boundary &B : P.bnd)
            for (const FieldBC &c : B.fbc)
                if (c.field == i) rv = rv || varies(c.flux) || varies(c.h) || varies(c.ambient);
        f.matrix_varies = mv, f.mass_varies = varies(q.mass), f.rhs_varies = rv, f.holds = f.ess.Size() > 0;
        f.symmetric = !q.convects && q.ndiff != 9;
        coupled = coupled || uses;
    }
    /* the library's elasticity driven by the fields: assembled and factored once, solved again at every output time
     * with the thermal strain of loads.temperature_k, a formula that may name the fields */
    struct Elastic {
        std::unique_ptr<FiniteElementSpace> V;
        std::unique_ptr<GridFunction> U, held;
        MatCoef lc, mc;
        std::vector<std::unique_ptr<Coefficient>> keep;
        std::vector<std::unique_ptr<VectorCoefficient>> keepv;
        std::unique_ptr<BilinearForm> a; /* after the coefficients it uses */
        std::unique_ptr<SparseMatrix> K; /* before the held values are taken out: the reactions */
        Vector f0, f;                    /* the mechanical loads, and the whole last right-hand side */
        Array<int> ess;
        SpDirect *chol = nullptr;
        TempField temp;
        double vm = 0;
        StressPeaks pk; /* at the last output time */
        ~Elastic() { spd_free(chol); }
    };
    std::unique_ptr<Elastic> el;
    H1_FECollection efec(P.order, 3);
    StressPeaks peak;           /* over every output time */
    double vm_t = 0, p1_t = 0, p3_t = 0, u_max = 0;
    if (P.elastic) {
        double ta = now();
        el.reset(new Elastic());
        el->V.reset(new FiniteElementSpace(&mesh, &efec, 3));
        el->U.reset(new GridFunction(el->V.get())), el->held.reset(new GridFunction(el->V.get()));
        *el->U = 0.0, *el->held = 0.0;
        el->lc.mats = el->mc.mats = &P.mats;
        el->lc.f = lame_lambda, el->mc.f = lame_mu;
        el->a.reset(new BilinearForm(el->V.get()));
        el->a->AddDomainIntegrator(new ElasticityIntegrator(el->lc, el->mc));
        LinearForm b0(el->V.get());
        if (!mech_loads(P, b0, el->keep, el->keepv, err)) return false;
        held_dofs(P, *el->V, *el->held, el->ess);
        el->a->Assemble(0); /* keep exact zeros: the pattern stays symmetric, as eliminating held rows and columns needs */
        el->a->Finalize(0);
        el->K.reset(new SparseMatrix(el->a->SpMat()));
        b0.Assemble();
        el->f0 = b0;
        if (P.temp_f.set) {
            const FNum *tf = &P.temp_f;
            const FieldEnv *ep = &env;
            el->temp.fn = [tf, ep](ElementTransformation &T, const IntegrationPoint &ip) { return ep->eval(*tf, T, ip); };
        }
        t_asm += now() - ta;
    }
    auto elastic_solve = [&](double t) -> bool {
        double ta = now();
        el->f = el->f0;
        if (el->temp.any()) {
            LinearForm lt(el->V.get());
            ThermalLoad *tl = new ThermalLoad();
            tl->mats = &P.mats, tl->temp = &el->temp, tl->T_ref = P.T_ref;
            lt.AddDomainIntegrator(tl);
            lt.Assemble();
            el->f += lt;
        }
        Vector b(el->f);
        GridFunction x(el->V.get());
        x = *el->held;
        SparseMatrix A;
        Vector X, B;
        el->a->FormLinearSystem(el->ess, x, b, A, X, B);
        t_asm += now() - ta;
        double ts = now();
        if (!el->chol) {
            char e2[256] = "";
            el->chol = spd_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), e2, sizeof e2);
            if (!el->chol) return err = std::string("elasticity: the system cannot be factored (is the body held against every rigid motion?): ") + e2, false;
            factor_mb = std::fmax(factor_mb, spd_factor_bytes(el->chol) / 1048576.0);
        }
        X = B;
        if (!spd_solve(el->chol, X.GetData())) return err = "elasticity: the solve failed", false;
        el->a->RecoverFEMSolution(X, b, x);
        *el->U = x;
        t_solve += now() - ts;
        el->pk = stress_peaks(mesh, *el->V, *el->U, el->temp, P);
        el->vm = el->pk.vm;
        if (el->pk.vm > peak.vm) peak.vm = el->pk.vm, vm_t = t, std::copy(el->pk.vm_at, el->pk.vm_at + 3, peak.vm_at);
        if (el->pk.p1 > peak.p1) peak.p1 = el->pk.p1, p1_t = t, std::copy(el->pk.p1_at, el->pk.p1_at + 3, peak.p1_at);
        if (el->pk.p3 < peak.p3) peak.p3 = el->pk.p3, p3_t = t, std::copy(el->pk.p3_at, el->pk.p3_at + 3, peak.p3_at);
        for (int i = 0; i < el->U->Size(); i++) u_max = std::fmax(u_max, std::fabs((*el->U)(i)));
        return true;
    };
    /* probes: found once */
    Array<int> pel;
    Array<IntegrationPoint> pip;
    if (!P.probes.empty()) {
        DenseMatrix pts(3, (int)P.probes.size());
        for (size_t i = 0; i < P.probes.size(); i++)
            for (int k = 0; k < 3; k++) pts(k, (int)i) = P.probes[i].p[k];
        mesh.FindPoints(pts, pel, pip, false);
    }
    auto probe_values = [&](JsonValue *arr, bool with_points) {
        for (size_t j = 0; j < P.probes.size(); j++) {
            JsonValue *o = json_object();
            json_set_string(o, "name", P.probes[j].name.c_str());
            if (with_points) json_set(o, "point_m", jvec(P.probes[j].p, 3));
            if (pel[(int)j] < 0) {
                json_set_string(o, "note", "outside the mesh");
            } else {
                ElementTransformation *T = mesh.GetElementTransformation(pel[(int)j]);
                T->SetIntPoint(&pip[(int)j]);
                for (int i = 0; i < nf; i++) json_set_number(o, P.fields[(size_t)i].name.c_str(), fs[(size_t)i]->u->GetValue(*T, pip[(int)j]));
                if (el) {
                    Vector v(3);
                    el->U->GetVectorValue(*T, pip[(int)j], v);
                    json_set(o, "displacement_m", jvec(v.GetData(), 3));
                    Stress st = stress_at(*el->U, el->temp, P.T_ref, P.mats, *T, pip[(int)j]);
                    json_set_number(o, "von_mises_pa", st.vm);
                    json_set(o, "stress_pa", jvec(st.s, 6)); /* xx yy zz xy yz xz */
                    json_set(o, "principal_pa", jvec(st.p, 3));
                }
            }
            json_push(arr, o);
        }
    };
    std::vector<std::unique_ptr<LinearForm>> ones; /* the integral of each field over the body */
    ConstantCoefficient one(1.0);
    for (int i = 0; i < nf; i++) {
        ones.emplace_back(new LinearForm(fs[(size_t)i]->fes.get()));
        ones.back()->AddDomainIntegrator(new DomainLFIntegrator(one));
        ones.back()->Assemble();
    }
    SurfacePic pic;
    if (P.frames) pic = surface_pic(mesh);
    JsonValue *series = P.transient ? json_set_array(rec, "series") : nullptr;
    auto output = [&](double t) -> bool {
        if (el && !elastic_solve(t)) return false;
        if (P.frames) {
            std::vector<std::vector<double>> vals;
            for (int i = 0; i < nf; i++) vals.push_back(vertex_values(mesh, *fs[(size_t)i]->u));
            std::vector<double> xyz = pic.xyz, vmv, dmv, p1v;
            if (el) { /* the solid's vertex values, each element's own averaged, and the shape deformed for the picture */
                vmv.assign((size_t)pic.nv, 0.0), dmv.assign((size_t)pic.nv, 0.0), p1v.assign((size_t)pic.nv, 0.0);
                std::vector<double> disp((size_t)pic.nv * 3, 0.0), cnt((size_t)pic.nv, 0.0);
                for (int e = 0; e < mesh.GetNE(); e++) {
                    Array<int> vv;
                    mesh.GetElementVertices(e, vv);
                    ElementTransformation *T = mesh.GetElementTransformation(e);
                    const IntegrationRule *ir = Geometries.GetVertices(mesh.GetElementBaseGeometry(e));
                    for (int j = 0; j < vv.Size(); j++) {
                        T->SetIntPoint(&ir->IntPoint(j));
                        size_t v = (size_t)vv[j];
                        Vector d(3);
                        el->U->GetVectorValue(*T, ir->IntPoint(j), d);
                        for (int k = 0; k < 3; k++) disp[3 * v + k] += d(k);
                        Stress st = stress_at(*el->U, el->temp, P.T_ref, P.mats, *T, ir->IntPoint(j));
                        vmv[v] += st.vm, p1v[v] += st.p[0];
                        cnt[v] += 1;
                    }
                }
                for (size_t v = 0; v < (size_t)pic.nv; v++) {
                    double c = cnt[v] > 0 ? cnt[v] : 1;
                    for (int k = 0; k < 3; k++) disp[3 * v + k] /= c, xyz[3 * v + k] += P.deform_scale * disp[3 * v + k];
                    vmv[v] /= c, p1v[v] /= c;
                    dmv[v] = std::sqrt(disp[3 * v] * disp[3 * v] + disp[3 * v + 1] * disp[3 * v + 1] + disp[3 * v + 2] * disp[3 * v + 2]);
                }
            }
            lab_frame_begin(w, t);
            for (int pass = 0; pass < 2; pass++) {
                std::vector<int> &c = pass ? pic.tris : pic.quads;
                if (c.empty()) continue;
                lab_part_cells(w, pass ? "surface (triangles)" : "surface", pic.nv, xyz.data(), (int)(c.size() / (pass ? 3 : 4)), pass ? LAB_TRI : LAB_QUAD, c.data());
                for (int i = 0; i < nf; i++) lab_field_d(w, P.fields[(size_t)i].name.c_str(), LAB_AT_NODE, (size_t)pic.nv, vals[(size_t)i].data());
                if (el) {
                    lab_field_d(w, "von_mises", LAB_AT_NODE, (size_t)pic.nv, vmv.data()), lab_field_d(w, "displacement", LAB_AT_NODE, (size_t)pic.nv, dmv.data());
                    lab_field_d(w, "max_principal", LAB_AT_NODE, (size_t)pic.nv, p1v.data());
                }
            }
            if (!lab_frame_end(w)) return err = "cannot write a frame (disk full?)", false;
            frames++;
        }
        if (series) {
            JsonValue *o = json_object(), *in = json_set_object(o, "integral");
            json_set_number(o, "t_s", t);
            for (int i = 0; i < nf; i++) json_set_number(in, P.fields[(size_t)i].name.c_str(), (*ones[(size_t)i]) * (*fs[(size_t)i]->u));
            if (el) json_set_number(o, "max_von_mises_pa", el->vm), json_set_number(o, "max_principal_pa", el->pk.p1), json_set_number(o, "min_principal_pa", el->pk.p3);
            if (!P.probes.empty()) probe_values(json_set_array(o, "probes"), false);
            json_push(series, o);
        }
        return true;
    };
    /* on hexahedra, p + 1 Gauss points a direction (exact to degree 2p + 1) instead of MFEM's p + 2 for diffusion: the
     * element loops are most of a coupled run's time; tetrahedra keep MFEM's rules */
    bool hexes = mesh.GetNE() > 0 && mesh.GetElementBaseGeometry(0) == Geometry::CUBE;
    auto ruled = [&](BilinearFormIntegrator *bi, int p) {
        if (hexes) bi->SetIntRule(&IntRules.Get(Geometry::CUBE, 2 * p + 1));
        return bi;
    };
    auto lruled = [&](LinearFormIntegrator *li, int p) {
        if (hexes) li->SetIntRule(&IntRules.Get(Geometry::CUBE, 2 * p + 1));
        return li;
    };
    /* one field's system at env.t: sigma M + K on the left, M hist and the loads on the right */
    auto solve_one = [&](int i, double sigma, const Vector *hist) -> bool {
        FieldState &f = *fs[(size_t)i];
        const FieldEq &q = P.fields[(size_t)i];
        int po = q.order;
        if (!q.mass.set) sigma = 0, hist = nullptr; /* a field without mass follows the others at each instant */
        GridFunction x(f.fes.get());
        x = *f.u;
        for (Boundary &B : P.bnd)
            for (const FieldBC &c : B.fbc)
                if (c.field == i && c.value.set && B.where.kind != Sel::POINT) {
                    FCoef g(&env, c.value);
                    x.ProjectBdrCoefficient(g, B.marker);
                }
        for (size_t k = 0; k < f.pdof.size(); k++) x(f.pdof[k]) = f.pval[k].e ? f.pval[k].e->eval(f.ppos[k].data(), env.t) : f.pval[k].c;
        double ta = now();
        bool rebuild = !f.a || f.matrix_varies || f.a_sigma != sigma;
        if (rebuild) {
            f.a.reset();
            f.acoef.clear(), f.vcoef.clear(), f.kcoef.clear();
            f.a.reset(new BilinearForm(f.fes.get()));
            if (sigma > 0) {
                f.acoef.emplace_back(new FCoef(&env, q.mass, sigma));
                f.a->AddDomainIntegrator(ruled(new MassIntegrator(*f.acoef.back()), po));
            }
            if (q.ndiff == 1) {
                f.acoef.emplace_back(new FCoef(&env, q.diffusion[0]));
                f.a->AddDomainIntegrator(ruled(new DiffusionIntegrator(*f.acoef.back()), po));
            } else if (q.ndiff > 1) {
                f.kcoef.emplace_back(new FMatCoef(&env, q.diffusion, q.ndiff));
                f.a->AddDomainIntegrator(ruled(new DiffusionIntegrator(*f.kcoef.back()), po));
            }
            if (q.convects) {
                f.vcoef.emplace_back(new FVecCoef(&env, q.convection));
                f.a->AddDomainIntegrator(ruled(new ConvectionIntegrator(*f.vcoef.back()), po));
            }
            if (q.reaction.set) {
                f.acoef.emplace_back(new FCoef(&env, q.reaction));
                f.a->AddDomainIntegrator(ruled(new MassIntegrator(*f.acoef.back()), po));
            }
            for (Boundary &B : P.bnd)
                for (const FieldBC &c : B.fbc)
                    if (c.field == i && c.h.set) {
                        f.acoef.emplace_back(new FCoef(&env, c.h));
                        f.a->AddBoundaryIntegrator(new BoundaryMassIntegrator(*f.acoef.back()), B.marker);
                    }
            f.a->Assemble(0);
            f.a->Finalize(0);
            f.a_sigma = sigma;
            if (f.holds) f.afull.reset(new SparseMatrix(f.a->SpMat()));
        }
        Vector b;
        if (!f.rhs_varies && f.b0) {
            b = *f.b0;
        } else {
            LinearForm lf(f.fes.get());
            std::vector<std::unique_ptr<FCoef>> bc;
            if (q.source.set) {
                bc.emplace_back(new FCoef(&env, q.source));
                lf.AddDomainIntegrator(lruled(new DomainLFIntegrator(*bc.back()), po));
            }
            for (Boundary &B : P.bnd)
                for (const FieldBC &c : B.fbc) {
                    if (c.field != i) continue;
                    if (c.flux.set) {
                        bc.emplace_back(new FCoef(&env, c.flux));
                        lf.AddBoundaryIntegrator(new BoundaryLFIntegrator(*bc.back()), B.marker);
                    }
                    if (c.h.set) {
                        bc.emplace_back(new FCoef(&env, c.h));
                        bc.back()->n2 = c.ambient;
                        lf.AddBoundaryIntegrator(new BoundaryLFIntegrator(*bc.back()), B.marker);
                    }
                }
            lf.Assemble();
            b = lf;
            if (!f.rhs_varies) f.b0.reset(new Vector(b));
        }
        if (sigma > 0 && hist) {
            if (!f.m || f.mass_varies) {
                f.m.reset();
                f.mcoef.clear();
                f.mcoef.emplace_back(new FCoef(&env, q.mass));
                f.m.reset(new BilinearForm(f.fes.get()));
                f.m->AddDomainIntegrator(ruled(new MassIntegrator(*f.mcoef.back()), po));
                f.m->Assemble();
                f.m->Finalize();
            }
            f.m->AddMult(*hist, b);
        }
        if (f.holds) f.bfull = b;
        SparseMatrix A;
        Vector X, B;
        f.a->FormLinearSystem(f.ess, x, b, A, X, B);
        t_asm += now() - ta;
        double ts = now();
        if (rebuild) {
            spd_free(f.chol), kn_lu_free(f.lu);
            f.chol = nullptr, f.lu = nullptr;
            char e2[256] = "";
            if (f.symmetric) f.chol = spd_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), e2, sizeof e2);
            else f.lu = kn_lu_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), e2, sizeof e2);
            if (!f.chol && !f.lu)
                return err = "field " + q.name + ": its system cannot be solved (is it held somewhere, or exchanging with the outside?): " + e2, false;
            f.factorizations++;
            factor_mb = std::fmax(factor_mb, (f.chol ? spd_factor_bytes(f.chol) : kn_lu_bytes(f.lu)) / 1048576.0);
        }
        X = B;
        if (!(f.chol ? spd_solve(f.chol, X.GetData()) : kn_lu_solve(f.lu, X.GetData()))) return err = "field " + q.name + ": the solve failed", false;
        f.a->RecoverFEMSolution(X, b, x);
        t_solve += now() - ts;
        for (int k = 0; k < x.Size(); k++)
            if (!std::isfinite(x(k)))
                return err = "field " + q.name + ": a value is not a number (a formula without a value somewhere: a material lacking the property it names, a log or root of a negative)", false;
        *f.u = x;
        env.invalidate();
        return true;
    };
    /* every field once, or in turn until they settle when any formula names a field */
    int it_max = 0;
    double last_change = 0, last_rel = 0;
    bool converged = true;
    auto solve_all = [&](double sigma, std::vector<Vector> *hist) -> bool {
        int it = 1;
        double ch = 0, rel = 0;
        for (; it <= (coupled ? P.max_iter : 1); it++) {
            ch = 0, rel = 0;
            for (int i = 0; i < nf; i++) {
                Vector old(*fs[(size_t)i]->u);
                if (!solve_one(i, sigma, hist ? &(*hist)[(size_t)i] : nullptr)) return false;
                double d = 0, m = 0;
                for (int k = 0; k < old.Size(); k++) d = std::fmax(d, std::fabs((*fs[(size_t)i]->u)(k) - old(k))), m = std::fmax(m, std::fabs((*fs[(size_t)i]->u)(k)));
                ch = std::fmax(ch, d), rel = std::fmax(rel, d / std::fmax(m, 1e-300));
            }
            if (!coupled || rel <= P.tol) break;
        }
        it_max = std::max(it_max, std::min(it, P.max_iter));
        last_change = ch, last_rel = rel;
        if (coupled && rel > P.tol) converged = false;
        return true;
    };
    /* crossings: each watched probe at every step, the first change of side placed within the step */
    std::vector<double> xprev(P.crossings.size(), NAN), xfirst(P.crossings.size(), NAN), xtprev(P.crossings.size(), 0);
    std::vector<int> xdir(P.crossings.size(), 0);
    auto watch = [&](double t) {
        for (size_t c = 0; c < P.crossings.size(); c++) {
            const Problem::Crossing &cr = P.crossings[c];
            if (pel[cr.probe] < 0 || !std::isnan(xfirst[c])) continue;
            ElementTransformation *T = mesh.GetElementTransformation(pel[cr.probe]);
            T->SetIntPoint(&pip[cr.probe]);
            double v = fs[(size_t)cr.field]->u->GetValue(*T, pip[cr.probe]);
            if (!std::isnan(xprev[c]) && (xprev[c] - cr.value) * (v - cr.value) <= 0 && v != xprev[c]) {
                xfirst[c] = xtprev[c] + (cr.value - xprev[c]) * (t - xtprev[c]) / (v - xprev[c]);
                xdir[c] = v > xprev[c] ? 1 : -1;
            }
            xprev[c] = v, xtprev[c] = t;
        }
    };
    if (!P.transient) {
        env.t = 0;
        if (!solve_all(0, nullptr) || !output(0)) return false;
    } else {
        for (auto &f : fs) *f->u1 = *f->u, *f->u2 = *f->u;
        if (!output(0)) return false;
        watch(0);
        std::set<int> outs;
        for (int k = 1; k <= P.outputs; k++) outs.insert((int)std::lround((double)k * P.steps / P.outputs));
        std::vector<Vector> hist((size_t)nf);
        for (int n = 1; n <= P.steps; n++) {
            env.t = n * P.dt;
            bool be = n == 1 || !P.bdf2; /* BDF2 starts with one backward Euler step */
            for (int i = 0; i < nf; i++) {
                const FieldState &f = *fs[(size_t)i];
                hist[(size_t)i] = *f.u1;
                if (be) {
                    hist[(size_t)i] *= 1 / P.dt;
                } else {
                    hist[(size_t)i] *= 2 / P.dt; /* (4 u1 - u2) / (2 dt) */
                    hist[(size_t)i].Add(-0.5 / P.dt, *f.u2);
                }
            }
            if (!solve_all(be ? 1 / P.dt : 1.5 / P.dt, &hist)) return false;
            for (auto &f : fs) *f->u2 = *f->u1, *f->u1 = *f->u;
            env.invalidate();
            watch(env.t);
            if (outs.count(n) && !output(env.t)) return false;
        }
    }
    /* the record */
    JsonValue *fo = json_set_object(rec, "fields");
    for (int i = 0; i < nf; i++) {
        const FieldState &f = *fs[(size_t)i];
        JsonValue *o = json_set_object(fo, P.fields[(size_t)i].name.c_str());
        json_set_string(o, "unit", P.fields[(size_t)i].unit.c_str());
        json_set_number(o, "order", P.fields[(size_t)i].order);
        json_set_number(o, "dofs", f.fes->GetTrueVSize());
        json_set_number(o, "min", f.u->Min()), json_set_number(o, "max", f.u->Max());
        json_set_number(o, "integral", (*ones[(size_t)i]) * (*f.u));
        json_set_string(o, "system", f.symmetric ? "symmetric, Cholesky" : f.lu && !strcmp(kn_lu_method(f.lu), "LU") ? "non-symmetric, LU" : "non-symmetric, QR");
        json_set_number(o, "factorizations", f.factorizations);
    }
    /* each boundary: its size, and what of each field flows in through it at the end: where the field is held, the
     * residual of its system before the held values were taken out; elsewhere, its flux and exchange */
    JsonValue *bo = json_set_object(rec, "boundaries");
    std::vector<Vector> res((size_t)nf);
    for (int i = 0; i < nf; i++) {
        FieldState &f = *fs[(size_t)i];
        if (!f.holds || !f.afull) continue;
        res[(size_t)i].SetSize(f.u->Size());
        f.afull->Mult(*f.u, res[(size_t)i]);
        res[(size_t)i] -= f.bfull;
    }
    for (Boundary &B : P.bnd) {
        JsonValue *o = json_set_object(bo, B.name.c_str()), *fl = nullptr;
        json_set_number(o, "faces", B.faces), json_set_number(o, "area_m2", B.area);
        for (const FieldBC &c : B.fbc) {
            FieldState &f = *fs[(size_t)c.field];
            if (!fl) fl = json_set_object(o, "flow_in");
            double q = 0;
            if (c.value.set && res[(size_t)c.field].Size()) {
                std::set<int> d;
                if (B.where.kind == Sel::POINT) {
                    Array<int> vd;
                    f.fes->GetVertexDofs(B.vertex, vd);
                    for (int k = 0; k < vd.Size(); k++) d.insert(vd[k]);
                } else {
                    for (int be = 0; be < mesh.GetNBE(); be++) {
                        int a = mesh.GetBdrAttribute(be);
                        if (a < 1 || a > B.marker.Size() || !B.marker[a - 1]) continue;
                        Array<int> bd;
                        f.fes->GetBdrElementDofs(be, bd);
                        for (int k = 0; k < bd.Size(); k++) d.insert(bd[k] >= 0 ? bd[k] : -1 - bd[k]);
                    }
                }
                for (int k : d) q += res[(size_t)c.field](k);
            } else {
                const GridFunction &u = *f.u;
                q = bnd_integral(mesh, B, P.fields[(size_t)c.field].order, [&](ElementTransformation &T, const IntegrationPoint &ip) {
                    double v = c.flux.set ? env.eval(c.flux, T, ip) : 0;
                    if (c.h.set) v += env.eval(c.h, T, ip) * (env.eval(c.ambient, T, ip) - u.GetValue(T, ip));
                    return v;
                });
            }
            json_set_number(fl, P.fields[(size_t)c.field].name.c_str(), q);
        }
    }
    if (el) {
        JsonValue *res = json_get(rec, "results") ? (JsonValue *)json_get(rec, "results") : json_set_object(rec, "results");
        json_set_number(res, "elasticity_dofs", el->V->GetTrueVSize());
        json_set_number(res, "max_von_mises_pa", peak.vm), json_set(res, "max_von_mises_at_m", jvec(peak.vm_at, 3)), json_set_number(res, "max_von_mises_t_s", vm_t);
        json_set_number(res, "max_principal_pa", peak.p1), json_set(res, "max_principal_at_m", jvec(peak.p1_at, 3)), json_set_number(res, "max_principal_t_s", p1_t);
        json_set_number(res, "min_principal_pa", peak.p3), json_set(res, "min_principal_at_m", jvec(peak.p3_at, 3)), json_set_number(res, "min_principal_t_s", p3_t);
        json_set_number(res, "max_displacement_component_m", u_max);
        Vector r(el->K->Height());
        el->K->Mult(*el->U, r);
        r -= el->f;
        record_reactions(mesh, P, *el->V, *el->U, r, bo); /* at the last output time */
    }
    if (!P.crossings.empty()) {
        JsonValue *xa = json_set_array(rec, "crossings");
        for (size_t c = 0; c < P.crossings.size(); c++) {
            JsonValue *o = json_object();
            json_set_string(o, "probe", P.probes[(size_t)P.crossings[c].probe].name.c_str());
            json_set_string(o, "field", P.fields[(size_t)P.crossings[c].field].name.c_str());
            json_set_number(o, "value", P.crossings[c].value);
            if (std::isnan(xfirst[c])) json_set(o, "first_s", json_null());
            else json_set_number(o, "first_s", xfirst[c]), json_set_string(o, "direction", xdir[c] > 0 ? "up" : "down");
            json_push(xa, o);
        }
    }
    JsonValue *so = json_set_object(rec, "solver");
    json_set_bool(so, "coupled", coupled), json_set_bool(so, "converged", converged);
    json_set_number(so, "iterations_max", it_max), json_set_number(so, "last_change", last_change), json_set_number(so, "last_relative_change", last_rel);
    json_set_number(so, "tolerance", P.tol);
    if (P.transient) {
        JsonValue *to = json_set_object(rec, "time");
        json_set_number(to, "end_s", P.t_end), json_set_number(to, "step_s", P.dt), json_set_number(to, "steps", P.steps);
        json_set_string(to, "method", P.bdf2 ? "bdf2" : "backward_euler");
    }
    if (!P.probes.empty()) probe_values(json_set_array(rec, "probes"), true);
    JsonValue *chk = nullptr;
    for (int i = 0; i < nf; i++) {
        const FNum &ex = P.check_field[(size_t)i];
        if (!ex.set) continue;
        if (!chk) chk = json_set_object(json_set_object(rec, "check"), "fields");
        double tf = env.t;
        FunctionCoefficient exc([&ex, tf](const Vector &xx) { return ex.e ? ex.e->eval(xx.GetData(), tf) : ex.c; });
        GridFunction zero(fs[(size_t)i]->fes.get());
        zero = 0.0;
        JsonValue *o = json_set_object(chk, P.fields[(size_t)i].name.c_str());
        json_set_number(o, "l2_error", fs[(size_t)i]->u->ComputeL2Error(exc));
        json_set_number(o, "l2_norm", zero.ComputeL2Error(exc));
        json_set_number(o, "t_s", tf);
    }
    if (!converged) err = "the fields did not settle within the iterations allowed (solver.max_iterations)"; /* reported, not fatal */
    return true;
}

SurfacePic surface_pic(Mesh &mesh) {
    SurfacePic s;
    s.nv = mesh.GetNV();
    std::vector<std::array<double, 3>> pos = vertex_positions(mesh);
    s.xyz.resize((size_t)s.nv * 3);
    for (int v = 0; v < s.nv; v++)
        for (int k = 0; k < 3; k++) s.xyz[3 * (size_t)v + k] = pos[(size_t)v][(size_t)k];
    for (int i = 0; i < mesh.GetNBE(); i++) {
        Array<int> vv;
        mesh.GetBdrElementVertices(i, vv);
        std::vector<int> &dst = vv.Size() == 4 ? s.quads : s.tris;
        if (vv.Size() != 4 && vv.Size() != 3) continue;
        for (int j = 0; j < vv.Size(); j++) dst.push_back(vv[j]);
    }
    return s;
}

/* a scalar field at the vertices: each element's own value, averaged */
std::vector<double> vertex_values(Mesh &mesh, const GridFunction &u) {
    int nv = mesh.GetNV();
    std::vector<double> val((size_t)nv, 0.0), cnt((size_t)nv, 0.0);
    for (int e = 0; e < mesh.GetNE(); e++) {
        Array<int> vv;
        mesh.GetElementVertices(e, vv);
        ElementTransformation *T = mesh.GetElementTransformation(e);
        const IntegrationRule *ir = Geometries.GetVertices(mesh.GetElementBaseGeometry(e));
        for (int j = 0; j < vv.Size(); j++) {
            T->SetIntPoint(&ir->IntPoint(j));
            val[(size_t)vv[j]] += u.GetValue(*T, ir->IntPoint(j));
            cnt[(size_t)vv[j]] += 1;
        }
    }
    for (int v = 0; v < nv; v++)
        if (cnt[(size_t)v] > 0) val[(size_t)v] /= cnt[(size_t)v];
    return val;
}

/* ---------------------------------------------------------------- contact with rigid obstacles */

/* the signed distance from a boundary's obstacle to the point y (positive on the body's side) and the obstacle's
 * normal there, toward the body; h is the step of a formula's gradient */
double obstacle_gap(const Boundary &B, const double *y, double *n, double h) {
    double d[3], g = 0;
    switch (B.obstacle) {
    case 1:
        for (int k = 0; k < 3; k++) g += (y[k] - B.op[k]) * B.on[k], n[k] = B.on[k];
        return g;
    case 2: {
        double r = 0;
        for (int k = 0; k < 3; k++) d[k] = y[k] - B.op[k], r += d[k] * d[k];
        r = std::sqrt(r);
        for (int k = 0; k < 3; k++) n[k] = r > 0 ? d[k] / r : 0;
        return r - B.orad;
    }
    case 3: {
        double a = 0, r = 0;
        for (int k = 0; k < 3; k++) d[k] = y[k] - B.op[k], a += d[k] * B.on[k];
        for (int k = 0; k < 3; k++) d[k] -= a * B.on[k], r += d[k] * d[k];
        r = std::sqrt(r);
        for (int k = 0; k < 3; k++) n[k] = r > 0 ? d[k] / r : 0;
        return r - B.orad;
    }
    case 4: {
        double nn = 0;
        for (int k = 0; k < 3; k++) {
            double yp[3] = {y[0], y[1], y[2]}, ym[3] = {y[0], y[1], y[2]};
            yp[k] += h, ym[k] -= h;
            n[k] = (B.odist->eval(yp) - B.odist->eval(ym)) / (2 * h), nn += n[k] * n[k];
        }
        nn = std::sqrt(nn);
        for (int k = 0; k < 3; k++) n[k] = nn > 0 ? n[k] / nn : 0;
        return B.odist->eval(y);
    }
    }
    return INFINITY;
}

/* a point of a contact face: its shape functions, weight and penalty, and its obstacle's multiplier (a pressure) */
struct CPoint {
    int b = 0, be = 0;
    std::vector<int> vd;    /* the face's degrees of freedom: all of x, then of y, then of z */
    std::vector<double> sh; /* the face's shape functions there */
    double x[3], dA = 0, kp = 0, lambda = 0, p = 0, g = 0, n[3] = {0, 0, 0};
    bool active = false;
};

/* frictionless contact of the body with each boundary's rigid obstacle: an augmented Lagrangian (the multipliers are the
 * pressures, updated until the deepest penetration is under tol of the largest displacement) around Newton's method on
 * the penalty's active set. K and f are the unconstrained system and its loads; U carries the held values on entry and
 * the solution on return; fc the obstacles' forces on the body. */
bool solve_contact(const Problem &P, Mesh &mesh, FiniteElementSpace &V, const SparseMatrix &K, const Vector &f, const Array<int> &ess, GridFunction &U,
                   std::vector<CPoint> &pts, Vector &fc, int &newton_total, int &al_iters, bool &converged, int &factorizations, double &factor_mb,
                   std::string &err) {
    double hgrad = 1e-7 * mesh_size(mesh), tol = P.contact_tol;
    /* the points */
    for (size_t bi = 0; bi < P.bnd.size(); bi++) {
        const Boundary &B = P.bnd[bi];
        if (!B.obstacle) continue;
        for (int be = 0; be < mesh.GetNBE(); be++) {
            int a = mesh.GetBdrAttribute(be);
            if (a < 1 || a > B.marker.Size() || !B.marker[a - 1]) continue;
            ElementTransformation *T = mesh.GetBdrElementTransformation(be);
            const FiniteElement *fe = V.GetBE(be);
            Array<int> vd;
            V.GetBdrElementVDofs(be, vd);
            /* on quadrilateral faces, the Gauss-Lobatto points, which are the nodes: one constraint per node (Gauss
             * points would be more constraints than nodes, over-constraining the surface and making the pressure
             * oscillate: K19's first run); triangles keep Gauss points */
            static IntegrationRules lobatto(0, Quadrature1D::GaussLobatto);
            Geometry::Type gt = mesh.GetBdrElementGeometry(be);
            const IntegrationRule &ir = gt == Geometry::SQUARE ? lobatto.Get(gt, 2 * fe->GetOrder() - 1) : IntRules.Get(gt, 2 * fe->GetOrder() + 1);
            for (int q = 0; q < ir.GetNPoints(); q++) {
                const IntegrationPoint &ip = ir.IntPoint(q);
                T->SetIntPoint(&ip);
                CPoint c;
                c.b = (int)bi, c.be = be, c.dA = ip.weight * T->Weight();
                Vector X(c.x, 3);
                T->Transform(ip, X);
                c.kp = B.penalty;
                Vector sh(fe->GetDof());
                fe->CalcShape(ip, sh);
                c.sh.assign(sh.GetData(), sh.GetData() + sh.Size());
                for (int k = 0; k < vd.Size(); k++) c.vd.push_back(vd[k] >= 0 ? vd[k] : -1 - vd[k]);
                pts.push_back(c);
            }
        }
    }
    if (pts.empty()) return err = "contact: no face of the mesh is in a contact boundary", false;
    /* a node shared by several faces is one constraint: points that coincide (the Gauss-Lobatto points of neighbouring
     * quadrilaterals) are merged and their areas summed. Kept apart, the shared node's pressure split arbitrarily
     * between its copies, only their sum being fixed (K19's second run) */
    {
        double eps = 1e-9 * mesh_size(mesh);
        std::map<std::array<long long, 4>, size_t> seen;
        std::vector<CPoint> merged;
        for (CPoint &c : pts) {
            std::array<long long, 4> key = {(long long)c.b, std::llround(c.x[0] / eps), std::llround(c.x[1] / eps), std::llround(c.x[2] / eps)};
            auto it = seen.find(key);
            if (it == seen.end()) {
                seen[key] = merged.size();
                merged.push_back(c);
            } else {
                merged[it->second].dA += c.dA;
            }
        }
        pts.swap(merged);
        /* a point without area (a degenerate corner of a mapped mesh) carries no force: it is left out */
        double amax = 0;
        for (const CPoint &c : pts) amax = std::fmax(amax, c.dA);
        pts.erase(std::remove_if(pts.begin(), pts.end(), [amax](const CPoint &c) { return !(c.dA > 1e-12 * amax); }), pts.end());
    }
    /* the penalty, unless the scenario sets it: a spring kappa times the stiffness of the point's own nodes (the diagonal of
     * K), as a pressure per metre over the point's area. Scaled by the face's size instead, the springs of a quadratic
     * face's corners (a small share of its area) were weaker than the body there and the multipliers converged slowly */
    {
        Vector kd;
        K.GetDiag(kd);
        const double kappa = 10;
        for (CPoint &c : pts) {
            if (c.kp > 0) continue;
            int nd = (int)c.sh.size();
            double w = 0, k = 0;
            for (int i = 0; i < nd; i++) {
                double s = std::fabs(c.sh[(size_t)i]);
                w += s;
                for (int a = 0; a < 3; a++) k += s * kd(c.vd[(size_t)(a * nd + i)]) / 3;
            }
            c.kp = kappa * k / (w * c.dA);
        }
    }
    Vector &u = U;
    fc.SetSize(u.Size());
    std::vector<char> was(pts.size(), 0);
    double scale_u = 0;
    converged = false, newton_total = 0;
    /* the gap, normal and trial pressure of every point at the current u */
    auto measure = [&](CPoint &c) {
        int nd = (int)c.sh.size();
        double y[3] = {c.x[0], c.x[1], c.x[2]};
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < nd; i++) y[k] += c.sh[(size_t)i] * u(c.vd[(size_t)(k * nd + i)]);
        c.g = obstacle_gap(P.bnd[(size_t)c.b], y, c.n, hgrad);
        return c.lambda - c.kp * c.g;
    };
    /* the tangent K + the active points' penalty springs is factored again only when the active set changes (or every
     * ten iterations of one set): in between, a step reuses the factor, the normals' small turns left to the iteration */
    SpDirect *sd = nullptr;
    std::vector<char> fact_active;
    int since = 0, nfact = 0;
    for (al_iters = 1; al_iters <= P.contact_max_al; al_iters++) {
        double last_r = INFINITY;
        for (int it = 1; it <= 100; it++) {
            fc = 0.0;
            bool changed = false;
            for (size_t q = 0; q < pts.size(); q++) {
                CPoint &c = pts[q];
                double trial = measure(c);
                c.active = trial >= 0 && !(c.lambda == 0 && c.g > 0);
                if (c.active != (bool)was[q]) changed = true;
                was[q] = c.active;
                if (!c.active) continue;
                int nd = (int)c.sh.size();
                for (int i = 0; i < nd; i++)
                    for (int a = 0; a < 3; a++) fc(c.vd[(size_t)(a * nd + i)]) += trial * c.n[a] * c.sh[(size_t)i] * c.dA;
            }
            Vector R(u.Size());
            K.Mult(u, R);
            R -= f;
            R -= fc;
            for (int i = 0; i < ess.Size(); i++) R(ess[i]) = 0;
            double ref = f.Norml2() + fc.Norml2() + 1e-300;
            double rn = R.Norml2();
            /* converged: the active set kept and the residual at the solve's rounding (or no longer falling there) */
            if (it > 1 && !changed && (rn <= 1e-11 * ref || (rn <= 1e-8 * ref && rn > 0.5 * last_r))) break;
            last_r = rn;
            if (!sd || fact_active != was || since >= 10) {
                SparseMatrix A(K);
                for (const CPoint &c : pts) {
                    if (!c.active) continue;
                    int nd = (int)c.sh.size();
                    for (int i = 0; i < nd; i++)
                        for (int a = 0; a < 3; a++)
                            for (int j = 0; j < nd; j++)
                                for (int bb = 0; bb < 3; bb++)
                                    A.Add(c.vd[(size_t)(a * nd + i)], c.vd[(size_t)(bb * nd + j)], c.kp * c.n[a] * c.n[bb] * c.sh[(size_t)i] * c.sh[(size_t)j] * c.dA);
                }
                Vector dummy(u.Size());
                dummy = 0.0;
                for (int i = 0; i < ess.Size(); i++) A.EliminateRowCol(ess[i], 0.0, dummy);
                spd_free(sd);
                char e2[256] = "";
                sd = spd_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), e2, sizeof e2);
                if (!sd) return err = std::string("contact: the system cannot be factored (is the body held against every rigid motion the obstacles do not stop?): ") + e2, false;
                factor_mb = std::fmax(factor_mb, spd_factor_bytes(sd) / 1048576.0);
                fact_active = was, since = 0, nfact++;
            }
            R.Neg();
            if (!spd_solve(sd, R.GetData())) return spd_free(sd), err = "contact: the solve failed", false;
            u += R;
            newton_total++, since++;
        }
        scale_u = 0;
        for (int i = 0; i < u.Size(); i++) scale_u = std::fmax(scale_u, std::fabs(u(i)));
        double deepest = 0;
        for (CPoint &c : pts) {
            double trial = measure(c);
            if (c.active) deepest = std::fmax(deepest, -c.g);
            c.lambda = std::fmax(0.0, trial);
        }
        if (deepest <= tol * std::fmax(scale_u, 1e-300)) {
            converged = true;
            break;
        }
    }
    spd_free(sd);
    factorizations = nfact;
    al_iters = std::min(al_iters, P.contact_max_al);
    /* the final pressures and forces, with the last multipliers */
    fc = 0.0;
    for (CPoint &c : pts) {
        double trial = measure(c);
        c.active = trial > 0 && !(c.lambda == 0 && c.g > 0);
        c.p = c.active ? trial : 0;
        int nd = (int)c.sh.size();
        for (int i = 0; i < nd && c.active; i++)
            for (int a = 0; a < 3; a++) fc(c.vd[(size_t)(a * nd + i)]) += c.p * c.n[a] * c.sh[(size_t)i] * c.dA;
    }
    return true;
}

/* the integral over a boundary of a function of the point (on its faces) and the area */
double bnd_integral(Mesh &mesh, const Boundary &B, int order, const std::function<double(ElementTransformation &, const IntegrationPoint &)> &f) {
    double s = 0;
    for (int i = 0; i < mesh.GetNBE(); i++) {
        int a = mesh.GetBdrAttribute(i);
        if (a < 1 || a > B.marker.Size() || !B.marker[a - 1]) continue;
        ElementTransformation *T = mesh.GetBdrElementTransformation(i);
        const IntegrationRule &ir = IntRules.Get(mesh.GetBdrElementGeometry(i), 2 * order + 2);
        for (int q = 0; q < ir.GetNPoints(); q++) {
            const IntegrationPoint &ip = ir.IntPoint(q);
            T->SetIntPoint(&ip);
            s += ip.weight * T->Weight() * f(*T, ip);
        }
    }
    return s;
}

JsonValue *jvec(const double *v, int n) {
    JsonValue *a = json_array();
    for (int k = 0; k < n; k++) json_push(a, json_number(v[k]));
    return a;
}

/* a part read from an STL: its mesh in the record, how good and how far from the file */
void record_part_mesh(JsonValue *rec, const PartMeshInfo &part, bool quiet) {
    if (!part.stl) return;
    const KnTetMesh &m = part.m;
    JsonValue *mo = json_set_object(rec, "mesh");
    json_set_number(mo, "stl_triangles", m.stl_triangles), json_set_number(mo, "patches", m.patches);
    json_set_number(mo, "tetrahedra", m.nelems), json_set_number(mo, "nodes", m.nnodes);
    json_set_bool(mo, "sharp_edges_kept", m.features_used != 0);
    json_set_number(mo, "min_dihedral_deg", m.min_dihedral_deg), json_set_number(mo, "max_dihedral_deg", m.max_dihedral_deg);
    json_set_number(mo, "volume_m3", m.volume_m3), json_set_number(mo, "stl_volume_m3", m.stl_volume_m3);
    json_set_number(mo, "surface_distance_max_m", m.surface_distance_max_m), json_set_number(mo, "seconds", m.seconds);
    if (!quiet)
        fprintf(stderr, "  kernel: %d tetrahedra from %d STL triangles in %.1f s, dihedral angles %.1f to %.1f degrees, volume %+.3f %% of the file's\n",
                m.nelems, m.stl_triangles, m.seconds, m.min_dihedral_deg, m.max_dihedral_deg, 100 * (m.volume_m3 / m.stl_volume_m3 - 1));
}

/* each sampled line in the record: its probes' values gathered into arrays along it, with the distance s_m from its start */
void record_lines(JsonValue *rec, const Problem &P) {
    const JsonValue *pa = json_get(rec, "probes");
    if (P.lines.empty() || !pa) return;
    JsonValue *lo = json_set_object(rec, "lines");
    for (const Problem::Line &l : P.lines) {
        JsonValue *o = json_set_object(lo, l.name.c_str()), *sa = json_set_array(o, "s_m");
        json_set(o, "from_m", jvec(l.a, 3)), json_set(o, "to_m", jvec(l.b, 3));
        double len = std::sqrt((l.b[0] - l.a[0]) * (l.b[0] - l.a[0]) + (l.b[1] - l.a[1]) * (l.b[1] - l.a[1]) + (l.b[2] - l.a[2]) * (l.b[2] - l.a[2]));
        const JsonValue *p0 = json_at(pa, l.first);
        for (int j = 0; j < l.n; j++) json_push(sa, json_number(len * j / (l.n - 1)));
        for (size_t k = 0; p0 && p0->type == JSON_OBJECT && k < p0->u.object.len; k++) {
            const char *key = p0->u.object.keys[k];
            if (!strcmp(key, "name") || !strcmp(key, "point_m") || !strcmp(key, "note")) continue;
            JsonValue *arr = json_set_array(o, key);
            for (int j = 0; j < l.n; j++) {
                const JsonValue *v = json_get(json_at(pa, l.first + (size_t)j), key);
                json_push(arr, v ? json_clone(v) : json_null());
            }
        }
    }
}

/* the run's whole record beside the result (OUT.lab.kernel.json), and as much of it as fits in the run's diagnostics */
void write_record(JsonValue *rec, const char *out, LabRunInfo *info, double t0) {
    char *d = json_dump(rec, 0, NULL, NULL);
    char path[1024];
    snprintf(path, sizeof path, "%s.kernel.json", out);
    FILE *fp = d ? fopen(path, "w") : NULL;
    if (fp) fputs(d, fp), fputc('\n', fp), fclose(fp);
    if (d && strlen(d) < sizeof info->diagnostics) snprintf(info->diagnostics, sizeof info->diagnostics, "%s", d);
    else snprintf(info->diagnostics, sizeof info->diagnostics, "{\"note\":\"the whole record is %s\",\"wall_s\":%.2f}", path, now() - t0);
    free(d);
    json_free(rec);
}

} // namespace

/* ---------------------------------------------------------------- the run */

extern "C" bool kn_run(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *errbuf, size_t errlen) {
    double t0 = now();
    std::string err;
    Problem P;
    Mesh mesh;
    auto fail = [&](const std::string &e) {
        snprintf(errbuf, errlen, "kernel: %s", e.c_str());
        return false;
    };
    RefCentres ref;
    PartMeshInfo part;
    struct Free {
        PartMeshInfo &p;
        ~Free() { kn_tet_free(&p.m); }
    } free_part{part};
    if (!read_problem(root, P, err) || !make_mesh(root, P.order, mesh, ref, part, err) || !assign(P, mesh, ref, err)) return fail(err);
    if (!P.fields.empty()) { /* the scenario's own equations */
        if (!quiet)
            fprintf(stderr, "  kernel: %d elements, %zu field%s%s%s\n", mesh.GetNE(), P.fields.size(), P.fields.size() > 1 ? "s" : "",
                    P.transient ? ", in time" : ", steady", P.elastic ? ", and the solid's elasticity at each output" : "");
        JsonValue *rec = json_object(), *res = json_set_object(rec, "results");
        json_set_string(rec, "title", P.title.c_str());
        record_part_mesh(rec, part, quiet);
        json_set_number(res, "elements", mesh.GetNE());
        JsonValue *meta = json_object();
        json_set_string(meta, "domain", "kernel");
        json_set_string(meta, "title", P.title.c_str());
        json_set_string(meta, "model", P.elastic ? "the physics kernel: the scenario's own equations on MFEM finite elements, and linear elasticity strained by them"
                                                 : "the physics kernel: the scenario's own equations on MFEM finite elements");
        JsonValue *fm = json_set_object(meta, "fields");
        for (const FieldEq &q : P.fields) json_set_string(fm, q.name.c_str(), q.unit.c_str());
        if (P.elastic) json_set_string(fm, "von_mises", "Pa"), json_set_string(fm, "displacement", "m"), json_set_string(fm, "max_principal", "Pa");
        json_set(meta, "scenario", json_clone(root));
        char *hdr = json_dump(meta, 0, NULL, NULL);
        json_free(meta);
        LabWriter *w = lab_create(out, hdr, errbuf, errlen);
        free(hdr);
        if (!w) return json_free(rec), false;
        int frames = 0;
        double fmb = 0, ta = 0, ts = 0;
        bool ok = run_fields(P, mesh, w, rec, err, frames, fmb, ta, ts);
        if (!ok) snprintf(errbuf, errlen, "kernel: %s", err.c_str());
        else if (!err.empty() && !quiet) fprintf(stderr, "  kernel: %s\n", err.c_str());
        if (!lab_close(w) && ok) ok = false, snprintf(errbuf, errlen, "write error on %s", out);
        json_set_number(res, "factor_mb", fmb), json_set_number(res, "assemble_s", ta), json_set_number(res, "solve_s", ts);
        json_set_number(res, "wall_s", now() - t0);
        record_lines(rec, P);
        write_record(rec, out, info, t0);
        info->frames = frames, info->steps = P.transient ? P.steps : 1;
        if (!quiet) fprintf(stderr, "  kernel: solved in %.2f s (assembly %.2f s, factor and solve %.2f s, factor %.0f MB)\n", now() - t0, ta, ts, fmb);
        return ok;
    }
    if (!quiet) fprintf(stderr, "  kernel: %d elements, order %d, %s%s%s\n", mesh.GetNE(), P.order, P.heat ? "heat" : "", P.heat && P.elastic ? " + " : "", P.elastic ? "elasticity" : "");
    H1_FECollection fec(P.order, 3);
    double factor_mb = 0, assemble_s = 0, solve_s = 0;
    JsonValue *rec = json_object(), *res = json_set_object(rec, "results"), *bres = json_set_object(rec, "boundaries"), *chk = json_set_object(rec, "check");
    json_set_string(rec, "title", P.title.c_str());
    record_part_mesh(rec, part, quiet);
    json_set_number(res, "elements", mesh.GetNE());
    json_set_number(res, "order", P.order);
    /* ---- heat */
    std::unique_ptr<FiniteElementSpace> Vt;
    std::unique_ptr<GridFunction> Tgf;
    if (P.heat) {
        double ta = now();
        Vt.reset(new FiniteElementSpace(&mesh, &fec));
        Tgf.reset(new GridFunction(Vt.get()));
        *Tgf = P.T_ref;
        MatCoef kc;
        kc.mats = &P.mats, kc.f = [](const Material &m, const double *x) { return m.k(x); };
        BilinearForm a(Vt.get());
        a.AddDomainIntegrator(new DiffusionIntegrator(kc));
        LinearForm b(Vt.get());
        std::vector<std::unique_ptr<Coefficient>> keep;
        if (P.heat_source.set) {
            keep.emplace_back(new NumCoef(P.heat_source));
            b.AddDomainIntegrator(new DomainLFIntegrator(*keep.back()));
        }
        Array<int> ess, held(mesh.bdr_attributes.Size() ? mesh.bdr_attributes.Max() : 0);
        held = 0;
        for (Boundary &B : P.bnd) {
            if (B.where.kind == Sel::POINT) continue;
            if (B.temperature.set)
                for (int k = 0; k < held.Size(); k++) held[k] |= B.marker[k];
            if (B.flux.set) {
                keep.emplace_back(new NumCoef(B.flux));
                b.AddBoundaryIntegrator(new BoundaryLFIntegrator(*keep.back()), B.marker);
            }
            if (B.h.set) {
                keep.emplace_back(new NumCoef(B.h));
                Coefficient *hc = keep.back().get();
                a.AddBoundaryIntegrator(new BoundaryMassIntegrator(*hc), B.marker);
                auto H = B.h, A = B.ambient;
                keep.emplace_back(new FunctionCoefficient([H, A](const Vector &x) { return H(x.GetData()) * A(x.GetData()); }));
                b.AddBoundaryIntegrator(new BoundaryLFIntegrator(*keep.back()), B.marker);
            }
        }
        if (held.Size()) Vt->GetEssentialTrueDofs(held, ess);
        for (Boundary &B : P.bnd) { /* held temperatures, faces then points */
            if (!B.temperature.set) continue;
            NumCoef tc(B.temperature);
            if (B.where.kind == Sel::POINT) {
                Array<int> vd;
                Vt->GetVertexVDofs(B.vertex, vd);
                const double *xx = B.vpos;
                for (int k = 0; k < vd.Size(); k++) (*Tgf)(vd[k]) = B.temperature(xx), ess.Append(vd[k]);
            } else {
                Tgf->ProjectBdrCoefficient(tc, B.marker);
            }
        }
        ess.Sort(), ess.Unique();
        a.Assemble(0); /* keep exact zeros: the pattern stays symmetric, as eliminating held rows and columns needs */
        a.Finalize(0);
        b.Assemble();
        SparseMatrix K(a.SpMat());
        Vector f(b);
        assemble_s += now() - ta;
        double ts = now();
        if (!solve_spd(a, b, ess, *Tgf, factor_mb, err)) return json_free(rec), fail("heat: " + err);
        solve_s += now() - ts;
        /* the heat through each boundary: held ones from the residual, others from their condition */
        Vector r(K.Height());
        K.Mult(*Tgf, r);
        r -= f;
        double Tmin = INFINITY, Tmax = -INFINITY;
        for (int i = 0; i < Tgf->Size(); i++) Tmin = std::fmin(Tmin, (*Tgf)(i)), Tmax = std::fmax(Tmax, (*Tgf)(i));
        json_set_number(res, "min_temperature_k", Tmin), json_set_number(res, "max_temperature_k", Tmax);
        json_set_number(res, "heat_dofs", Vt->GetTrueVSize());
        for (Boundary &B : P.bnd) {
            JsonValue *o = json_get(bres, B.name.c_str()) ? (JsonValue *)json_get(bres, B.name.c_str()) : json_set_object(bres, B.name.c_str());
            if (B.where.kind == Sel::POINT) continue;
            double q = 0;
            if (B.temperature.set) {
                Array<int> bd;
                Vt->GetEssentialTrueDofs(B.marker, bd);
                for (int k = 0; k < bd.Size(); k++) q += r(bd[k]);
            } else {
                const GridFunction &T = *Tgf;
                q = bnd_integral(mesh, B, P.order, [&](ElementTransformation &Tr, const IntegrationPoint &ip) {
                    double x[3];
                    Vector X(x, 3);
                    Tr.Transform(ip, X);
                    double v = B.flux.set ? B.flux(x) : 0;
                    if (B.h.set) v += B.h(x) * (B.ambient(x) - T.GetValue(Tr, ip));
                    return v;
                });
            }
            json_set_number(o, "heat_in_w", q);
            const GridFunction &T = *Tgf;
            json_set_number(o, "mean_temperature_k", bnd_integral(mesh, B, P.order, [&](ElementTransformation &Tr, const IntegrationPoint &ip) { return T.GetValue(Tr, ip); }) / B.area);
        }
        if (P.check_T.set) {
            NumCoef ex(P.check_T);
            GridFunction zero(Vt.get());
            zero = 0.0;
            json_set_number(chk, "temperature_l2_error_k", Tgf->ComputeL2Error(ex));
            json_set_number(chk, "temperature_l2_norm_k", zero.ComputeL2Error(ex));
        }
    }
    TempField temp;
    if (Tgf) temp.gf = Tgf.get();
    else temp.formula = P.temperature_field;
    /* ---- elasticity */
    std::unique_ptr<FiniteElementSpace> Vu;
    std::unique_ptr<GridFunction> U;
    std::vector<double> cpv; /* the contact pressure at the vertices, for the pictures */
    double vm_max = 0, u_max = 0;
    if (P.elastic) {
        double ta = now();
        Vu.reset(new FiniteElementSpace(&mesh, &fec, 3));
        U.reset(new GridFunction(Vu.get()));
        *U = 0.0;
        MatCoef lc, mc;
        lc.mats = mc.mats = &P.mats;
        lc.f = lame_lambda, mc.f = lame_mu;
        BilinearForm a(Vu.get());
        a.AddDomainIntegrator(new ElasticityIntegrator(lc, mc));
        LinearForm b(Vu.get());
        std::vector<std::unique_ptr<Coefficient>> keep;
        std::vector<std::unique_ptr<VectorCoefficient>> keepv;
        if (!mech_loads(P, b, keep, keepv, err)) return json_free(rec), fail(err);
        if (temp.any()) {
            ThermalLoad *tl = new ThermalLoad();
            tl->mats = &P.mats, tl->temp = &temp, tl->T_ref = P.T_ref;
            b.AddDomainIntegrator(tl);
        }
        Array<int> ess;
        held_dofs(P, *Vu, *U, ess);
        a.Assemble(0); /* keep exact zeros: the pattern stays symmetric, as eliminating held rows and columns needs */
        a.Finalize(0);
        b.Assemble();
        SparseMatrix K(a.SpMat());
        Vector f(b);
        assemble_s += now() - ta;
        double ts = now();
        bool contact = false;
        for (const Boundary &B : P.bnd) contact = contact || B.obstacle;
        std::vector<CPoint> cpts;
        Vector fc;
        if (!contact) {
            if (!solve_spd(a, b, ess, *U, factor_mb, err)) return json_free(rec), fail("elasticity: " + err);
        } else {
            int nt = 0, al = 0, nf = 0;
            bool conv = false;
            if (!solve_contact(P, mesh, *Vu, K, f, ess, *U, cpts, fc, nt, al, conv, nf, factor_mb, err)) return json_free(rec), fail("elasticity: " + err);
            JsonValue *co = json_set_object(res, "contact");
            json_set_number(co, "newton_iterations", nt), json_set_number(co, "multiplier_updates", al), json_set_bool(co, "converged", conv);
            json_set_number(co, "factorizations", nf);
            json_set_number(co, "points", (double)cpts.size());
            cpv.assign((size_t)mesh.GetNV(), 0.0);
            std::vector<double> cnt((size_t)mesh.GetNV(), 0.0);
            for (const CPoint &c : cpts) {
                Array<int> vv;
                mesh.GetBdrElementVertices(c.be, vv);
                for (int j = 0; j < vv.Size(); j++) cpv[(size_t)vv[j]] += c.p, cnt[(size_t)vv[j]] += 1;
            }
            for (size_t v = 0; v < cpv.size(); v++)
                if (cnt[v] > 0) cpv[v] /= cnt[v];
            if (!conv && !quiet) fprintf(stderr, "  kernel: contact did not settle within %d multiplier updates\n", al);
            /* each contact boundary: the obstacle's resultant on the body, the area touching, the peak pressure, the
             * deepest penetration, and the points if asked */
            for (size_t bi = 0; bi < P.bnd.size(); bi++) {
                const Boundary &B = P.bnd[bi];
                if (!B.obstacle) continue;
                double F[3] = {0, 0, 0}, area = 0, pmax = 0, pen = 0;
                JsonValue *o = json_set_object(json_get(bres, B.name.c_str()) ? (JsonValue *)json_get(bres, B.name.c_str()) : json_set_object(bres, B.name.c_str()), "contact");
                JsonValue *pa = B.report_points ? json_set_array(o, "points") : nullptr;
                for (const CPoint &c : cpts) {
                    if (c.b != (int)bi) continue;
                    if (c.p > 0) {
                        for (int k = 0; k < 3; k++) F[k] += c.p * c.n[k] * c.dA;
                        area += c.dA, pmax = std::fmax(pmax, c.p), pen = std::fmax(pen, -c.g);
                    }
                    if (pa) {
                        double row[4] = {c.x[0], c.x[1], c.x[2], c.p};
                        json_push(pa, jvec(row, 4));
                    }
                }
                json_set(o, "force_n", jvec(F, 3)), json_set_number(o, "area_m2", area), json_set_number(o, "max_pressure_pa", pmax);
                json_set_number(o, "max_penetration_m", pen);
            }
        }
        solve_s += now() - ts;
        json_set_number(res, "elasticity_dofs", Vu->GetTrueVSize());
        /* reactions: the unconstrained residual at each boundary's held degrees of freedom */
        Vector r(K.Height());
        K.Mult(*U, r);
        r -= f;
        if (contact) r -= fc;
        record_reactions(mesh, P, *Vu, *U, r, bres);
        StressPeaks pk = stress_peaks(mesh, *Vu, *U, temp, P);
        double *vm_at = pk.vm_at;
        vm_max = pk.vm;
        json_set_number(res, "max_principal_pa", pk.p1), json_set(res, "max_principal_at_m", jvec(pk.p1_at, 3));
        json_set_number(res, "min_principal_pa", pk.p3), json_set(res, "min_principal_at_m", jvec(pk.p3_at, 3));
        for (int i = 0; i < U->Size(); i++) u_max = std::fmax(u_max, std::fabs((*U)(i)));
        json_set_number(res, "max_von_mises_pa", vm_max);
        json_set(res, "max_von_mises_at_m", jvec(vm_at, 3));
        json_set_number(res, "max_displacement_component_m", u_max);
        if (P.check_u[0].set) {
            Num3Coef ex(P.check_u);
            GridFunction zero(Vu.get());
            zero = 0.0;
            json_set_number(chk, "displacement_l2_error_m", U->ComputeL2Error(ex));
            json_set_number(chk, "displacement_l2_norm_m", zero.ComputeL2Error(ex));
        }
    }
    /* ---- boundaries' sizes, probes */
    for (Boundary &B : P.bnd) {
        JsonValue *o = json_get(bres, B.name.c_str()) ? (JsonValue *)json_get(bres, B.name.c_str()) : json_set_object(bres, B.name.c_str());
        if (B.where.kind == Sel::POINT) {
            json_set(o, "vertex_m", jvec(B.vpos, 3));
        } else {
            json_set_number(o, "faces", B.faces), json_set_number(o, "area_m2", B.area);
        }
    }
    if (!P.probes.empty()) {
        DenseMatrix pts(3, (int)P.probes.size());
        for (size_t i = 0; i < P.probes.size(); i++)
            for (int k = 0; k < 3; k++) pts(k, (int)i) = P.probes[i].p[k];
        Array<int> elem;
        Array<IntegrationPoint> ips;
        mesh.FindPoints(pts, elem, ips, false);
        JsonValue *pa = json_set_array(rec, "probes");
        for (size_t i = 0; i < P.probes.size(); i++) {
            JsonValue *o = json_object();
            json_set_string(o, "name", P.probes[i].name.c_str());
            json_set(o, "point_m", jvec(P.probes[i].p, 3));
            if (elem[(int)i] < 0) {
                json_set_string(o, "note", "outside the mesh");
            } else {
                ElementTransformation *T = mesh.GetElementTransformation(elem[(int)i]);
                T->SetIntPoint(&ips[(int)i]);
                if (Tgf) json_set_number(o, "temperature_k", Tgf->GetValue(*T, ips[(int)i]));
                if (U) {
                    Vector v(3);
                    U->GetVectorValue(*T, ips[(int)i], v);
                    json_set(o, "displacement_m", jvec(v.GetData(), 3));
                    Stress st = stress_at(*U, temp, P.T_ref, P.mats, *T, ips[(int)i]);
                    json_set_number(o, "von_mises_pa", st.vm);
                    json_set(o, "stress_pa", jvec(st.s, 6)); /* xx yy zz xy yz xz */
                    json_set(o, "principal_pa", jvec(st.p, 3));
                }
            }
            json_push(pa, o);
        }
    }
    json_set_number(res, "factor_mb", factor_mb);
    json_set_number(res, "assemble_s", assemble_s);
    json_set_number(res, "solve_s", solve_s);
    json_set_number(res, "wall_s", now() - t0);
    /* ---- the picture: the body's surface with its fields at the vertices */
    JsonValue *meta = json_object();
    json_set_string(meta, "domain", "kernel");
    json_set_string(meta, "title", P.title.c_str());
    json_set_string(meta, "model", "the physics kernel: MFEM finite elements composed from the scenario (heat conduction, linear elasticity, thermal strain)");
    JsonValue *fields = json_set_object(meta, "fields");
    if (U) json_set_string(fields, "von_mises", "Pa"), json_set_string(fields, "displacement", "m"), json_set_string(fields, "max_principal", "Pa");
    if (!cpv.empty()) json_set_string(fields, "contact_pressure", "Pa");
    if (Tgf) json_set_string(fields, "temperature", "K");
    json_set(meta, "scenario", json_clone(root));
    char *hdr = json_dump(meta, 0, NULL, NULL);
    json_free(meta);
    LabWriter *w = lab_create(out, hdr, errbuf, errlen);
    free(hdr);
    if (!w) return json_free(rec), false;
    bool ok = true;
    if (P.frames) {
        int nv = mesh.GetNV();
        std::vector<double> xyz((size_t)nv * 3), vmv((size_t)nv, 0.0), dmv((size_t)nv, 0.0), tv((size_t)nv, 0.0), cnt((size_t)nv, 0.0), p1v((size_t)nv, 0.0);
        std::vector<double> disp((size_t)nv * 3, 0.0), pos((size_t)nv * 3, 0.0);
        for (int e = 0; e < mesh.GetNE(); e++) { /* vertex values: each element's own, averaged */
            Array<int> vv;
            mesh.GetElementVertices(e, vv);
            ElementTransformation *T = mesh.GetElementTransformation(e);
            const IntegrationRule *ir = Geometries.GetVertices(mesh.GetElementBaseGeometry(e));
            for (int j = 0; j < vv.Size(); j++) {
                const IntegrationPoint &ip = ir->IntPoint(j);
                T->SetIntPoint(&ip);
                size_t v = (size_t)vv[j];
                cnt[v] += 1;
                Vector X(3); /* the true position: a curved mesh's vertices lie on its nodes' geometry */
                T->Transform(ip, X);
                for (int k = 0; k < 3; k++) pos[3 * v + k] = X(k);
                if (U) {
                    Vector d(3);
                    U->GetVectorValue(*T, ip, d);
                    for (int k = 0; k < 3; k++) disp[3 * v + k] += d(k);
                    Stress st = stress_at(*U, temp, P.T_ref, P.mats, *T, ip);
                    vmv[v] += st.vm, p1v[v] += st.p[0];
                }
                if (Tgf) tv[v] += Tgf->GetValue(*T, ip);
            }
        }
        for (int v = 0; v < nv; v++) {
            double c = cnt[(size_t)v] > 0 ? cnt[(size_t)v] : 1;
            for (int k = 0; k < 3; k++) disp[3 * v + k] /= c, xyz[3 * v + k] = pos[3 * v + k] + P.deform_scale * disp[3 * v + k];
            vmv[(size_t)v] /= c, tv[(size_t)v] /= c, p1v[(size_t)v] /= c;
            dmv[(size_t)v] = std::sqrt(disp[3 * v] * disp[3 * v] + disp[3 * v + 1] * disp[3 * v + 1] + disp[3 * v + 2] * disp[3 * v + 2]);
        }
        /* the surface: quads and triangles of the boundary */
        std::vector<int> quads, tris;
        for (int i = 0; i < mesh.GetNBE(); i++) {
            Array<int> vv;
            mesh.GetBdrElementVertices(i, vv);
            std::vector<int> &dst = vv.Size() == 4 ? quads : tris;
            if (vv.Size() != 4 && vv.Size() != 3) continue;
            for (int j = 0; j < vv.Size(); j++) dst.push_back(vv[j]);
        }
        lab_frame_begin(w, 0);
        for (int pass = 0; pass < 2; pass++) {
            std::vector<int> &c = pass ? tris : quads;
            if (c.empty()) continue;
            lab_part_cells(w, pass ? "surface (triangles)" : "surface", nv, xyz.data(), (int)(c.size() / (pass ? 3 : 4)), pass ? LAB_TRI : LAB_QUAD, c.data());
            if (U) {
                lab_field_d(w, "von_mises", LAB_AT_NODE, (size_t)nv, vmv.data()), lab_field_d(w, "displacement", LAB_AT_NODE, (size_t)nv, dmv.data());
                lab_field_d(w, "max_principal", LAB_AT_NODE, (size_t)nv, p1v.data());
            }
            if (!cpv.empty()) lab_field_d(w, "contact_pressure", LAB_AT_NODE, (size_t)nv, cpv.data());
            if (Tgf) lab_field_d(w, "temperature", LAB_AT_NODE, (size_t)nv, tv.data());
        }
        if (!lab_frame_end(w)) ok = false, snprintf(errbuf, errlen, "cannot write the frame (disk full?)");
    }
    if (!lab_close(w) && ok) ok = false, snprintf(errbuf, errlen, "write error on %s", out);
    record_lines(rec, P);
    write_record(rec, out, info, t0);
    info->frames = P.frames ? 1 : 0;
    info->steps = 1;
    if (!quiet) fprintf(stderr, "  kernel: solved in %.2f s (assembly %.2f s, factor and solve %.2f s, factor %.0f MB)\n", now() - t0, assemble_s, solve_s, factor_mb);
    return ok;
}
