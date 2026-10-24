/* openphysics_mcp.c - MCP server for OpenPhysicsAI over stdio (newline-delimited JSON-RPC)
 *
 * Modes
 *   --connect [PATH]  bridge to a running openphysics-server (or GUI) on a control socket: projects and jobs outlive the
 *                     AI client and can be watched in the viewer
 *   --embedded        run the engine inside this process
 *   (default)         connect to ~/.openphysicsai/run/control.sock when a server answers there, otherwise embedded
 * stdout carries only MCP messages: file descriptor 1 is redirected to stderr at start-up, so no library output can
 * corrupt the protocol stream. Logs go to stderr. */
#include "../common.h"
#include "../ctl/engine.h"
#include "../ctl/ops.h"
#include "../net/ctlclient.h"
#include "../net/ctlserver.h"
#include "../net/mcp.h"
#include "../net/netutil.h"
#include "../core/base64.h"
#include "../core/xrlink.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
    Engine *engine;
} Embedded;

static JsonValue *embedded_list(void *ctx, char *err, size_t errlen) {
    JsonValue *arr = json_array();
    for (int i = 0; i < ops_count(); i++) {
        const OpInfo *op = ops_at(i);
        JsonValue *o = json_object();
        json_set_string(o, "name", op->name);
        json_set_string(o, "title", op->title);
        json_set_string(o, "description", op->description);
        json_set_string(o, "kind", op->mutating ? "mutation" : "query");
        json_set_bool(o, "idempotent", op->idempotent);
        json_set_bool(o, "destructive", op->destructive);
        json_set(o, "input_schema", ops_input_schema(op));
        json_push(arr, o);
    }
    return arr;
}

static JsonValue *embedded_call(void *ctx, const char *name, const JsonValue *args, const char *client, char *err, size_t errlen) {
    Embedded *m = ctx;
    OpResult r;
    OpCaller caller = {"mcp", client};
    ops_invoke(m->engine, name, args, &caller, &r);
    JsonValue *res = op_result_json(&r, true);
    op_result_free(&r);
    return res;
}

typedef struct {
    char path[512];
    CtlClient *client;
} Bridge;

static CtlClient *bridge_connect(Bridge *b, char *err, size_t errlen) {
    if (!b->client) b->client = ctl_connect_unix(b->path, err, errlen);
    return b->client;
}

static JsonValue *bridge_list(void *ctx, char *err, size_t errlen) {
    Bridge *b = ctx;
    for (int attempt = 0; attempt < 2; attempt++) {
        CtlClient *c = bridge_connect(b, err, errlen);
        if (!c) return NULL;
        JsonValue *resp = ctl_request(c, "ops.list", NULL, 60000, err, errlen);
        if (!resp) { /* listing is read-only, so one reconnect attempt is safe */
            ctl_close(b->client);
            b->client = NULL;
            continue;
        }
        JsonValue *ops = json_take(json_get(resp, "result"), "operations");
        if (!ops) snprintf(err, errlen, "unexpected ops.list response from the server");
        json_free(resp);
        return ops;
    }
    return NULL;
}

static JsonValue *bridge_call(void *ctx, const char *name, const JsonValue *args, const char *client, char *err, size_t errlen) {
    Bridge *b = ctx;
    CtlClient *c = bridge_connect(b, err, errlen);
    if (!c) return NULL;
    /* No automatic retry: if the connection drops mid-call the outcome is unknown. The client may retry with an
     * idempotency_key, which the server deduplicates. */
    JsonValue *resp = ctl_request(c, name, args, -1, err, errlen);
    if (!resp) {
        ctl_close(b->client);
        b->client = NULL;
        return NULL;
    }
    JsonValue *result = json_take(resp, "result");
    if (result) {
        json_free(resp);
        return result;
    }
    const JsonValue *e = json_get(resp, "error"), *d = json_get(e, "data");
    JsonValue *out = json_object();
    json_set_bool(out, "ok", false);
    json_set_number(out, "revision", json_get_num(d, "revision", 0));
    JsonValue *eo = json_set_object(out, "error");
    json_set_string(eo, "code", json_get_str(d, "error_code", "INTERNAL_ERROR"));
    json_set_string(eo, "message", json_get_str(e, "message", "request failed"));
    const char *hint = json_get_str(d, "hint", NULL);
    if (hint) json_set_string(eo, "hint", hint);
    const JsonValue *details = json_get(d, "details");
    if (details) json_set(eo, "details", json_clone(details));
    json_free(resp);
    return out;
}

/* ---- the physics lab and approvals, served by this program itself (not by the engine) ----
 * With --lab the agent lists, reads, changes and runs lab scenarios through these tools alone, so changing a
 * simulation never needs a shell or file permission in the AI tool. With --approvals, Claude Code's permission prompts
 * (--permission-prompt-tool mcp__openphysics__approve) are answered by the user in the app's window: a request file is
 * written there and the answer file the window writes back is awaited. */
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

typedef struct {
    McpBackend inner;
    char lab[1024], results[1024], runs[1100], approvals[1024];
    JsonValue *tools;
    bool lean; /* --lean: the engine's operations behind engine_ops, engine_op and engine_call, not each listed */
} Local;

/* --lean, for a spoken conversation: the engine's 75 operations are about 60 000 tokens of definitions that every
 * question would carry (10 s before the first word on 2026-10-07); behind these three they cost a call when needed */
static const char *const LEAN_TOOLS[][4] = {
    {"engine_ops", "The engine's operations",
     "Every operation of the engineering engine (projects, geometry, materials, loads, meshes, analyses, studies, printing, "
     "mechanics): its name, kind and what it does, in one line each. match narrows them by words in the name or the line. "
     "engine_op gives one operation's input, engine_call runs it.",
     "{\"type\":\"object\",\"properties\":{\"match\":{\"type\":\"string\"}},\"additionalProperties\":false}"},
    {"engine_op", "One engine operation",
     "One operation of the engine in full: what it does, and the JSON schema of the input engine_call passes to it.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"],\"additionalProperties\":false}"},
    {"engine_call", "Run an engine operation",
     "Runs one engine operation with its input (as engine_op describes it). Its result, its errors and its images are the "
     "operation's own, exactly as if it had been called by name.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},"
     "\"required\":[\"name\"],\"additionalProperties\":false}"},
};
enum { NLEAN = sizeof LEAN_TOOLS / sizeof LEAN_TOOLS[0] };

