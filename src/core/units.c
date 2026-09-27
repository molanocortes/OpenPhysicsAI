/* units.c - unit expression parser and SI conversion */
#include "units.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { EL = 0, EM, ET, ETH, EA };
enum { U_PREFIX = 1, U_TEMP = 2 };

typedef struct {
    const char *sym;
    double factor, offset;
    signed char e[5];
    unsigned flags;
} UnitSym;

#define D_LEN {1, 0, 0, 0, 0}
#define D_MASS {0, 1, 0, 0, 0}
#define D_TIME {0, 0, 1, 0, 0}
#define D_TEMP {0, 0, 0, 1, 0}
#define D_ANGLE {0, 0, 0, 0, 1}
#define D_FORCE {1, 1, -2, 0, 0}
#define D_PRESS {-1, 1, -2, 0, 0}
#define D_ENERGY {2, 1, -2, 0, 0}
#define D_POWER {2, 1, -3, 0, 0}

static const UnitSym SYMS[] = {
    {"m", 1, 0, D_LEN, U_PREFIX},
    {"micron", 1e-6, 0, D_LEN, 0},
    {"in", 0.0254, 0, D_LEN, 0},
    {"inch", 0.0254, 0, D_LEN, 0},
    {"inches", 0.0254, 0, D_LEN, 0},
    {"ft", 0.3048, 0, D_LEN, 0},
    {"mil", 25.4e-6, 0, D_LEN, 0},
    {"g", 1e-3, 0, D_MASS, U_PREFIX},
    {"lb", 0.45359237, 0, D_MASS, 0},
    {"s", 1, 0, D_TIME, U_PREFIX},
    {"sec", 1, 0, D_TIME, 0},
    {"min", 60, 0, D_TIME, 0},
    {"h", 3600, 0, D_TIME, 0},
    {"hr", 3600, 0, D_TIME, 0},
    {"Hz", 1, 0, {0, 0, -1, 0, 0}, U_PREFIX},
    {"K", 1, 0, D_TEMP, U_TEMP},
    {"degC", 1, 273.15, D_TEMP, U_TEMP},
    {"\xC2\xB0" "C", 1, 273.15, D_TEMP, U_TEMP},
    {"\xE2\x84\x83", 1, 273.15, D_TEMP, U_TEMP},
    {"C", 1, 273.15, D_TEMP, U_TEMP},
    {"celsius", 1, 273.15, D_TEMP, U_TEMP},
    {"degF", 5.0 / 9.0, 459.67 * 5.0 / 9.0, D_TEMP, U_TEMP},
    {"\xC2\xB0" "F", 5.0 / 9.0, 459.67 * 5.0 / 9.0, D_TEMP, U_TEMP},
    {"F", 5.0 / 9.0, 459.67 * 5.0 / 9.0, D_TEMP, U_TEMP},
    {"fahrenheit", 5.0 / 9.0, 459.67 * 5.0 / 9.0, D_TEMP, U_TEMP},
    {"rad", 1, 0, D_ANGLE, 0},
    {"deg", M_PI / 180.0, 0, D_ANGLE, 0},
    {"degree", M_PI / 180.0, 0, D_ANGLE, 0},
    {"degrees", M_PI / 180.0, 0, D_ANGLE, 0},
    {"\xC2\xB0", M_PI / 180.0, 0, D_ANGLE, 0},
    {"N", 1, 0, D_FORCE, U_PREFIX},
    {"lbf", 4.4482216152605, 0, D_FORCE, 0},
    {"Pa", 1, 0, D_PRESS, U_PREFIX},
    {"bar", 1e5, 0, D_PRESS, U_PREFIX},
    {"atm", 101325, 0, D_PRESS, 0},
    {"psi", 6894.757293168361, 0, D_PRESS, 0},
    {"ksi", 6894757.293168361, 0, D_PRESS, 0},
    {"J", 1, 0, D_ENERGY, U_PREFIX},
    {"cal", 4.184, 0, D_ENERGY, U_PREFIX},
    {"W", 1, 0, D_POWER, U_PREFIX},
    {"L", 1e-3, 0, {3, 0, 0, 0, 0}, U_PREFIX},
    {"%", 0.01, 0, {0, 0, 0, 0, 0}, 0},
};

