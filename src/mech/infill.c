/* infill.c - the infill core of a printed part (see infill.h; GOALS.md G17 step 3)
 *
 * Gyroid: Bean P, Lopez-Anido RA, Vel S, "Numerical Modeling and Experimental Investigation of Effective Elastic
 * Properties of the 3D Printed Gyroid Infill", Applied Sciences 12(4):2180 (2022), doi:10.3390/app12042180, CC BY 4.0.
 * Their closed forms, fitted to periodic finite-element models of the slicer's gyroid (Cura) for isotropic road material
 * from 1 to 4 GPa and Poisson's ratio 0 to 0.45, give the nearly isotropic core's modulus, shear modulus and Poisson's
 * ratio against the solid fraction D (0 to 1): E/Em within 0.44 % on average and 1.13 % at most of their models, and
 * within 4 % of the solid modulus against their PLA tests at 50 %. The printed road is orthotropic here, so each modulus,
 * shear modulus and Poisson's ratio of the printed material is scaled by the same functions: an assumption, stated in
 * every result, and the slicer's infill percentage is taken as the solid fraction D. Grid, lines and triangles are
 * refused: the only open closed forms found for them hold below a relative density of 0.3 (Gibson and Ashby's lattice
 * relations), not across 20 to 100 %.
 *
 * Strength: Lambiase F, Pace F, Andreucci E, "Compressive behavior of PA6-CF lattice structures for rapid tooling:
 * influence of infill pattern, infill density, and anisotropy", Int J Adv Manuf Technol 145:745-769 (2026),
 * doi:10.1007/s00170-026-18397-8, CC BY 4.0: printed gyroid infill of PA6-CF without walls, compressed along the build
 * direction at 10, 20 and 30 %, yields at sigma/sigma_s = 5.32 D^2.72 (their Table 6, R^2 0.998), sigma_s the yield of
 * full-density prints. No measured strength of printed gyroid was found above 30 % or in tension. The core's strengths
 * are the printed material's times the smaller of that law and the modulus fraction (the core failing when its roads
 * reach their own failure strain), the modulus fraction alone above 30 %, where it lies below the law; below 10 % the
 * strength is refused. A rule for this model, stated in every result; step 5's breaks test it. */
#include "infill.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { INFILL_GYROID = 0, INFILL_PATTERNS };
static const char *const NAMES[INFILL_PATTERNS] = {"gyroid"};