static const char *const LOCAL_TOOLS[][4] = {
    {"lab_scenarios", "Lab scenarios",
     "The physics lab's scenarios (name, title, domain) and the ones made in this lab before. Start here to make or change "
     "a simulation: read one with lab_scenario_get, change its keys, run your version with lab_run.",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"lab_scenario_get", "Read a lab scenario",
     "One scenario as JSON, with the documentation of every key its solver takes (units are in the key names) and of its "
     "limits. A version made here before is returned in preference to the example of the same name.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"a name from lab_scenarios\"}},"
     "\"required\":[\"name\"],\"additionalProperties\":false}"},
    {"lab_run", "Run a lab scenario",
     "Saves the scenario under a new name in the lab folder and computes it; the app's window opens the result when it is "
     "written. Answers when the run ends (with the solver's record: frames, steps, wall time and its own diagnostics) or "
     "after wait_seconds, while a longer run goes on (follow it with lab_run_status). Keep runs small first: a coarse "
     "grid and a short time show whether the setup is right in seconds.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"pattern\":\"^[a-z0-9][a-z0-9_-]{0,63}$\","
     "\"description\":\"a new name (lowercase, digits, _ and -), not one of the examples, so the gallery keeps its own\"},"
     "\"scenario\":{\"type\":\"object\",\"description\":\"the whole scenario, keys as lab_scenario_get documents them\"},"
     "\"wait_seconds\":{\"type\":\"number\",\"minimum\":0,\"maximum\":600,\"description\":\"default 120\"}},"
     "\"required\":[\"name\",\"scenario\"],\"additionalProperties\":false}"},
    {"lab_run_status", "Follow a lab run",
     "Whether a run started with lab_run is still computing, and when it has ended its record and the end of its log.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"wait_seconds\":{\"type\":\"number\",\"minimum\":0,"
     "\"maximum\":600}},\"required\":[\"name\"],\"additionalProperties\":false}"},
    {"lab_change", "Change a simulation and run it",
     "The quickest way to change a simulation: the scenario of a result (base: its name, as the headset's question or "
     "lab_scenarios gives it) with only the values you name changed, each by its path ('stream.speed_m_s', "
     "'bodies.0.radius_m', 'run.end_s'); everything else stays as it was. A path must already be in the scenario and keep "
     "its kind of value (a number stays a number, a list of numbers keeps its length); one that is not there is refused "
     "with the paths that are (lab_scenario_get and lab_run add keys). The title notes the change unless you set 'title', "
     "and the source beside a changed value records it. Saved as BASE_v2, _v3... (or name) and computed as lab_run does: "
     "the window and the headset open the result when it is written. Answers after wait_seconds (default 2, so that a "
     "value the solver refuses is reported) while a longer run goes on (lab_run_status follows it).",
     "{\"type\":\"object\",\"properties\":{\"base\":{\"type\":\"string\",\"description\":\"the result or scenario to "
     "change\"},\"set\":{\"type\":\"object\",\"description\":\"path: new value, one to 24 of them\"},\"name\":{\"type\":"
     "\"string\",\"pattern\":\"^[a-z0-9][a-z0-9_-]{0,63}$\"},\"wait_seconds\":{\"type\":\"number\",\"minimum\":0,"
     "\"maximum\":600}},\"required\":[\"base\",\"set\"],\"additionalProperties\":false}"},
    {"headset_status", "What the headset shows",
     "What the person sees in the Meta Quest (docs/xr.md), when the headset link runs: the result the app shows (a "
     "finite-element part or a lab simulation, its field, unit, range, frame or stored time, section), whether the "
     "headset page is connected, the points the person pinned there (each with its part, its place in the result's own "
     "coordinates, a part's build frame in mm or a lab result's axes in m, and the value read there), the marks "
     "placed with headset_mark, and how the headset runs (AR, VR or a screen, its frame rate, the page's errors).",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"headset_view", "Change what the headset shows",
     "Changes the view in the app and the headset together: playback, a frame, a stored time, a field, a cut. "
     "Commands: 'lab play', 'lab pause', 'lab next', 'lab prev', 'lab frame N' (from 0), 'lab field NAME', "
     "'lab section x|y|z FRACTION' (0 to 1), 'lab section off|flip', 'lab range auto'; for a finite-element part "
     "'fem play', 'fem pause', 'fem step N' (from 1) or 'fem step next|prev|first|last', 'fem field NAME', "
     "'fem deform auto|true|SCALE', 'fem section x|y|z FRACTION', 'fem section off|flip'. Anything else is refused.",
     "{\"type\":\"object\",\"properties\":{\"commands\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
     "\"minItems\":1,\"maxItems\":12}},\"required\":[\"commands\"],\"additionalProperties\":false}"},
    {"headset_mark", "Mark places in the headset",
     "Shows labelled pins on the result in the headset, at places in the result's own coordinates (a part's build "
     "frame in mm, a lab result's axes in m: the coordinates headset_status gives for the person's pins): to show where "
     "a peak is, where to look, what you mean. The marks replace the ones placed before; an empty list removes them. "
     "They stay with the result they were made for.",
     "{\"type\":\"object\",\"properties\":{\"marks\":{\"type\":\"array\",\"maxItems\":12,\"items\":{"
     "\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\",\"maxLength\":80},\"at\":{\"type\":"
     "\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3}},\"required\":[\"label\",\"at\"],"
     "\"additionalProperties\":false}}},\"required\":[\"marks\"],\"additionalProperties\":false}"},
    {"headset_look", "See what the person sees",
     "A picture (JPEG, 960 x 720) of what the person sees in the headset: the result, the card with its field and "
     "legend, their pointers, pins and marks, drawn from between their eyes with a 70 degree view (on a computer screen, "
     "the window's view). The room is never in it: where passthrough shows the room the picture has the studio's colour. "
     "moment 'now' (the default) takes a new picture, which needs the headset page open; 'question' is the picture "
     "taken as the person asked their last question, of what they were looking at then.",
     "{\"type\":\"object\",\"properties\":{\"moment\":{\"type\":\"string\",\"enum\":[\"now\",\"question\"]}},"
     "\"additionalProperties\":false}"},
    {"setup_confirm", "Show the setup to the person and ask them to confirm it",
     "Before analysis_run on a part the person described in their own words (\"it holds a 5 kg bag from this hook\", \"I press "
     "here with my thumb\"): make the supports and loads of their words with selection_create and boundary_apply (the faces "
     "they pointed at are named in the question), then call this with the setup in plain words: where the part is held, "
     "what pushes or pulls where and which way, and how much, in kg and in N. The window colours the held faces and the "
     "loaded ones and asks the person to confirm. Run the analysis only when confirmed is true; when it is false, ask "
     "what to change, change it and confirm again. One question at a time, in the person's words; never call a part safe.",
     "{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":600}},"
     "\"required\":[\"summary\"],\"additionalProperties\":false}"},
    {"cad_part", "Make an exact part",
     "Makes a part from a list of features in millimetres, built in order with Open CASCADE (docs/cad.md), and writes "
     "it as STEP and as a closed STL; answers with every feature's volume, area, box and validity and the part's named "
     "faces. model: {\"features\": [{\"id\", \"op\", ...}], \"result\": id}. ops: box {size_mm, at_mm (lowest "
     "corner) or centre_mm}; cylinder {radius_mm, height_mm, at_mm (base centre), axis \"z\" or \"-x\" or [x,y,z]}; "
     "cone {radius1_mm, radius2_mm, height_mm, at_mm, axis}; sphere {radius_mm, centre_mm}; sketch {plane xy|xz|yz, "
     "offset_mm, points_mm [[u,v]...] or circle {centre_mm [u,v], radius_mm}}; extrude {sketch, distance_mm}; revolve "
     "{sketch, axis, axis_point_mm, angle_deg}; union/intersect {of [ids]}; cut {from, remove [ids]}; translate {of, "
     "by_mm}; rotate {of, axis, about_mm, angle_deg}; pattern {of, count, step_mm or axis+about_mm+angle_deg}; "
     "fillet {of, edges, radius_mm}; chamfer {of, edges, distance_mm}; shell {of, open_faces, thickness_mm}. Edges and "
     "faces are chosen by selectors, never by index: {parallel: \"z\"} {between: \"planes\"} {extreme: \"+z\"} "
     "{kind: line|circle|plane|cylinder} {near_mm: [x,y,z], count: n}; all keys of one must hold, a list chooses what "
     "any chooses. A feature that fails refuses the part with its id and the reason. To analyse it: geometry_import "
     "with the STL path and units mm.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"pattern\":\"^[a-z0-9][a-z0-9_-]{0,63}$\"},"
     "\"model\":{\"type\":\"object\"},\"deflection_mm\":{\"type\":\"number\",\"exclusiveMinimum\":0,"
     "\"description\":\"the STL's chordal deviation, 0.01 by default\"}},\"required\":[\"name\",\"model\"],"
     "\"additionalProperties\":false}"},
    {"cad_lattice", "Give a part a skin and a lattice core",
     "Turns a part into what a printer makes of it with a lattice inside: a solid skin of skin_mm under its surface "
     "and a core of gyroid, schwarz_p or diamond (form sheet of wall_mm, or network) or bcc or octet struts (wall_mm "
     "is the strut's diameter), or none (hollow) or solid; or a solid_fraction instead of wall_mm, solved on one cell. "
     "One level set (Manifold), so the result is always closed and manifold; writes it as an STL and answers with the "
     "core's measured solid fraction, the skin's thinnest depth where sampled, volumes, triangles and the seconds it "
     "took (a few seconds to minutes; resolution_mm, the grid step, sets both detail and time). part: a name made by "
     "cad_part, or a STEP or STL path. A closed skin round a core leaves enclosed voids (the answer counts them), which "
     "keep powder or resin when printed: drains {radius_mm} drills one hole along each void's shortest way out, or "
     "drains [{from_mm, to_mm, radius_mm}] the holes given. To analyse it: geometry_import with the STL path and units mm.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"pattern\":\"^[a-z0-9][a-z0-9_-]{0,63}$\"},"
     "\"part\":{\"type\":\"string\"},\"skin_mm\":{\"type\":\"number\",\"minimum\":0},\"resolution_mm\":{"
     "\"type\":\"number\",\"exclusiveMinimum\":0},\"origin_mm\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},"
     "\"minItems\":3,\"maxItems\":3},\"core\":{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\","
     "\"enum\":[\"gyroid\",\"schwarz_p\",\"diamond\",\"bcc\",\"octet\",\"none\",\"solid\"]},\"form\":{\"type\":"
     "\"string\",\"enum\":[\"sheet\",\"network\"]},\"cell_mm\":{\"type\":\"number\"},\"wall_mm\":{\"type\":"
     "\"number\"},\"solid_fraction\":{\"type\":\"number\"},\"level\":{\"type\":\"number\"}},\"required\":[\"kind\"]},"
     "\"drains\":{\"anyOf\":[{\"type\":\"object\",\"properties\":{\"radius_mm\":{\"type\":\"number\",\"exclusiveMinimum\":0}},"
     "\"required\":[\"radius_mm\"]},{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from_mm\":{"
     "\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3},\"to_mm\":{\"type\":\"array\","
     "\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3},\"radius_mm\":{\"type\":\"number\",\"exclusiveMinimum\":0}},"
     "\"required\":[\"from_mm\",\"to_mm\",\"radius_mm\"]}}]}},"
     "\"required\":[\"name\",\"part\",\"core\"],\"additionalProperties\":false}"},
    {"cad_cell", "Homogenise one lattice cell",
     "The stiffness of a lattice core as a material (docs/cad.md#a-cells-stiffness): one periodic cell of core (the "
     "same core object as cad_lattice: gyroid, schwarz_p or diamond as a sheet of wall_mm, a band (|G| < level, the sheet "
     "most papers use) or a network; bcc or octet struts; or a solid_fraction) voxelised and solved by periodic finite "
     "elements, six strains. Answers with the cell's stiffness over the solid's modulus E_s (6 x 6, order xx yy zz xy yz "
     "zx), the modulus, shear modulus and Poisson's ratio along the cube axes, the Zener ratio, and the measured solid "
     "fraction. poisson_ratio is the solid's own, never guessed. voxels across the cell: 48 by default (about a minute; "
     "64 for 2 per cent, 32 for a quick look). A periodic cell is an infinite lattice: printed specimens of a few cells "
     "measure softer.",
     "{\"type\":\"object\",\"properties\":{\"core\":{\"type\":\"object\"},\"poisson_ratio\":{\"type\":\"number\","
     "\"exclusiveMinimum\":-1,\"exclusiveMaximum\":0.5},\"voxels\":{\"type\":\"integer\",\"minimum\":8,\"maximum\":96}},"
     "\"required\":[\"core\",\"poisson_ratio\"],\"additionalProperties\":false}"},
    {"cad_check", "Check that a part can be printed",
     "Checks a part for printing (docs/cad.md#printability): the downward surfaces at less than overhang_deg to the "
     "plate and the support under them, along build_direction (the part's +z when not given) and along each of the six "
     "axes, with the best named; walls thinner than min_wall_mm (the depth along the inward normal), in regions with "
     "their thinnest point; enclosed voids, which keep powder (powder_bed_fusion, binder_jetting), resin "
     "(vat_photopolymerization) or support material (material_jetting), each with its shortest way out (for "
     "cad_lattice drains or a hole cut with cad_part); in material_extrusion they print as closed cavities. process is "
     "the ISO/ASTM 52900 category; overhang_deg and min_wall_mm are the process's or the machine's own values, never "
     "guessed: ask when they are not known. part: a name made by cad_part or cad_lattice, or a STEP or STL path. "
     "Answers with issues in words, the numbers, and a coloured STL (overhang red, thin blue, voids orange).",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"pattern\":\"^[a-z0-9][a-z0-9_-]{0,63}$\"},"
     "\"part\":{\"type\":\"string\"},\"process\":{\"type\":\"string\",\"enum\":[\"powder_bed_fusion\",\"binder_jetting\","
     "\"vat_photopolymerization\",\"material_jetting\",\"material_extrusion\"]},\"overhang_deg\":{\"type\":\"number\","
     "\"exclusiveMinimum\":0,\"exclusiveMaximum\":90},\"min_wall_mm\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
     "\"build_direction\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"+x\",\"-x\",\"+y\",\"-y\",\"+z\",\"-z\"]},{\"type\":"
     "\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3}]},\"deflection_mm\":{\"type\":\"number\","
     "\"exclusiveMinimum\":0}},\"required\":[\"name\",\"part\",\"process\",\"overhang_deg\",\"min_wall_mm\"],"
     "\"additionalProperties\":false}"},
    {"approve", "Ask the user in the app",
     "Permission prompts for Claude Code (--permission-prompt-tool): the user answers Allow or Deny in the app's window.",
     "{\"type\":\"object\",\"properties\":{\"tool_name\":{\"type\":\"string\"},\"input\":{\"type\":\"object\"},"
     "\"tool_use_id\":{\"type\":\"string\"}},\"required\":[\"tool_name\"]}"},
};
enum { NLOCAL = sizeof LOCAL_TOOLS / sizeof LOCAL_TOOLS[0] };

