/* mcontact.h - frictionless unilateral contact between elastic parts, solved exactly on their condensed compliances.
 *
 * Each candidate contact j is a slave node of one part facing a point on another part's surface. Its gap, positive
 * when open, is
 *
 *     g_j = g0_j + m_j theta + sum_k W_jk lambda_k,      W_jk = sum over parts of a_j' G_part a_k
 *
 * where g0_j is the gap of the unloaded parts in their positions, theta the rigid rotation of the driving part about
 * its axis beyond its kinematic position (positive in the driving sense, so m_j < 0 on the face that drives), and
 * lambda_k >= 0 the contact forces. a_j collects contact j's points on each part: the slave node with weight 1, and the
 * master point as the zone nodes of its face with their interpolation weights; G_part is that part's condensed
 * compliance (mbody.h) along the zone directions, which point into the part along its surface normal, so a contact
 * force pushes each part into itself and opens the gap. W is symmetric positive semidefinite.
 *
 * A contact may also carry a compliance of its own in series (cdiag), added to W_jj: the local flattening at a
 * contact narrower than the elements under it, which the parts' condensed compliances cannot show.
 *
 * The driving part carries a torque T, balanced by the contacts: sum_j (-m_j) lambda_j = T. The solution satisfies
 * lambda >= 0, g >= 0 and lambda_j g_j = 0 for every j (the Signorini conditions), and is found by active-set pivoting
 * on the bordered system [W_AA m_A; m_A' 0] of the contacts that are closed: a contact with a negative force leaves the
 * set, the most penetrated open one joins it, until both conditions hold to the tolerance. The result reports the
 * pivots, the largest penetration and the smallest force left, so a solution that stopped short is never mistaken
 * for one that converged.
 *
 * Friction is not modelled here (spur gears run with mu about 0.05; its effect on the root stress is a few per cent
 * and is reported as an omission by the callers). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mbody.h"

enum { MC_MAX_TERMS = 64, MC_MAX_DOF = 48 };

typedef struct MContactSet {
    int n, cap;
    /* per contact: up to MC_MAX_TERMS (part, zone index, weight) terms; the slave node first */
    int *nterm;
    int *term_body;  /* MC_MAX_TERMS each */
    int *term_zone;
    double *term_w;
    double *g0;      /* m */
    double *m;       /* m/rad */
    /* where it is, for reports and pictures: the slave's part and node, the world position of the contact */
    int *slave_body, *slave_node;
    double *where;   /* 3 each, m */
    double *normal;  /* 3 each: the master's outward normal (pointing at the slave), world frame */
    /* a compliance in series at the contact, m/N, added to W_jj: the near field the parts' meshes do not resolve
     * (mcontact_add sets it to 0; a caller may set it, and change it between solves) */
    double *cdiag;
    /* several rigid degrees of freedom (mcontact_solve_q): per contact, the gap's change with each, m/rad */
    int ndof;
    double *mq;      /* MC_MAX_DOF each */
} MContactSet;

void mcontact_clear(MContactSet *cs);
void mcontact_free(MContactSet *cs);
/* adds a contact; returns its index or -1 (out of memory). The terms' weights are 1 for the slave node and the
 * interpolation weights of the master point on its zone nodes. */
int mcontact_add(MContactSet *cs, double g0, double m, int nterm, const int *body, const int *zone, const double *w, int slave_body,
                 int slave_node, const double where[3], const double normal[3]);

typedef struct MContactResult {
    double theta;        /* rad: the driving part's rotation beyond its kinematic position */
    int active;          /* contacts closed */
    int pivots;
    bool converged;
    double max_penetration; /* m, the most negative gap left (>= 0 when there is none) */
    double min_force;       /* N, the most negative force left */
    double total_force;     /* N */
} MContactResult;

/* bodies[i] is part i of the terms. lambda receives n forces (N). tol: the gap tolerance (m), e.g. 1e-12. */
bool mcontact_solve(MBody *const *bodies, int nbodies, const MContactSet *cs, double torque, double tol, double *lambda, MContactResult *res,
                    char *err, size_t errlen);

/* Several rigid degrees of freedom q (a sun's rotation, each planet's on its pin, ...): the gap is
 *
 *     g_j = g0_j + sum_d M_jd q_d + sum_k W_jk lambda_k
 *
 * and each degree of freedom is in equilibrium under its external generalised force f_d (a torque), the contacts'
 * sum_j M_jd lambda_j and a soft spring -k_d q_d (k_d tiny against the parts' stiffness: it only fixes a part that no
 * contact holds yet). The closed set A solves [W_AA M_A; M_A' -K] [lambda_A; q] = [-g0_A; -f], by the Schur
 * complement S = M_A' W_AA^-1 M_A + K; pivoting as in mcontact_solve, from an empty set. mcontact_add_q adds a contact
 * with its row of M (cs->ndof values; set cs->ndof before adding). res->theta is q[0].
 * A degree of freedom with ksoft < 0 is held at 0: its column of M stays in the set only so that the caller can sum
 * the contacts' generalised force on it (sum_j M_jd lambda_j: the reaction that holds it). */
int mcontact_add_q(MContactSet *cs, double g0, const double *mrow, int nterm, const int *body, const int *zone, const double *w, int slave_body,
                   int slave_node, const double where[3], const double normal[3]);
bool mcontact_solve_q(MBody *const *bodies, int nbodies, const MContactSet *cs, const double *force, const double *ksoft, double tol,
                      double *lambda, double *q, MContactResult *res, char *err, size_t errlen);

/* the nodal forces each part receives from the contact forces (3 * nnodes per part, in the part's own frame) */
void mcontact_forces(MBody *const *bodies, int nbodies, const MContactSet *cs, const double *lambda, double **f_per_body);
