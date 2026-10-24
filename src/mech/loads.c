/* loads.c - wrench distribution, inertial body forces and isostatic supports (see loads.h) */
#include "loads.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../fem/hex8.h"
#include "mmath.h"

static void elem_coords(const FemMeshView *mv, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++) {
        const double *p = mv->xyz + 3 * (size_t)mv->conn[8 * (size_t)e + a];
        X[a][0] = p[0], X[a][1] = p[1], X[a][2] = p[2];
    }
}

void loads_resultant(const FemMeshView *mv, const double *f, const double p[3], double F[3], double M[3]) {
    double sF[3] = {0, 0, 0}, sM[3] = {0, 0, 0};
    for (int n = 0; n < mv->nnodes; n++) {
        const double *fn = f + 3 * (size_t)n;
        if (fn[0] == 0 && fn[1] == 0 && fn[2] == 0) continue;
        double r[3], c[3];
        mv3_sub(r, mv->xyz + 3 * (size_t)n, p);
        mv3_cross(c, r, fn);
        mv3_addto(sF, fn);
        mv3_addto(sM, c);
    }
    mv3_copy(F, sF);
    mv3_copy(M, sM);
}

bool loads_distribute_wrench(const FemMeshView *mv, const FaceSet *fs, const double F[3], const double M[3], const double p[3], double *nodal_force,
                             WrenchTransfer *rep, char *err, size_t errlen) {
    WrenchTransfer local;
    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);
    if (fs->nfaces <= 0) {
        snprintf(err, errlen, "the attachment region has no mesh faces");
        return false;
    }
    /* face areas and centroids */
    double A = 0, c[3] = {0, 0, 0};
    for (int f = 0; f < fs->nfaces; f++) {
        double X[8][3], fe[24], area, nrm[3], zero[3] = {0, 0, 0}, fc[3] = {0, 0, 0};
        elem_coords(mv, fs->elem[f], X);
        hex8_face_load(X, fs->local[f], zero, fe, &area, nrm);
        for (int k = 0; k < 4; k++) mv3_addscaled(fc, X[HEX8_FACE_NODES[fs->local[f]][k]], 0.25);
        A += area;
        mv3_addscaled(c, fc, area);
    }
    if (!(A > 0)) {
        snprintf(err, errlen, "the attachment region has zero area");
        return false;
    }
    mv3_scale(c, c, 1 / A);
    double J[9] = {0};
    for (int f = 0; f < fs->nfaces; f++) {
        double X[8][3], fe[24], area, nrm[3], zero[3] = {0, 0, 0}, fc[3] = {0, 0, 0}, r[3];
        elem_coords(mv, fs->elem[f], X);
        hex8_face_load(X, fs->local[f], zero, fe, &area, nrm);
        for (int k = 0; k < 4; k++) mv3_addscaled(fc, X[HEX8_FACE_NODES[fs->local[f]][k]], 0.25);
        mv3_sub(r, fc, c);
        double rr = mv3_dot(r, r);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) J[3 * i + j] += area * ((i == j ? rr : 0) - r[i] * r[j]);
    }
    /* moment about the centroid: M_c = M_p + (p - c) x F */
    double pc[3], pcxF[3], Mc[3];
    mv3_sub(pc, p, c);
    mv3_cross(pcxF, pc, F);
    mv3_add(Mc, M, pcxF);
    double Jc[9], lam[3], V[9];
    memcpy(Jc, J, sizeof J);
    mm_sym_eig(Jc, 3, lam, V);
    rep->area = A;
    mv3_copy(rep->centroid, c);
    memcpy(rep->J, J, sizeof J);
    mv3_copy(rep->J_eig, lam);
    /* w = J^-1 M_c on the non-degenerate eigen-directions; a moment along a degenerate direction cannot be carried */
    double w[3] = {0, 0, 0}, Mn = mv3_norm(Mc), Fn = mv3_norm(F);
    double scale_len = sqrt(A);
    for (int k = 0; k < 3; k++) {
        double vk[3] = {V[k], V[3 + k], V[6 + k]}, mk = mv3_dot(vk, Mc);
        if (lam[k] > 1e-10 * lam[0] && lam[0] > 0) {
            mv3_addscaled(w, vk, mk / lam[k]);
        } else if (fabs(mk) > 1e-9 * (Mn + Fn * scale_len) && fabs(mk) > 1e-12) {
            snprintf(err, errlen,
                     "the attachment region cannot carry a moment about direction (%.3f, %.3f, %.3f): its faces have no extent across it (moment component %.4g N m)",
                     vk[0], vk[1], vk[2], mk);
            return false;
        }
    }
    double t0[3];
    mv3_scale(t0, F, 1 / A);
    for (int f = 0; f < fs->nfaces; f++) {
        int e = fs->elem[f];
        double X[8][3], fe[24], area, nrm[3], fc[3] = {0, 0, 0}, r[3], wr[3], t[3];
        elem_coords(mv, e, X);
        for (int k = 0; k < 4; k++) mv3_addscaled(fc, X[HEX8_FACE_NODES[fs->local[f]][k]], 0.25);
        mv3_sub(r, fc, c);
        mv3_cross(wr, w, r);
        mv3_add(t, t0, wr);
        rep->max_traction = fmax(rep->max_traction, mv3_norm(t));
        hex8_face_load(X, fs->local[f], t, fe, &area, nrm);
        for (int a = 0; a < 8; a++) {
            int nd = mv->conn[8 * (size_t)e + a];
            for (int k = 0; k < 3; k++) nodal_force[3 * (size_t)nd + k] += fe[3 * a + k];
        }
    }
    /* check the transfer on its own nodal forces (rebuild them separately from anything else in nodal_force) */
    double sF[3] = {0, 0, 0}, sM[3] = {0, 0, 0};
    for (int f = 0; f < fs->nfaces; f++) {
        int e = fs->elem[f];
        double X[8][3], fe[24], area, nrm[3], fc[3] = {0, 0, 0}, r[3], wr[3], t[3];
        elem_coords(mv, e, X);
        for (int k = 0; k < 4; k++) mv3_addscaled(fc, X[HEX8_FACE_NODES[fs->local[f]][k]], 0.25);
        mv3_sub(r, fc, c);
        mv3_cross(wr, w, r);
        mv3_add(t, t0, wr);
        hex8_face_load(X, fs->local[f], t, fe, &area, nrm);
        for (int a = 0; a < 8; a++) {
            double rp[3], m[3];
            mv3_sub(rp, X[a], p);
            mv3_cross(m, rp, fe + 3 * a);
            mv3_addto(sF, fe + 3 * a);
            mv3_addto(sM, m);
        }
    }
    mv3_copy(rep->applied_force, sF);
    mv3_copy(rep->applied_moment, sM);
    double dF[3], dM[3];
    mv3_sub(dF, sF, F);
    mv3_sub(dM, sM, M);
    rep->force_error = mv3_norm(dF) / fmax(Fn, 1e-300);
    rep->moment_error = mv3_norm(dM) / fmax(mv3_norm(M) + Fn * scale_len, 1e-300);
    return true;
}

