/* mechdiag.h - diagnostics collected while importing, compiling and running mechanical models.
 *
 * Every problem is reported with a stable code, the subject it concerns (body, joint, file path, field), a message and a
 * hint. Severity "missing_input" marks information that materially changes results and must be supplied (or explicitly
 * assumed with provenance) by the user; it is never filled in silently. */
#pragma once

#include <stdarg.h>
#include <stdbool.h>

#include "../core/json.h"

typedef enum { MD_INFO = 0, MD_WARNING, MD_ERROR, MD_MISSING_INPUT } MechSeverity;

typedef struct MechMessage {
    MechSeverity severity;
    char code[48];     /* e.g. "LOOP_REDUNDANT", "MESH_NOT_CLOSED" */
    char subject[128]; /* body/joint name or document path such as "joints[2].axis" */
    char message[512];
    char hint[256];
} MechMessage;

typedef struct MechDiag {
    MechMessage *msgs;
    int n, cap;
    int nerrors, nwarnings, nmissing;
} MechDiag;

void mdiag_init(MechDiag *d);
void mdiag_free(MechDiag *d);
void mdiag_clear(MechDiag *d);
void mdiag_add(MechDiag *d, MechSeverity sev, const char *code, const char *subject, const char *hint, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
bool mdiag_has(const MechDiag *d, const char *code);
const char *mdiag_severity_name(MechSeverity s);
JsonValue *mdiag_json(const MechDiag *d);
void mdiag_print(const MechDiag *d, const char *prefix); /* stderr, for tools and tests */
