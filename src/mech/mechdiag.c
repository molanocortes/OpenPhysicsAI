/* mechdiag.c - diagnostics list for the mechanics layer */
#include "mechdiag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mdiag_init(MechDiag *d) { memset(d, 0, sizeof *d); }

void mdiag_free(MechDiag *d) {
    free(d->msgs);
    memset(d, 0, sizeof *d);
}

void mdiag_clear(MechDiag *d) {
    d->n = d->nerrors = d->nwarnings = d->nmissing = 0;
}

void mdiag_add(MechDiag *d, MechSeverity sev, const char *code, const char *subject, const char *hint, const char *fmt, ...) {
    if (!d) return;
    if (d->n == d->cap) {
        int nc = d->cap ? 2 * d->cap : 16;
        MechMessage *m = realloc(d->msgs, (size_t)nc * sizeof *m);
        if (!m) return;
        d->msgs = m, d->cap = nc;
    }
    MechMessage *m = &d->msgs[d->n++];
    memset(m, 0, sizeof *m);
    m->severity = sev;
    snprintf(m->code, sizeof m->code, "%s", code ? code : "");
    snprintf(m->subject, sizeof m->subject, "%s", subject ? subject : "");
    snprintf(m->hint, sizeof m->hint, "%s", hint ? hint : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->message, sizeof m->message, fmt, ap);
    va_end(ap);
    if (sev == MD_ERROR) d->nerrors++;
    if (sev == MD_WARNING) d->nwarnings++;
    if (sev == MD_MISSING_INPUT) d->nmissing++;
}

bool mdiag_has(const MechDiag *d, const char *code) {
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->msgs[i].code, code)) return true;
    return false;
}

const char *mdiag_severity_name(MechSeverity s) {
    static const char *const N[] = {"info", "warning", "error", "missing_input"};
    return (unsigned)s < 4 ? N[s] : "?";
}

JsonValue *mdiag_json(const MechDiag *d) {
    JsonValue *a = json_array();
    for (int i = 0; i < d->n; i++) {
        const MechMessage *m = &d->msgs[i];
        JsonValue *o = json_object();
        json_set_string(o, "severity", mdiag_severity_name(m->severity));
        json_set_string(o, "code", m->code);
        if (m->subject[0]) json_set_string(o, "subject", m->subject);
        json_set_string(o, "message", m->message);
        if (m->hint[0]) json_set_string(o, "hint", m->hint);
        json_push(a, o);
    }
    return a;
}

void mdiag_print(const MechDiag *d, const char *prefix) {
    for (int i = 0; i < d->n; i++) {
        const MechMessage *m = &d->msgs[i];
        fprintf(stderr, "%s%s %s%s%s: %s%s%s\n", prefix ? prefix : "", mdiag_severity_name(m->severity), m->code, m->subject[0] ? " " : "",
                m->subject, m->message, m->hint[0] ? " | hint: " : "", m->hint);
    }
}
