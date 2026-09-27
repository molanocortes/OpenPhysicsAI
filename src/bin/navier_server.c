/* navier_server.c - headless NAVIER-AM server: the engine behind a local control socket (no OpenGL, no UI).
 * Keeps projects and jobs alive independently of any AI client; navier-mcp can bridge to it. */
#include "../common.h"
#include "../ctl/engine.h"
#include "../ctl/ops.h"
#include "../net/ctlserver.h"
#include "../net/netutil.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
            "usage: navier-server [options]\n"
            "  --socket PATH           Unix socket (default ~/.navier/run/control.sock)\n"
            "  --no-socket             disable the Unix socket (requires --tcp)\n"
            "  --tcp HOST:PORT         also listen on TCP; loopback addresses only, token authentication\n"
            "  --token-file PATH       where the generated TCP token is written (default ~/.navier/run/token)\n"
            "  --workspace DIR         default parent directory of new projects (default ~/NAVIER-Projects)\n"
            "  --allow-read DIR        additional root for input files (repeatable)\n"
            "  --allow-write DIR       additional root for projects and exports (repeatable)\n"
            "  --max-connections N     simultaneous clients (default 16)\n"
            "  --idle-timeout SECONDS  close silent connections (default 1800)\n"
            "  --max-message-bytes N   largest accepted request line (default 16777216)\n"
            "  --max-stl-bytes N       largest STL file (default 1073741824)\n"
            "  --max-triangles N       largest STL triangle count (default 5000000)\n"
            "  --ready-file PATH       write \"ready\" to PATH once listening (for scripts)\n"
            "  --version, --help\n");
}

static const char *arg(int *i, int argc, char **argv) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "navier-server: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

int main(int argc, char **argv) {
    EngineConfig ec;
    engine_config_default(&ec);
    CtlServerConfig sc;
    ctl_server_config_default(&sc);
    char err[1024], ready_file[1024] = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--socket")) {
            snprintf(sc.unix_path, sizeof sc.unix_path, "%s", arg(&i, argc, argv));
        } else if (!strcmp(a, "--no-socket")) {
            sc.unix_path[0] = 0;
        } else if (!strcmp(a, "--tcp")) {
            const char *v = arg(&i, argc, argv);
            const char *colon = strrchr(v, ':');
            if (!colon) {
                fprintf(stderr, "navier-server: --tcp expects HOST:PORT\n");
                return 2;
            }
            size_t hl = (size_t)(colon - v);
            if (hl >= sizeof sc.tcp_host) hl = sizeof sc.tcp_host - 1;
            if (v[0] == '[' && hl >= 2 && v[hl - 1] == ']') snprintf(sc.tcp_host, sizeof sc.tcp_host, "%.*s", (int)hl - 2, v + 1);
            else snprintf(sc.tcp_host, sizeof sc.tcp_host, "%.*s", (int)hl, v);
            sc.tcp_port = atoi(colon + 1);
            sc.tcp_enabled = true;
        } else if (!strcmp(a, "--token-file")) {
            snprintf(sc.token_file, sizeof sc.token_file, "%s", arg(&i, argc, argv));
        } else if (!strcmp(a, "--workspace")) {
            if (!engine_config_set_workspace(&ec, arg(&i, argc, argv), err, sizeof err)) {
                fprintf(stderr, "navier-server: %s\n", err);
                return 2;
            }
        } else if (!strcmp(a, "--allow-read") || !strcmp(a, "--allow-write")) {
            if (!engine_config_add_root(&ec, !strcmp(a, "--allow-write"), arg(&i, argc, argv), err, sizeof err)) {
                fprintf(stderr, "navier-server: %s\n", err);
                return 2;
            }
        } else if (!strcmp(a, "--max-connections")) {
            sc.max_connections = atoi(arg(&i, argc, argv));
        } else if (!strcmp(a, "--idle-timeout")) {
            sc.idle_timeout_ms = (int)(atof(arg(&i, argc, argv)) * 1000);
        } else if (!strcmp(a, "--max-message-bytes")) {
            sc.max_message_bytes = (size_t)strtoull(arg(&i, argc, argv), NULL, 10);
        } else if (!strcmp(a, "--max-stl-bytes")) {
            ec.max_stl_bytes = strtoull(arg(&i, argc, argv), NULL, 10);
        } else if (!strcmp(a, "--max-triangles")) {
            ec.max_triangles = (uint32_t)strtoul(arg(&i, argc, argv), NULL, 10);
        } else if (!strcmp(a, "--ready-file")) {
            snprintf(ready_file, sizeof ready_file, "%s", arg(&i, argc, argv));
        } else if (!strcmp(a, "--version")) {
            printf("navier-server %s (contract %s, protocol %s/%d)\n", NAVIER_AM_VERSION, "see capabilities_get", CTL_PROTOCOL_NAME, CTL_PROTOCOL_VERSION);
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "navier-server: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (sc.tcp_enabled && !sc.token_file[0]) {
        const char *home = getenv("HOME");
        if (home) snprintf(sc.token_file, sizeof sc.token_file, "%s/.navier/run/token", home);
    }

    /* termination signals are handled synchronously by the main thread */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);

    Engine *e = engine_create(&ec, err, sizeof err);
    if (!e) {
        fprintf(stderr, "navier-server: %s\n", err);
        return 1;
    }
    CtlServer *s = ctl_server_start(e, &sc, err, sizeof err);
    if (!s) {
        fprintf(stderr, "navier-server: %s\n", err);
        engine_destroy(e);
        return 1;
    }
    if (sc.unix_path[0]) LOGOK("navier-server %s: listening on %s", NAVIER_AM_VERSION, sc.unix_path);
    if (sc.tcp_enabled)
        LOGOK("navier-server: TCP %s:%d (token in %s)", sc.tcp_host, ctl_server_tcp_port(s), sc.token_file[0] ? sc.token_file : "(not written)");
    LOGI("workspace %s; %d operations; stop with Ctrl-C or SIGTERM", ec.workspace, ops_count());
    if (ready_file[0]) {
        char text[128];
        snprintf(text, sizeof text, "ready %d\n", sc.tcp_enabled ? ctl_server_tcp_port(s) : 0);
        if (!net_write_private_file(ready_file, text, err, sizeof err)) LOGW("%s", err);
    }
    int sig = 0;
    sigwait(&set, &sig);
    LOGI("signal %d received: closing connections", sig);
    ctl_server_stop(s);
    engine_destroy(e);
    LOGOK("navier-server stopped");
    return 0;
}