static JsonValue *local_ok(JsonValue *value) {
    JsonValue *o = json_object();
    json_set_bool(o, "ok", true);
    json_set(o, "value", value ? value : json_object());
    return o;
}

static JsonValue *local_fail(const char *code, const char *msg, const char *hint) {
    JsonValue *o = json_object(), *e;
    json_set_bool(o, "ok", false);
    e = json_set_object(o, "error");
    json_set_string(e, "code", code);
    json_set_string(e, "message", msg);
    if (hint) json_set_string(e, "hint", hint);
    return o;
}

static char *read_text(const char *path, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

static bool lab_name_ok(const char *n) {
    size_t len = strlen(n);
    if (!len || len > 64 || !((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) return false;
    for (size_t i = 0; i < len; i++)
        if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '_' || n[i] == '-')) return false;
    return true;
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static JsonValue *lab_list(Local *L) {
    JsonValue *v = json_object(), *list = json_set_array(v, "scenarios"), *mine = json_set_array(v, "made_here");
    json_set_string(v, "results_folder", L->results);
    char dir[1200], p[1400];
    snprintf(dir, sizeof dir, "%s/examples/lab", L->lab);
    for (int pass = 0; pass < 2; pass++) {
        DIR *d = opendir(pass ? L->runs : dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
            snprintf(p, sizeof p, "%s/%s", pass ? L->runs : dir, e->d_name);
            JsonValue *j = json_read_file(p, 4u << 20, NULL), *o = json_object();
            json_set(o, "name", json_stringn(e->d_name, n - 5));
            json_set_string(o, "title", json_get_str(j, "title", ""));
            json_set_string(o, "domain", json_get_str(j, "domain", ""));
            json_push(pass ? mine : list, o);
            json_free(j);
        }
        if (d) closedir(d);
    }
    return local_ok(v);
}

static JsonValue *lab_get(Local *L, const JsonValue *args) {
    const char *name = json_get_str(args, "name", "");
    char p[1400], doc[1400];
    if (!lab_name_ok(name)) return local_fail("INVALID_ARGUMENT", "name: use a name from lab_scenarios", NULL);
    snprintf(p, sizeof p, "%s/%s.json", L->runs, name);
    if (!file_exists(p)) snprintf(p, sizeof p, "%s/examples/lab/%s.json", L->lab, name);
    JsonValue *j = json_read_file(p, 8u << 20, NULL);
    if (!j) return local_fail("NOT_FOUND", "no scenario of that name", "call lab_scenarios for the names");
    JsonValue *v = json_object();
    json_set_string(v, "name", name);
    json_set_string(v, "path", p);
    const char *domain = json_get_str(j, "domain", "");
    json_set(v, "scenario", j);
    snprintf(doc, sizeof doc, "%s/docs/lab/%s.md", L->lab, domain);
    char *text = domain[0] ? read_text(doc, 60000) : NULL;
    if (text) {
        json_set_string(v, "documentation_path", doc);
        json_set_string(v, "documentation", text);
        free(text);
    }
    snprintf(doc, sizeof doc, "%s/docs/lab/README.md", L->lab);
    json_set_string(v, "all_solvers", doc);
    return local_ok(v);
}

static pid_t run_pid(Local *L, const char *name) {
    char p[1400];
    snprintf(p, sizeof p, "%s/%s.pid", L->runs, name);
    char *t = read_text(p, 32);
    pid_t pid = t ? (pid_t)atol(t) : 0;
    free(t);
    return pid > 0 && kill(pid, 0) == 0 ? pid : 0;
}

/* waits up to `wait` seconds; then the run's state, its record (the last JSON line labrun prints) and its log's end */
static JsonValue *lab_state(Local *L, const char *name, double wait) {
    char p[1400];
    pid_t pid = run_pid(L, name);
    int status = 0;
    bool ours = false, exited = !pid;
    for (double t = 0; pid && t <= wait; t += 0.2) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { ours = exited = true; break; }
        if (w < 0 && kill(pid, 0) != 0) { exited = true; break; } /* not our child, and gone */
        if (t + 0.2 > wait) break;
        usleep(200000);
    }
    JsonValue *v = json_object();
    json_set_string(v, "name", name);
    snprintf(p, sizeof p, "%s/%s.lab", L->results, name);
    json_set_string(v, "result_path", p);
    json_set_bool(v, "running", !exited);
    if (exited && ours) json_set_number(v, "exit_code", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    snprintf(p, sizeof p, "%s/%s.log", L->runs, name);
    json_set_string(v, "log_path", p);
    char *log = read_text(p, 4u << 20);
    if (log) {
        size_t n = strlen(log);
        for (char *line = log + n; exited && line > log;) { /* the record: the last line that is a JSON object */
            char *start = line - 1;
            while (start > log && start[-1] != '\n') start--;
            if (*start == '{') {
                JsonValue *r = json_parse(start, (size_t)(line - start), NULL, NULL);
                if (r) { json_set(v, "record", r); break; }
            }
            line = start;
        }
        json_set_string(v, "log_tail", log + (n > 1500 ? n - 1500 : 0));
        free(log);
    }
    if (exited) {
        snprintf(p, sizeof p, "%s/%s.pid", L->runs, name);
        unlink(p);
    } else json_set_string(v, "note", "still computing: the window opens the result when it is written; follow with lab_run_status");
    return local_ok(v);
}

/* saves the scenario as NAME in the lab folder and starts labrun on it; the run's state after up to `wait` seconds */
static JsonValue *lab_start(Local *L, const char *name, const JsonValue *sc, double wait) {
    char p[1400], out[1400], log[1400], labrun[1100];
    if (!lab_name_ok(name)) return local_fail("INVALID_ARGUMENT", "name: lowercase letters, digits, _ and - (up to 64)", NULL);
    snprintf(p, sizeof p, "%s/examples/lab/%s.json", L->lab, name);
    if (file_exists(p))
        return local_fail("INVALID_ARGUMENT", "that is an example's name", "give your version a new name, e.g. NAME_v2, so the gallery keeps its own result");
    if (!sc || sc->type != JSON_OBJECT || !json_get_str(sc, "domain", NULL))
        return local_fail("INVALID_ARGUMENT", "scenario: a JSON object with its \"domain\"", "start from lab_scenario_get");
    if (run_pid(L, name)) return local_fail("BUSY", "a run of that name is still computing", "lab_run_status follows it");
    snprintf(labrun, sizeof labrun, "%s/build/labrun", L->lab);
    if (access(labrun, X_OK) != 0) return local_fail("UNAVAILABLE", "the lab's labrun is not built", "make lab in the repository");
    mkdir(L->results, 0755);
    mkdir(L->runs, 0755);
    snprintf(p, sizeof p, "%s/%s.json", L->runs, name);
    size_t n;
    char *text = json_dump(sc, JSON_PRETTY, &n, NULL);
    FILE *f = text ? fopen(p, "w") : NULL;
    if (f) fwrite(text, 1, n, f), fputc('\n', f), fclose(f);
    free(text);
    if (!f) return local_fail("IO_ERROR", "cannot write the scenario in the lab folder", NULL);
    snprintf(out, sizeof out, "%s/%s.lab", L->results, name);
    snprintf(log, sizeof log, "%s/%s.log", L->runs, name);
    pid_t pid = fork();
    if (pid < 0) return local_fail("IO_ERROR", "cannot start labrun", NULL);
    if (pid == 0) { /* its own session: the run goes on when the agent's answer ends */
        setsid();
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644), nul = open("/dev/null", O_RDONLY);
        if (fd >= 0) dup2(fd, 1), dup2(fd, 2);
        if (nul >= 0) dup2(nul, 0);
        for (int k = 3; k < 256; k++) close(k);
        execl(labrun, labrun, p, out, (char *)NULL);
        _exit(127);
    }
    char pp[1400], num[32];
    snprintf(pp, sizeof pp, "%s/%s.pid", L->runs, name);
    snprintf(num, sizeof num, "%d\n", (int)pid);
    f = fopen(pp, "w");
    if (f) fputs(num, f), fclose(f);
    return lab_state(L, name, wait < 0 ? 0 : wait > 600 ? 600 : wait);
}

static JsonValue *lab_run(Local *L, const JsonValue *args) {
    return lab_start(L, json_get_str(args, "name", ""), json_get(args, "scenario"), json_get_num(args, "wait_seconds", 120));
}

/* ---- lab_change: a spoken change in one step (docs/xr.md "Talking with the AI") ----
 * The scenario of the result shown (its file's header: what was computed and is on screen), else one made here, else
 * the example, with only the named values changed by their path. Every path must be there already and keep its kind of
 * value: the solvers ignore keys they do not know, so a misspelt key is refused here instead of changing nothing. */

/* the scenario in a result's header (src/lab/labio.h: "OPLAB01\n", a uint32 length, the header's JSON) */
static JsonValue *result_scenario(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char magic[8], len[4];
    JsonValue *sc = NULL;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "OPLAB01\n", 8) && fread(len, 1, 4, f) == 4) {
        uint32_t n = (uint32_t)len[0] | (uint32_t)len[1] << 8 | (uint32_t)len[2] << 16 | (uint32_t)len[3] << 24;
        char *text = n <= (16u << 20) ? malloc((size_t)n + 1) : NULL;
        if (text && fread(text, 1, n, f) == n) {
            JsonValue *h = json_parse(text, n, NULL, NULL), *s = h ? json_take(h, "scenario") : NULL;
            if (s && s->type == JSON_OBJECT) sc = s;
            else json_free(s);
            json_free(h);
        }
        free(text);
    }
    fclose(f);
    return sc;
}

