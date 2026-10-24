/* mag3d.cpp - 3D magnetostatics by MFEM's Nedelec elements (mag3d.h). */
#include "mag3d.h"
#include "spdirect.h"
#include "mag3d_mesh.hpp"

#include "mfem.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

using namespace mfem;
static_assert(sizeof(real_t) == sizeof(double), "MFEM must use double precision");

namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/* a coefficient that is constant on each element, read by the element's number */
class ElementScalar : public Coefficient {
    const double *v;
    double scale;
  public:
    ElementScalar(const double *values, double s = 1) : v(values), scale(s) {}
    real_t Eval(ElementTransformation &T, const IntegrationPoint &) override { return scale * v[T.ElementNo]; }
};

class ElementVector : public VectorCoefficient {
    const double *v, *w; /* w: an optional per-element factor */
  public:
    ElementVector(const double *values, const double *factor = nullptr) : VectorCoefficient(3), v(values), w(factor) {}
    void Eval(Vector &V, ElementTransformation &T, const IntegrationPoint &) override {
        V.SetSize(3);
        double f = w ? w[T.ElementNo] : 1.0;
        for (int d = 0; d < 3; d++) V[d] = f * v[3 * T.ElementNo + d];
    }
};

/* Cholesky of an assembled MFEM matrix, solving in place */
struct Direct {
    SpDirect *s = nullptr;
    ~Direct() { spd_free(s); }
    bool factor(SparseMatrix &A, char *err, size_t len) {
        A.Finalize();
        s = spd_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), err, len);
        return s != nullptr;
    }
    bool solve(const Vector &b, Vector &x) {
        x = b;
        return spd_solve(s, x.GetData());
    }
};

} // namespace

/* the cylindrical grid as an MFEM mesh (mag3d_mesh.hpp): element order e = ir + nr (it + nt iz); boundary attribute 1 the
 * outer cylinder and the ends, 2 the inner cylinder; periodic in z through a discontinuous nodal field */
Mesh *mag3d_build_mesh(const Mag3DGrid *g, Mesh &mesh, Mesh &periodic) {
        const int nr = g->nr, nt = g->nt, nz = g->nz, nvr = nr + 1;
        const bool sec = g->sector > 1;
        const int ncol = sec ? nt + 1 : nt; /* a sector has its own last column of vertices, mapped onto the first */
        const int nv = nvr * ncol * (nz + 1), ne = nr * nt * nz;
        const int nbe = 2 * nt * nz + (g->periodic_z ? 0 : 2 * nr * nt);
        const double span = mag3d_span(g);
        auto vid = [&](int ir, int it, int iz) { return ir + nvr * ((sec ? it : it % nt) + ncol * iz); };
        mesh = Mesh(3, nv, ne, nbe);
        for (int iz = 0; iz <= nz; iz++)
            for (int it = 0; it < ncol; it++)
                for (int ir = 0; ir <= nr; ir++) {
                    double th = g->theta0 + span * it / nt;
                    mesh.AddVertex(g->r[ir] * std::cos(th), g->r[ir] * std::sin(th), g->z[iz]);
                }
        for (int iz = 0; iz < nz; iz++)
            for (int it = 0; it < nt; it++)
                for (int ir = 0; ir < nr; ir++) {
                    int v[8] = {vid(ir, it, iz), vid(ir + 1, it, iz), vid(ir + 1, it + 1, iz), vid(ir, it + 1, iz),
                                vid(ir, it, iz + 1), vid(ir + 1, it, iz + 1), vid(ir + 1, it + 1, iz + 1), vid(ir, it + 1, iz + 1)};
                    mesh.AddHex(v, 1);
                }
        for (int iz = 0; iz < nz; iz++)
            for (int it = 0; it < nt; it++) {
                int in[4] = {vid(0, it, iz), vid(0, it, iz + 1), vid(0, it + 1, iz + 1), vid(0, it + 1, iz)};
                int out[4] = {vid(nr, it, iz), vid(nr, it + 1, iz), vid(nr, it + 1, iz + 1), vid(nr, it, iz + 1)};
                mesh.AddBdrQuad(in, 2), mesh.AddBdrQuad(out, 1);
            }
        if (!g->periodic_z)
            for (int it = 0; it < nt; it++)
                for (int ir = 0; ir < nr; ir++) {
                    int lo[4] = {vid(ir, it, 0), vid(ir, it + 1, 0), vid(ir + 1, it + 1, 0), vid(ir + 1, it, 0)};
                    int hi[4] = {vid(ir, it, nz), vid(ir + 1, it, nz), vid(ir + 1, it + 1, nz), vid(ir, it + 1, nz)};
                    mesh.AddBdrQuad(lo, 1), mesh.AddBdrQuad(hi, 1);
                }
        mesh.FinalizeHexMesh(1, 0, true);
        Mesh *m = &mesh;
        if (g->periodic_z || sec) {
            std::vector<int> v2v(nv);
            for (int i = 0; i < nv; i++) v2v[i] = i;
            for (int iz = 0; iz <= nz; iz++)
                for (int it = 0; it < ncol; it++)
                    for (int ir = 0; ir <= nr; ir++) {
                        int jt = sec && it == nt ? 0 : it, jz = g->periodic_z && iz == nz ? 0 : iz;
                        v2v[vid(ir, it, iz)] = vid(ir, jt, jz);
                    }
            periodic = Mesh::MakePeriodic(mesh, v2v);
            m = &periodic;
        }

        return m;
}


