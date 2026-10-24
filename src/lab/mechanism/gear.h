/* gear.h - involute spur gear teeth as a gear cutter makes them: the tooth space is the envelope of a generating rack
 * (the basic rack of ISO 53 / ISO 21771 with a tip fillet) rolling on the pitch circle. So the flank is the exact
 * involute and the root is the exact trochoidal fillet the cutter leaves, which is where a tooth's bending stress
 * peaks and where strain gauges are put; a fillet drawn as an arc would put the peak in the wrong place.
 *
 * Geometry (all lengths in metres, angles in radians unless the name says degrees):
 *   reference (pitch) radius   r   = m z / 2
 *   base radius                r_b = r cos(alpha)
 *   profile shift              x m  (the rack's datum line at r + x m)
 *   tip radius                 r_a = r + (h_a + x) m, unless given
 *   root radius                r_f = r + x m - h_fP m, h_fP the generating rack's addendum (the gear's dedendum)
 *   rack tip fillet            rho_fP m
 *   tooth thinning             each flank moved inward by `thinning` along its normal, by widening the rack: the
 *                              tooth is thinner by 2 thinning / cos(alpha) on the reference circle, which gives the
 *                              pair its backlash
 *   tip relief                 a linear profile modification over the roll length from the radius where it starts
 *                              to the tip, `tip_relief` deep at the tip (along the normal)
 *
 * Helical gears (helix_deg not 0): module and pressure angle are the normal ones, as gears are specified and cut. The
 * generating rack is the hob's normal section seen in the transverse plane (stretched along the pitch line by
 * 1 / cos(beta), so its flank is at the transverse pressure angle and its tip fillet an ellipse), and the outline is the
 * transverse section at z = 0; the section turns about the axis by `twist` = tan(beta) / r per unit length along it,
 * which is the involute helicoid. helix_deg > 0: the section turns counter-clockwise (about +z) as z grows; two external
 * gears in mesh have opposite signs, an external and an internal gear the same.
 *   transverse module  m_t = m / cos(beta)          transverse pressure angle  tan(alpha_t) = tan(alpha) / cos(beta)
 *   base helix angle   tan(beta_b) = tan(beta) cos(alpha_t)
 *
 * Frame: the gear centre at the origin, its axis along +z, the tooth this module describes centred on the +y axis. The
 * outline runs counter-clockwise around the gear's material, from the middle of the tooth space on the right of the
 * tooth (polar angle pi/2 - pi/z), along the root, up the right flank, over the tip and down the left flank to the
 * middle of the space on the left (pi/2 + pi/z). Every outline point carries its outward unit normal and what made
 * it (root circle, fillet, involute, relief, tip). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct GearSpec {
    int teeth;               /* z */
    double module_m;         /* m */
    double pressure_deg;     /* alpha, on the reference circle */
    double shift;            /* profile shift coefficient x */
    double addendum;         /* h_a (coefficient), default 1 */
    double rack_addendum;    /* h_fP of the generating rack (the gear's dedendum coefficient), default 1.25 */
    double rack_tip_radius;  /* rho_fP (coefficient), default 0.38 */
    double tip_radius_m;     /* r_a; 0: from the addendum */
    double thinning_m;       /* normal thinning of each flank, default 0 */
    double tip_relief_m;     /* depth of the linear tip relief at the tip, along the normal; 0: none */
    double relief_start_m;   /* radius where the relief starts */
    double lead_crown_m;     /* lead crowning: the flank stands back by this much at both ends of the face, parabolic
                                across it (0 at mid-face); 0: none */
    double face_width_m;     /* b */
    double bore_radius_m;    /* the radius of the bore (the shaft fit), where the gear is held */
    double helix_deg;        /* helix angle on the reference cylinder; 0: spur (see above for the sign) */
} GearSpec;

enum { GEAR_ROOT = 0, GEAR_FILLET = 1, GEAR_INVOLUTE = 2, GEAR_RELIEF = 3, GEAR_TIP = 4 };

