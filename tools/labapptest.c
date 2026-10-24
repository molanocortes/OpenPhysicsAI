/* App scenario lifecycle contracts, not numerical validation. Criteria fixed before execution:
 * every shipped scenario prepares without computing; exposed controls edit their named values; completing work
 * respects subsequent selection/close/control edits; failed publication restores old input provenance; Saved
 * returns the newest files even when its directory contains more files than the display limit.
 * Added 2026-10-08, written before its first run: a run the AI started that is still computing (agent-runs/NAME.pid
 * holding a live process) is opened by the follower once its result holds two frames, before the run ends, paused on
 * its newest frame; as frames are appended the frame count follows them and the view moves to the newest; the auto
 * range never narrows while it grows; the open's serial stays the same; when the process ends, growing stops, the range
 * is the one over all frames as a fresh open computes it, and it plays; a person who opened another result meanwhile
 * is not taken back to it while it computes. Amended openly after its first run the same day (two failures: a second
 * run written in the same second as the first was not followed, and this case's own files disturbed the completion
 * check below): "ends" includes a process that has ended but that its parent has not reaped yet (openphysics-mcp reaps a
 * finished labrun only when lab_run_status asks), and the case removes its files when it is done.
 *
 * No window, GL context, or physical solve is needed. As in femviewtest, include the bridge to exercise private
 * completion/publication boundaries with synthetic data, and abort if a window-only callback is reached. */
#include "../src/labapp.c"

#include <ftw.h>
#include <sys/time.h>
#include <sys/wait.h>

int app_inject_click(float x, float y, int n) { abort(); }
int app_inject_drag(float x, float y, float x1, float y1, uint32_t m, int n) { abort(); }
int app_inject_scroll(float x, float y, float dy) { abort(); }
int app_inject_key(int key) { abort(); }
bool app_rake_handle(float *x, float *y) { abort(); }
bool app_export_image(const char *path, int scale) { abort(); }

static int passed, failed;
#define CHECK(condition, message) do { \
    if (condition) passed++; \
    else { failed++; fprintf(stderr, "FAIL: %s\n", message); } \
} while (0)

static void require(bool ok, const char *message) {
    if (!ok) { fprintf(stderr, "SETUP FAILED: %s\n", message); exit(1); }
}

static void pause_worker(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static void fixture_result(const char *path) {
    char err[256];
    LabWriter *w = lab_create(path,
        "{\"domain\":\"flow\",\"title\":\"Synthetic completion\",\"fields\":{\"speed\":\"m/s\"}}", err, sizeof err);
    require(w != NULL, err);
    double xyz[6] = {0, 0, 0, 1, 1, 1};
    float speed[2] = {0, 1};
    lab_frame_begin(w, 0);
    lab_part_points(w, "points", 2, xyz);
    lab_field(w, "speed", LAB_AT_NODE, 2, speed);
    require(lab_frame_end(w), "write fixture frame");
    require(lab_close(w), "close fixture result");
}

static void prepare_and_controls(void) {
    DIR *d = opendir("examples/lab");
    require(d != NULL, "open shipped scenarios (run from repository root)");
    struct dirent *e;
    int scenarios = 0, controls = 0;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".json")) continue;
        char path[1024];
        snprintf(path, sizeof path, "examples/lab/%s", e->d_name);
        labapp_select(path);
        CHECK(labapp_prepared() && !labapp_running() && !labapp_active(), path);
        CHECK(labapp_scenario_title()[0] && labapp_scenario_domain()[0], "selected scenario exposes title and domain");
        LabControl before[32], after[32];
        int count = labapp_controls(before, 32);
        for (int i = 0; i < count; i++) {
            CHECK(before[i].min < before[i].max, "control bounds are ordered");
            double value = (before[i].min + before[i].max) / 2;
            labapp_control_set(i, value);
            CHECK(labapp_controls(after, 32) == count, "editing preserves the exposed control list");
            CHECK(after[i].value == (before[i].integer ? round(value) : value), "control edits round only integer inputs");
            controls++;
        }
        scenarios++;
    }
    closedir(d);
    CHECK(scenarios > 0, "the shipped scenario directory is nonempty");
    labapp_close();
    CHECK(!labapp_prepared(), "closing clears the selected scenario");

    const char *source = "{\"value\":5,\"controls\":["
        "{\"path\":\"missing\",\"min\":0,\"max\":10},"
        "{\"path\":\"value\",\"min\":0,\"max\":10}]}";
    g.scenario = json_parse(source, strlen(source), NULL, NULL);
    require(g.scenario != NULL, "parse malformed-control fixture");
    LabControl shown[2];
    CHECK(labapp_controls(shown, 2) == 1, "a missing control path is omitted");
    labapp_control_set(0, 8);
    CHECK(json_get_num(g.scenario, "value", 0) == 8, "an omitted control does not redirect the next slider");
    labapp_control_set(0, 100);
    CHECK(json_get_num(g.scenario, "value", 0) == 10, "control edits clamp to declared bounds");
    labapp_close();
    printf("Prepared %d shipped scenarios and edited %d exposed controls without a solver.\n", scenarios, controls);
}