static const struct {
    const char *p;
    double f;
} PREFIXES[] = {
    {"G", 1e9}, {"M", 1e6}, {"k", 1e3}, {"h", 1e2}, {"d", 1e-1}, {"c", 1e-2}, {"m", 1e-3},
    {"u", 1e-6}, {"\xC2\xB5", 1e-6}, {"\xCE\xBC", 1e-6}, {"n", 1e-9},
};

static const struct {
    const char *name;
    const char *si;
    signed char e[5];
} DIMS[DIM_COUNT] = {
    [DIM_DIMENSIONLESS] = {"dimensionless", "1", {0, 0, 0, 0, 0}},
    [DIM_LENGTH] = {"length", "m", D_LEN},
    [DIM_AREA] = {"area", "m^2", {2, 0, 0, 0, 0}},
    [DIM_VOLUME] = {"volume", "m^3", {3, 0, 0, 0, 0}},
    [DIM_MASS] = {"mass", "kg", D_MASS},
    [DIM_TIME] = {"time", "s", D_TIME},
    [DIM_FREQUENCY] = {"frequency", "Hz", {0, 0, -1, 0, 0}},
    [DIM_TEMPERATURE] = {"temperature", "K", D_TEMP},
    [DIM_TEMPERATURE_DIFFERENCE] = {"temperature_difference", "K", D_TEMP},
    [DIM_ANGLE] = {"angle", "rad", D_ANGLE},
    [DIM_FORCE] = {"force", "N", D_FORCE},
    [DIM_PRESSURE] = {"pressure", "Pa", D_PRESS},
    [DIM_ENERGY] = {"energy", "J", D_ENERGY},
    [DIM_POWER] = {"power", "W", D_POWER},
    [DIM_SPEED] = {"speed", "m/s", {1, 0, -1, 0, 0}},
    [DIM_ACCELERATION] = {"acceleration", "m/s^2", {1, 0, -2, 0, 0}},
    [DIM_DENSITY] = {"density", "kg/m^3", {-3, 1, 0, 0, 0}},
    [DIM_THERMAL_CONDUCTIVITY] = {"thermal_conductivity", "W/(m*K)", {1, 1, -3, -1, 0}},
    [DIM_SPECIFIC_HEAT] = {"specific_heat", "J/(kg*K)", {2, 0, -2, -1, 0}},
    [DIM_HEAT_TRANSFER_COEFFICIENT] = {"heat_transfer_coefficient", "W/(m^2*K)", {0, 1, -3, -1, 0}},
    [DIM_HEAT_FLUX] = {"heat_flux", "W/m^2", {0, 1, -3, 0, 0}},
    [DIM_VOLUMETRIC_POWER] = {"volumetric_power", "W/m^3", {-1, 1, -3, 0, 0}},
    [DIM_EXPANSION_COEFFICIENT] = {"expansion_coefficient", "1/K", {0, 0, 0, -1, 0}},
    [DIM_SPECIFIC_ENERGY] = {"specific_energy", "J/kg", {2, 0, -2, 0, 0}},
    [DIM_ENERGY_DENSITY] = {"energy_density", "J/m^3", {-1, 1, -2, 0, 0}},
    [DIM_STIFFNESS] = {"stiffness", "N/m", {0, 1, -2, 0, 0}},
};

const char *dimension_name(Dimension d) { return d >= 0 && d < DIM_COUNT ? DIMS[d].name : "?"; }
const char *dimension_si_unit(Dimension d) { return d >= 0 && d < DIM_COUNT ? DIMS[d].si : "?"; }

int dimension_from_name(const char *name) {
    for (int i = 0; name && i < DIM_COUNT; i++)
        if (strcmp(DIMS[i].name, name) == 0) return i;
    return -1;
}

/* ---- parser ------------------------------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t pos, len;
    char *err;
    size_t errlen;
    bool failed;
} UP;

static void uerr(UP *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void uerr(UP *p, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = true;
    if (!p->err || !p->errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, p->errlen, fmt, ap);
    va_end(ap);
}

static bool starts_with(const UP *p, const char *lit) {
    size_t n = strlen(lit);
    return p->len - p->pos >= n && memcmp(p->s + p->pos, lit, n) == 0;
}

static size_t mul_op_len(const UP *p) {
    if (p->pos >= p->len) return 0;
    if (p->s[p->pos] == '*') return 1;
    if (starts_with(p, "\xC2\xB7")) return 2;     /* middle dot */
    if (starts_with(p, "\xE2\x8B\x85")) return 3; /* dot operator */
    if (starts_with(p, "\xC3\x97")) return 2;     /* multiplication sign */
    return 0;
}