extern "C" void mag3d_centre(const Mag3DGrid *g, int ir, int it, int iz, double c[3]) {
    double cx = 0, cy = 0;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) {
            double th = g->theta0 + mag3d_span(g) * (it + b) / g->nt;
            cx += g->r[ir + a] * std::cos(th) / 4, cy += g->r[ir + a] * std::sin(th) / 4;
        }
    c[0] = cx, c[1] = cy, c[2] = 0.5 * (g->z[iz] + g->z[iz + 1]);
}

extern "C" double mag3d_volume(const Mag3DGrid *g, int ir, int it, int iz) {
    (void)it;
    double r0 = g->r[ir], r1 = g->r[ir + 1];
    return 0.5 * std::sin(mag3d_span(g) / g->nt) * (r1 * r1 - r0 * r0) * (g->z[iz + 1] - g->z[iz]);
}

extern "C" bool mag3d_solve(const Mag3DGrid *g, const double *nu, const double *J, const double *T, const double *Br, double *B, Mag3DStats *st, char *err, size_t errlen) {
    Mag3DStats stats = {};
    if (!g || g->nr < 1 || g->nt < 3 || g->nz < 1 || (g->periodic_z && g->nz < 3) || !(g->r[0] > 0)) {
        std::snprintf(err, errlen, "mag3d: need nr >= 1, nt >= 3, nz >= 1 (3 when periodic) and r[0] > 0");
        return false;
    }
    try {
        double t0 = now();
        const int nr = g->nr, nt = g->nt, nz = g->nz, ne = nr * nt * nz;
        Mesh mesh, periodic;
        Mesh *m = mag3d_build_mesh(g, mesh, periodic);

        ND_FECollection nd(1, 3);
        H1_FECollection h1(1, 3);
        FiniteElementSpace V(m, &nd), S(m, &h1);
        Array<int> ess_bdr(m->bdr_attributes.Max());
        ess_bdr = 1;
        ess_bdr[1] = g->inner_flux_wall ? 1 : 0; /* the inner cylinder: a magnetic wall (n x H = 0) unless asked, mag3d.h */
        Array<int> ess_nd, ess_h1;
        V.GetEssentialTrueDofs(ess_bdr, ess_nd);
        S.GetEssentialTrueDofs(ess_bdr, ess_h1);
        stats.dofs = V.GetTrueVSize(), stats.elements = ne;

        /* loads: the current, and the magnets' remanence through curl W */
        std::vector<double> zero(J && T && Br ? 0 : 3 * (size_t)ne, 0.0);
        ElementVector Jc(J ? J : zero.data()), nuBr(Br ? Br : zero.data(), nu), Tc(T ? T : zero.data());
        LinearForm bJ(&V), bM(&V);
        bJ.AddDomainIntegrator(new VectorFEDomainLFIntegrator(Jc));
        bM.AddDomainIntegrator(new VectorFEDomainLFCurlIntegrator(nuBr));
        if (T) bM.AddDomainIntegrator(new VectorFEDomainLFCurlIntegrator(Tc)); /* (curl T, W) = (T, curl W): no divergence */
        bJ.Assemble(), bM.Assemble();
        for (int d : ess_nd) bJ[d] = 0, bM[d] = 0;

        /* the current's gradient part: G^T bJ = K phi, bJ -= M G phi (the discrete Helmholtz projection) */
        DiscreteLinearOperator G(&S, &V);
        G.AddDomainInterpolator(new GradientInterpolator);
        G.Assemble(), G.Finalize();
        SparseMatrix &Gm = G.SpMat();
        for (int d : ess_nd) Gm.EliminateRow(d); /* the flux walls' tangential edges carry no gradient */
        Vector gt(S.GetVSize());
        Gm.MultTranspose(bJ, gt);
        double jnorm = bJ.Norml2();
        if (gt.Norml2() > 1e-14 * std::fmax(jnorm, 1e-300)) {
            /* K = G^T M G exactly (not a separately integrated Laplacian), so that the projected load is compatible to
             * rounding: on these non-affine hexahedra the two differ by quadrature */
            ConstantCoefficient one(1.0);
            BilinearForm M(&V);
            M.AddDomainIntegrator(new VectorFEMassIntegrator(one));
            M.Assemble(), M.Finalize();
            SparseMatrix *K = RAP(Gm, M.SpMat(), Gm);
            for (int d : ess_h1) K->EliminateRowCol(d, Operator::DIAG_ONE), gt[d] = 0;
            Direct dk;
            if (!dk.factor(*K, err, errlen)) { delete K; return false; }
            delete K;
            Vector phi(S.GetVSize());
            if (!dk.solve(gt, phi)) { std::snprintf(err, errlen, "mag3d: projection solve failed"); return false; }
            Vector gphi(V.GetVSize()), mg(V.GetVSize());
            Gm.Mult(phi, gphi);
            M.SpMat().Mult(gphi, mg);
            for (int d : ess_nd) mg[d] = 0;
            stats.current_removed = mg.Norml2() / std::fmax(jnorm, 1e-300);
            bJ -= mg;
        }

        /* curl-curl, with a mass term far below the stiffness of air on the domain's scale */
        double rmax = g->r[nr], zspan = g->z[nz] - g->z[0], L = std::fmax(rmax, zspan);
        double nu_air = 1.0 / (4e-7 * M_PI);
        ElementScalar nuc(nu);
        ConstantCoefficient eps(1e-9 * nu_air / (L * L));
        BilinearForm a(&V);
        a.AddDomainIntegrator(new CurlCurlIntegrator(nuc));
        a.AddDomainIntegrator(new VectorFEMassIntegrator(eps));
        a.Assemble();
        LinearForm rhs(&V);
        rhs = bJ;
        rhs += bM;
        GridFunction A(&V);
        A = 0.0;
        OperatorPtr Aop;
        Vector X, Bv;
        a.FormLinearSystem(ess_nd, A, rhs, Aop, X, Bv);
        stats.assemble_s = now() - t0;
        double t1 = now();
        Direct da;
        if (!da.factor(*Aop.As<SparseMatrix>(), err, errlen)) return false;
        stats.factor_mb = spd_factor_bytes(da.s) / 1048576.0;
        if (!da.solve(Bv, X)) { std::snprintf(err, errlen, "mag3d: solve failed"); return false; }
        a.RecoverFEMSolution(X, rhs, A);
        stats.solve_s = now() - t1;

        /* B = curl A at each element's centre */
        IntegrationPoint ip;
        ip.Set3(0.5, 0.5, 0.5);
        Vector curl(3);
        for (int e = 0; e < ne; e++) {
            ElementTransformation *T = m->GetElementTransformation(e);
            T->SetIntPoint(&ip);
            A.GetCurl(*T, curl);
            for (int d = 0; d < 3; d++) B[3 * e + d] = curl[d];
        }
        if (st) *st = stats;
        return true;
    } catch (const std::exception &x) {
        std::snprintf(err, errlen, "mag3d (MFEM): %s", x.what());
        return false;
    }
}
