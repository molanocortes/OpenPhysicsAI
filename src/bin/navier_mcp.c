/* navier_mcp.c - MCP server for NAVIER-AM over stdio (newline-delimited JSON-RPC)
 *
 * Modes
 *   --connect [PATH]  bridge to a running navier-server (or GUI) on a control socket: projects and jobs outlive the
 *                     AI client and can be watched in the viewer
 *   --embedded        run the engine inside this process
 *   (default)         connect to ~/.navier/run/control.sock when a server answers there, otherwise embedded
 * stdout carries only MCP messages: file descriptor 1 is redirected to stderr at start-up, so no library output can
 * corrupt the protocol stream. Logs go to stderr. */
#include "../common.h"
#include "../ctl/engine.h"
#include "../ctl/ops.h"
#include "../net/ctlclient.h"
#include "../net/ctlserver.h"
#include "../net/mcp.h"
#include "../net/netutil.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void usage(FILE *f) {
    fprintf(f, "usage: navier-mcp [--connect [SOCKET] | --embedded] [--workspace DIR] [--allow-read DIR] [--allow-write DIR]\n"
               "MCP server on stdio. See docs/mcp-clients.md for Claude Code and Codex configuration.\n");
}

int main(int argc, char **argv) {
    enum { AUTO, CONNECT, EMBEDDED } mode = AUTO;
    char socket_path[512] = "", err[1024];
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
                fprintf(stderr, "navier-mcp: %s\n", err);
                return 2;
            }
        } else if ((!strcmp(a, "--allow-read") || !strcmp(a, "--allow-write")) && i + 1 < argc) {
            if (!engine_config_add_root(&ec, !strcmp(a, "--allow-write"), argv[++i], err, sizeof err)) {
                fprintf(stderr, "navier-mcp: %s\n", err);
                return 2;
            }
        } else if (!strcmp(a, "--version")) {
            fprintf(stdout, "navier-mcp %s (MCP %s)\n", NAVIER_AM_VERSION, MCP_LATEST_VERSION);
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "navier-mcp: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    int proto_fd = dup(STDOUT_FILENO);
    if (proto_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        fprintf(stderr, "navier-mcp: cannot set up the protocol stream\n");
        return 1;
    }
    net_set_cloexec(proto_fd);
    signal(SIGPIPE, SIG_IGN);

    McpBackend be = {0};
    Embedded emb = {NULL};
    Bridge br = {{0}, NULL};
    if (mode != EMBEDDED) {
        if (!socket_path[0]) ctl_default_socket_path(socket_path, sizeof socket_path);
        CtlClient *c = socket_path[0] ? ctl_connect_unix(socket_path, err, sizeof err) : NULL;
        if (c) {
            br.client = c;
            snprintf(br.path, sizeof br.path, "%s", socket_path);
            be.list_ops = bridge_list;
            be.call_op = bridge_call;
            be.ctx = &br;
            LOGI("navier-mcp: bridging to the server at %s", socket_path);
        } else if (mode == CONNECT) {
            LOGE("navier-mcp: %s", socket_path[0] ? err : "no socket path");
            return 1;
        }
    }
    if (!be.ctx) {
        emb.engine = engine_create(&ec, err, sizeof err);
        if (!emb.engine) {
            LOGE("navier-mcp: %s", err);
            return 1;
        }
        be.list_ops = embedded_list;
        be.call_op = embedded_call;
        be.ctx = &emb;
        LOGI("navier-mcp: embedded engine, workspace %s", ec.workspace);
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