/* superscript digit value, or -1 for superscript minus, or -2 if not a superscript; *n = byte length */
static int superscript_at(const UP *p, size_t *n) {
    const unsigned char *s = (const unsigned char *)p->s + p->pos;
    size_t avail = p->len - p->pos;
    if (avail >= 2 && s[0] == 0xC2) {
        *n = 2;
        if (s[1] == 0xB2) return 2;
        if (s[1] == 0xB3) return 3;
        if (s[1] == 0xB9) return 1;
    }
    if (avail >= 3 && s[0] == 0xE2 && s[1] == 0x81) {
        *n = 3;
        if (s[2] == 0xBB) return -1;
        if (s[2] == 0xB0) return 0;
        if (s[2] >= 0xB4 && s[2] <= 0xB9) return 4 + (s[2] - 0xB4);
    }
    return -2;
}

static bool ident_char_at(const UP *p, size_t *n) {
    if (p->pos >= p->len) return false;
    unsigned char c = (unsigned char)p->s[p->pos];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '%') {
        *n = 1;
        return true;
    }
    if (c < 0x80) return false;
    size_t sn;
    if (mul_op_len(p) || superscript_at(p, &sn) != -2) return false;
    int len = utf8_sequence_length((const unsigned char *)p->s + p->pos, p->len - p->pos);
    if (len <= 0) return false;
    *n = (size_t)len;
    return true;
}

static const UnitSym *find_exact(const char *tok, size_t n) {
    for (size_t i = 0; i < sizeof SYMS / sizeof SYMS[0]; i++)
        if (strlen(SYMS[i].sym) == n && memcmp(SYMS[i].sym, tok, n) == 0) return &SYMS[i];
    return NULL;
}

static bool lookup_unit(const char *tok, size_t n, double *f, int e[5]) {
    const UnitSym *u = find_exact(tok, n);
    double mult = 1;
    if (!u) {
        for (size_t i = 0; i < sizeof PREFIXES / sizeof PREFIXES[0] && !u; i++) {
            size_t pl = strlen(PREFIXES[i].p);
            if (n > pl && memcmp(tok, PREFIXES[i].p, pl) == 0) {
                const UnitSym *base = find_exact(tok + pl, n - pl);
                if (base && (base->flags & U_PREFIX)) u = base, mult = PREFIXES[i].f;
            }
        }
    }
    if (!u) return false;
    *f = u->factor * mult;
    for (int k = 0; k < 5; k++) e[k] = u->e[k];
    return true;
}

static void skip_spaces(UP *p) {
    while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t')) p->pos++;
}

static bool parse_expr(UP *p, double *f, int e[5], int depth);

static bool parse_atom(UP *p, double *f, int e[5], int depth) {
    skip_spaces(p);
    if (p->pos >= p->len) {
        uerr(p, "unit expression ends unexpectedly");
        return false;
    }
    if (p->s[p->pos] == '(') {
        p->pos++;
        if (!parse_expr(p, f, e, depth + 1)) return false;
        skip_spaces(p);
        if (p->pos >= p->len || p->s[p->pos] != ')') {
            uerr(p, "missing ')' in unit");
            return false;
        }
        p->pos++;
        return true;
    }
    if (p->s[p->pos] == '1' && (p->pos + 1 >= p->len || p->s[p->pos + 1] < '0' || p->s[p->pos + 1] > '9')) {
        p->pos++;
        *f = 1;
        memset(e, 0, 5 * sizeof(int));
        return true;
    }
    size_t start = p->pos, n;
    while (ident_char_at(p, &n)) p->pos += n;
    if (p->pos == start) {
        uerr(p, "unexpected character '%c' in unit", p->s[p->pos]);
        return false;
    }
    if (!lookup_unit(p->s + start, p->pos - start, f, e)) {
        uerr(p, "unknown unit '%.*s'", (int)(p->pos - start), p->s + start);
        return false;
    }
    return true;
}

