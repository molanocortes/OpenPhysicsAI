/* mechunits.c - mechanical quantities by unit exponents, electrical quantities by a fixed unit list */
#include "mechunits.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/units.h"

typedef struct QtyInfo {
    const char *name, *si;
    signed char exp[5]; /* length, mass, time, temperature, angle */
    bool electrical;
} QtyInfo;

static const QtyInfo QTY[MQ_COUNT] = {
    [MQ_DIMENSIONLESS] = {"dimensionless", "1", {0, 0, 0, 0, 0}, false},
    [MQ_LENGTH] = {"length", "m", {1, 0, 0, 0, 0}, false},
    [MQ_ANGLE] = {"angle", "rad", {0, 0, 0, 0, 1}, false},
    [MQ_MASS] = {"mass", "kg", {0, 1, 0, 0, 0}, false},
    [MQ_TIME] = {"time", "s", {0, 0, 1, 0, 0}, false},
    [MQ_FREQUENCY] = {"frequency", "Hz", {0, 0, -1, 0, 0}, false},
    [MQ_FORCE] = {"force", "N", {1, 1, -2, 0, 0}, false},
    [MQ_TORQUE] = {"torque", "N*m", {2, 1, -2, 0, 0}, false},
    [MQ_ENERGY] = {"energy", "J", {2, 1, -2, 0, 0}, false},
    [MQ_POWER] = {"power", "W", {2, 1, -3, 0, 0}, false},
    [MQ_PRESSURE] = {"pressure", "Pa", {-1, 1, -2, 0, 0}, false},
    [MQ_DENSITY] = {"density", "kg/m^3", {-3, 1, 0, 0, 0}, false},
    [MQ_INERTIA] = {"inertia", "kg*m^2", {2, 1, 0, 0, 0}, false},
    [MQ_VELOCITY] = {"velocity", "m/s", {1, 0, -1, 0, 0}, false},
    [MQ_ANGULAR_VELOCITY] = {"angular_velocity", "rad/s", {0, 0, -1, 0, 1}, false},
    [MQ_ACCELERATION] = {"acceleration", "m/s^2", {1, 0, -2, 0, 0}, false},
    [MQ_ANGULAR_ACCELERATION] = {"angular_acceleration", "rad/s^2", {0, 0, -2, 0, 1}, false},
    [MQ_LINEAR_STIFFNESS] = {"linear_stiffness", "N/m", {0, 1, -2, 0, 0}, false},
    [MQ_ROTATIONAL_STIFFNESS] = {"rotational_stiffness", "N*m/rad", {2, 1, -2, 0, -1}, false},
    [MQ_LINEAR_DAMPING] = {"linear_damping", "N*s/m", {0, 1, -1, 0, 0}, false},
    [MQ_ROTATIONAL_DAMPING] = {"rotational_damping", "N*m*s/rad", {2, 1, -1, 0, -1}, false},
    [MQ_TEMPERATURE] = {"temperature", "K", {0, 0, 0, 1, 0}, false},
    [MQ_VOLTAGE] = {"voltage", "V", {0}, true},
    [MQ_CURRENT] = {"current", "A", {0}, true},
    [MQ_RESISTANCE] = {"resistance", "ohm", {0}, true},
    [MQ_INDUCTANCE] = {"inductance", "H", {0}, true},
    [MQ_TORQUE_CONSTANT] = {"torque_constant", "N*m/A", {0}, true},
    [MQ_BACK_EMF_CONSTANT] = {"back_emf_constant", "V*s/rad", {0}, true},
};

const char *mech_qty_name(MechQty q) { return (unsigned)q < MQ_COUNT ? QTY[q].name : "?"; }
const char *mech_qty_si_unit(MechQty q) { return (unsigned)q < MQ_COUNT ? QTY[q].si : "?"; }

typedef struct ExtraUnit {
    MechQty q;
    const char *unit;
    double factor;
} ExtraUnit;

#define RPM (2 * M_PI / 60)
static const ExtraUnit EXTRA[] = {
    {MQ_ANGULAR_VELOCITY, "rpm", RPM},
    {MQ_ANGULAR_VELOCITY, "rps", 2 * M_PI},
    {MQ_ANGULAR_VELOCITY, "rev/s", 2 * M_PI},
    {MQ_ANGLE, "rev", 2 * M_PI},
    {MQ_ANGLE, "turn", 2 * M_PI},
    {MQ_VOLTAGE, "V", 1},
    {MQ_VOLTAGE, "mV", 1e-3},
    {MQ_VOLTAGE, "kV", 1e3},
    {MQ_CURRENT, "A", 1},
    {MQ_CURRENT, "mA", 1e-3},
    {MQ_CURRENT, "uA", 1e-6},
    {MQ_RESISTANCE, "ohm", 1},
    {MQ_RESISTANCE, "Ohm", 1},
    {MQ_RESISTANCE, "\xCE\xA9", 1},
    {MQ_RESISTANCE, "mohm", 1e-3},
    {MQ_RESISTANCE, "mOhm", 1e-3},
    {MQ_RESISTANCE, "m\xCE\xA9", 1e-3},
    {MQ_RESISTANCE, "kohm", 1e3},
    {MQ_RESISTANCE, "kOhm", 1e3},
    {MQ_RESISTANCE, "k\xCE\xA9", 1e3},
    {MQ_INDUCTANCE, "H", 1},
    {MQ_INDUCTANCE, "mH", 1e-3},
    {MQ_INDUCTANCE, "uH", 1e-6},
    {MQ_TORQUE_CONSTANT, "N*m/A", 1},
    {MQ_TORQUE_CONSTANT, "Nm/A", 1},
    {MQ_TORQUE_CONSTANT, "mN*m/A", 1e-3},
    {MQ_TORQUE_CONSTANT, "mNm/A", 1e-3},
    {MQ_TORQUE_CONSTANT, "oz*in/A", 0.00706155183333},
    {MQ_BACK_EMF_CONSTANT, "V*s/rad", 1},
    {MQ_BACK_EMF_CONSTANT, "V/(rad/s)", 1},
    {MQ_BACK_EMF_CONSTANT, "mV/rpm", 1e-3 / RPM},
    {MQ_BACK_EMF_CONSTANT, "V/krpm", 1.0 / (1000 * RPM)},
    {MQ_BACK_EMF_CONSTANT, "V/rpm", 1.0 / RPM},
};

