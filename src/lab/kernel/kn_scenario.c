/* kn_scenario.c - the lab's door to the physics kernel (kernel.cpp, G24): a scenario that composes its own physics from
 * the kernel's library (docs/lab/kernel.md). The kernel is built on MFEM and solves with the sparse direct solvers of
 * Apple's Accelerate framework, so it links only on macOS where MFEM is built (src/lab/lab.mk); elsewhere this says
 * what to build, or, on other systems, that it needs macOS for now. */
#include <stdio.h>

#include "../../core/json.h"
#include "../lab_domains.h"

#if OPENPHYSICS_HAS_MFEM
bool kn_run(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
#endif

bool lab_run_kernel(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
#if OPENPHYSICS_HAS_MFEM
    return kn_run(root, out, quiet, info, err, errlen);
#else
    (void)root, (void)out, (void)quiet, (void)info;
#ifdef __APPLE__
    snprintf(err, errlen, "this build has no physics kernel: build MFEM first (make mfem), then rebuild");
#else
    snprintf(err, errlen, "the physics kernel needs macOS for now: it solves with MFEM and the sparse direct solvers of Apple's "
                          "Accelerate framework, which this system does not have (docs/lab/kernel.md)");
#endif
    return false;
#endif
}