static bool parse_power(UP *p, double *f, int e[5], int depth) {
    if (!parse_atom(p, f, e, depth)) return false;
    int expo = 1;
    bool have = false;
    if (p->pos < p->len && p->s[p->pos] == '^') {
        p->pos++;
        bool paren = p->pos < p->len && p->s[p->pos] == '(';
        if (paren) p->pos++;
        int sign = 1;
        if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) sign = p->s[p->pos++] == '-' ? -1 : 1;
        int v = 0, digits = 0;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9' && digits < 3) v = v * 10 + (p->s[p->pos++] - '0'), digits++;
        if (!digits) {
            uerr(p, "expected an integer exponent after '^'");
            return false;
        }
        if (paren) {
            if (p->pos >= p->len || p->s[p->pos] != ')') {
                uerr(p, "missing ')' after exponent");
                return false;
            }
            p->pos++;
        }
        expo = sign * v;
        have = true;
    } else {
        size_t n;
        int sign = 1, v = 0, digits = 0, s;
        while (p->pos < p->len && (s = superscript_at(p, &n)) != -2) {
            if (s == -1) {
                if (digits) break;
                sign = -1;
            } else {
                v = v * 10 + s, digits++;
            }
            p->pos += n;
        }
        if (digits) expo = sign * v, have = true;
        else if (sign < 0) {
            uerr(p, "superscript minus without digits");
            return false;
        }
    }
    if (have) {
        *f = pow(*f, expo);
        for (int k = 0; k < 5; k++) e[k] *= expo;
    }
    return true;
}

static bool parse_product(UP *p, double *f, int e[5], int depth) {
    if (!parse_power(p, f, e, depth)) return false;
    for (;;) {
        size_t save = p->pos;
        skip_spaces(p);
        size_t ol = mul_op_len(p), n;
        if (ol) {
            p->pos += ol;
        } else if (p->pos > save && p->pos < p->len && (p->s[p->pos] == '(' || ident_char_at(p, &n))) {
            /* implicit multiplication: "N m" */
        } else {
            p->pos = save;
            return true;
        }
        double f2;
        int e2[5];
        if (!parse_power(p, &f2, e2, depth)) return false;
        *f *= f2;
        for (int k = 0; k < 5; k++) e[k] += e2[k];
    }
}

static bool parse_expr(UP *p, double *f, int e[5], int depth) {
    if (depth > 8) {
        uerr(p, "unit expression nested too deeply");
        return false;
    }
    if (!parse_product(p, f, e, depth)) return false;
    for (;;) {
        skip_spaces(p);
        if (p->pos >= p->len || p->s[p->pos] != '/') return true;
        p->pos++;
        double f2;
        int e2[5];
        if (!parse_product(p, &f2, e2, depth)) return false;
        *f /= f2;
        for (int k = 0; k < 5; k++) e[k] -= e2[k];
    }
}

bool unit_parse(const char *text, UnitExpr *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (err && errlen) err[0] = 0;
    if (!text) {
        if (err) snprintf(err, errlen, "no unit");
        return false;
    }
    size_t a = 0, b = strlen(text);
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) b--;
    if (a == b) {
        if (err) snprintf(err, errlen, "empty unit");
        return false;
    }
    if (b - a > 64) {
        if (err) snprintf(err, errlen, "unit string too long");
        return false;
    }
    UP p = {text + a, 0, b - a, err, errlen, false};
    double f;
    int e[5];
    if (!parse_expr(&p, &f, e, 0)) return false;
    skip_spaces(&p);
    if (p.pos != p.len) {
        uerr(&p, "unexpected '%.*s' in unit", (int)(p.len - p.pos), p.s + p.pos);
        return false;
    }
    if (!isfinite(f) || f <= 0) {
        uerr(&p, "unit factor out of range");
        return false;
    }
    const UnitSym *bare = find_exact(text + a, b - a);
    out->factor = f;
    for (int k = 0; k < 5; k++) {
        if (e[k] < -60 || e[k] > 60) {
            uerr(&p, "unit exponent out of range");
            return false;
        }
        out->exp[k] = (signed char)e[k];
    }
    if (bare && (bare->flags & U_TEMP)) {
        out->bare_temperature = true;
        out->offset = bare->offset;
    }
    return true;
}

