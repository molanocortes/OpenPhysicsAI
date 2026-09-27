/* ctltest.c - control socket protocol tests: framing, partial reads and writes, pipelining, malformed requests,
 * size limits, timeouts, connection limits, disconnects, idempotent retries across reconnects, TCP authentication
 * and listener safety checks. The server runs in-process on temporary socket paths.
 *   make build/ctltest && ./build/ctltest */
#include "../src/core/errors.h"
#include "../src/ctl/engine.h"
#include "../src/ctl/ops.h"
#include "../src/net/ctlclient.h"
#include "../src/net/ctlserver.h"
#include "../src/net/netutil.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond) g_pass++;                                                                                           \
        else {                                                                                                        \
            g_fail++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static char g_sock[256];

static CtlClient *conn(void) {
    char err[256];
    CtlClient *c = ctl_connect_unix(g_sock, err, sizeof err);
    if (!c) printf("  connect failed: %s\n", err);
    return c;
}

static bool send_str(CtlClient *c, const char *s) { return ctl_send_raw(c, s, strlen(s), 2000); }

static JsonValue *recv_msg(CtlClient *c, int timeout_ms) {
    char err[256];
    return ctl_read_message(c, timeout_ms, err, sizeof err);
}

static int err_code(const JsonValue *resp) { return (int)json_get_num(json_get(resp, "error"), "code", 0); }
static const char *err_name(const JsonValue *resp) { return json_get_str(json_get(json_get(resp, "error"), "data"), "error_code", ""); }

/* true when the server closed the connection (EOF) within timeout */
static bool closed_by_server(CtlClient *c, int timeout_ms) {
    char err[256];
    long long deadline = net_now_ms() + timeout_ms;
    while (net_now_ms() < deadline) {
        JsonValue *m = ctl_read_message(c, (int)(deadline - net_now_ms()), err, sizeof err);
        if (!m) return strstr(err, "closed") != NULL;
        json_free(m);
    }
    return false;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *tmp = getenv("TMPDIR");
    char base[256], ws[300];
    /* short base path: Unix socket paths are limited to about 100 bytes */
    snprintf(base, sizeof base, "/tmp/nvct%ld", (long)getpid());
    snprintf(ws, sizeof ws, "%s/ws", base);
    snprintf(g_sock, sizeof g_sock, "%s/run/ctl.sock", base);
    (void)tmp;

    EngineConfig ec;
    engine_config_default(&ec);
    char err[1024];
    CHECK(engine_config_set_workspace(&ec, ws, err, sizeof err), "workspace");
    Engine *e = engine_create(&ec, err, sizeof err);
    CHECK(e != NULL, "engine: %s", err);
    if (!e) return 1;

    CtlServerConfig sc;
    ctl_server_config_default(&sc);
    snprintf(sc.unix_path, sizeof sc.unix_path, "%s", g_sock);
    sc.max_connections = 3;
    sc.idle_timeout_ms = 1500;
    sc.message_timeout_ms = 400;
    sc.write_timeout_ms = 2000;
    sc.max_message_bytes = 8192;
    CtlServer *s = ctl_server_start(e, &sc, err, sizeof err);
    CHECK(s != NULL, "server start: %s", err);
    if (!s) return 1;

    printf("== listener security\n");
    struct stat st;
    CHECK(stat(g_sock, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600, "socket mode 0600 (%o)", st.st_mode & 0777);
    char dir[300];
    snprintf(dir, sizeof dir, "%s/run", base);
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700, "socket directory mode 0700 (%o)", st.st_mode & 0777);
    CtlServer *dup = ctl_server_start(e, &sc, err, sizeof err);
    CHECK(!dup && strstr(err, "already listening"), "second server on the same path refused: %s", err);
    if (dup) ctl_server_stop(dup);
    CtlServerConfig bad = sc;
    snprintf(bad.unix_path, sizeof bad.unix_path, "%s/run/notasocket", base);
    FILE *f = fopen(bad.unix_path, "w");
    if (f) fclose(f);
    dup = ctl_server_start(e, &bad, err, sizeof err);
    CHECK(!dup && strstr(err, "not a socket"), "regular file at the socket path is not replaced: %s", err);
    if (dup) ctl_server_stop(dup);
    bad = sc;
    bad.unix_path[0] = 0;
    bad.tcp_enabled = true;
    snprintf(bad.tcp_host, sizeof bad.tcp_host, "0.0.0.0");
    dup = ctl_server_start(e, &bad, err, sizeof err);
    CHECK(!dup && strstr(err, "loopback"), "non-loopback TCP bind refused: %s", err);
    if (dup) ctl_server_stop(dup);

    printf("== requests and responses\n");
    CtlClient *c = conn();
    JsonValue *r = ctl_request(c, "hello", NULL, 2000, err, sizeof err);
    const JsonValue *res = json_get(r, "result");
    CHECK(res && !strcmp(json_get_str(res, "protocol", ""), "navier-ctl") && json_get_int(res, "protocol_version", 0) == 1 &&
              !json_get_bool(res, "auth_required", true),
          "hello");
    json_free(r);
    r = ctl_request(c, "ops.list", NULL, 2000, err, sizeof err);
    const JsonValue *ops = json_get(json_get(r, "result"), "operations");
    bool found = false;
    for (size_t i = 0; i < json_len(ops); i++)
        if (!strcmp(json_get_str(json_at(ops, i), "name", ""), "geometry_import"))
            found = !strcmp(json_get_str(json_get(json_at(ops, i), "input_schema"), "type", ""), "object");
    CHECK(found, "ops.list includes geometry_import with its schema");
    json_free(r);
    r = ctl_request(c, "capabilities_get", NULL, 2000, err, sizeof err);
    CHECK(json_get_bool(json_get(r, "result"), "ok", false), "operation name as method");
    json_free(r);
    JsonValue *p = json_object();
    json_set_string(p, "op", "capabilities_get");
    r = ctl_request(c, "ops.call", p, 2000, err, sizeof err);
    CHECK(json_get_bool(json_get(r, "result"), "ok", false), "ops.call");
    json_free(r);
    json_free(p);
    r = ctl_request(c, "project_inspect", NULL, 2000, err, sizeof err);
    CHECK(err_code(r) == nv_err_rpc_code(NV_ERR_NO_PROJECT) && !strcmp(err_name(r), "NO_PROJECT") &&
              json_get(json_get(json_get(r, "error"), "data"), "revision") && json_get_str(json_get(json_get(r, "error"), "data"), "hint", NULL),
          "operation error carries code, hint and revision");
    json_free(r);
    p = json_object();
    json_set_number(p, "name", 12);
    r = ctl_request(c, "project_create", p, 2000, err, sizeof err);
    CHECK(err_code(r) == -32602 && json_len(json_get(json_get(json_get(json_get(r, "error"), "data"), "details"), "issues")) > 0, "schema errors listed");
    json_free(r);
    json_free(p);

    printf("== malformed messages keep the connection usable\n");
    static const struct {
        const char *line;
        int code;
    } bad_lines[] = {
        {"{bad json\n", -32700},
        {"[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]\n", -32600},
        {"{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}\n", -32600},
        {"{\"jsonrpc\":\"2.0\",\"id\":1}\n", -32600},
        {"{\"jsonrpc\":\"2.0\",\"id\":{\"x\":1},\"method\":\"ping\"}\n", -32600},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":[1]}\n", -32602},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"no_such_method\"}\n", -32601},
        {"\"just a string\"\n", -32600},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"p\xFFing\"}\n", -32700},
    };
    for (size_t i = 0; i < sizeof bad_lines / sizeof bad_lines[0]; i++) {
        send_str(c, bad_lines[i].line);
        JsonValue *m = recv_msg(c, 2000);
        CHECK(m && err_code(m) == bad_lines[i].code, "bad line %zu -> %d (got %d)", i, bad_lines[i].code, m ? err_code(m) : 0);
        json_free(m);
    }
    r = ctl_request(c, "ping", NULL, 2000, err, sizeof err);
    CHECK(r && json_get(r, "result"), "still usable after malformed input");
    json_free(r);

    printf("== framing\n");
    send_str(c, "{\"jsonrpc\":\"2.0\",");
    usleep(30000);
    send_str(c, "\"id\":\"split\",\"meth");
    usleep(30000);
    send_str(c, "od\":\"ping\"}\n");
    JsonValue *m = recv_msg(c, 2000);
    CHECK(m && !strcmp(json_get_str(m, "id", ""), "split") && json_get(m, "result"), "request split over three writes");
    json_free(m);
    send_str(c, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\r\n\n  \n{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n");
    for (int k = 1; k <= 3; k++) {
        m = recv_msg(c, 2000);
        CHECK(m && json_get_int(m, "id", 0) == k, "pipelined response %d in order (CRLF and blank lines tolerated)", k);
        json_free(m);
    }
    send_str(c, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
    m = recv_msg(c, 2000);
    CHECK(m && json_get_int(m, "id", 0) == 7, "notifications get no response");
    json_free(m);
    send_str(c, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n");
    m = recv_msg(c, 2000);
    JsonValue *m2 = recv_msg(c, 2000);
    CHECK(m && m2 && json_get_int(m, "id", 0) == 9 && json_get_int(m2, "id", 0) == 9, "duplicate ids each receive a response");
    json_free(m);
    json_free(m2);
    ctl_close(c);

    printf("== limits and timeouts\n");
    c = conn();
    char big[9100];
    memset(big, 'x', sizeof big);
    ctl_send_raw(c, big, sizeof big, 2000);
    m = recv_msg(c, 2000);
    CHECK(m && !strcmp(err_name(m), "RESOURCE_LIMIT"), "oversized message rejected (%s)", m ? err_name(m) : "no reply");
    json_free(m);
    CHECK(closed_by_server(c, 2000), "connection closed after an oversized message");
    ctl_close(c);

    c = conn();
    send_str(c, "{\"jsonrpc\":\"2.0\"");
    long long t0 = net_now_ms();
    m = recv_msg(c, 3000);
    CHECK(m && !strcmp(err_name(m), "TIMEOUT") && net_now_ms() - t0 >= 350, "incomplete message times out (%lld ms)", net_now_ms() - t0);
    json_free(m);
    CHECK(closed_by_server(c, 2000), "closed after the message timeout");
    ctl_close(c);

    c = conn();
    t0 = net_now_ms();
    CHECK(closed_by_server(c, 4000) && net_now_ms() - t0 >= 1400, "idle connection closed after %lld ms", net_now_ms() - t0);
    ctl_close(c);

    CtlClient *c1 = conn(), *c2 = conn(), *c3 = conn();
    usleep(100000);
    CtlClient *c4 = conn();
    m = c4 ? recv_msg(c4, 2000) : NULL;
    CHECK(m && !strcmp(err_name(m), "BUSY"), "connection limit enforced (%s)", m ? err_name(m) : "no reply");
    json_free(m);
    ctl_close(c4);
    ctl_close(c1);
    ctl_close(c2);
    ctl_close(c3);

    printf("== disconnects and retries\n");
    c = conn();
    send_str(c, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"project_create\",\"params\":{\"name\":\"gone\",\"idempotency_key\":\"k-gone\"}}\n");
    ctl_close(c); /* the client disappears without reading the response */
    usleep(300000);
    c = conn();
    p = json_object();
    json_set_string(p, "name", "gone");
    json_set_string(p, "idempotency_key", "k-gone");
    r = ctl_request(c, "project_create", p, 3000, err, sizeof err);
    res = json_get(r, "result");
    CHECK(res && json_get_bool(res, "ok", false) && json_get_bool(res, "replayed", false),
          "retry after a disconnect replays the completed request instead of repeating it");
    json_free(r);
    json_free(p);
    r = ctl_request(c, "project_inspect", NULL, 3000, err, sizeof err);
    CHECK(json_get(r, "result"), "server healthy after the disconnect");
    json_free(r);
    ctl_close(c);
    usleep(400000);
    CHECK(ctl_server_connection_count(s) == 0, "all connection threads finished (%d left)", ctl_server_connection_count(s));
    ctl_server_stop(s);
    CHECK(access(g_sock, F_OK) != 0, "socket file removed on stop");

    printf("== TCP with token authentication\n");
    CtlServerConfig tc;
    ctl_server_config_default(&tc);
    tc.unix_path[0] = 0;
    tc.tcp_enabled = true;
    tc.tcp_port = 0;
    snprintf(tc.token, sizeof tc.token, "secret-token-0123456789");
    CtlServer *ts = ctl_server_start(e, &tc, err, sizeof err);
    CHECK(ts != NULL, "tcp server: %s", err);
    if (ts) {
        int port = ctl_server_tcp_port(ts);
        CtlClient *tcpc = ctl_connect_tcp("127.0.0.1", port, NULL, 2000, err, sizeof err);
        r = tcpc ? ctl_request(tcpc, "hello", NULL, 2000, err, sizeof err) : NULL;
        CHECK(r && json_get_bool(json_get(r, "result"), "auth_required", false) && !json_get_bool(json_get(r, "result"), "authenticated", true), "hello reports auth required");
        json_free(r);
        r = tcpc ? ctl_request(tcpc, "ping", NULL, 2000, err, sizeof err) : NULL;
        CHECK(r && !strcmp(err_name(r), "PERMISSION_DENIED"), "unauthenticated request refused");
        json_free(r);
        p = json_object();
        json_set_string(p, "token", "wrong");
        r = tcpc ? ctl_request(tcpc, "auth", p, 2000, err, sizeof err) : NULL;
        CHECK(r && !strcmp(err_name(r), "PERMISSION_DENIED"), "wrong token refused");
        json_free(r);
        json_set_string(p, "token", "secret-token-0123456789");
        r = tcpc ? ctl_request(tcpc, "auth", p, 2000, err, sizeof err) : NULL;
        CHECK(r && json_get_bool(json_get(r, "result"), "authenticated", false), "correct token accepted");
        json_free(r);
        r = tcpc ? ctl_request(tcpc, "ping", NULL, 2000, err, sizeof err) : NULL;
        CHECK(r && json_get(r, "result"), "authenticated request served");
        json_free(r);
        ctl_close(tcpc);
        tcpc = ctl_connect_tcp("127.0.0.1", port, NULL, 2000, err, sizeof err);
        json_set_string(p, "token", "bad");
        for (int k = 0; k < 3 && tcpc; k++) {
            r = ctl_request(tcpc, "auth", p, 2000, err, sizeof err);
            json_free(r);
        }
        CHECK(tcpc && closed_by_server(tcpc, 2000), "connection closed after three failed authentications");
        ctl_close(tcpc);
        json_free(p);
        ctl_server_stop(ts);
    }

    engine_destroy(e);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0) printf("  (cleanup failed)\n");
    printf("\n%s: %d passed, %d failed\n", g_fail ? "CONTROL SOCKET TESTS FAILED" : "ALL CONTROL SOCKET TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
