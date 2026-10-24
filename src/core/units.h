/* units.h - physical quantities for the control API: unit expressions, dimensions and SI conversion
 *
 * Every solver quantity is stored in SI (m, kg, s, K, N, Pa, J, W). The API accepts either a plain number in the
 * unit documented for a field, or a string with an explicit unit, converted here deterministically:
 *   "0.2 mm"  "60 degC"  "60 °C"  "333.15 K"  "3.5 GPa"  "0.13 W/(m*K)"  "1800 J/kg/K"  "25 W/m^2/K"  "68e-6 1/K"
 * Unit grammar: products with '*', '·' or a space, powers with '^n' or superscripts (m², m³), quotients with '/'.
 * Engineering convention: everything after '/' up to the next '/' is in the denominator, so "W/m*K" = W/(m·K).
 * Temperature offsets (degC, degF) apply only to a bare absolute temperature; inside compound units and for
 * temperature differences degC counts as K and degF as 5/9 K. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "json.h"

typedef enum {
    DIM_DIMENSIONLESS = 0,
    DIM_LENGTH,
    DIM_AREA,
    DIM_VOLUME,
    DIM_MASS,
    DIM_TIME,
    DIM_FREQUENCY,
    DIM_TEMPERATURE,            /* absolute temperature (offsets apply) */
    DIM_TEMPERATURE_DIFFERENCE, /* temperature interval */
    DIM_ANGLE,
    DIM_FORCE,
    DIM_PRESSURE, /* also stress and elastic moduli */
    DIM_ENERGY,
    DIM_POWER,
    DIM_SPEED,
    DIM_ACCELERATION,
    DIM_DENSITY,
    DIM_THERMAL_CONDUCTIVITY,
    DIM_SPECIFIC_HEAT,
    DIM_HEAT_TRANSFER_COEFFICIENT,
    DIM_HEAT_FLUX,
    DIM_VOLUMETRIC_POWER,
    DIM_EXPANSION_COEFFICIENT,
    DIM_SPECIFIC_ENERGY, /* latent heat, J/kg */
    DIM_ENERGY_DENSITY,  /* J/m^3 */
    DIM_STIFFNESS,       /* N/m */
    DIM_COUNT
} Dimension;

typedef struct UnitExpr {
    double factor;       /* SI value of one unit */
    double offset;       /* SI offset for bare absolute temperatures (degC: 273.15 K) */
    signed char exp[5];  /* exponents of length, mass, time, temperature, angle */
    bool bare_temperature;
} UnitExpr;

bool unit_parse(const char *text, UnitExpr *out, char *err, size_t errlen);
bool unit_has_dimension(const UnitExpr *u, Dimension d);

/* value expressed in `unit` -> SI value of dimension d */
bool unit_to_si(double value, const char *unit, Dimension d, double *si, char *err, size_t errlen);
/* SI value of dimension d -> value expressed in `unit` */
bool unit_from_si(double si, const char *unit, Dimension d, double *value, char *err, size_t errlen);
/* Converts with a unit known to be valid (programming error otherwise: returns NAN). */
double to_si(double value, const char *unit, Dimension d);
double from_si(double si, const char *unit, Dimension d);

/* "<number> <unit>" -> SI. A missing unit is an error unless d is dimensionless. */
bool quantity_parse(const char *text, Dimension d, double *si, char *err, size_t errlen);
/* A JSON number in default_unit, or a string with its own unit -> SI. */
bool quantity_from_json(const JsonValue *v, Dimension d, const char *default_unit, double *si, char *err, size_t errlen);

const char *dimension_name(Dimension d);
int dimension_from_name(const char *name); /* -1 if unknown */
const char *dimension_si_unit(Dimension d);