/* the value at a dotted path ("bodies.0.radius_m") and the container holding it; NULL if the path is not there */
static JsonValue *path_find(JsonValue *root, const char *path, JsonValue **holder, char *last, size_t lcap) {
    JsonValue *node = root;
    for (const char *p = path; node;) {
        size_t k = strcspn(p, ".");
        if (!k || k >= lcap) return NULL;
        snprintf(last, lcap, "%.*s", (int)k, p);
        JsonValue *up = node;
        if (node->type == JSON_OBJECT) node = json_get(node, last);
        else if (node->type == JSON_ARRAY) {
            size_t i = 0;
            for (const char *c = last; *c; c++) {
                if (*c < '0' || *c > '9' || i > 100000) return NULL;
                i = i * 10 + (size_t)(*c - '0');
            }
            node = json_at(node, i);
        } else return NULL;
        if (!p[k]) return *holder = up, node;
        p += k + 1;
    }
    return NULL;
}

/* why a new value cannot take the old one's place, or NULL: the same kind, and a list of numbers keeps its length */
static const char *kind_refusal(const JsonValue *old, const JsonValue *nv) {
    switch (old->type) {
    case JSON_NUMBER: return nv->type == JSON_NUMBER ? NULL : "it is a number";
    case JSON_STRING: return nv->type == JSON_STRING ? NULL : "it is a text";
    case JSON_BOOL: return nv->type == JSON_BOOL ? NULL : "it is true or false";
    case JSON_ARRAY:
        for (size_t i = 0; i < json_len(old); i++)
            if (json_at(old, i)->type == JSON_ARRAY || json_at(old, i)->type == JSON_OBJECT)
                return "it is a list of groups: change their values one by one (PATH.0.KEY)";
        if (nv->type != JSON_ARRAY || json_len(nv) != json_len(old)) return "it is a list that keeps its length";
        for (size_t i = 0; i < json_len(old); i++)
            if (json_at(nv, i)->type != json_at(old, i)->type) return "its entries keep their kind (numbers stay numbers)";
        return NULL;
    case JSON_OBJECT: return "it is a group of keys: change them one by one (PATH.KEY)";
    default: return "it holds nothing to change here: lab_scenario_get and lab_run set it";
    }
}

/* the scenario's paths, shallow ones first (top-level values, then one level down...), "source" texts left out */
static void path_collect(const JsonValue *v, char *path, size_t plen, int depth, int want, char *out, size_t cap) {
    bool leaf = v->type != JSON_OBJECT && v->type != JSON_ARRAY;
    if (v->type == JSON_ARRAY) {
        leaf = true;
        for (size_t i = 0; i < json_len(v); i++)
            if (json_at(v, i)->type == JSON_ARRAY || json_at(v, i)->type == JSON_OBJECT) leaf = false;
    }
    if (leaf) {
        size_t n = strlen(out);
        if (depth == want && plen && n + plen + 3 < cap) snprintf(out + n, cap - n, "%s%s", n ? ", " : "", path);
        return;
    }
    if (depth >= want) return;
    for (size_t i = 0; i < json_len(v); i++) {
        char seg[64];
        if (v->type == JSON_OBJECT) snprintf(seg, sizeof seg, "%s", json_key_at(v, i));
        else snprintf(seg, sizeof seg, "%zu", i);
        if (v->type == JSON_OBJECT && (!strcmp(seg, "source") || !strcmp(seg, "changed_from") || (!plen && !strcmp(seg, "domain"))))
            continue;
        size_t k = (size_t)snprintf(path + plen, 256 - plen, "%s%s", plen ? "." : "", seg);
        if (plen + k < 255) path_collect(v->type == JSON_OBJECT ? json_value_at(v, i) : json_at(v, i), path, plen + k, depth + 1, want, out, cap);
        path[plen] = 0;
    }
}

static void path_list(const JsonValue *sc, char *out, size_t cap) {
    char path[256] = "";
    out[0] = 0;
    for (int d = 1; d <= 6; d++) path_collect(sc, path, 0, 0, d, out, cap);
}

/* the title notes what was changed: "A flag in a 6 m/s wind (changed: stream.speed_m_s 12; cell_m 0.03)", earlier
 * changes kept and a value changed again shown once */
static void title_note(JsonValue *sc, const JsonValue *set) {
    const char *t = json_get_str(sc, "title", NULL);
    if (!t || json_get(set, "title")) return;
    char base[400], items[24][128];
    int n = 0;
    snprintf(base, sizeof base, "%s", t);
    char *open = strstr(base, " (changed: ");
    size_t bl = strlen(base);
    if (open && bl && base[bl - 1] == ')') {
        base[bl - 1] = 0;
        for (char *it = open + 11; it && *it && n < 24;) {
            char *semi = strstr(it, "; ");
            snprintf(items[n++], sizeof items[0], "%.*s", semi ? (int)(semi - it) : (int)strlen(it), it);
            it = semi ? semi + 2 : NULL;
        }
        *open = 0;
    }
    for (size_t i = 0; i < json_len(set); i++) {
        const char *path = json_key_at(set, i);
        char *val = json_dump(json_value_at(set, i), 0, NULL, NULL);
        int at = n;
        for (int k = 0; k < n; k++) {
            size_t pl = strlen(path);
            if (!strncmp(items[k], path, pl) && items[k][pl] == ' ') at = k;
        }
        if (at < 24) snprintf(items[at], sizeof items[0], "%s %s", path, val ? val : "?");
        if (at == n && n < 24) n++;
        free(val);
    }
    char title[1200];
    size_t w = (size_t)snprintf(title, sizeof title, "%s (changed: ", base);
    for (int k = 0; k < n && w < sizeof title; k++) w += (size_t)snprintf(title + w, sizeof title - w, "%s%s", k ? "; " : "", items[k]);
    if (w < sizeof title) snprintf(title + w, sizeof title - w, ")");
    json_set_string(sc, "title", title);
}

/* the next free name for a change of BASE: BASE_v2, _v3... (BASE's own _vN taken off first) */
static bool change_name(Local *L, const char *base, char *name, size_t cap) {
    char stem[80], p[1400];
    snprintf(stem, sizeof stem, "%s", base);
    size_t n = strlen(stem), d = n;
    while (d > 0 && stem[d - 1] >= '0' && stem[d - 1] <= '9') d--;
    if (d < n && d >= 3 && stem[d - 1] == 'v' && stem[d - 2] == '_') stem[d - 2] = 0;
    if (strlen(stem) > 56) stem[56] = 0;
    for (int k = 2; k < 10000; k++) {
        snprintf(name, cap, "%s_v%d", stem, k);
        snprintf(p, sizeof p, "%s/%s.json", L->runs, name);
        if (file_exists(p)) continue;
        snprintf(p, sizeof p, "%s/%s.lab", L->results, name);
        if (file_exists(p)) continue;
        snprintf(p, sizeof p, "%s/examples/lab/%s.json", L->lab, name);
        if (!file_exists(p)) return true;
    }
    return false;
}