typedef struct GearPoint {
    double x, y;   /* m */
    double nx, ny; /* outward unit normal */
    int kind;      /* GEAR_ROOT ... GEAR_TIP */
    double roll;   /* on an involute: the roll length sqrt(r^2 - r_b^2), m; else -1 */
} GearPoint;

/* One tooth's side: from the middle of the tooth space (on the root circle) to the tip corner, in order. The left side
 * is the mirror image (x -> -x) of the right side. */
typedef struct GearFlank {
    GearPoint *p;
    int n;
    int form;       /* index of the first involute point (the end of the fillet) */
    double r_form;  /* the radius where the involute begins (the form circle), m */
} GearFlank;

/* Derived radii and checks. Returns false, with a message, for a gear that cannot be made as specified (an undercut
 * the fillet would cut into the involute, a tip thinner than zero, a bore outside the root circle). */
typedef struct GearGeometry {
    GearSpec spec;
    bool internal;                /* teeth pointing inward (gear_build_internal) */
    double r, r_b, r_a, r_f, alpha;  /* transverse: the reference and base radii, the pressure angle */
    double helix, twist, beta_b;    /* the helix angle on the reference cylinder (signed), the section's turn per unit
                                       length (rad/m, signed), the base helix angle (magnitude); all 0 for spur */
    double tooth_thickness_ref_m; /* arc tooth thickness on the reference circle, after thinning */
    double tip_thickness_m;       /* arc thickness on the tip circle */
    GearFlank right;              /* sampled outline, right side of the tooth (see above) */
} GearGeometry;

void gear_spec_defaults(GearSpec *s);
/* samples: points along the involute (the fillet and root get proportionate counts); about 200 is ample */
bool gear_build(const GearSpec *s, int samples, GearGeometry *g, char *err, size_t errlen);
void gear_free(GearGeometry *g);

/* The involute function inv(a) = tan a - a, and the polar angle of the right flank's involute at radius r measured
 * from the tooth's centre line (the +y axis), for the unmodified flank. */
double gear_inv(double a);
double gear_flank_half_angle(const GearGeometry *g, double r);
/* A point of the right flank at radius r (involute part only), with its outward normal; false outside it. */
bool gear_involute_point(const GearGeometry *g, double r, GearPoint *out);

/* An internal gear (a ring with its teeth pointing inward), cut by a pinion-type shaper: the tooth space is the
 * envelope of the cutter's tooth (its involute flank, the corner and its tip land) as the cutter rolls inside the ring,
 * so the flanks are the ring's exact involutes and the root fillet the exact path of the cutter's corner.
 *   ring:   teeth, module, pressure angle, shift (the ring's profile shift: positive thins the ring's tooth), tip
 *           radius (its inner, minor radius; 0: r - (addendum - shift) m), thinning (each flank moved into the space,
 *           which gives backlash), face width; bore_radius_m is the OUTER radius of the rim, where the ring is held
 *   cutter: the shaper, an external gear of the same module and pressure angle (its teeth, addendum, its own shift)
 * The outline keeps the external convention: tooth 0 centred on +y (its tip toward the centre), the right side from
 * the middle of the space (on the root circle, now the larger radius) along the fillet and down the involute to the
 * tip corner (the smaller radius), outward normals pointing out of the material into the space. r_a < r_f. */
bool gear_build_internal(const GearSpec *ring, const GearSpec *cutter, int samples, GearGeometry *g, char *err, size_t errlen);

/* the operating centre distance and pressure angle of a pair with these shifts, with no backlash from the shifts:
 * inv(alpha_w) = inv(alpha) + 2 tan(alpha) (x1 + x2) / (z1 + z2) */
double gear_working_pressure_angle(const GearSpec *a, const GearSpec *b);
double gear_centre_distance(const GearSpec *a, const GearSpec *b);
