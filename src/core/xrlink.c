/* xrlink.c - see xrlink.h */
#include "xrlink.h"
#include "paths.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool xrlink_dir(char *out, size_t cap) {
    const char *dir = op_getenv("XR_DIR");
    if (dir && dir[0]) return (size_t)snprintf(out, cap, "%s", dir) < cap;
    if (!op_state_dir(out, cap, "run/xr")) return false;
    char old[1024]; /* a headset folder kept at the place from before the rename, while the new one does not exist */
    if (!path_exists(out) && op_legacy_state_path(old, sizeof old, "run/xr") && strlen(old) < cap) snprintf(out, cap, "%s", old);
    return true;
}

static bool word_in(const char *w, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(w, *list)) return true;
    return false;
}
static bool is_int(const char *w) {
    if (*w == '-') w++;
    if (!*w) return false;
    for (; *w; w++)
        if (!isdigit((unsigned char)*w)) return false;
    return true;
}
/* a plain decimal number (digits, point, exponent, sign; no hex, no inf or nan, nothing after it) */
static bool is_number(const char *w, double *x) {
    if (!*w || strspn(w, "0123456789.eE+-") != strlen(w)) return false;
    char *end;
    *x = strtod(w, &end);
    return end != w && !*end;
}
static bool is_fraction(const char *w) {
    double x;
    return is_number(w, &x) && x >= 0 && x <= 1;
}
static bool is_positive(const char *w) {
    double x;
    return is_number(w, &x) && x > 0;
}
static bool is_name(const char *w) { /* a field of a lab result: letters, digits and _ . + - */
    size_t n = strlen(w);
    if (!n || n > 47) return false;
    for (; *w; w++)
        if (!isalnum((unsigned char)*w) && !strchr("_.+-", *w)) return false;
    return true;
}
static bool is_lower_name(const char *w) { /* a finite-element field: von_mises, temperature, a new one tomorrow */
    size_t n = strlen(w);
    if (!n || n > 40) return false;
    for (; *w; w++)
        if (!islower((unsigned char)*w) && *w != '_') return false;
    return true;
}

bool xrlink_command_allowed(const char *line) {
    static const char *const LAB1[] = {"play", "pause", "next", "prev", NULL}, *const FEM1[] = {"play", "pause", NULL};
    static const char *const OFFFLIP[] = {"off", "flip", NULL}, *const STEPS[] = {"next", "prev", "last", "first", NULL};
    static const char *const DEF[] = {"auto", "true", NULL}, *const AXES[] = {"x", "y", "z", NULL};
    char w[5][64];
    if (!line || strlen(line) > 120) return false;
    for (const char *c = line; *c; c++)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) return false;
    int n = sscanf(line, "%63s %63s %63s %63s %63s", w[0], w[1], w[2], w[3], w[4]);
    if (n < 2 || n > 4) return false;
    if (!strcmp(w[0], "lab")) {
        if (n == 2) return word_in(w[1], LAB1);
        if (!strcmp(w[1], "frame")) return n == 3 && is_int(w[2]);
        if (!strcmp(w[1], "field")) return n == 3 && is_name(w[2]);
        if (!strcmp(w[1], "range")) return n == 3 && !strcmp(w[2], "auto");
        if (!strcmp(w[1], "section")) return (n == 3 && word_in(w[2], OFFFLIP)) || (n == 4 && word_in(w[2], AXES) && is_fraction(w[3]));
        return false;
    }
    if (!strcmp(w[0], "fem")) {
        if (n == 2) return word_in(w[1], FEM1);
        if (!strcmp(w[1], "step")) return n == 3 && (is_int(w[2]) || word_in(w[2], STEPS));
        if (!strcmp(w[1], "field")) return n == 3 && is_lower_name(w[2]); /* the app ignores a field it does not have */
        if (!strcmp(w[1], "deform")) return n == 3 && (word_in(w[2], DEF) || is_positive(w[2]));
        if (!strcmp(w[1], "section")) return (n == 3 && word_in(w[2], OFFFLIP)) || (n == 4 && word_in(w[2], AXES) && is_fraction(w[3]));
        return false;
    }
    return false;
}