bool unit_has_dimension(const UnitExpr *u, Dimension d) {
    if (d < 0 || d >= DIM_COUNT) return false;
    return memcmp(u->exp, DIMS[d].e, 5) == 0;
}

static bool check_dim(const UnitExpr *u, const char *unit, Dimension d, char *err, size_t errlen) {
    if (!unit_has_dimension(u, d)) {
        if (err) snprintf(err, errlen, "unit '%s' is not a %s unit (expected something like %s)", unit, dimension_name(d), dimension_si_unit(d));
        return false;
    }
    if (d == DIM_TEMPERATURE && !u->bare_temperature) {
        if (err) snprintf(err, errlen, "an absolute temperature needs K, degC or degF (got '%s')", unit);
        return false;
    }
    return true;
}

bool unit_to_si(double value, const char *unit, Dimension d, double *si, char *err, size_t errlen) {
    UnitExpr u;
    if (!unit_parse(unit, &u, err, errlen) || !check_dim(&u, unit, d, err, errlen)) return false;
    *si = value * u.factor + (d == DIM_TEMPERATURE ? u.offset : 0.0);
    return true;
}

bool unit_from_si(double si, const char *unit, Dimension d, double *value, char *err, size_t errlen) {
    UnitExpr u;
    if (!unit_parse(unit, &u, err, errlen) || !check_dim(&u, unit, d, err, errlen)) return false;
    *value = (si - (d == DIM_TEMPERATURE ? u.offset : 0.0)) / u.factor;
    return true;
}

double to_si(double value, const char *unit, Dimension d) {
    double si;
    return unit_to_si(value, unit, d, &si, NULL, 0) ? si : NAN;
}

double from_si(double si, const char *unit, Dimension d) {
    double v;
    return unit_from_si(si, unit, d, &v, NULL, 0) ? v : NAN;
}

bool quantity_parse(const char *text, Dimension d, double *si, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!text) {
        if (err) snprintf(err, errlen, "empty quantity");
        return false;
    }
    const char *s = text;
    while (*s == ' ' || *s == '\t') s++;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    size_t digits = 0;
    while (s[i] >= '0' && s[i] <= '9') i++, digits++;
    if (s[i] == '.') {
        i++;
        while (s[i] >= '0' && s[i] <= '9') i++, digits++;
    }
    if (!digits) {
        if (err) snprintf(err, errlen, "'%s' does not start with a number", text);
        return false;
    }
    /* exponent only when followed by digits, so "5 e" is not mistaken ("1e-3 m" works) */
    if ((s[i] == 'e' || s[i] == 'E') && ((s[i + 1] >= '0' && s[i + 1] <= '9') ||
                                         ((s[i + 1] == '+' || s[i + 1] == '-') && s[i + 2] >= '0' && s[i + 2] <= '9'))) {
        i += 2;
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    if (i > 64) {
        if (err) snprintf(err, errlen, "number too long");
        return false;
    }
    char num[80];
    memcpy(num, s, i);
    num[i] = 0;
    char dp = localeconv()->decimal_point[0];
    if (dp != '.')
        for (char *q = num; *q; q++)
            if (*q == '.') *q = dp;
    double v = strtod(num, NULL);
    if (!isfinite(v)) {
        if (err) snprintf(err, errlen, "number out of range");
        return false;
    }
    const char *unit = s + i;
    while (*unit == ' ' || *unit == '\t') unit++;
    if (!*unit) {
        if (d == DIM_DIMENSIONLESS) {
            *si = v;
            return true;
        }
        if (err) snprintf(err, errlen, "'%s' has no unit (expected a %s unit such as %s)", text, dimension_name(d), dimension_si_unit(d));
        return false;
    }
    return unit_to_si(v, unit, d, si, err, errlen);
}

bool quantity_from_json(const JsonValue *v, Dimension d, const char *default_unit, double *si, char *err, size_t errlen) {
    if (v && v->type == JSON_NUMBER) {
        if (!default_unit) {
            *si = v->u.number;
            return true;
        }
        return unit_to_si(v->u.number, default_unit, d, si, err, errlen);
    }
    if (v && v->type == JSON_STRING) return quantity_parse(v->u.string.ptr, d, si, err, errlen);
    if (err) snprintf(err, errlen, "expected a number in %s or a string with a unit", default_unit ? default_unit : dimension_si_unit(d));
    return false;
}
