/* gas_scenario.h - a gas run read from JSON: the solver spec plus what to run and what to write. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../../core/json.h"
#include "gas.h"

typedef struct GasScenario {
    GasSpec spec;       /* bodies' points owned here */
    char title[160];
    double end_time;    /* s */
    int frames;         /* written at equal intervals, the first at t = 0 */
    char fields[160];
} GasScenario;

bool gas_scenario_parse(const JsonValue *root, GasScenario *sc, char *err, size_t errlen);
void gas_scenario_free(GasScenario *sc);
