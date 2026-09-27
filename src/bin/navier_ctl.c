/* navier_ctl.c - command-line client for the navier-ctl control socket */
#include "../core/json.h"
#include "../ctl/cli_local.h"
#include "../ctl/engine.h"
#include "../net/ctlclient.h"
#include "../net/ctlserver.h"
#include "../net/netutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
            "usage: navier-ctl [--socket PATH | --tcp HOST:PORT [--token-file FILE]] [--timeout SECONDS] COMMAND\n"
            "       navier-ctl --embedded [--workspace DIR] [--allow-read DIR] [--allow-write DIR] COMMAND\n"
            "commands (server on a control socket):\n"
            "  hello                      protocol and server information\n"
            "  ops [--schemas]            list operations\n"
            "  call OP [JSON | @FILE | -] run an operation; prints the result (exit 0) or the error (exit 1)\n"
            "  raw LINE                   send one raw protocol line and print the reply\n"
            "  wait-ready [SECONDS]       wait until the server answers (exit 0) or give up (exit 3)\n"
            "commands without a server (an engine inside this process; study and doctor always run this way):\n"
            "  call OP [JSON | @FILE | -] [--wait]   run an operation; --wait follows a started job to its end\n"
            "  study check FILE                      check a comparison-study definition (exit 0 ready, 4 needs input, 5 not supported)\n"
            "  study run FILE [--dir DIR]            check, run and wait; prints the outcome and the report path\n"
            "  study replay DIR [--into DIR]         rerun a study from its study.json and stored inputs and compare (exit 6 when it differs)\n"
            "  study evidence DIR [--full]           print the evidence record\n"
            "  study report DIR                      print report.md\n"
            "  doctor [--json] [--no-solve]          check executables, libraries, writable folders, MCP stdio and a reference solve\n"
            "exit codes: 0 success, 1 operation error, 2 usage or transport error, 3 not ready or job failed, 4 study needs input, 5 not supported,\n"
            "            6 replay differs, 7 a diagnostic failed\n");
}

static char *read_all(FILE *f) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 2) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
    }
    if (buf) buf[len] = 0;
    return buf;
}

static void print_json(const JsonValue *v) {
    char *t = json_dump(v, JSON_PRETTY, NULL, NULL);
    if (t) printf("%s\n", t);
    free(t);
}

