/* ctlserver.c - Unix/TCP control socket: framing, limits, timeouts, authentication, JSON-RPC dispatch */
#include "ctlserver.h"
#include "../ctl/ops.h"
#include "netutil.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

bool ctl_default_socket_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    int n = snprintf(out, cap, "%s/.navier/run/control.sock", home);
    return n > 0 && (size_t)n < cap;
}

void ctl_server_config_default(CtlServerConfig *c) {
    memset(c, 0, sizeof *c);
    ctl_default_socket_path(c->unix_path, sizeof c->unix_path);
    snprintf(c->tcp_host, sizeof c->tcp_host, "127.0.0.1");
    c->max_connections = 16;
    c->idle_timeout_ms = 30 * 60 * 1000;
    c->message_timeout_ms = 30 * 1000;
    c->write_timeout_ms = 30 * 1000;
    c->max_message_bytes = (size_t)16 << 20;
}

typedef struct Conn {
    struct CtlServer *srv;
    int fd;
    pthread_t thread;
    bool done;
    CtlSession st;
    struct Conn *next;
} Conn;

struct CtlServer {
    Engine *engine;
    CtlServerConfig cfg;
    int unix_fd, tcp_fd;
    int wake[2];
    int tcp_port;
    pthread_t accept_thread;
    bool accept_started;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    Conn *conns;
    int nconns;
    bool stopping;
    char bound_path[512];
};

/* ---- JSON-RPC helpers -------------------------------------------------------------------------- */

JsonValue *ctl_error_response(const JsonValue *id, int code, const char *message, const char *error_code, const char *hint) {
    JsonValue *r = json_object();
    json_set_string(r, "jsonrpc", "2.0");
    json_set(r, "id", id ? json_clone(id) : json_null());
    JsonValue *e = json_set_object(r, "error");
    json_set_int(e, "code", code);
    json_set_string(e, "message", message);
    if (error_code || hint) {
        JsonValue *d = json_set_object(e, "data");
        if (error_code) json_set_string(d, "error_code", error_code);
        if (hint) json_set_string(d, "hint", hint);
    }
    return r;
}

static JsonValue *result_response(const JsonValue *id, JsonValue *result) {
    JsonValue *r = json_object();
    json_set_string(r, "jsonrpc", "2.0");
    json_set(r, "id", json_clone(id));
    json_set(r, "result", result ? result : json_object());
    return r;
}

static JsonValue *op_error_response(const JsonValue *id, const OpResult *res) {
    const char *code = json_get_str(res->error, "code", "INTERNAL_ERROR");
    JsonValue *r = ctl_error_response(id, nv_err_rpc_code(nv_err_from_name(code)), json_get_str(res->error, "message", "operation failed"), code,
                                      json_get_str(res->error, "hint", NULL));
    JsonValue *data = json_get(json_get(r, "error"), "data");
    const JsonValue *details = json_get(res->error, "details");
    if (details) json_set(data, "details", json_clone(details));
    json_set_int(data, "revision", (long long)res->revision);
    return r;
}

static bool token_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < la && i < lb; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0 && la > 0;
}

static JsonValue *hello_result(const CtlSession *st) {
    JsonValue *o = json_object();
    json_set_string(o, "protocol", CTL_PROTOCOL_NAME);
    json_set_int(o, "protocol_version", CTL_PROTOCOL_VERSION);
    json_set_string(o, "server", "navier-am");
    json_set_string(o, "server_version", NAVIER_AM_VERSION);
    json_set_string(o, "contract_version", ops_contract_version());
    json_set_bool(o, "auth_required", st->auth_required);
    json_set_bool(o, "authenticated", !st->auth_required || st->authenticated);
    JsonValue *m = json_set_array(o, "methods");
    static const char *METHODS[] = {"hello", "auth", "ping", "ops.list", "ops.call", NULL};
    for (int i = 0; METHODS[i]; i++) json_push(m, json_string(METHODS[i]));
    json_set_string(o, "operation_methods", "every operation name is also a method, e.g. {\"method\": \"geometry_import\", \"params\": {...}}");
    return o;
}

