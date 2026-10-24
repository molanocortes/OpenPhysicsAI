/* eddy3d.cpp - eddy currents and the heat they make (eddy3d.h). */
#include "eddy3d.h"
#include "mag3d_mesh.hpp"
#include "spdirect.h"

#include "mfem.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

using namespace mfem;

namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

class ElemScalar : public Coefficient {
    const double *v;
  public:
    explicit ElemScalar(const double *values) : v(values) {}
    real_t Eval(ElementTransformation &T, const IntegrationPoint &) override { return v[T.ElementNo]; }
};

class ElemVector : public VectorCoefficient {
    const double *v;
  public:
    explicit ElemVector(const double *values) : VectorCoefficient(3), v(values) {}
    void Eval(Vector &V, ElementTransformation &T, const IntegrationPoint &) override {
        V.SetSize(3);
        for (int d = 0; d < 3; d++) V[d] = v[3 * T.ElementNo + d];
    }
};

} // namespace

extern "C" bool eddy3d_solve(const Mag3DGrid *g, const double *nu, const double *sigma, const double *T, double omega, double *q, double *Bamp,
                             double *Jamp, Mag3DStats *st, char *err, size_t errlen) {
    Mag3DStats stats = {};
    try {
        double t0 = now();
        Mesh mesh, periodic;
        Mesh *m = mag3d_build_mesh(g, mesh, periodic);
        const int ne = g->nr * g->nt * g->nz;
        ND_FECollection nd(1, 3);
        FiniteElementSpace V(m, &nd);
        Array<int> ess_bdr(m->bdr_attributes.Max());
        ess_bdr = 1;
        ess_bdr[1] = g->inner_flux_wall ? 1 : 0; /* the inner cylinder: a magnetic wall unless asked (mag3d.h) */
        Array<int> ess;
        V.GetEssentialTrueDofs(ess_bdr, ess);
        const int n = V.GetTrueVSize();
        stats.dofs = 2 * n, stats.elements = ne;
        double rmax = g->r[g->nr], zspan = g->z[g->nz] - g->z[0], L = std::fmax(rmax, zspan);
        ElemScalar nuc(nu), sig(sigma);
        ConstantCoefficient eps(1e-9 / (4e-7 * M_PI) / (L * L));
        BilinearForm kf(&V), mf(&V);
        kf.AddDomainIntegrator(new CurlCurlIntegrator(nuc));
        kf.AddDomainIntegrator(new VectorFEMassIntegrator(eps));
        mf.AddDomainIntegrator(new VectorFEMassIntegrator(sig));
        kf.Assemble(), mf.Assemble(), kf.Finalize(), mf.Finalize();
        SparseMatrix &K = kf.SpMat(), &M = mf.SpMat();
        ElemVector Tc(T);
        LinearForm b(&V);
        b.AddDomainIntegrator(new VectorFEDomainLFCurlIntegrator(Tc));
        b.Assemble();
        for (int d : ess) K.EliminateRowCol(d, Operator::DIAG_ONE), M.EliminateRowCol(d, Operator::DIAG_ZERO), b[d] = 0;
        /* [[K, -w M], [-w M, -K]] [Ar; Ai] = [b; 0]: (K + j w M)(Ar + j Ai) = b, the second row negated to be symmetric */
        const int *KI = K.GetI(), *KJ = K.GetJ(), *MI = M.GetI(), *MJ = M.GetJ();
        const double *KV = K.GetData(), *MV = M.GetData();
        std::vector<int> I(2 * (size_t)n + 1), J;
        std::vector<double> A;
        J.reserve(2 * ((size_t)K.NumNonZeroElems() + (size_t)M.NumNonZeroElems())), A.reserve(J.capacity());
        for (int blk = 0; blk < 2; blk++)
            for (int r = 0; r < n; r++) {
                I[(size_t)blk * n + r] = (int)J.size();
                double s1 = blk == 0 ? 1 : -omega, s2 = blk == 0 ? -omega : -1; /* left block, right block */
                const int *c1 = blk == 0 ? KJ : MJ, *c2 = blk == 0 ? MJ : KJ;
                const double *v1 = blk == 0 ? KV : MV, *v2 = blk == 0 ? MV : KV;
                int a1 = blk == 0 ? KI[r] : MI[r], e1 = blk == 0 ? KI[r + 1] : MI[r + 1], a2 = blk == 0 ? MI[r] : KI[r], e2 = blk == 0 ? MI[r + 1] : KI[r + 1];
                for (int k = a1; k < e1; k++) J.push_back(c1[k]), A.push_back(s1 * v1[k]);
                for (int k = a2; k < e2; k++) J.push_back(n + c2[k]), A.push_back(s2 * v2[k]);
            }
        I[2 * (size_t)n] = (int)J.size();
        stats.assemble_s = now() - t0;
        double t1 = now();
        SpDirect *fac = spd_factor_indefinite(2 * n, I.data(), J.data(), A.data(), err, errlen);
        if (!fac) return false;
        stats.factor_mb = spd_factor_bytes(fac) / 1048576.0;
        std::vector<double> x(2 * (size_t)n, 0.0);
        for (int r = 0; r < n; r++) x[r] = b[r];
        bool ok = spd_solve(fac, x.data());
        spd_free(fac);
        if (!ok) {
            std::snprintf(err, errlen, "eddy3d: solve failed");
            return false;
        }
        stats.solve_s = now() - t1;
        GridFunction Ar(&V), Ai(&V);
        for (int r = 0; r < n; r++) Ar[r] = x[r], Ai[r] = x[(size_t)n + r];
        IntegrationPoint ip;
        ip.Set3(0.5, 0.5, 0.5);
        Vector ar(3), ai(3), br(3), bi(3);
        for (int e = 0; e < ne; e++) {
            ElementTransformation *Tr = m->GetElementTransformation(e);
            Tr->SetIntPoint(&ip);
            Ar.GetVectorValue(*Tr, ip, ar), Ai.GetVectorValue(*Tr, ip, ai);
            double a2 = ar * ar + ai * ai;
            if (q) q[e] = 0.5 * sigma[e] * omega * omega * a2;
            if (Jamp) Jamp[e] = sigma[e] * omega * std::sqrt(a2);
            if (Bamp) {
                Ar.GetCurl(*Tr, br), Ai.GetCurl(*Tr, bi);
                Bamp[e] = std::sqrt(br * br + bi * bi);
            }
        }
        if (st) *st = stats;
        return true;
    } catch (const std::exception &x) {
        std::snprintf(err, errlen, "eddy3d (MFEM): %s", x.what());
        return false;
    }
}