static JsonValue *lab_change(Local *L, const JsonValue *args) {
    const char *base = json_get_str(args, "base", "");
    const JsonValue *set = json_get(args, "set");
    char p[1400], from[1400] = "", name[96], why[2400], keys[1800];
    if (!lab_name_ok(base)) return local_fail("INVALID_ARGUMENT", "base: the name of the result or scenario to change", "the headset's question and lab_scenarios give the names");
    if (!set || set->type != JSON_OBJECT || !json_len(set) || json_len(set) > 24)
        return local_fail("INVALID_ARGUMENT", "set: one to 24 values to change, each by its path: {\"stream.speed_m_s\": 12}", NULL);
    /* the base: what was computed and is shown, else the version made here, else the example */
    JsonValue *sc = NULL;
    snprintf(p, sizeof p, "%s/%s.lab", L->results, base);
    if ((sc = result_scenario(p))) snprintf(from, sizeof from, "%s", p);
    for (int k = 0; !sc && k < 2; k++) {
        if (k == 0) snprintf(p, sizeof p, "%s/%s.json", L->runs, base);
        else snprintf(p, sizeof p, "%s/examples/lab/%s.json", L->lab, base);
        JsonValue *j = file_exists(p) ? json_read_file(p, 16u << 20, NULL) : NULL;
        if (j && j->type == JSON_OBJECT) sc = j, snprintf(from, sizeof from, "%s", p);
        else json_free(j);
    }
    if (!sc) return local_fail("NOT_FOUND", "no result or scenario of that name", "the headset's question and lab_scenarios give the names");
    if (!json_get_str(sc, "domain", NULL)) {
        json_free(sc);
        return local_fail("INVALID_ARGUMENT", "that scenario names no domain", "lab_scenario_get and lab_run");
    }
    /* every value is checked before any is changed: a refusal leaves nothing behind */
    path_list(sc, keys, sizeof keys);
    for (size_t i = 0; i < json_len(set); i++) {
        const char *path = json_key_at(set, i);
        JsonValue *holder = NULL, *old;
        char last[128];
        const char *bad = NULL;
        if (!strcmp(path, "domain")) bad = "the solver is not changed here: lab_scenario_get and lab_run make a scenario of another domain";
        else if (!strncmp(path, "changed_from", 12)) bad = "it is kept by lab_change itself";
        else if (!(old = path_find(sc, path, &holder, last, sizeof last))) {
            snprintf(why, sizeof why, "%s is not in the scenario %s", path, base);
            json_free(sc);
            return local_fail("INVALID_ARGUMENT", why, keys[0] ? keys : NULL);
        } else if (!strcmp(last, "source") && holder->type == JSON_OBJECT) bad = "sources are kept as they are; the source beside a changed value records the change";
        else bad = kind_refusal(old, json_value_at(set, i));
        if (bad) {
            snprintf(why, sizeof why, "%s: %s", path, bad);
            json_free(sc);
            return local_fail("INVALID_ARGUMENT", why, NULL);
        }
    }
    const char *given = json_get_str(args, "name", NULL);
    if (given) {
        snprintf(name, sizeof name, "%s", given);
        snprintf(p, sizeof p, "%s/examples/lab/%s.json", L->lab, name);
        if (!lab_name_ok(name) || file_exists(p)) {
            json_free(sc);
            return local_fail("INVALID_ARGUMENT", "name: a new name (lowercase, digits, _ and -), not one of the examples",
                              "leave it out: the change is saved as BASE_v2, _v3...");
        }
    } else if (!change_name(L, base, name, sizeof name)) {
        json_free(sc);
        return local_fail("IO_ERROR", "no free name for the change", NULL);
    }
    /* the change, the source notes beside it, the title, and where it came from */
    char date[16];
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    JsonValue *changed = json_array();
    title_note(sc, set);
    for (size_t i = 0; i < json_len(set); i++) {
        const char *path = json_key_at(set, i);
        JsonValue *holder = NULL, *old = path_find(sc, path, &holder, p, sizeof p), *nv = json_clone(json_value_at(set, i));
        char *o = json_dump(old, 0, NULL, NULL), *n = json_dump(nv, 0, NULL, NULL);
        JsonValue *c = json_object();
        json_set_string(c, "path", path);
        json_set(c, "from", json_clone(old));
        json_set(c, "to", json_clone(nv));
        json_push(changed, c);
        const char *src = holder->type == JSON_OBJECT ? json_get_str(holder, "source", NULL) : NULL;
        if (src && strcmp(p, "title"))
            json_set(holder, "source", json_stringf("%s Changed on request on %s: %s was %s, now %s (what is cited above is for %s).",
                                                    src, date, p, o ? o : "?", n ? n : "?", o ? o : "?"));
        if (holder->type == JSON_OBJECT) json_set(holder, p, nv);
        else { /* an entry of a list */
            size_t at = (size_t)strtoul(p, NULL, 10);
            json_free(holder->u.array.items[at]);
            holder->u.array.items[at] = nv;
        }
        free(o), free(n);
    }
    JsonValue *cf = json_object();
    json_set_string(cf, "base", base);
    json_set(cf, "set", json_clone(set));
    json_set(sc, "changed_from", cf);
    JsonValue *r = lab_start(L, name, sc, json_get_num(args, "wait_seconds", 2));
    json_free(sc);
    JsonValue *v = json_get(r, "value");
    if (v) {
        json_set_string(v, "base", base);
        json_set_string(v, "base_from", from);
        json_set(v, "changed", changed);
    } else json_free(changed);
    return r;
}

/* a question for the window: a request file it shows as a card, then its answer (allow or deny), at most 10 min */
static bool window_ask(Local *L, const char *tool, const JsonValue *input, bool *answered) {
    static int seq;
    char base[1200], req[1300], ans[1300], tmp[1300];
    snprintf(base, sizeof base, "%s/%ld-%d-%d", L->approvals, (long)time(NULL), (int)getpid(), ++seq);
    snprintf(req, sizeof req, "%s.req", base), snprintf(ans, sizeof ans, "%s.ans", base), snprintf(tmp, sizeof tmp, "%s.tmp", base);
    JsonValue *r = json_object();
    json_set_string(r, "tool_name", tool);
    json_set(r, "input", input ? json_clone(input) : json_object());
    size_t n;
    char *text = json_dump(r, 0, &n, NULL);
    json_free(r);
    mkdir(L->approvals, 0700);
    FILE *f = text ? fopen(tmp, "w") : NULL;
    if (f) fwrite(text, 1, n, f), fclose(f);
    free(text);
    bool allow = false;
    *answered = false;
    if (f && rename(tmp, req) == 0) {
        for (int i = 0; i < 3000 && !*answered; i++) { /* 10 minutes */
            char *a = read_text(ans, 16);
            if (a) *answered = true, allow = !strncmp(a, "allow", 5), free(a);
            else usleep(200000);
        }
    }
    unlink(req), unlink(ans), unlink(tmp);
    return allow;
}

/* the setup in the person's words, confirmed in the window before a run (GOALS.md G17 step 6) */
static JsonValue *setup_confirm(Local *L, const JsonValue *args) {
    const char *summary = json_get_str(args, "summary", "");
    if (!summary[0]) return local_fail("INVALID_ARGUMENT", "summary: where the part is held and what loads it, in plain words with units", NULL);
    JsonValue *in = json_object();
    json_set_string(in, "summary", summary);
    bool answered, ok = window_ask(L, "setup_confirm", in, &answered);
    json_free(in);
    JsonValue *v = json_object();
    json_set_bool(v, "confirmed", ok);
    json_set_string(v, "message", ok ? "The person confirmed the setup: run it."
                                  : answered ? "The person says this is not right: ask what to change, change it, and confirm again."
                                             : "No answer came from the window within 10 minutes: do not run; ask again when they are back.");
    return local_ok(v);
}

/* ---- printed-part geometry (docs/cad.md): build/openphysics-cad as a program of its own, its record as the answer ---- */

/* runs argv, its stdout into out (cap bytes); false with a reason when it cannot be run or outlives timeout_s */
static bool run_capture(char *const argv[], double timeout_s, char *out, size_t cap, char *why, size_t wcap) {
    int fd[2];
    if (pipe(fd) != 0) return snprintf(why, wcap, "cannot open a pipe"), false;
    pid_t pid = fork();
    if (pid < 0) return close(fd[0]), close(fd[1]), snprintf(why, wcap, "cannot start %s", argv[0]), false;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(fd[1], 1);
        if (nul >= 0) dup2(nul, 0), dup2(nul, 2);
        for (int k = 3; k < 256; k++) close(k);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    fcntl(fd[0], F_SETFL, O_NONBLOCK);
    size_t n = 0;
    out[0] = 0;
    int status = 0;
    bool done = false;
    for (double t = 0; t < timeout_s; t += 0.05) {
        ssize_t got;
        while ((got = read(fd[0], out + n, cap - 1 - n)) > 0) n += (size_t)got;
        out[n] = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            done = true;
            while ((got = read(fd[0], out + n, cap - 1 - n)) > 0) n += (size_t)got;
            out[n] = 0;
            break;
        }
        usleep(50000);
    }
    close(fd[0]);
    if (!done) {
        kill(pid, SIGKILL), waitpid(pid, NULL, 0);
        return snprintf(why, wcap, "it took longer than %.0f s: give a coarser resolution_mm", timeout_s), false;
    }
    return true;
}

/* openphysics-cad's record, as this server's answer */
static JsonValue *cad_answer(const char *out, const char *dir, const char *next) {
    const char *line = strrchr(out, '{') ? out : "";
    for (const char *p = out; *p; p++) /* the last line that is a JSON object */
        if ((p == out || p[-1] == '\n') && *p == '{') line = p;
    JsonValue *r = json_parse(line, strlen(line), NULL, NULL);
    if (!r) return local_fail("UNAVAILABLE", "openphysics-cad gave no record", "make build/openphysics-cad in the repository (docs/cad.md)");
    if (!json_get_bool(r, "ok", false)) {
        const JsonValue *e = json_get(r, "error");
        char msg[1200];
        snprintf(msg, sizeof msg, "%s%s%s", json_get_str(e, "feature", NULL) ? "feature " : "", json_get_str(e, "feature", ""),
                 json_get_str(e, "feature", NULL) ? ": " : "");
        snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "%s", json_get_str(e, "message", "refused"));
        JsonValue *f = local_fail("INVALID_ARGUMENT", msg, "change that feature or value and call again");
        json_free(r);
        return f;
    }
    json_remove(r, "ok");
    json_set_string(r, "folder", dir);
    json_set_string(r, "next", next);
    return local_ok(r);
}

enum { CAD_PART, CAD_LATTICE, CAD_CHECK };
/* one lattice cell homogenised by openphysics-cad cell: its record as the answer */
static JsonValue *cad_cell(Local *L, const JsonValue *args) {
    char dir[1300], file[1400], tool[1100], out[1u << 16], why[300];
    static int serial;
    snprintf(tool, sizeof tool, "%s/build/openphysics-cad", L->lab);
    if (access(tool, X_OK) != 0) return local_fail("UNAVAILABLE", "openphysics-cad is not built", "make cad && make build/openphysics-cad in the repository (docs/cad.md)");
    snprintf(dir, sizeof dir, "%s/cad", L->results);
    mkdir(L->results, 0755), mkdir(dir, 0755);
    snprintf(file, sizeof file, "%s/cell-request-%d-%d.json", dir, (int)getpid(), ++serial);
    if (!json_write_file(file, args, JSON_PRETTY)) return local_fail("IO_ERROR", "cannot write into the lab folder", NULL);
    char *argv[] = {tool, "cell", file, NULL};
    bool ran = run_capture(argv, 900, out, sizeof out, why, sizeof why);
    unlink(file);
    if (!ran) return local_fail("TIMEOUT", why, "fewer voxels answer sooner");
    return cad_answer(out, dir, "cad_lattice makes a part's core of this cell; the moduli are the infinite lattice's");
}

