/* mechunits.h - mechanical and electromechanical quantities on top of the shared unit parser (core/units.h)
 *
 * A quantity is either a JSON number in a default unit or a string "<number> <unit>". Mechanical units are checked by
 * their exponents of length, mass, time, temperature and angle (so N*m/rad is a rotational stiffness and N*m is not).
 * Electrical units have no base dimension in the shared parser and are accepted from a fixed list per quantity. Values
 * are returned in SI (rad for angles). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"

typedef enum {
    MQ_DIMENSIONLESS = 0,
    MQ_LENGTH,
    MQ_ANGLE,
    MQ_MASS,
    MQ_TIME,
    MQ_FREQUENCY,
    MQ_FORCE,
    MQ_TORQUE,
    MQ_ENERGY,
    MQ_POWER,
    MQ_PRESSURE,
    MQ_DENSITY,
    MQ_INERTIA,
    MQ_VELOCITY,
    MQ_ANGULAR_VELOCITY,
    MQ_ACCELERATION,
    MQ_ANGULAR_ACCELERATION,
    MQ_LINEAR_STIFFNESS,
    MQ_ROTATIONAL_STIFFNESS,
    MQ_LINEAR_DAMPING,
    MQ_ROTATIONAL_DAMPING,
    MQ_TEMPERATURE,
    MQ_VOLTAGE,
    MQ_CURRENT,
    MQ_RESISTANCE,
    MQ_INDUCTANCE,
    MQ_TORQUE_CONSTANT,   /* N m/A */
    MQ_BACK_EMF_CONSTANT, /* V s/rad (numerically equal to the torque constant in SI) */
    MQ_COUNT
} MechQty;

const char *mech_qty_name(MechQty q);
const char *mech_qty_si_unit(MechQty q);
/* value in SI from a JSON number (in default_unit; NULL = SI) or a string with its own unit */
bool mech_qty_from_json(const JsonValue *v, MechQty q, const char *default_unit, double *si, char *err, size_t errlen);
/* as above; with require_unit a plain number of a dimensional quantity needs a declared default unit (no silent SI) */
bool mech_qty_from_json_ex(const JsonValue *v, MechQty q, const char *default_unit, bool require_unit, double *si, char *err, size_t errlen);
bool mech_qty_parse(const char *text, MechQty q, double *si, char *err, size_t errlen);
/* SI factor of a unit for a quantity (false if the unit does not fit) */
bool mech_unit_factor(const char *unit, MechQty q, double *factor, double *offset, char *err, size_t errlen);