static void failed_run_keeps_cache(const char *home) {
    JsonValue *bad = json_object();
    json_set_string(bad, "domain", "unknown");
    json_set_string(bad, "title", "Expected failure check");
    char input[1024], output[1024];
    snprintf(input, sizeof input, "%s/failure.json", home);
    require(json_write_file(input, bad, 0), "save invalid-domain input");
    json_free(bad);
    snprintf(output, sizeof output, "%s/failure.lab", labapp_results_dir());
    fixture_result(output);
    struct stat before, after;
    require(stat(output, &before) == 0, "inspect original cached result");
    labapp_run(input);
    CHECK(labapp_running(), "explicit Run starts the background worker");
    for (int i = 0; labapp_running() && i < 5000; i++) { pause_worker(); labapp_tick(0); }
    require(!labapp_running(), "invalid-domain worker completes within five seconds");
    CHECK(strstr(labapp_run_error(), "unknown domain") != NULL, "failed calculation exposes its error");
    CHECK(stat(output, &after) == 0 && before.st_ino == after.st_ino && before.st_size == after.st_size,
          "failed calculation leaves the cached result intact");
    require(unlink(output) == 0, "remove failed-run fixture before follower checks");
    labapp_close();
}

/* Synthesize the publication boundary, without depending on a solver's speed or a timing race. */
static void *complete_worker(void *arg) {
    g.run_ok = true;
    g.finished = true;
    return NULL;
}

static void queue_completed_result(void) {
    g.running = true;
    g.finished = false;
    g.run_scenario = json_object();
    snprintf(g.run_name, sizeof g.run_name, "Synthetic completion");
    require(pthread_create(&g.worker, NULL, complete_worker, NULL) == 0, "start synthetic completion worker");
    for (int i = 0; !g.finished && i < 5000; i++) pause_worker();
    require(g.finished, "synthetic worker completes within five seconds");
}

static void completion_ownership(void) {
    snprintf(g.run_out, sizeof g.run_out, "%s/complete.lab", labapp_results_dir());
    fixture_result(g.run_out);
    labapp_select("examples/lab/micro_mixer.json");
    g.run_selection_serial = g.selection_serial;
    labapp_select("examples/lab/dam_break.json");
    queue_completed_result();
    labapp_tick(0);
    CHECK(!labapp_active() && !strcmp(labapp_scenario_domain(), "water") && labapp_run_notice()[0],
          "completion preserves a different preparation and announces Saved availability");
    g.follow_seen = time(NULL) - 1;
    labapp_follow(2);
    labapp_follow(2);
    CHECK(!labapp_active() && !strcmp(labapp_scenario_domain(), "water"), "Agentic follower does not reopen its own handled completion");

    g.run_selection_serial = g.selection_serial;
    labapp_close();
    queue_completed_result();
    labapp_tick(0);
    CHECK(!labapp_active() && !labapp_prepared(), "completion respects leaving the simulation workflow");

    labapp_select("examples/lab/micro_mixer.json");
    g.run_selection_serial = g.selection_serial;
    LabControl control;
    require(labapp_controls(&control, 1) == 1, "mixer exposes a control for the edit-ownership check");
    labapp_control_set(0, control.value == control.min ? control.max : control.min);
    queue_completed_result();
    labapp_tick(0);
    CHECK(!labapp_active() && labapp_prepared(), "completion preserves controls edited since the worker started");

    g.run_selection_serial = g.selection_serial;
    queue_completed_result();
    labapp_tick(0);
    CHECK(labapp_active() && !strcmp(labapp_title(), "Synthetic completion"), "unchanged selection automatically opens its result");
    CHECK(!labapp_run_notice()[0], "opening a result clears the completion notice");
    labapp_close();
}