JsonValue *ctl_handle_message(Engine *e, CtlSession *st, const JsonValue *msg) {
    if (msg->type == JSON_ARRAY)
        return ctl_error_response(NULL, -32600, "batch requests are not supported by navier-ctl/1: send one request per line", "INVALID_REQUEST", NULL);
    if (msg->type != JSON_OBJECT) return ctl_error_response(NULL, -32600, "a message must be a JSON object", "INVALID_REQUEST", NULL);
    const JsonValue *id = json_get(msg, "id");
    if (id && !(id->type == JSON_STRING || json_is_integer(id)))
        return ctl_error_response(NULL, -32600, "id must be a string or an integer", "INVALID_REQUEST", NULL);
    if (strcmp(json_get_str(msg, "jsonrpc", ""), "2.0") != 0)
        return ctl_error_response(id, -32600, "jsonrpc must be \"2.0\"", "INVALID_REQUEST", NULL);
    const char *method = json_get_str(msg, "method", NULL);
    if (!method) {
        if (json_get(msg, "result") || json_get(msg, "error")) return NULL; /* a stray response: nothing to answer */
        return ctl_error_response(id, -32600, "missing method", "INVALID_REQUEST", NULL);
    }
    const JsonValue *params = json_get(msg, "params");
    if (params && params->type != JSON_OBJECT && params->type != JSON_NULL)
        return id ? ctl_error_response(id, -32602, "params must be an object", "INVALID_PARAMS", NULL) : NULL;
    if (params && params->type == JSON_NULL) params = NULL;
    if (!id) return NULL; /* notifications: none are defined, they are ignored */

    if (!strcmp(method, "hello")) {
        const char *tok = json_get_str(params, "token", NULL);
        if (tok && st->auth_required && !st->authenticated) {
            if (token_equal(tok, st->token)) st->authenticated = true;
            else st->auth_failures++;
        }
        const char *client = json_get_str(params, "client", NULL);
        if (client) snprintf(st->client, sizeof st->client, "%s", client);
        return result_response(id, hello_result(st));
    }
    if (!strcmp(method, "auth")) {
        const char *tok = json_get_str(params, "token", "");
        if (!st->auth_required || token_equal(tok, st->token)) {
            st->authenticated = true;
            JsonValue *r = json_object();
            json_set_bool(r, "authenticated", true);
            return result_response(id, r);
        }
        st->auth_failures++;
        return ctl_error_response(id, nv_err_rpc_code(NV_ERR_PERMISSION), "invalid token", "PERMISSION_DENIED",
                                  "use the token from the server's token file; the connection closes after 3 failures");
    }
    if (st->auth_required && !st->authenticated)
        return ctl_error_response(id, nv_err_rpc_code(NV_ERR_PERMISSION), "authentication required", "PERMISSION_DENIED",
                                  "call auth with the server token before any other method");
    if (!strcmp(method, "ping")) return result_response(id, json_object());
    if (!strcmp(method, "ops.list")) {
        JsonValue *r = json_object();
        json_set_string(r, "contract_version", ops_contract_version());
        JsonValue *arr = json_set_array(r, "operations");
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
        return result_response(id, r);
    }
    const char *opname = NULL;
    const JsonValue *op_params = params;
    if (!strcmp(method, "ops.call")) {
        opname = json_get_str(params, "op", NULL);
        op_params = json_get(params, "params");
        if (!opname) return ctl_error_response(id, -32602, "ops.call needs params.op (the operation name)", "INVALID_PARAMS", NULL);
    } else if (ops_find(method)) {
        opname = method;
    } else {
        char hint[200] = "list methods with hello and operations with ops.list";
        return ctl_error_response(id, -32601, "method not found", "UNKNOWN_OPERATION", hint);
    }
    OpResult r;
    OpCaller caller = {st->transport ? st->transport : "socket", st->client};
    ops_invoke(e, opname, op_params, &caller, &r);
    JsonValue *resp = r.ok ? result_response(id, op_result_json(&r, true)) : op_error_response(id, &r);
    op_result_free(&r);
    return resp;
}

/* ---- connections ------------------------------------------------------------------------------- */

static bool send_json_line(int fd, const JsonValue *v, int timeout_ms) {
    size_t n;
    char *text = json_dump(v, 0, &n, NULL);
    if (!text) return false;
    char *line = realloc(text, n + 2);
    if (!line) {
        free(text);
        return false;
    }
    line[n] = '\n';
    bool ok = net_write_all(fd, line, n + 1, timeout_ms);
    free(line);
    return ok;
}