void loads_inertial(const FemMeshView *mv, const int *elems, int nelems, const double s0[3], const double omega[3], const double alpha[3], double *nodal_force,
                    InertialLoad *rep) {
    InertialLoad local;
    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);
    int count = elems ? nelems : mv->nelems;
    static const double G = 0.5773502691896257645;
    for (int i = 0; i < count; i++) {
        int e = elems ? elems[i] : i;
        double X[8][3], fe[24] = {0};
        elem_coords(mv, e, X);
        double rho = mv->density[e];
        for (int g = 0; g < 8; g++) {
            double N[8], dN[8][3], Jm[3][3], dNdx[8][3], x[3] = {0, 0, 0};
            hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, N, dN);
            double detJ = hex8_jacobian(X, dN, Jm, dNdx);
            for (int a = 0; a < 8; a++) mv3_addscaled(x, X[a], N[a]);
            double ax[3], wx[3], wwx[3], b[3];
            mv3_cross(ax, alpha, x);
            mv3_cross(wx, omega, x);
            mv3_cross(wwx, omega, wx);
            for (int k = 0; k < 3; k++) b[k] = -rho * (s0[k] + ax[k] + wwx[k]);
            double dm = rho * detJ;
            rep->mass += dm;
            mv3_addscaled(rep->com, x, dm);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) fe[3 * a + k] += N[a] * b[k] * detJ;
            double bx[3];
            mv3_cross(bx, x, b);
            mv3_addscaled(rep->resultant, b, detJ);
            mv3_addscaled(rep->moment, bx, detJ);
        }
        for (int a = 0; a < 8; a++) {
            int nd = mv->conn[8 * (size_t)e + a];
            for (int k = 0; k < 3; k++) nodal_force[3 * (size_t)nd + k] += fe[3 * a + k];
        }
    }
    if (rep->mass > 0) mv3_scale(rep->com, rep->com, 1 / rep->mass);
}

void loads_body_linear(const FemMeshView *mv, const double B[9], const double c[3], double *nodal_force) {
    static const double G = 0.5773502691896257645;
    for (int e = 0; e < mv->nelems; e++) {
        double X[8][3], fe[24] = {0};
        elem_coords(mv, e, X);
        double rho = mv->density[e];
        for (int g = 0; g < 8; g++) {
            double N[8], dN[8][3], Jm[3][3], dNdx[8][3], x[3] = {0, 0, 0}, b[3];
            hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, N, dN);
            double detJ = hex8_jacobian(X, dN, Jm, dNdx);
            for (int a = 0; a < 8; a++) mv3_addscaled(x, X[a], N[a]);
            for (int k = 0; k < 3; k++) b[k] = -rho * (B[3 * k] * x[0] + B[3 * k + 1] * x[1] + B[3 * k + 2] * x[2] + c[k]);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) fe[3 * a + k] += N[a] * b[k] * detJ;
        }
        for (int a = 0; a < 8; a++) {
            int nd = mv->conn[8 * (size_t)e + a];
            for (int k = 0; k < 3; k++) nodal_force[3 * (size_t)nd + (size_t)k] += fe[3 * a + k];
        }
    }
}