static JsonValue *cad_run(Local *L, const JsonValue *args, int tool_kind) {
    bool lattice = tool_kind != CAD_PART;
    const char *name = json_get_str(args, "name", "");
    char dir[1300], file[1400], tool[1100], out[1u << 16], why[300], p[1400];
    if (!lab_name_ok(name)) return local_fail("INVALID_ARGUMENT", "name: lowercase letters, digits, _ and - (up to 64)", NULL);
    snprintf(tool, sizeof tool, "%s/build/openphysics-cad", L->lab);
    if (access(tool, X_OK) != 0) return local_fail("UNAVAILABLE", "openphysics-cad is not built", "make cad && make build/openphysics-cad in the repository (docs/cad.md)");
    snprintf(dir, sizeof dir, "%s/cad", L->results);
    mkdir(L->results, 0755), mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/cad/%s", L->results, name);
    mkdir(dir, 0755);
    JsonValue *req;
    if (!lattice) {
        const JsonValue *model = json_get(args, "model");
        if (!model || model->type != JSON_OBJECT) return local_fail("INVALID_ARGUMENT", "model: {\"features\": [...]}", NULL);
        req = json_clone(model);
        snprintf(file, sizeof file, "%s/model.json", dir);
    } else {
        req = json_clone(args);
        json_remove(req, "name");
        const char *part = json_get_str(args, "part", "");
        /* a part by its name: a lattice made by cad_lattice (checked as it would print), or a part made by cad_part */
        snprintf(p, sizeof p, "%s/cad/%s/infilled.stl", L->results, part);
        if (tool_kind != CAD_CHECK || !lab_name_ok(part) || access(p, R_OK) != 0) snprintf(p, sizeof p, "%s/cad/%s/part.step", L->results, part);
        if (lab_name_ok(part) && access(p, R_OK) == 0) json_set_string(req, "part", p);
        snprintf(file, sizeof file, "%s/request.json", dir);
    }
    bool wrote = json_write_file(file, req, JSON_PRETTY);
    json_free(req);
    if (!wrote) return local_fail("IO_ERROR", "cannot write into the lab folder", NULL);
    char defl[32];
    snprintf(defl, sizeof defl, "%g", json_get_num(args, "deflection_mm", 0.01));
    char *argv_part[] = {tool, "part", file, dir, "--deflection-mm", defl, NULL};
    char *argv_lat[] = {tool, tool_kind == CAD_CHECK ? "check" : "lattice", file, dir, NULL};
    if (!run_capture(lattice ? argv_lat : argv_part, tool_kind == CAD_LATTICE ? 900 : tool_kind == CAD_CHECK ? 600 : 120, out, sizeof out,
                     why, sizeof why))
        return local_fail("TIMEOUT", why, NULL);
    return cad_answer(out, dir, tool_kind == CAD_CHECK ? "change the part where the issues say (cad_part, or cad_lattice with drains), then check it again"
                                                       : "geometry_import with the STL path and units mm brings it into the project, for a mesh and an analysis");
}

/* Claude Code's permission prompt: a request file for the window, then its answer (Allow or Deny), at most 10 min */
static JsonValue *approve(Local *L, const JsonValue *args) {
    const JsonValue *input = json_get(args, "input");
    bool answered, allow = window_ask(L, json_get_str(args, "tool_name", "?"), input, &answered);
    size_t n;
    char *text;
    JsonValue *d = json_object();
    if (allow) {
        json_set_string(d, "behavior", "allow");
        json_set(d, "updatedInput", input ? json_clone(input) : json_object());
    } else {
        json_set_string(d, "behavior", "deny");
        json_set_string(d, "message", answered ? "The user declined this in the app." : "No answer came from the app's window.");
    }
    text = json_dump(d, 0, &n, NULL);
    json_free(d);
    JsonValue *o = json_object();
    json_set_bool(o, "ok", true);
    json_set_string(o, "raw_text", text ? text : "{\"behavior\":\"deny\",\"message\":\"internal error\"}");
    free(text);
    return o;
}

/* ---- the headset (docs/xr.md): what the person sees in the Meta Quest, changed and marked by the agent ----
 * While the headset link's bridge runs it keeps a folder (src/core/xrlink.h): the app writes state.json there and runs
 * the view commands it finds (cmd-*.txt) when xrlink_command_allowed allows them, as this program checks first; the
 * bridge writes page.json (the page seen, the person's pins) and serves marks.json to the page, which draws each mark
 * as a labelled pin. */
static bool headset_dir(char *dir, size_t cap, JsonValue **fail) {
    struct stat st;
    if (!xrlink_dir(dir, cap) || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        *fail = local_fail("UNAVAILABLE", "the headset link is not running",
                           "the person starts it on the Mac with python3 tools/xr/xr_bridge.py (docs/xr.md)");
        return false;
    }
    return true;
}

static double file_age(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? difftime(time(NULL), st.st_mtime) : -1;
}

static JsonValue *headset_status(void) {
    char dir[1024], p[1100];
    JsonValue *fail;
    if (!headset_dir(dir, sizeof dir, &fail)) return fail;
    snprintf(p, sizeof p, "%s/state.json", dir);
    JsonValue *st = json_read_file(p, 1 << 20, NULL), *out = json_object();
    double age = file_age(p);
    json_set_bool(out, "app_running", st && age >= 0 && age <= 6);
    if (st) { /* what is shown, without the AI's own conversation */
        json_remove(st, "agent");
        json_set(out, "shown", st);
    }
    snprintf(p, sizeof p, "%s/page.json", dir);
    JsonValue *page = json_read_file(p, 1 << 20, NULL);
    double seen = json_get_num(page, "seen", 0);
    json_set_bool(out, "headset_connected", seen > 0 && difftime(time(NULL), (time_t)seen) <= 5);
    json_set(out, "pins", page && json_get(page, "pins") ? json_clone(json_get(page, "pins")) : json_array());
    json_free(page);
    snprintf(p, sizeof p, "%s/marks.json", dir);
    JsonValue *marks = json_read_file(p, 1 << 20, NULL);
    json_set(out, "marks", marks && json_get(marks, "marks") ? json_clone(json_get(marks, "marks")) : json_array());
    json_free(marks);
    snprintf(p, sizeof p, "%s/health.json", dir); /* how the page runs: frame rate, the session, errors (fresh only) */
    JsonValue *health = json_read_file(p, 1 << 16, NULL);
    double hage = file_age(p);
    if (health && hage >= 0 && hage <= 15) json_set(out, "headset", health);
    else json_free(health);
    return local_ok(out);
}

static bool write_whole(const char *dir, const char *name, const char *text) { /* written whole before it appears */
    char tmp[1200], path[1200];
    snprintf(tmp, sizeof tmp, "%s/tmp-mcp-%d-%s", dir, (int)getpid(), name), snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    ok = (fclose(f) == 0) && ok;
    if (ok && rename(tmp, path) == 0) return true;
    unlink(tmp);
    return false;
}