int infill_pattern_from_name(const char *name) {
    for (int i = 0; name && i < INFILL_PATTERNS; i++)
        if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

const char *infill_pattern_name(int pattern) { return pattern >= 0 && pattern < INFILL_PATTERNS ? NAMES[pattern] : "?"; }

const char *infill_pattern_list(void) {
    return "gyroid is modelled; grid, lines and triangles are not yet, because no published model covers them from 20 to 100 %";
}

/* Bean, Lopez-Anido and Vel (2022): the gyroid core against its solid fraction D and the road's Poisson's ratio nu */
static double gyroid_E(double D) { return D - pow(1 - D, 0.77) * pow(D, 1.2); }
static double gyroid_G(double D, double nu) { return gyroid_E(D) + 0.5 * nu * D * D * sin(M_PI * D); }
static double gyroid_nu(double D, double nu) {
    double nug = 0.088 * (1 - D * D * D) + 0.16 * (1 - D);
    return nug + nu * (gyroid_E(D) + 0.28 * (1 - D * D));
}

/* the strength fraction of the core: the measured law from 10 to 30 %, capped by the modulus fraction; 0 below 10 % */
static double gyroid_strength(double D, double *measured) {
    *measured = D >= 0.1 && D <= 0.3 ? 5.32 * pow(D, 2.72) : NAN;
    if (D < 0.1) return 0;
    return D <= 0.3 ? fmin(gyroid_E(D), *measured) : gyroid_E(D);
}

bool infill_core(int pattern, double rho, const OrthoConstants *k, const OrthoStrength *st, bool has_strength, OrthoConstants *core,
                 OrthoStrength *core_st, JsonValue *rules, char *err, size_t errlen) {
    if (pattern != INFILL_GYROID) {
        snprintf(err, errlen, "infill pattern not modelled: %s", infill_pattern_list());
        return false;
    }
    if (!(rho > 0 && rho <= 1)) {
        snprintf(err, errlen, "infill density %g is outside (0, 1]", rho);
        return false;
    }
    if (has_strength && rho < 0.1) {
        snprintf(err, errlen, "no measured strength of printed gyroid infill below 10 %% was found, so the strength of a %.3g %% core would be a "
                              "guess: analyse at 10 %% or more", 100 * rho);
        return false;
    }
    double measured = NAN, fs = rho == 1 ? 1 : gyroid_strength(rho, &measured);
    if (rho == 1) { /* the solid: exactly the printed material */
        *core = *k;
        if (has_strength) *core_st = *st;
    } else {
        double fE = gyroid_E(rho);
        for (int i = 0; i < 3; i++) core->E[i] = fE * k->E[i];
        core->G12 = gyroid_G(rho, k->nu12) * k->G12, core->G23 = gyroid_G(rho, k->nu23) * k->G23, core->G13 = gyroid_G(rho, k->nu13) * k->G13;
        core->nu12 = gyroid_nu(rho, k->nu12), core->nu13 = gyroid_nu(rho, k->nu13), core->nu23 = gyroid_nu(rho, k->nu23);
        if (has_strength) { /* every strength by one fraction */
            *core_st = *st;
            core_st->Xt *= fs, core_st->Xc *= fs, core_st->Yt *= fs, core_st->Yc *= fs, core_st->Zt *= fs, core_st->Zc *= fs;
            core_st->S12 *= fs, core_st->S23 *= fs, core_st->S31 *= fs;
        }
    }
    char why[200];
    double C[6][6];
    if (!ortho_stiffness(core, C, why, sizeof why)) {
        snprintf(err, errlen, "the gyroid core at %.3g is not admissible: %s", rho, why);
        return false;
    }
    if (rules) {
        json_push(rules, json_stringf("gyroid core at a solid fraction of %.3g (the slicer's infill percentage, taken as the printed fraction): E = Em (D - "
                                      "(1 - D)^0.77 D^1.2) = %.4g Em, G = Gm (E/Em + 0.5 nu_m D^2 sin(pi D)), nu = 0.088 (1 - D^3) + 0.16 (1 - D) + nu_m "
                                      "(E/Em + 0.28 (1 - D^2)) (Bean, Lopez-Anido and Vel, Applied Sciences 12:2180, 2022, CC BY 4.0: E within 1.13 %% of "
                                      "their finite-element models, 4 %% of the solid modulus against their PLA tests at 50 %%)",
                                      rho, gyroid_E(rho)));
        json_push(rules, json_string("the gyroid relations were fitted for an isotropic road; here each modulus, shear modulus and Poisson's ratio of the "
                                     "printed material is scaled by them, an assumption for a road that is stiffer along than across its layers"));
        if (has_strength && rho < 1) {
            if (rho <= 0.3)
                json_push(rules, json_stringf("the core's strengths are the printed material's times %.4g, the smaller of its modulus fraction %.4g and "
                                              "the yield fraction %.4g measured on printed gyroid infill of PA6-CF compressed along the build direction "
                                              "from 10 to 30 %% (Lambiase, Pace and Andreucci, Int J Adv Manuf Technol 145:745-769, 2026, CC BY 4.0, "
                                              "Table 6: 5.32 D^2.72); a rule of this model, not a measurement of your material",
                                              fs, gyroid_E(rho), measured));
            else
                json_push(rules, json_stringf("the core's strengths are the printed material's times its modulus fraction %.4g: no measured strength "
                                              "of printed gyroid infill above 30 %% was found, and this fraction lies below the law measured from 10 "
                                              "to 30 %% (5.32 D^2.72, Lambiase, Pace and Andreucci 2026); a rule of this model, not a measurement of "
                                              "your material",
                                              fs));
        }
    }
    return true;
}
