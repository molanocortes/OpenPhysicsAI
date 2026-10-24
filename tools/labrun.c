/* labrun.c - run a physics-lab scenario from its JSON description and write the result (src/lab/labio.h).
 *
 *   build/labrun SCENARIO.json OUT.lab [--quiet]
 *
 * The scenario's "domain" key picks the solver. Progress goes to stderr; the last line on stdout is a JSON record of
 * the run (scenario hash, frames, steps, wall time, the solver's own diagnostics), which run records and flags keep. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core/json.h"
#include "../src/core/sha256.h"
#include "../src/lab/labio.h"
#include "../src/lab/lab_domains.h"

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: labrun SCENARIO.json OUT.lab [--quiet]\n");
        return 2;
    }
    bool quiet = argc > 3 && !strcmp(argv[3], "--quiet");
    JsonError je;
    JsonValue *root = json_read_file(argv[1], 16u << 20, &je);
    if (!root) {
        fprintf(stderr, "labrun: %s: line %d: %s\n", argv[1], je.line, je.message);
        return 1;
    }
    /* the scenario's identity: SHA-256 of its canonical form */
    size_t len;
    char *canon = json_dump(root, JSON_SORTED, &len, NULL);
    char hex[65] = {0};
    sha256_hex_of(canon, len, hex);
    free(canon);
    const char *domain = json_get_str(root, "domain", "");
    LabRunInfo info = {0};
    char err[512] = {0};
    double t0 = wall();
    bool ok = lab_run_domain(domain, root, argv[2], quiet, &info, err, sizeof err);
    double secs = wall() - t0;
    if (!ok) {
        fprintf(stderr, "labrun: %s\n", err);
        json_free(root);
        return 1;
    }
    printf("{\"scenario\":\"%s\",\"scenario_sha256\":\"%s\",\"domain\":\"%s\",\"result\":\"%s\",\"frames\":%d,\"steps\":%ld,\"wall_s\":%.2f,"
           "\"diagnostics\":%s}\n",
           argv[1], hex, domain, argv[2], info.frames, info.steps, secs, info.diagnostics[0] ? info.diagnostics : "{}");
    json_free(root);
    return 0;
}
