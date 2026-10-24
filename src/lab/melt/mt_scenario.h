/* mt_scenario.h - the melt domain's scenario (docs/lab/melt.md) read into a MeltSpec, for labrun and for tools that
 * run the solver themselves (tools/g20melt.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../core/json.h"
#include "melt.h"

/* the metal, the block, the beam and its tracks, the flow; end_s the run's end. False with the reason in err. */
bool melt_scenario_spec(const JsonValue *root, MeltSpec *s, double *end_s, char *err, size_t errlen);