static void publish_rollback(void) {
    /* A directory at the destination guarantees the final result rename fails after the snapshot is staged. */
    snprintf(g.run_out, sizeof g.run_out, "%s/blocked.lab", labapp_results_dir());
    require(mkdir(g.run_out, 0755) == 0, "create blocked result destination");
    snprintf(g.run_work, sizeof g.run_work, "%s/.publish-test", labapp_results_dir());
    fixture_result(g.run_work);
    char snapshot[1100];
    snprintf(snapshot, sizeof snapshot, "%s/blocked.json", labapp_results_dir());
    JsonValue *previous = json_object();
    json_set_string(previous, "marker", "original");
    require(json_write_file(snapshot, previous, JSON_PRETTY), "save original provenance");
    json_free(previous);
    g.run_scenario = json_object();
    json_set_string(g.run_scenario, "marker", "new");
    CHECK(!publish_run(), "blocked result publication fails");
    previous = json_read_file(snapshot, 1024, NULL);
    CHECK(!strcmp(json_get_str(previous, "marker", ""), "original"), "failed publication restores original provenance");
    json_free(previous);
    require(rmdir(g.run_out) == 0, "unblock result destination");
    CHECK(publish_run(), "unblocked publication succeeds");
    previous = json_read_file(snapshot, 1024, NULL);
    CHECK(!strcmp(json_get_str(previous, "marker", ""), "new"), "successful publication installs matching provenance");
    json_free(previous);
    json_free(g.run_scenario);
    g.run_scenario = NULL;
}

/* a stand-in for labrun: a process that lives until it is killed, its pid where openphysics-mcp lab_run writes it */
static pid_t fake_run(const char *name) {
    pid_t pid = fork();
    if (pid == 0) {
        sleep(120);
        _exit(0);
    }
    require(pid > 0, "start a stand-in run");
    char dir[1100], p[1200];
    snprintf(dir, sizeof dir, "%s/agent-runs", labapp_results_dir());
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/%s.pid", dir, name);
    FILE *f = fopen(p, "w");
    require(f != NULL, "write the run's pid file");
    fprintf(f, "%d\n", (int)pid);
    fclose(f);
    return pid;
}