struct Heat3DFem {
    Mag3DGrid g;
    std::vector<double> r, z;
    Mesh mesh, periodic;
    Mesh *m = nullptr;
    H1_FECollection *fec = nullptr;
    FiniteElementSpace *S = nullptr;
    SparseMatrix *Mc = nullptr; /* rho c mass */
    SpDirect *fac = nullptr;
    Array<int> ess;
    Vector theta;               /* T - ambient */
    double ambient = 0, dt = 0;
    int ne = 0;
};

extern "C" Heat3DFem *heat3dfem_create(const Mag3DGrid *g, const double *k, const double *rhocp, double ambient_k, double dt, char *err, size_t errlen) {
    Heat3DFem *H = new Heat3DFem;
    try {
        H->r.assign(g->r, g->r + g->nr + 1), H->z.assign(g->z, g->z + g->nz + 1);
        H->g = *g, H->g.r = H->r.data(), H->g.z = H->z.data();
        H->m = mag3d_build_mesh(&H->g, H->mesh, H->periodic);
        H->fec = new H1_FECollection(1, 3);
        H->S = new FiniteElementSpace(H->m, H->fec);
        H->ne = g->nr * g->nt * g->nz, H->ambient = ambient_k, H->dt = dt;
        Array<int> ess_bdr(H->m->bdr_attributes.Max());
        ess_bdr = 1;
        ess_bdr[1] = 0; /* the inner cylinder insulated */
        H->S->GetEssentialTrueDofs(ess_bdr, H->ess);
        ElemScalar kc(k), cc(rhocp);
        BilinearForm mf(H->S), af(H->S);
        mf.AddDomainIntegrator(new MassIntegrator(cc));
        mf.Assemble(), mf.Finalize();
        H->Mc = new SparseMatrix(mf.SpMat());
        ProductCoefficient cdt(1.0 / dt, cc);
        af.AddDomainIntegrator(new MassIntegrator(cdt));
        af.AddDomainIntegrator(new DiffusionIntegrator(kc));
        af.Assemble(), af.Finalize();
        SparseMatrix &A = af.SpMat();
        for (int d : H->ess) A.EliminateRowCol(d, Operator::DIAG_ONE);
        H->fac = spd_factor(A.Height(), A.GetI(), A.GetJ(), A.GetData(), err, errlen);
        if (!H->fac) {
            heat3dfem_free(H);
            return nullptr;
        }
        H->theta.SetSize(H->S->GetVSize());
        H->theta = 0.0;
        return H;
    } catch (const std::exception &x) {
        std::snprintf(err, errlen, "heat3dfem (MFEM): %s", x.what());
        heat3dfem_free(H);
        return nullptr;
    }
}

extern "C" bool heat3dfem_step(Heat3DFem *H, const double *q) {
    ElemScalar qc(q);
    LinearForm f(H->S);
    f.AddDomainIntegrator(new DomainLFIntegrator(qc));
    f.Assemble();
    Vector rhs(H->theta.Size());
    H->Mc->Mult(H->theta, rhs);
    rhs *= 1.0 / H->dt;
    rhs += f;
    for (int d : H->ess) rhs[d] = 0;
    if (!spd_solve(H->fac, rhs.GetData())) return false;
    H->theta = rhs;
    return true;
}

extern "C" void heat3dfem_temperature(const Heat3DFem *H, double *T) {
    GridFunction th(H->S);
    th = H->theta;
    IntegrationPoint ip;
    ip.Set3(0.5, 0.5, 0.5);
    for (int e = 0; e < H->ne; e++) {
        ElementTransformation *Tr = H->m->GetElementTransformation(e);
        Tr->SetIntPoint(&ip);
        T[e] = H->ambient + th.GetValue(*Tr, ip);
    }
}

extern "C" double heat3dfem_energy(const Heat3DFem *H) {
    Vector mt(H->theta.Size());
    H->Mc->Mult(H->theta, mt);
    return mt.Sum();
}

extern "C" void heat3dfem_free(Heat3DFem *H) {
    if (!H) return;
    spd_free(H->fac);
    delete H->Mc, delete H->S, delete H->fec;
    delete H;
}