static void send_error_line(Conn *c, int code, const char *error_code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void send_error_line(Conn *c, int code, const char *error_code, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JsonValue *r = ctl_error_response(NULL, code, msg, error_code, NULL);
    send_json_line(c->fd, r, c->srv->cfg.write_timeout_ms);
    json_free(r);
}

/* returns false when the connection must be closed */
static bool process_line(Conn *c, const char *line, size_t n) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    if (i == n) return true;
    JsonLimits lim = {64, c->srv->cfg.max_message_bytes, 4096, (size_t)1 << 22};
    JsonError jerr;
    JsonValue *msg = json_parse(line, n, &lim, &jerr);
    JsonValue *resp;
    if (!msg) {
        char text[300];
        snprintf(text, sizeof text, "parse error: %s (byte %zu)", jerr.message, jerr.offset);
        resp = ctl_error_response(NULL, -32700, text, "INVALID_REQUEST", "send exactly one UTF-8 JSON object per line");
    } else {
        resp = ctl_handle_message(c->srv->engine, &c->st, msg);
        json_free(msg);
    }
    bool ok = true;
    if (resp) {
        ok = send_json_line(c->fd, resp, c->srv->cfg.write_timeout_ms);
        json_free(resp);
    }
    return ok && c->st.auth_failures < 3;
}

static bool server_stopping(CtlServer *s) {
    pthread_mutex_lock(&s->mtx);
    bool v = s->stopping;
    pthread_mutex_unlock(&s->mtx);
    return v;
}