static void grow_frame(LabWriter *w, double t, float a, float b, float c) {
    double xyz[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    float speed[3] = {a, b, c};
    lab_frame_begin(w, t);
    lab_part_points(w, "points", 3, xyz);
    lab_field(w, "speed", LAB_AT_NODE, 3, speed);
    require(lab_frame_end(w), "write a growing frame");
}

static void growing_run(void) {
    char err[256], path[1100], path2[1100], other[1100];
    snprintf(path, sizeof path, "%s/grow.lab", labapp_results_dir());
    snprintf(path2, sizeof path2, "%s/grow2.lab", labapp_results_dir());
    snprintf(other, sizeof other, "%s/other.lab", labapp_results_dir());
    labapp_follow(1.1); /* the follower takes results that appear from now on */
    sleep(2);
    pid_t pid = fake_run("grow");
    const char *hdr = "{\"domain\":\"flow\",\"title\":\"Growing\",\"fields\":{\"speed\":\"m/s\"}}";
    LabWriter *w = lab_create(path, hdr, err, sizeof err);
    require(w != NULL, err);
    grow_frame(w, 0.0, 0, 0.5, 1);
    labapp_follow(1.1);
    CHECK(strcmp(labapp_path(), path) != 0, "a computing result with one frame is not opened yet");
    grow_frame(w, 0.1, 0, 0.5, 1);
    labapp_follow(1.1);
    CHECK(labapp_active() && !strcmp(labapp_path(), path) && labapp_growing(), "a computing result is opened at its second frame");
    CHECK(g.nframes == 2 && g.frame == 1 && !g.playing, "opened paused on its newest frame");
    unsigned serial = labapp_open_serial();
    double lo0 = g.lo, hi0 = g.hi;
    grow_frame(w, 0.2, 0, 2.5, 5);
    labapp_follow(0.6);
    CHECK(g.nframes == 3 && g.frame == 2, "a frame appended: the count follows and the view moves to the newest");
    CHECK(g.lo <= lo0 && g.hi > hi0, "a wider frame widens the range");
    double lo1 = g.lo, hi1 = g.hi;
    grow_frame(w, 0.3, 0.4, 0.5, 0.6);
    labapp_follow(0.6);
    CHECK(g.nframes == 4 && g.frame == 3 && g.lo == lo1 && g.hi == hi1, "a narrower frame does not narrow the range");
    CHECK(labapp_open_serial() == serial && labapp_growing(), "the open's serial stays while it grows");
    require(lab_close(w), "close the growing result");
    kill(pid, SIGTERM);
    for (int k = 0; k < 200 && kill(pid, 0) == 0 && labapp_run_alive(pid, time(NULL)); k++) pause_worker();
    labapp_follow(0.6); /* ended but not reaped yet: a zombie, as labrun is under openphysics-mcp */
    waitpid(pid, NULL, 0);
    LabViewOpts o = g.opt;
    double lo = 0, hi = 0;
    o.first = 0, o.last = g.nframes - 1, o.every = g.nframes > 40 ? g.nframes / 20 : 1;
    labview_range(g.lf, &o, &lo, &hi);
    CHECK(!labapp_growing() && g.nframes == 4 && g.lo == lo && g.hi == hi && g.playing,
          "the run ended: no longer growing, the range over all frames, playing");
    /* another result opened by the person while a second run computes */
    pid_t pid2 = fake_run("grow2");
    LabWriter *w2 = lab_create(path2, hdr, err, sizeof err);
    require(w2 != NULL, err);
    grow_frame(w2, 0.0, 0, 1, 2);
    grow_frame(w2, 0.1, 0, 1, 2);
    labapp_follow(1.1);
    CHECK(!strcmp(labapp_path(), path2) && labapp_growing(), "a second computing run is followed");
    fixture_result(other);
    require(labapp_open(other), "open another result");
    CHECK(labapp_open_serial() != serial, "another open is another serial");
    grow_frame(w2, 0.2, 0, 1, 2);
    labapp_follow(1.1);
    labapp_follow(1.1);
    CHECK(!strcmp(labapp_path(), other) && !labapp_growing(), "the person is not taken back to the computing run");
    require(lab_close(w2), "close the second growing result");
    kill(pid2, SIGTERM);
    waitpid(pid2, NULL, 0);
    labapp_close();
    char p[1300];
    unlink(path), unlink(path2), unlink(other);
    snprintf(p, sizeof p, "%s/agent-runs/grow.pid", labapp_results_dir()), unlink(p);
    snprintf(p, sizeof p, "%s/agent-runs/grow2.pid", labapp_results_dir()), unlink(p);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) { return remove(path); }

static void newest_saved_results(void) {
    time_t first = time(NULL) + 100;
    for (int i = 0; i < 80; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/recent-%03d.lab", labapp_results_dir(), i);
        fixture_result(path);
        struct timeval tv[2] = {{first + i, 0}, {first + i, 0}};
        require(utimes(path, tv) == 0, "set ordering fixture timestamp");
    }
    char paths[5][1024], titles[5][120];
    CHECK(labapp_recent(paths, titles, 5) == 5, "Saved returns its requested display count");
    CHECK(strstr(paths[0], "recent-079.lab") && strstr(paths[4], "recent-075.lab"),
          "Saved retains the newest five across all 80+ files");
}

int main(void) {
    char home[] = "/tmp/openphysics-labapptest-XXXXXX", projects[1024];
    require(mkdtemp(home) != NULL, "create isolated home");
    require(setenv("HOME", home, 1) == 0, "isolate result paths from user work");
    snprintf(projects, sizeof projects, "%s/OpenPhysicsAI-Projects", home);
    require(mkdir(projects, 0755) == 0 && mkdir(labapp_results_dir(), 0755) == 0, "create isolated lab folder");
    prepare_and_controls();
    growing_run(); /* before the fixtures below, whose times would be newer than its frames */
    failed_run_keeps_cache(home);
    completion_ownership();
    publish_rollback();
    newest_saved_results();
    labapp_close();
    if (!failed) nftw(home, remove_entry, 16, FTW_DEPTH | FTW_PHYS); /* a passing test leaves nothing in /tmp */
    printf("labapptest: %d passed, %d failed; fixtures %s\n", passed, failed, failed ? home : "removed");
    return failed != 0;
}