static JsonValue *headset_view(const JsonValue *args) {
    char dir[1024];
    JsonValue *fail;
    if (!headset_dir(dir, sizeof dir, &fail)) return fail;
    const JsonValue *cmds = json_get(args, "commands");
    if (!cmds || cmds->type != JSON_ARRAY || json_len(cmds) < 1 || json_len(cmds) > 12)
        return local_fail("INVALID_ARGUMENT", "commands: 1 to 12 view commands", "see this tool's description");
    JsonValue *out = json_object(), *sent = json_set_array(out, "sent"), *refused = json_set_array(out, "refused");
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    long long ns = (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
    char names[12][64];
    int nn = 0;
    for (size_t i = 0; i < json_len(cmds); i++) {
        const char *c = json_str(json_at(cmds, i));
        char line[200];
        if (!c || !xrlink_command_allowed(c)) {
            json_push(refused, json_string(c ? c : "(not a text)"));
            continue;
        }
        snprintf(names[nn], sizeof names[nn], "cmd-%lld.txt", ns + (long long)i); /* the app takes them oldest first */
        snprintf(line, sizeof line, "%s\n", c);
        bool ok = write_whole(dir, names[nn], line);
        json_push(ok ? sent : refused, json_string(c));
        nn += ok;
    }
    bool taken = nn == 0;
    for (int k = 0; k < 40 && !taken; k++) { /* the app takes them within a quarter of a second while it runs */
        usleep(100000);
        taken = true;
        for (int i = 0; i < nn; i++) {
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            taken = taken && access(p, F_OK) != 0;
        }
    }
    json_set_bool(out, "taken_by_app", taken);
    if (!taken) { /* withdrawn, so that they do not run whenever the app starts next */
        for (int i = 0; i < nn; i++) {
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            unlink(p);
        }
        json_set_string(out, "note", "the app did not take the commands within 4 s, so they were withdrawn: is it running? (headset_status)");
    }
    return local_ok(out);
}

static JsonValue *headset_mark(const JsonValue *args) {
    char dir[1024], p[1100];
    JsonValue *fail;
    if (!headset_dir(dir, sizeof dir, &fail)) return fail;
    const JsonValue *marks = json_get(args, "marks");
    if (!marks || marks->type != JSON_ARRAY || json_len(marks) > 12) return local_fail("INVALID_ARGUMENT", "marks: up to 12 marks", NULL);
    JsonValue *doc = json_object(), *list = json_set_array(doc, "marks");
    for (size_t i = 0; i < json_len(marks); i++) {
        const JsonValue *m = json_at(marks, i);
        const char *label = json_get_str(m, "label", NULL);
        double at[3];
        if (!label || strlen(label) > 80 || !json_get_numbers(json_get(m, "at"), at, 3) || !isfinite(at[0]) || !isfinite(at[1]) || !isfinite(at[2])) {
            json_free(doc);
            return local_fail("INVALID_ARGUMENT", "each mark: a label (up to 80 characters) and at, three numbers", NULL);
        }
        JsonValue *o = json_object();
        json_set_string(o, "label", label);
        json_set(o, "at", json_numbers(at, 3));
        json_push(list, o);
    }
    snprintf(p, sizeof p, "%s/state.json", dir); /* the result they belong to: the page shows them only on it */
    JsonValue *st = json_read_file(p, 1 << 20, NULL);
    json_set_string(doc, "title", json_get_str(st, "title", ""));
    json_set_string(doc, "scene", json_get_str(st, "scene", ""));
    json_free(st);
    json_set_number(doc, "made", (double)time(NULL));
    char *text = json_dump(doc, JSON_PRETTY, NULL, NULL);
    json_free(doc);
    bool ok = text && write_whole(dir, "marks.json", text);
    free(text);
    if (!ok) return local_fail("INTERNAL_ERROR", "the marks could not be written into the headset's folder", NULL);
    JsonValue *out = json_object();
    json_set_int(out, "marks", (long long)json_len(marks));
    return local_ok(out);
}

static unsigned char *read_bytes(const char *path, size_t max, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *b = malloc(max);
    *len = b ? fread(b, 1, max, f) : 0;
    bool whole = b && fgetc(f) == EOF;
    fclose(f);
    if (!whole) free(b), b = NULL;
    return b;
}

/* a picture of what the person sees: the page draws it when the bridge's state carries this request (look-request.json)
 * and the bridge writes look-now.jpg, then look-now.json with the request's id; a picture taken as a question was asked
 * is look-question.jpg, kept until the next question */
static JsonValue *headset_look(const JsonValue *args) {
    char dir[1024], p[1100], jpg[1100];
    JsonValue *fail;
    if (!headset_dir(dir, sizeof dir, &fail)) return fail;
    const char *moment = json_get_str(args, "moment", "now");
    if (strcmp(moment, "now") && strcmp(moment, "question")) return local_fail("INVALID_ARGUMENT", "moment: now or question", NULL);
    bool now = !strcmp(moment, "now");
    char id[40] = "";
    if (now) {
        snprintf(p, sizeof p, "%s/page.json", dir);
        JsonValue *page = json_read_file(p, 1 << 20, NULL);
        double seen = json_get_num(page, "seen", 0);
        json_free(page);
        if (!(seen > 0 && difftime(time(NULL), (time_t)seen) <= 5))
            return local_fail("UNAVAILABLE", "the headset page is not open",
                              "headset_status says what is shown; moment 'question' is the picture taken at the last question");
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        char req[160];
        snprintf(id, sizeof id, "%lld", (long long)t.tv_sec * 1000000000LL + t.tv_nsec);
        snprintf(req, sizeof req, "{\"id\": \"%s\", \"made\": %.3f}\n", id, (double)t.tv_sec + t.tv_nsec * 1e-9);
        if (!write_whole(dir, "look-request.json", req)) return local_fail("INTERNAL_ERROR", "cannot ask the headset page for a picture", NULL);
    }
    snprintf(p, sizeof p, "%s/look-%s.json", dir, moment);
    snprintf(jpg, sizeof jpg, "%s/look-%s.jpg", dir, moment);
    JsonValue *facts = NULL;
    for (int k = 0; k < (now ? 60 : 1); k++) { /* the page sees the request at its next look at the state */
        if (k) usleep(100000);
        facts = json_read_file(p, 1 << 16, NULL);
        if (facts && (!now || !strcmp(json_get_str(facts, "id", ""), id))) break;
        json_free(facts), facts = NULL;
    }
    if (!facts) {
        if (!now) return local_fail("NOT_FOUND", "no picture was taken at a question since the headset link started", "moment 'now' takes one");
        snprintf(p, sizeof p, "%s/look-request.json", dir); /* withdrawn, unless a newer request took its place */
        JsonValue *r = json_read_file(p, 4096, NULL);
        if (r && !strcmp(json_get_str(r, "id", ""), id)) unlink(p);
        json_free(r);
        return local_fail("UNAVAILABLE", "the headset page did not send a picture within 6 s",
                          "is the headset asleep or the page in the background? headset_status says what is shown");
    }
    size_t n = 0;
    unsigned char *b = read_bytes(jpg, 4u << 20, &n);
    char *b64 = b && n == (size_t)json_get_num(facts, "bytes", -1) ? base64_encode(b, n, NULL) : NULL;
    free(b);
    if (!b64) {
        json_free(facts);
        return local_fail("UNAVAILABLE", "the picture changed while it was read", "ask again");
    }
    JsonValue *v = json_object();
    const char *view = json_get_str(facts, "view", "");
    json_set_string(v, "moment", moment);
    json_set_string(v, "view", view);
    json_set_string(v, "seen_from", !strcmp(view, "screen") ? "the window's view on the computer's screen"
                                    : "between the person's eyes in the headset, a 70 degree view");
    json_set_number(v, "taken_s_ago", round(difftime(time(NULL), (time_t)json_get_num(facts, "made", 0))));
    json_set_string(v, "title", json_get_str(facts, "title", ""));
    json_set_string(v, "scene", json_get_str(facts, "scene", ""));
    json_set_int(v, "width", (long long)json_get_num(facts, "width", 0));
    json_set_int(v, "height", (long long)json_get_num(facts, "height", 0));
    json_free(facts);
    JsonValue *res = local_ok(v), *imgs = json_set_array(res, "images"), *im = json_object();
    json_set_string(im, "name", "headset-view.jpg");
    json_set_string(im, "mime_type", "image/jpeg");
    json_set_string(im, "data", b64);
    json_push(imgs, im);
    free(b64);
    return res;
}

static bool local_has(Local *L, const char *name) {
    for (int i = 0; i < NLOCAL; i++)
        if (!strcmp(LOCAL_TOOLS[i][0], name)) /* the window's own questions need its folder; the rest, the lab */
            return !strcmp(name, "approve") || !strcmp(name, "setup_confirm") ? L->approvals[0] != 0 : L->lab[0] != 0;
    return false;
}

static bool lean_has(Local *L, const char *name) {
    for (int i = 0; L->lean && i < NLEAN; i++)
        if (!strcmp(LEAN_TOOLS[i][0], name)) return true;
    return false;
}

/* the first sentence of a description: what an operation does, in a line */
static void first_line(const char *d, char *out, size_t cap) {
    const char *e = strstr(d, ". ");
    size_t n = e ? (size_t)(e - d + 1) : strlen(d);
    if (n > 220) n = 220;
    snprintf(out, cap, "%.*s", (int)n, d);
}

static bool contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return !n;
}

static JsonValue *lean_call(Local *L, const char *name, const JsonValue *args, const char *client, char *err, size_t errlen) {
    if (!strcmp(name, "engine_call")) {
        const char *op = json_get_str(args, "name", "");
        if (local_has(L, op) || lean_has(L, op)) return local_fail("INVALID_ARGUMENT", "that is not an engine operation: call it by its own name", NULL);
        const JsonValue *a = json_get(args, "arguments");
        return L->inner.call_op(L->inner.ctx, op, a && a->type == JSON_OBJECT ? a : NULL, client, err, errlen);
    }
    JsonValue *all = L->inner.list_ops(L->inner.ctx, err, errlen);
    if (!all) return NULL;
    JsonValue *v = json_object();
    if (!strcmp(name, "engine_op")) {
        const char *op = json_get_str(args, "name", "");
        for (size_t i = 0; i < json_len(all); i++)
            if (!strcmp(json_get_str(json_at(all, i), "name", ""), op)) {
                json_free(v);
                v = json_clone(json_at(all, i));
                json_free(all);
                return local_ok(v);
            }
        json_free(all), json_free(v);
        return local_fail("NOT_FOUND", "no engine operation of that name", "engine_ops lists them");
    }
    const char *match = json_get_str(args, "match", "");
    JsonValue *list = json_set_array(v, "operations");
    for (size_t i = 0; i < json_len(all); i++) {
        const JsonValue *o = json_at(all, i);
        char line[240];
        first_line(json_get_str(o, "description", ""), line, sizeof line);
        if (match[0] && !contains_ci(json_get_str(o, "name", ""), match) && !contains_ci(line, match)) continue;
        JsonValue *x = json_object();
        json_set_string(x, "name", json_get_str(o, "name", ""));
        json_set_string(x, "kind", json_get_str(o, "kind", ""));
        json_set_string(x, "does", line);
        json_push(list, x);
    }
    json_set_string(v, "next", "engine_op NAME gives an operation's input; engine_call {name, arguments} runs it");
    json_free(all);
    return local_ok(v);
}

static JsonValue *local_list(void *ctx, char *err, size_t errlen) {
    Local *L = ctx;
    JsonValue *arr = L->lean ? json_array() : L->inner.list_ops(L->inner.ctx, err, errlen);
    if (!arr) return NULL;
    for (int i = 0; i < NLEAN && L->lean; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", LEAN_TOOLS[i][0]);
        json_set_string(o, "title", LEAN_TOOLS[i][1]);
        json_set_string(o, "description", LEAN_TOOLS[i][2]);
        json_set_string(o, "kind", i == 2 ? "mutation" : "query");
        json_set_bool(o, "idempotent", i != 2);
        json_set_bool(o, "destructive", false);
        json_set(o, "input_schema", json_parse(LEAN_TOOLS[i][3], strlen(LEAN_TOOLS[i][3]), NULL, NULL));
        json_push(arr, o);
    }
    for (int i = 0; i < NLOCAL; i++) {
        if (!local_has(L, LOCAL_TOOLS[i][0])) continue;
        JsonValue *o = json_object();
        json_set_string(o, "name", LOCAL_TOOLS[i][0]);
        json_set_string(o, "title", LOCAL_TOOLS[i][1]);
        json_set_string(o, "description", LOCAL_TOOLS[i][2]);
        const char *n = LOCAL_TOOLS[i][0];
        bool query = !strcmp(n, "lab_scenarios") || !strcmp(n, "lab_scenario_get") || !strcmp(n, "lab_run_status") ||
                     !strcmp(n, "headset_status") || !strcmp(n, "headset_look") || !strcmp(n, "cad_check") || !strcmp(n, "cad_cell");
        json_set_string(o, "kind", query ? "query" : "mutation");
        json_set_bool(o, "idempotent", query);
        json_set_bool(o, "destructive", false);
        json_set(o, "input_schema", json_parse(LOCAL_TOOLS[i][3], strlen(LOCAL_TOOLS[i][3]), NULL, NULL));
        json_push(arr, o);
    }
    return arr;
}