static void *conn_main(void *arg) {
    Conn *c = arg;
    CtlServer *s = c->srv;
    size_t limit = s->cfg.max_message_bytes + 2, cap = limit < 65536 ? limit : 65536, len = 0;
    char *buf = malloc(cap);
    long long last = net_now_ms(), partial_since = 0;
    bool open = buf != NULL;
    while (open && !server_stopping(s)) {
        struct pollfd p = {c->fd, POLLIN, 0};
        int r = poll(&p, 1, 200);
        long long t = net_now_ms();
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) {
            if (len > 0 && t - partial_since > s->cfg.message_timeout_ms) {
                send_error_line(c, -32600, "TIMEOUT", "incomplete message not finished within %d ms; closing the connection", s->cfg.message_timeout_ms);
                break;
            }
            if (len == 0 && t - last > s->cfg.idle_timeout_ms) break;
            continue;
        }
        if (p.revents & (POLLERR | POLLNVAL)) break;
        if (len == cap) {
            size_t ncap = cap * 2 < limit ? cap * 2 : limit;
            char *nb = ncap > cap ? realloc(buf, ncap) : NULL;
            if (!nb) {
                send_error_line(c, -32600, "RESOURCE_LIMIT", "message exceeds the %zu byte limit; closing the connection", s->cfg.max_message_bytes);
                break;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t n = read(c->fd, buf + len, cap - len);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (len == 0) partial_since = t;
        len += (size_t)n;
        last = t;
        size_t start = 0;
        for (;;) {
            char *nl = memchr(buf + start, '\n', len - start);
            if (!nl) break;
            size_t L = (size_t)(nl - (buf + start)), eff = L;
            if (eff && buf[start + eff - 1] == '\r') eff--;
            if (eff > s->cfg.max_message_bytes) {
                send_error_line(c, -32600, "RESOURCE_LIMIT", "message exceeds the %zu byte limit; closing the connection", s->cfg.max_message_bytes);
                open = false;
                break;
            }
            if (!process_line(c, buf + start, eff)) {
                open = false;
                break;
            }
            start += L + 1;
        }
        if (!open) break;
        if (start) {
            memmove(buf, buf + start, len - start);
            len -= start;
            partial_since = len ? t : 0;
        }
        if (len > s->cfg.max_message_bytes) {
            send_error_line(c, -32600, "RESOURCE_LIMIT", "message exceeds the %zu byte limit; closing the connection", s->cfg.max_message_bytes);
            break;
        }
    }
    free(buf);
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    pthread_mutex_lock(&s->mtx);
    c->done = true;
    s->nconns--;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

static void reap_finished(CtlServer *s, bool wait_all) {
    pthread_mutex_lock(&s->mtx);
    for (;;) {
        Conn **pp = &s->conns;
        while (*pp) {
            Conn *c = *pp;
            if (c->done) {
                *pp = c->next;
                pthread_mutex_unlock(&s->mtx);
                pthread_join(c->thread, NULL);
                free(c);
                pthread_mutex_lock(&s->mtx);
                pp = &s->conns;
            } else {
                pp = &c->next;
            }
        }
        if (!wait_all || !s->conns) break;
        pthread_cond_wait(&s->cv, &s->mtx);
    }
    pthread_mutex_unlock(&s->mtx);
}

static void accept_one(CtlServer *s, int lfd, bool is_tcp) {
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return;
    net_socket_setup(fd);
    if (!is_tcp && !net_peer_is_same_user(fd)) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&s->mtx);
    if (s->nconns >= s->cfg.max_connections || s->stopping) {
        pthread_mutex_unlock(&s->mtx);
        JsonValue *r = ctl_error_response(NULL, nv_err_rpc_code(NV_ERR_BUSY), "too many connections", "BUSY", "close an idle connection and retry");
        send_json_line(fd, r, 1000);
        json_free(r);
        close(fd);
        return;
    }
    Conn *c = calloc(1, sizeof *c);
    if (!c) {
        pthread_mutex_unlock(&s->mtx);
        close(fd);
        return;
    }
    c->srv = s;
    c->fd = fd;
    c->st.auth_required = is_tcp;
    c->st.transport = "socket";
    snprintf(c->st.token, sizeof c->st.token, "%s", s->cfg.token);
    snprintf(c->st.client, sizeof c->st.client, "%s", is_tcp ? "tcp-client" : "unix-client");
    c->next = s->conns;
    s->conns = c;
    s->nconns++;
    if (pthread_create(&c->thread, NULL, conn_main, c) != 0) {
        s->conns = c->next;
        s->nconns--;
        pthread_mutex_unlock(&s->mtx);
        close(fd);
        free(c);
        return;
    }
    pthread_mutex_unlock(&s->mtx);
}

static void *accept_main(void *arg) {
    CtlServer *s = arg;
    while (!server_stopping(s)) {
        struct pollfd pfd[3];
        int n = 0;
        pfd[n++] = (struct pollfd){s->wake[0], POLLIN, 0};
        int unix_idx = -1, tcp_idx = -1;
        if (s->unix_fd >= 0) unix_idx = n, pfd[n++] = (struct pollfd){s->unix_fd, POLLIN, 0};
        if (s->tcp_fd >= 0) tcp_idx = n, pfd[n++] = (struct pollfd){s->tcp_fd, POLLIN, 0};
        int r = poll(pfd, (nfds_t)n, 1000);
        reap_finished(s, false);
        if (r <= 0 || server_stopping(s)) continue;
        if (unix_idx >= 0 && (pfd[unix_idx].revents & POLLIN)) accept_one(s, s->unix_fd, false);
        if (tcp_idx >= 0 && (pfd[tcp_idx].revents & POLLIN)) accept_one(s, s->tcp_fd, true);
    }
    return NULL;
}

/* ---- listeners --------------------------------------------------------------------------------- */

static bool prepare_socket_dir(const char *path, char *err, size_t errlen) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return true;
    *slash = 0;
    /* create missing components owner-only */
    for (char *q = dir + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        mkdir(dir, 0700);
        *q = '/';
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        snprintf(err, errlen, "cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, errlen, "%s is not a directory", dir);
        return false;
    }
    if (st.st_uid != getuid() || (st.st_mode & 022)) {
        snprintf(err, errlen, "refusing to create the control socket in %s: it must be owned by you and not group/world writable", dir);
        return false;
    }
    return true;
}

static int listen_unix(const char *path, char *err, size_t errlen) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        snprintf(err, errlen, "socket path too long (%zu bytes, limit %zu)", strlen(path), sizeof addr.sun_path - 1);
        return -1;
    }
    if (!prepare_socket_dir(path, err, errlen)) return -1;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            snprintf(err, errlen, "%s exists and is not a socket; refusing to replace it", path);
            return -1;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        bool live = probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0;
        if (probe >= 0) close(probe);
        if (live) {
            snprintf(err, errlen, "another server is already listening on %s", path);
            return -1;
        }
        unlink(path); /* stale socket from a server that exited without cleaning up */
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    net_set_cloexec(fd);
    mode_t old = umask(0177);
    int b = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old);
    if (b != 0 || chmod(path, 0600) != 0 || listen(fd, 16) != 0) {
        snprintf(err, errlen, "cannot listen on %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_tcp(const char *host, int port, int *actual, char *err, size_t errlen) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    int g = getaddrinfo(host, portstr, &hints, &res);
    if (g != 0 || !res) {
        snprintf(err, errlen, "cannot resolve %s: %s", host, gai_strerror(g));
        return -1;
    }
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (!net_addr_is_loopback(ai->ai_addr)) {
            snprintf(err, errlen, "refusing to listen on %s: the control service binds to loopback addresses only", host);
            freeaddrinfo(res);
            return -1;
        }
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }
    net_set_cloexec(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 16) != 0) {
        snprintf(err, errlen, "cannot listen on %s:%d: %s", host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    *actual = port;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
        if (ss.ss_family == AF_INET) *actual = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        else if (ss.ss_family == AF_INET6) *actual = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    return fd;
}

CtlServer *ctl_server_start(Engine *e, const CtlServerConfig *cfg, char *err, size_t errlen) {
    CtlServer *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->engine = e;
    s->cfg = *cfg;
    if (s->cfg.max_connections <= 0) s->cfg.max_connections = 16;
    if (s->cfg.max_message_bytes < 1024) s->cfg.max_message_bytes = 1024;
    s->unix_fd = s->tcp_fd = -1;
    s->wake[0] = s->wake[1] = -1;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    if (!s->cfg.unix_path[0] && !s->cfg.tcp_enabled) {
        snprintf(err, errlen, "no listener configured");
        goto fail;
    }
    if (s->cfg.tcp_enabled && !s->cfg.token[0]) {
        net_random_hex(s->cfg.token, 32);
        if (s->cfg.token_file[0] && !net_write_private_file(s->cfg.token_file, s->cfg.token, err, errlen)) goto fail;
    }
    if (pipe(s->wake) != 0) {
        snprintf(err, errlen, "pipe: %s", strerror(errno));
        goto fail;
    }
    if (s->cfg.unix_path[0]) {
        s->unix_fd = listen_unix(s->cfg.unix_path, err, errlen);
        if (s->unix_fd < 0) goto fail;
        snprintf(s->bound_path, sizeof s->bound_path, "%s", s->cfg.unix_path);
    }
    if (s->cfg.tcp_enabled) {
        s->tcp_fd = listen_tcp(s->cfg.tcp_host, s->cfg.tcp_port, &s->tcp_port, err, errlen);
        if (s->tcp_fd < 0) goto fail;
    }
    if (pthread_create(&s->accept_thread, NULL, accept_main, s) != 0) {
        snprintf(err, errlen, "cannot start the accept thread");
        goto fail;
    }
    s->accept_started = true;
    return s;
fail:
    if (s->unix_fd >= 0) close(s->unix_fd), unlink(s->bound_path);
    if (s->tcp_fd >= 0) close(s->tcp_fd);
    if (s->wake[0] >= 0) close(s->wake[0]), close(s->wake[1]);
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cv);
    free(s);
    return NULL;
}

void ctl_server_stop(CtlServer *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mtx);
    s->stopping = true;
    for (Conn *c = s->conns; c; c = c->next) shutdown(c->fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->mtx);
    if (write(s->wake[1], "x", 1) < 0) { /* the accept loop also polls with a timeout */
    }
    if (s->accept_started) pthread_join(s->accept_thread, NULL);
    reap_finished(s, true);
    if (s->unix_fd >= 0) {
        close(s->unix_fd);
        unlink(s->bound_path);
    }
    if (s->tcp_fd >= 0) close(s->tcp_fd);
    close(s->wake[0]);
    close(s->wake[1]);
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cv);
    free(s);
}

int ctl_server_tcp_port(const CtlServer *s) { return s->tcp_port; }
const char *ctl_server_token(const CtlServer *s) { return s->cfg.token; }

int ctl_server_connection_count(CtlServer *s) {
    pthread_mutex_lock(&s->mtx);
    int n = s->nconns;
    pthread_mutex_unlock(&s->mtx);
    return n;
}
