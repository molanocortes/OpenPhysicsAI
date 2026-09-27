/* lab_domains.h - one entry point for every physics-lab solver: run a scenario (parsed JSON) to a result file.
 * The command-line runner (tools/labrun.c), the MCP operation and the app all come through here. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"

typedef struct LabRunInfo {
    int frames;
    long steps;
    char diagnostics[2048]; /* a JSON object the solver fills: its own measured quantities */
} LabRunInfo;

/* the domains this build knows, comma separated */
const char *lab_domain_list(void);
bool lab_run_domain(const char *domain, const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err,
                    size_t errlen);

/* the domains' own entry points */
bool lab_run_acoustic(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_impact(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_orbit(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_em(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_flow(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_heat(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_fracture(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_fire(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_battery(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_melt(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_sheet(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_magnet(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_water(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_relativity(const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err, size_t errlen);