static JsonValue *local_call(void *ctx, const char *name, const JsonValue *args, const char *client, char *err, size_t errlen) {
    Local *L = ctx;
    if (lean_has(L, name)) return lean_call(L, name, args, client, err, errlen);
    if (!local_has(L, name)) return L->inner.call_op(L->inner.ctx, name, args, client, err, errlen);
    if (!strcmp(name, "lab_scenarios")) return lab_list(L);
    if (!strcmp(name, "lab_scenario_get")) return lab_get(L, args);
    if (!strcmp(name, "lab_run")) return lab_run(L, args);
    if (!strcmp(name, "lab_change")) return lab_change(L, args);
    if (!strcmp(name, "cad_part")) return cad_run(L, args, CAD_PART);
    if (!strcmp(name, "cad_lattice")) return cad_run(L, args, CAD_LATTICE);
    if (!strcmp(name, "cad_check")) return cad_run(L, args, CAD_CHECK);
    if (!strcmp(name, "cad_cell")) return cad_cell(L, args);
    if (!strcmp(name, "headset_status")) return headset_status();
    if (!strcmp(name, "headset_view")) return headset_view(args);
    if (!strcmp(name, "headset_mark")) return headset_mark(args);
    if (!strcmp(name, "headset_look")) return headset_look(args);
    if (!strcmp(name, "lab_run_status")) {
        const char *n = json_get_str(args, "name", "");
        if (!lab_name_ok(n)) return local_fail("INVALID_ARGUMENT", "name: the name given to lab_run", NULL);
        double w = json_get_num(args, "wait_seconds", 0);
        return lab_state(L, n, w < 0 ? 0 : w > 600 ? 600 : w);
    }
    if (!strcmp(name, "setup_confirm")) return setup_confirm(L, args);
    return approve(L, args);
}

static void usage(FILE *f) {
    fprintf(f, "usage: openphysics-mcp [--connect [SOCKET] | --embedded] [--workspace DIR] [--allow-read DIR] [--allow-write DIR]\n"
               "                  [--lab REPO --lab-results DIR] [--approvals DIR]\n"
               "MCP server on stdio. See docs/mcp-clients.md for Claude Code and Codex configuration.\n"
               "--lab adds the physics lab's tools (lab_scenarios, lab_scenario_get, lab_run, lab_run_status) over the\n"
               "repository's examples/lab, docs/lab and build/labrun, writing results into --lab-results, and the headset's\n"
               "(headset_status, headset_look, headset_view, headset_mark), which act while the headset link runs\n"
               "(docs/xr.md); --approvals adds approve, Claude Code's --permission-prompt-tool answered in the app's window\n"
               "through that folder. --lean (with --lab), for a spoken conversation: the engine's operations are reached\n"
               "through engine_ops, engine_op and engine_call instead of each being listed (about 60 000 tokens fewer).\n");
}

int main(int argc, char **argv) {
    enum { AUTO, CONNECT, EMBEDDED } mode = AUTO;
    char socket_path[512] = "", err[1024];
    static Local local;
    EngineConfig ec;
    engine_config_default(&ec);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--embedded")) {
            mode = EMBEDDED;
        } else if (!strcmp(a, "--connect")) {
            mode = CONNECT;
            if (i + 1 < argc && argv[i + 1][0] != '-') snprintf(socket_path, sizeof socket_path, "%s", argv[++i]);
        } else if (!strcmp(a, "--socket") && i + 1 < argc) {
            snprintf(socket_path, sizeof socket_path, "%s", argv[++i]);
        } else if (!strcmp(a, "--workspace") && i + 1 < argc) {
            if (!engine_config_set_workspace(&ec, argv[++i], err, sizeof err)) {
                fprintf(stderr, "openphysics-mcp: %s\n", err);
                return 2;
            }
        } else if ((!strcmp(a, "--allow-read") || !strcmp(a, "--allow-write")) && i + 1 < argc) {
            if (!engine_config_add_root(&ec, !strcmp(a, "--allow-write"), argv[++i], err, sizeof err)) {
                fprintf(stderr, "openphysics-mcp: %s\n", err);
                return 2;
            }
        } else if (!strcmp(a, "--lean")) {
            local.lean = true;
        } else if ((!strcmp(a, "--lab") || !strcmp(a, "--lab-results") || !strcmp(a, "--approvals")) && i + 1 < argc) {
            char *dst = !strcmp(a, "--lab") ? local.lab : !strcmp(a, "--lab-results") ? local.results : local.approvals;
            snprintf(dst, 1024, "%s", argv[++i]);
        } else if (!strcmp(a, "--version")) {
            fprintf(stdout, "openphysics-mcp %s (MCP %s)\n", OPENPHYSICS_VERSION, MCP_LATEST_VERSION);
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "openphysics-mcp: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    int proto_fd = dup(STDOUT_FILENO);
    if (proto_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        fprintf(stderr, "openphysics-mcp: cannot set up the protocol stream\n");
        return 1;
    }
    net_set_cloexec(proto_fd);
    signal(SIGPIPE, SIG_IGN);

    McpBackend be = {0};
    Embedded emb = {NULL};
    Bridge br = {{0}, NULL};
    if (mode != EMBEDDED) {
        bool chosen = socket_path[0] != 0;
        if (!chosen) ctl_default_socket_path(socket_path, sizeof socket_path);
        CtlClient *c = socket_path[0] ? ctl_connect_unix(socket_path, err, sizeof err) : NULL;
        char old[512], oerr[256];
        if (!c && !chosen && ctl_legacy_socket_path(old, sizeof old) && (c = ctl_connect_unix(old, oerr, sizeof oerr)) != NULL)
            snprintf(socket_path, sizeof socket_path, "%s", old); /* a server started before the rename */
        if (c) {
            br.client = c;
            snprintf(br.path, sizeof br.path, "%s", socket_path);
            be.list_ops = bridge_list;
            be.call_op = bridge_call;
            be.ctx = &br;
            LOGI("openphysics-mcp: bridging to the server at %s", socket_path);
        } else if (mode == CONNECT) {
            LOGE("openphysics-mcp: %s", socket_path[0] ? err : "no socket path");
            return 1;
        }
    }
    if (!be.ctx) {
        /* what this server's own tools write (lab results, cad_part's and cad_lattice's files) can be read back, e.g.
         * by geometry_import: the lab's results folder is a read root of the embedded engine */
        if (local.results[0]) {
            mkdir(local.results, 0755);
            if (!engine_config_add_root(&ec, false, local.results, err, sizeof err)) LOGW("openphysics-mcp: %s", err);
        }
        emb.engine = engine_create(&ec, err, sizeof err);
        if (!emb.engine) {
            LOGE("openphysics-mcp: %s", err);
            return 1;
        }
        be.list_ops = embedded_list;
        be.call_op = embedded_call;
        be.ctx = &emb;
        LOGI("openphysics-mcp: embedded engine, workspace %s", ec.workspace);
    }

    if (local.lab[0] || local.approvals[0]) { /* the lab's and the window's own tools, in front of the engine's */
        if (local.lab[0] && !local.results[0]) { /* the projects folder (the one from before the rename while only it exists) */
            char projects[NV_PATH_MAX];
            if (!op_projects_dir(projects, sizeof projects, NULL)) snprintf(projects, sizeof projects, "/tmp/OpenPhysicsAI-Projects");
            snprintf(local.results, sizeof local.results, "%s/lab", projects);
        }
        snprintf(local.runs, sizeof local.runs, "%s/agent-runs", local.results);
        local.inner = be;
        be.list_ops = local_list;
        be.call_op = local_call;
        be.ctx = &local;
    }
    McpSession *s = mcp_session_create(&be);
    LineReader rd;
    line_reader_init(&rd, STDIN_FILENO, (size_t)64 << 20);
    int status = 0;
    for (;;) {
        char *line;
        size_t n;
        int r = line_reader_next(&rd, -1, &line, &n);
        if (r == -1) break;
        char *resp = NULL;
        size_t rlen = 0;
        if (r == -2) {
            static const char TOO_BIG[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Request: message exceeds 67108864 bytes\"}}";
            resp = strdup(TOO_BIG);
            rlen = sizeof TOO_BIG - 1;
        } else if (r == 1) {
            size_t k = 0;
            while (k < n && (line[k] == ' ' || line[k] == '\t')) k++;
            if (k == n) continue; /* blank line */
            resp = mcp_handle_text(s, line, n, &rlen);
        }
        if (!resp) continue;
        char *msg = realloc(resp, rlen + 1);
        if (!msg) {
            free(resp);
            status = 1;
            break;
        }
        msg[rlen] = '\n';
        bool ok = net_write_all(proto_fd, msg, rlen + 1, 120000);
        free(msg);
        if (!ok) {
            status = 1;
            break;
        }
    }
    line_reader_free(&rd);
    mcp_session_destroy(s);
    if (br.client) ctl_close(br.client);
    if (emb.engine) engine_destroy(emb.engine);
    close(proto_fd);
    return status;
}
