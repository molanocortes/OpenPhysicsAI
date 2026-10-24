/* infill.h - the infill core of a printed part (GOALS.md G17 step 3)
 *
 * A printed part is solid walls around an infill core. The core is a lattice of printed roads; homogenised, it is an
 * orthotropic solid whose constants and strengths follow from the infill pattern, its solid fraction and the printed
 * material the roads are made of. The model and its source are stated in infill.c and repeated in every result. The
 * material axes are the part's: axis 3 the build direction, axis 1 the raster reference in the layer plane. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "ortho.h"

int infill_pattern_from_name(const char *name); /* -1 when the pattern is not modelled */
const char *infill_pattern_name(int pattern);
const char *infill_pattern_list(void);           /* the modelled patterns, for an error message */

/* the core of an infill of solid fraction rho (0 < rho <= 1) printed in the material with constants k and, when
 * has_strength, strengths st: its constants (and strengths) in the same material axes; rules receives the formulas used
 * and their source, one string each, which every result repeats. At rho = 1 the core is the solid material. False with
 * a reason outside the model's range: a pattern not modelled, or strengths asked for below 10 %. */
bool infill_core(int pattern, double rho, const OrthoConstants *k, const OrthoStrength *st, bool has_strength, OrthoConstants *core,
                 OrthoStrength *core_st, JsonValue *rules, char *err, size_t errlen);