bool mech_unit_factor(const char *unit, MechQty q, double *factor, double *offset, char *err, size_t errlen) {
    if ((unsigned)q >= MQ_COUNT) {
        snprintf(err, errlen, "unknown quantity");
        return false;
    }
    *offset = 0;
    for (size_t i = 0; i < sizeof EXTRA / sizeof *EXTRA; i++)
        if (EXTRA[i].q == q && !strcmp(EXTRA[i].unit, unit)) {
            *factor = EXTRA[i].factor;
            return true;
        }
    if (QTY[q].electrical) {
        char list[256] = "";
        size_t used = 0;
        for (size_t i = 0; i < sizeof EXTRA / sizeof *EXTRA && used < sizeof list - 16; i++)
            if (EXTRA[i].q == q) used += (size_t)snprintf(list + used, sizeof list - used, "%s%s", used ? ", " : "", EXTRA[i].unit);
        snprintf(err, errlen, "unit '%s' is not accepted for %s (use %s)", unit, QTY[q].name, list);
        return false;
    }
    UnitExpr u;
    char uerr[160];
    if (!unit_parse(unit, &u, uerr, sizeof uerr)) {
        snprintf(err, errlen, "unit '%s': %s", unit, uerr);
        return false;
    }
    for (int k = 0; k < 5; k++)
        if (u.exp[k] != QTY[q].exp[k]) {
            snprintf(err, errlen, "unit '%s' does not measure %s (expected something like %s)", unit, QTY[q].name, QTY[q].si);
            return false;
        }
    *factor = u.factor;
    if (q == MQ_TEMPERATURE && u.bare_temperature) *offset = u.offset;
    return true;
}

bool mech_qty_parse(const char *text, MechQty q, double *si, char *err, size_t errlen) {
    if (!text) {
        snprintf(err, errlen, "missing value");
        return false;
    }
    char *end;
    double val = strtod(text, &end);
    if (end == text || !isfinite(val)) {
        snprintf(err, errlen, "'%s' does not start with a number", text);
        return false;
    }
    while (*end && isspace((unsigned char)*end)) end++;
    if (!*end) {
        if (q == MQ_DIMENSIONLESS) {
            *si = val;
            return true;
        }
        snprintf(err, errlen, "'%s' needs a unit of %s (for example %s)", text, QTY[q].name, QTY[q].si);
        return false;
    }
    double f, off;
    if (!mech_unit_factor(end, q, &f, &off, err, errlen)) return false;
    *si = val * f + off;
    return true;
}

bool mech_qty_from_json_ex(const JsonValue *v, MechQty q, const char *default_unit, bool require_unit, double *si, char *err, size_t errlen) {
    if (require_unit && v && v->type == JSON_NUMBER && v->u.number != 0 && (!default_unit || !*default_unit) && q != MQ_DIMENSIONLESS) {
        snprintf(err, errlen, "plain number %g needs a unit: write it as a string such as \"%g %s\", or declare units.%s", v->u.number, v->u.number,
                 mech_qty_si_unit(q), mech_qty_name(q));
        return false;
    }
    return mech_qty_from_json(v, q, default_unit, si, err, errlen);
}

bool mech_qty_from_json(const JsonValue *v, MechQty q, const char *default_unit, double *si, char *err, size_t errlen) {
    if (!v) {
        snprintf(err, errlen, "missing value");
        return false;
    }
    if (v->type == JSON_NUMBER) {
        if (!isfinite(v->u.number)) {
            snprintf(err, errlen, "value is not finite");
            return false;
        }
        if (!default_unit || !*default_unit) {
            *si = v->u.number;
            return true;
        }
        double f, off;
        if (!mech_unit_factor(default_unit, q, &f, &off, err, errlen)) return false;
        *si = v->u.number * f + off;
        return true;
    }
    if (v->type == JSON_STRING) return mech_qty_parse(v->u.string.ptr, q, si, err, errlen);
    snprintf(err, errlen, "expected a number or a string with a unit, got %s", json_type_name(v->type));
    return false;
}
