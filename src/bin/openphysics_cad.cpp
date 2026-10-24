// openphysics_cad.cpp - printed-part geometry in a program of its own (GOALS.md G23, docs/cad.md): Open CASCADE and
// Manifold never run inside the app or openphysics-mcp, which call this as the lab calls labrun.
//
//     build/openphysics-cad part MODEL.json OUTDIR [--deflection-mm 0.01]   a part from a feature list (src/cad/cadpart.h):
//                                                                       OUTDIR/part.step and part.stl, and its record
//     build/openphysics-cad measure PART.step                                a STEP file's solids measured, its faces named
//     build/openphysics-cad lattice REQUEST.json OUTDIR                       a part's skin and lattice core (src/cad/lattice.h):
//                                                                       OUTDIR/infilled.stl and its record
//     build/openphysics-cad cell REQUEST.json                                 one periodic cell of a core homogenised: its stiffness
//                                                                       over the solid's (src/cad/lattice.h lattice_cell)
//     build/openphysics-cad specimen REQUEST.json                             a test cube of a core between rigid platens: its apparent
//                                                                       modulus and its size factor (src/cad/specimen.h)
//     build/openphysics-cad check REQUEST.json OUTDIR                         can it be printed (src/cad/printcheck.h): overhangs,
//                                                                       support, thin walls, enclosed voids; OUTDIR/checked.stl,
//                                                                       OUTDIR/check.json and its record
//
// The record is one JSON object on stdout ({"ok": true, ...} or {"ok": false, "error": {...}}); the exit code is 0
// when the record says ok, 1 when it says why not, 2 for a command line that is not understood.
#include "../cad/cadpart.h"
#include "../cad/lattice.h"
#include "../cad/printcheck.h"

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

static int emit(JsonValue *rec) {
    size_t n = 0;
    char *text = json_dump(rec, 0, &n, nullptr);
    bool ok = json_get_bool(rec, "ok", false);
    if (text) std::fwrite(text, 1, n, stdout), std::fputc('\n', stdout);
    std::free(text);
    json_free(rec);
    return ok ? 0 : 1;
}

static int usage() {
    std::fprintf(stderr, "usage: openphysics-cad part MODEL.json OUTDIR [--deflection-mm D] | openphysics-cad measure PART.step | "
                         "openphysics-cad lattice REQUEST.json OUTDIR | openphysics-cad check REQUEST.json OUTDIR | openphysics-cad cell REQUEST.json | openphysics-cad specimen REQUEST.json\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    /* Open CASCADE's own messages (STEP transfer statistics) would mix with the record on stdout */
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
    if (!std::strcmp(argv[1], "part")) {
        if (argc < 4) return usage();
        double deflection = 0.01;
        for (int i = 4; i + 1 < argc; i++)
            if (!std::strcmp(argv[i], "--deflection-mm")) deflection = std::atof(argv[i + 1]);
        JsonError je;
        JsonValue *model = json_read_file(argv[2], 16u << 20, &je);
        if (!model) {
            JsonValue *r = json_object(), *e;
            json_set_bool(r, "ok", false);
            e = json_set_object(r, "error");
            char msg[300];
            std::snprintf(msg, sizeof msg, "%s: line %d: %s", argv[2], je.line, je.message);
            json_set_string(e, "message", msg);
            return emit(r);
        }
        struct stat st;
        if (stat(argv[3], &st) != 0 || !S_ISDIR(st.st_mode)) {
            json_free(model);
            JsonValue *r = json_object(), *e;
            json_set_bool(r, "ok", false);
            e = json_set_object(r, "error");
            json_set_string(e, "message", "OUTDIR must be an existing folder");
            return emit(r);
        }
        JsonValue *rec = cadpart_build(model, argv[3], deflection);
        json_free(model);
        return emit(rec);
    }
    if (!std::strcmp(argv[1], "measure")) return emit(cadpart_measure_step(argv[2]));
    if (!std::strcmp(argv[1], "cell") || !std::strcmp(argv[1], "specimen")) {
        JsonError je;
        JsonValue *req = json_read_file(argv[2], 1u << 20, &je);
        if (!req) {
            JsonValue *r = json_object(), *e;
            json_set_bool(r, "ok", false);
            e = json_set_object(r, "error");
            char msg[300];
            std::snprintf(msg, sizeof msg, "%s: line %d: %s", argv[2], je.line, je.message);
            json_set_string(e, "message", msg);
            return emit(r);
        }
        JsonValue *rec = !std::strcmp(argv[1], "specimen") ? lattice_specimen(req) : lattice_cell(req);
        json_free(req);
        return emit(rec);
    }
    if (!std::strcmp(argv[1], "lattice") || !std::strcmp(argv[1], "check")) {
        if (argc < 4) return usage();
        JsonError je;
        JsonValue *req = json_read_file(argv[2], 1u << 20, &je);
        struct stat st;
        if (!req || stat(argv[3], &st) != 0 || !S_ISDIR(st.st_mode)) {
            JsonValue *r = json_object(), *e;
            json_set_bool(r, "ok", false);
            e = json_set_object(r, "error");
            char msg[300];
            if (!req) std::snprintf(msg, sizeof msg, "%s: line %d: %s", argv[2], je.line, je.message);
            else std::snprintf(msg, sizeof msg, "OUTDIR must be an existing folder");
            json_set_string(e, "message", msg);
            json_free(req);
            return emit(r);
        }
        JsonValue *rec = !std::strcmp(argv[1], "check") ? printcheck_run(req, argv[3]) : lattice_build(req, argv[3]);
        json_free(req);
        return emit(rec);
    }
    return usage();
}
