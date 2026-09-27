/* Presentation reconstruction, not a fluid solver. See docs/lab/README.md. */
#pragma once
#include "labio.h"
/* Wendland coverage from reference particle volumes, scalar kernel average.
 * Returns one regular grid owned by out; release with lab_frame_free. */
bool labwater_volume(const LabFrame *frame,double spacing,const char *field,LabFrame *out);