bool loads_isostatic_choose(const FemMeshView *mv, const int *nodes, int nnodes, IsostaticSupport *s) {
    int n = nodes ? nnodes : mv->nnodes;
    if (n < 3) return false;
#define NODE(i) (nodes ? nodes[i] : (i))
    double c[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) mv3_addto(c, mv->xyz + 3 * (size_t)NODE(i));
    mv3_scale(c, c, 1.0 / n);
    int A = NODE(0), B = -1, C = -1;
    double best = -1;
    for (int i = 0; i < n; i++) {
        double d[3];
        mv3_sub(d, mv->xyz + 3 * (size_t)NODE(i), c);
        if (mv3_dot(d, d) > best) best = mv3_dot(d, d), A = NODE(i);
    }
    best = -1;
    for (int i = 0; i < n; i++) {
        double d[3];
        mv3_sub(d, mv->xyz + 3 * (size_t)NODE(i), mv->xyz + 3 * (size_t)A);
        if (mv3_dot(d, d) > best) best = mv3_dot(d, d), B = NODE(i);
    }
    double ab[3];
    mv3_sub(ab, mv->xyz + 3 * (size_t)B, mv->xyz + 3 * (size_t)A);
    double L = mv3_norm(ab);
    if (!(L > 0)) return false;
    best = -1;
    for (int i = 0; i < n; i++) {
        double d[3], cr[3];
        mv3_sub(d, mv->xyz + 3 * (size_t)NODE(i), mv->xyz + 3 * (size_t)A);
        mv3_cross(cr, ab, d);
        if (mv3_dot(cr, cr) > best) best = mv3_dot(cr, cr), C = NODE(i);
    }
#undef NODE
    if (!(best > 1e-12 * L * L * L * L)) return false; /* all nodes collinear */
    s->node[0] = A, s->node[1] = B, s->node[2] = C;
    /* B: the two Cartesian axes other than AB's dominant one; C: the dominant axis of the plane normal */
    int ia = fabs(ab[0]) >= fabs(ab[1]) && fabs(ab[0]) >= fabs(ab[2]) ? 0 : (fabs(ab[1]) >= fabs(ab[2]) ? 1 : 2);
    double ac[3], nrm[3];
    mv3_sub(ac, mv->xyz + 3 * (size_t)C, mv->xyz + 3 * (size_t)A);
    mv3_cross(nrm, ab, ac);
    int in = fabs(nrm[0]) >= fabs(nrm[1]) && fabs(nrm[0]) >= fabs(nrm[2]) ? 0 : (fabs(nrm[1]) >= fabs(nrm[2]) ? 1 : 2);
    memset(s->dir, 0, sizeof s->dir);
    int k = 0;
    for (int axis = 0; axis < 3; axis++)
        if (axis != ia) s->dir[k++][axis] = 1;
    s->dir[2][in] = 1;
    /* the six constrained components must remove all rigid motions: rows (translation, rotation about A) must be full rank */
    double Rm[36] = {0};
    int row = 0;
    for (int comp = 0; comp < 3; comp++, row++) Rm[6 * row + comp] = 1; /* A: translations */
    int who[3] = {B, B, C};
    for (int q = 0; q < 3; q++, row++) {
        double r[3];
        mv3_sub(r, mv->xyz + 3 * (size_t)who[q], mv->xyz + 3 * (size_t)A);
        const double *e = s->dir[q];
        /* displacement of the node for translation u and rotation th about A: u + th x r; component along e */
        for (int comp = 0; comp < 3; comp++) Rm[6 * row + comp] = e[comp];
        double rxe[3];
        mv3_cross(rxe, r, e); /* e . (th x r) = th . (r x e) */
        for (int comp = 0; comp < 3; comp++) Rm[6 * row + 3 + comp] = rxe[comp];
    }
    double sv[6], Vv[36];
    mm_svd_jacobi(Rm, 6, 6, sv, Vv);
    return sv[5] > 1e-9 * sv[0] * (1 + L);
}

void loads_isostatic_fixed(const IsostaticSupport *s, unsigned char *fixed) {
    for (int k = 0; k < 3; k++) fixed[3 * (size_t)s->node[0] + (size_t)k] = 1;
    for (int q = 0; q < 3; q++) {
        int nd = q < 2 ? s->node[1] : s->node[2];
        for (int k = 0; k < 3; k++)
            if (s->dir[q][k] != 0) fixed[3 * (size_t)nd + (size_t)k] = 1;
    }
}