int main(int argc, char **argv) {
    char socket_path[512] = "", host[128] = "", token_file[512] = "";
    int port = 0, i = 1;
    double timeout_s = 600;
    bool embedded = false;
    EngineConfig ec;
    engine_config_default(&ec);
    char cerr[600];
    ctl_default_socket_path(socket_path, sizeof socket_path);
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--embedded")) embedded = true;
        else if (!strcmp(argv[i], "--workspace") && i + 1 < argc) {
            if (!engine_config_set_workspace(&ec, argv[++i], cerr, sizeof cerr)) {
                fprintf(stderr, "navier-ctl: %s\n", cerr);
                return 2;
            }
        } else if ((!strcmp(argv[i], "--allow-read") || !strcmp(argv[i], "--allow-write")) && i + 1 < argc) {
            bool write = !strcmp(argv[i], "--allow-write");
            if (!engine_config_add_root(&ec, write, argv[++i], cerr, sizeof cerr)) {
                fprintf(stderr, "navier-ctl: %s\n", cerr);
                return 2;
            }
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) snprintf(socket_path, sizeof socket_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tcp") && i + 1 < argc) {
            const char *v = argv[++i], *colon = strrchr(v, ':');
            if (!colon) {
                fprintf(stderr, "navier-ctl: --tcp expects HOST:PORT\n");
                return 2;
            }
            snprintf(host, sizeof host, "%.*s", (int)(colon - v), v);
            port = atoi(colon + 1);
        } else if (!strcmp(argv[i], "--token-file") && i + 1 < argc) snprintf(token_file, sizeof token_file, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "navier-ctl: unknown option %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    if (embedded || !strcmp(argv[i], "study") || !strcmp(argv[i], "doctor")) {
        if (!strcmp(argv[i], "call") || !strcmp(argv[i], "study") || !strcmp(argv[i], "doctor")) return cli_local_main(argc, argv, i, &ec);
        fprintf(stderr, "navier-ctl: %s needs a server; without one use call, study or doctor\n", argv[i]);
        return 2;
    }
    const char *cmd = argv[i++];
    int timeout_ms = timeout_s > 0 ? (int)(timeout_s * 1000) : -1;
    char err[1024];

    if (!strcmp(cmd, "wait-ready")) {
        double wait = i < argc ? atof(argv[i]) : 10;
        long long deadline = net_now_ms() + (long long)(wait * 1000);
        do {
            CtlClient *c = host[0] ? NULL : ctl_connect_unix(socket_path, err, sizeof err);
            if (c) {
                JsonValue *r = ctl_request(c, "ping", NULL, 2000, err, sizeof err);
                bool ok = r && json_get(r, "result");
                json_free(r);
                ctl_close(c);
                if (ok) return 0;
            }
            usleep(100000);
        } while (net_now_ms() < deadline);
        fprintf(stderr, "navier-ctl: server not ready: %s\n", err);
        return 3;
    }

    CtlClient *c;
    if (host[0]) {
        char token[256] = "";
        if (token_file[0]) {
            FILE *tf = fopen(token_file, "r");
            if (!tf || !fgets(token, sizeof token, tf)) {
                fprintf(stderr, "navier-ctl: cannot read token file %s\n", token_file);
                if (tf) fclose(tf);
                return 2;
            }
            fclose(tf);
            token[strcspn(token, "\r\n")] = 0;
        }
        c = ctl_connect_tcp(host, port, token[0] ? token : NULL, timeout_ms, err, sizeof err);
    } else {
        c = ctl_connect_unix(socket_path, err, sizeof err);
    }
    if (!c) {
        fprintf(stderr, "navier-ctl: %s\n", err);
        return 2;
    }
    int status = 0;
    if (!strcmp(cmd, "hello") || !strcmp(cmd, "ops")) {
        JsonValue *r = ctl_request(c, !strcmp(cmd, "hello") ? "hello" : "ops.list", NULL, timeout_ms, err, sizeof err);
        if (!r) {
            fprintf(stderr, "navier-ctl: %s\n", err);
            status = 2;
        } else if (!strcmp(cmd, "ops") && !(i < argc && !strcmp(argv[i], "--schemas"))) {
            const JsonValue *ops = json_get(json_get(r, "result"), "operations");
            for (size_t k = 0; k < json_len(ops); k++) {
                const JsonValue *o = json_at(ops, k);
                printf("%-24s %-9s %s\n", json_get_str(o, "name", ""), json_get_str(o, "kind", ""), json_get_str(o, "title", ""));
            }
        } else {
            print_json(json_get(r, "result") ? json_get(r, "result") : r);
        }
        json_free(r);
    } else if (!strcmp(cmd, "call")) {
        if (i >= argc) {
            usage(stderr);
            ctl_close(c);
            return 2;
        }
        const char *op = argv[i++];
        char *text = NULL;
        if (i < argc && !strcmp(argv[i], "-")) text = read_all(stdin);
        else if (i < argc && argv[i][0] == '@') {
            FILE *f = fopen(argv[i] + 1, "rb");
            text = f ? read_all(f) : NULL;
            if (f) fclose(f);
        } else if (i < argc) text = strdup(argv[i]);
        JsonValue *params = NULL;
        if (text) {
            JsonError jerr;
            params = json_parse(text, strlen(text), NULL, &jerr);
            if (!params) {
                fprintf(stderr, "navier-ctl: invalid JSON parameters: %s (line %d, column %d)\n", jerr.message, jerr.line, jerr.column);
                free(text);
                ctl_close(c);
                return 2;
            }
            free(text);
        }
        JsonValue *r = ctl_request(c, op, params, timeout_ms, err, sizeof err);
        json_free(params);
        if (!r) {
            fprintf(stderr, "navier-ctl: %s\n", err);
            status = 2;
        } else if (json_get(r, "result")) {
            JsonValue *res = json_get(r, "result");
            /* do not flood the terminal with base64 image data */
            const JsonValue *imgs = json_get(res, "images");
            for (size_t k = 0; k < json_len(imgs); k++) json_remove(json_at(imgs, k), "data");
            print_json(res);
        } else {
            print_json(json_get(r, "error"));
            status = 1;
        }
        json_free(r);
    } else if (!strcmp(cmd, "raw")) {
        if (i >= argc) {
            usage(stderr);
            ctl_close(c);
            return 2;
        }
        size_t n = strlen(argv[i]);
        char *line = malloc(n + 2);
        memcpy(line, argv[i], n);
        line[n] = '\n';
        if (!ctl_send_raw(c, line, n + 1, timeout_ms)) {
            fprintf(stderr, "navier-ctl: send failed\n");
            status = 2;
        } else {
            JsonValue *r = ctl_read_message(c, timeout_ms, err, sizeof err);
            if (r) print_json(r);
            else fprintf(stderr, "navier-ctl: %s\n", err), status = 2;
            json_free(r);
        }
        free(line);
    } else {
        fprintf(stderr, "navier-ctl: unknown command %s\n", cmd);
        usage(stderr);
        status = 2;
    }
    ctl_close(c);
    return status;
}
