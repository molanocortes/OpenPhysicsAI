/* study_storage.c - storage estimates and free-space checks of a comparison study
 *
 * The estimate is deliberately simple and conservative: a voxel analysis record (results.nvr, spec.json, summary.json)
 * takes about 700 bytes per element plus 80 kB. results.nvr stores per node coordinates, supports, loads, displacement,
 * reaction and averaged stress (190 bytes), per element connectivity and eight integration-point stress tensors
 * (440 bytes); voxel meshes of compact parts have 1.1 to 1.3 nodes per element. Measured on rc1 runs: 26.2 MB for
 * 40,960 elements (estimate 28.8 MB), 4.4 MB for 6,600 elements (estimate 4.7 MB). */
#include "study_internal.h"

#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

enum { STORAGE_FIXED_BYTES = 2 * 1024 * 1024 };

double study_run_bytes(double elements) { return 700.0 * (elements > 0 ? elements : 0) + 80e3; }

double study_storage_reserve_bytes(void) { return 256.0 * 1024 * 1024; }

double study_free_bytes(const char *path, char *measured_at, size_t cap) {
    char p[NV_PATH_MAX];
    snprintf(p, sizeof p, "%s", path && path[0] ? path : "/");
    for (;;) {
        struct statvfs vfs;
        if (statvfs(p, &vfs) == 0) {
            if (measured_at) snprintf(measured_at, cap, "%s", p);
            return (double)vfs.f_bavail * (double)vfs.f_frsize;
        }
        char *slash = strrchr(p, '/');
        if (!slash) return -1;
        if (slash == p) {
            if (!p[1]) return -1;
            p[1] = 0;
        } else {
            *slash = 0;
        }
    }
}

JsonValue *study_storage_plan(const JsonValue *res, const double *volume_mm3, int nd, const char *dir) {
    const JsonValue *sizes = json_get(json_get(res, "refinement"), "element_sizes_mm"), *sens = json_get(res, "sensitivity");
    const char *retain = json_get_str(res, "retain_results", "refinement");
    long long maxe = json_get_int(json_get(res, "limits"), "max_elements", 400000);
    int nl = (int)json_len(sizes);
    int nsens = (int)json_len(json_get(sens, "poisson_ratio")) + (int)json_len(json_get(sens, "mounting_alternatives")) +
                (json_len(json_get(sens, "youngs_modulus_relative")) == 2) + json_get_bool(sens, "self_weight", false);
    bool keep_levels = strcmp(retain, "none") != 0, keep_sens = !strcmp(retain, "all");
    double retained = STORAGE_FIXED_BYTES, largest = 0;
    JsonValue *o = json_object(), *per = json_object();
    for (int i = 0; i < nd; i++) {
        double finest = 0, sens_h = NAN;
        for (int li = 0; li < nl; li++) {
            double h = json_at(sizes, (size_t)li)->u.number, el = volume_mm3[i] / (h * h * h);
            if (!(el <= (double)maxe)) break; /* that level and finer ones will not run */
            double b = study_run_bytes(el);
            if (keep_levels) retained += b;
            largest = fmax(largest, b);
            finest = b, sens_h = h;
        }
        if (nsens > 0 && isfinite(sens_h)) {
            double b = study_run_bytes(volume_mm3[i] / (sens_h * sens_h * sens_h));
            if (keep_sens) retained += nsens * b;
            largest = fmax(largest, b);
        }
        const JsonValue *g = json_get(json_at(json_get(res, "designs"), (size_t)i), "geometry");
        struct stat st;
        if (stat(json_get_str(g, "path", ""), &st) == 0) retained += (double)st.st_size;
        json_set_number(per, json_get_str(json_at(json_get(res, "designs"), (size_t)i), "name", "design"), finest);
    }
    /* a field that is not retained still exists until its quantities and hash are recorded */
    double peak = retained + (keep_sens ? 0 : largest), reserve = study_storage_reserve_bytes();
    char at[NV_PATH_MAX] = "";
    double free_b = study_free_bytes(dir, at, sizeof at);
    json_set_string(o, "retain_results", retain);
    json_set(o, "estimated_bytes_per_analysis_at_finest_level", per);
    json_set_number(o, "estimated_retained_bytes", retained);
    json_set_number(o, "estimated_peak_bytes", peak);
    json_set_number(o, "reserve_bytes", reserve);
    if (free_b >= 0) {
        json_set_number(o, "free_bytes", free_b);
        json_set_string(o, "free_space_measured_at", at);
        json_set_bool(o, "sufficient", free_b - reserve >= peak);
    } else {
        json_set(o, "free_bytes", json_null());
        json_set_string(o, "free_space_measured_at", "unknown: the file system could not be queried");
        json_set_bool(o, "sufficient", true);
    }
    json_set_string(o, "basis",
                    "about 700 bytes per element plus 80 kB per analysis (results.nvr, spec.json, summary.json), measured on voxel meshes; "
                    "2 MB for the record and the design projects plus the geometry copies; the peak adds the largest field that is removed "
                    "after its quantities and hash are recorded; a reserve of 256 MB is kept free");
    json_set_string(o, "retention_options",
                    "retain_results: refinement (default) keeps the field of every refinement level; all also keeps the sensitivity runs; none keeps "
                    "quantities, checks and SHA-256 of every field but no field (study_replay regenerates them)");
    return o;
}
