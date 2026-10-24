/* breakest.h - the break estimate of a printed part (GOALS.md G20 step 5): the load at which it breaks once the
 * plastic has flowed, beside the first-failure load the G17 answer states.
 *
 * A first-failure estimate stops where the most stressed point first reaches the strength. A printed plastic bent to
 * breaking carries more: its outer fibres flow at their strength while the fibres inside take up load, and printed
 * PLA and ABS are stronger in compression than in tension, so the neutral axis moves toward the compressed side. For
 * a rectangular section of a material elastic up to its tensile strength s_t, flowing at s_t in tension and at c s_t
 * in compression, the section breaks when its outermost tensile fibre reaches m times the elastic limit strain s_t/E.
 * The bending stress it then carries, by the elastic formula 6 M / (b d^2) that a three-point test reports, is
 * g(c, m) s_t: g(1, inf) = 3/2 (the plastic moment), g(1, m) = 3/2 - 1/(2 m^2), g(c, inf) = 3c / (1 + c).
 *
 * Membrane and bending together (stress linearisation through the wall, as ASME VIII Division 2 Annex 5-A does): the
 * section's exact interaction for that material, an axial stress n s_t (tension positive) with a bending stress b
 * s_t, gives the load factor at which the combination breaks.
 *
 * Notches and holes (the theory of critical distances, point method; Taylor, The Theory of Critical Distances,
 * Elsevier 2007): the part breaks when the linear-elastic stress at L/2 from the root, along the path of the largest
 * stress, reaches the inherent strength s0, with L = (1/pi)(K_c/s0)^2. */
#pragma once
#include <stdbool.h>

/* the material constants fixed on the calibration studies of GOALS.md G20 step 5 (tools/g20_strength.py calibrate,
 * 2026-10-08): c, the compressive over the tensile strength, the median of 16 pairs for PLA and its blends, of 8 for ABS
 * and its kin, of all 24 for any other polymer; m, the outer tensile fibre's strain at the break over s_t/E, one for
 * every material (28 bending groups) */
#define BREAKEST_C_PLA 1.2421
#define BREAKEST_C_STYRENIC 1.8766
#define BREAKEST_C_OTHER 1.6442
#define BREAKEST_M 1.7457

/* the bending capacity ratio g(c, m) of a rectangular section (see above); c > 0, m >= 1 (inf allowed) */
double breakest_bending_ratio(double c, double m);

/* the largest load factor lambda such that the stresses lambda (n, b) s_t (n axial, b the outer-fibre bending stress
 * of the elastic formula) can be carried before the outermost tensile fibre reaches m times the elastic limit strain;
 * the section's exact moment-axial interaction for the material above. n and b may have either sign; b >= 0 puts the
 * tension on the side the bending stress is measured at. */
double breakest_load_factor(double n, double b, double c, double m);

/* the theory of critical distances: the critical distance L (m) from a fracture toughness K_c (Pa sqrt(m)) and the
 * inherent strength s0 (Pa) */
double breakest_critical_distance(double Kc, double s0);

/* the Creager-Paris stress ahead of a blunt notch of root radius rho (m) carrying stress intensity K (Pa sqrt(m)), at
 * the distance d (m) ahead of the root along the notch's plane: K / sqrt(2 pi r) (1 + rho / (2 r)) with r = d + rho/2
 * measured from the origin half a radius behind the root */
double breakest_creager_paris(double K, double rho, double d);

/* the stress intensity of a single-edge-notched bend bar (ASTM D5045, span 4W) for load P (N), thickness B, width W
 * (m) and notch depth a (m): P / (B sqrt(W)) f(a/W) */
double breakest_senb_K(double P, double B, double W, double a);
