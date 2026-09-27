/* ctlclient.c - navier-ctl client */
#include "ctlclient.h"
#include "netutil.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct CtlClient {
    int fd;
    LineReader rd;
    long long next_id;
};

static CtlClient *wrap(int fd) {
    CtlClient *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    c->next_id = 1;
    net_socket_setup(fd);
    line_reader_init(&c->rd, fd, (size_t)64 << 20);
    return c;
}

CtlClient *ctl_connect_unix(const char *path, char *err, size_t errlen) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        snprintf(err, errlen, "socket path too long");
        return NULL;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return NULL;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        snprintf(err, errlen, "cannot connect to %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    return wrap(fd);
}

CtlClient *ctl_connect_tcp(const char *host, int port, const char *token, int timeout_ms, char *err, size_t errlen) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int g = getaddrinfo(host, ps, &hints, &res);
    if (g != 0 || !res) {
        snprintf(err, errlen, "cannot resolve %s: %s", host, gai_strerror(g));
        return NULL;
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(err, errlen, "cannot connect to %s:%d: %s", host, port, strerror(errno));
        if (fd >= 0) close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);
    CtlClient *c = wrap(fd);
    if (c && token) {
        JsonValue *p = json_object();
        json_set_string(p, "token", token);
        JsonValue *r = ctl_request(c, "auth", p, timeout_ms, err, errlen);
        json_free(p);
        bool ok = r && json_get(r, "result");
        if (r && !ok) snprintf(err, errlen, "authentication failed: %s", json_get_str(json_get(r, "error"), "message", "?"));
        json_free(r);
        if (!ok) {
            ctl_close(c);
            return NULL;
        }
    }
    return c;
}

void ctl_close(CtlClient *c) {
    if (!c) return;
    close(c->fd);
    line_reader_free(&c->rd);
    free(c);
}

int ctl_fd(const CtlClient *c) { return c->fd; }

bool ctl_send_raw(CtlClient *c, const char *data, size_t n, int timeout_ms) { return net_write_all(c->fd, data, n, timeout_ms); }

JsonValue *ctl_read_message(CtlClient *c, int timeout_ms, char *err, size_t errlen) {
    char *line;
    size_t n;
    for (;;) {
        int r = line_reader_next(&c->rd, timeout_ms, &line, &n);
        if (r == 0) {
            snprintf(err, errlen, "timed out waiting for the server");
            return NULL;
        }
        if (r == -1) {
            snprintf(err, errlen, "connection closed by the server");
            return NULL;
        }
        if (r == -2) {
            snprintf(err, errlen, "response exceeds the client limit");
            return NULL;
        }
        if (n == 0) continue;
        JsonError jerr;
        JsonValue *v = json_parse(line, n, NULL, &jerr);
        if (!v) snprintf(err, errlen, "invalid JSON from the server: %s", jerr.message);
        return v;
    }
}

JsonValue *ctl_request(CtlClient *c, const char *method, const JsonValue *params, int timeout_ms, char *err, size_t errlen) {
    long long id = c->next_id++;
    JsonValue *req = json_object();
    json_set_string(req, "jsonrpc", "2.0");
    json_set_int(req, "id", id);
    json_set_string(req, "method", method);
    if (params) json_set(req, "params", json_clone(params));
    size_t n;
    char *text = json_dump(req, 0, &n, NULL);
    json_free(req);
    if (!text) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    char *line = realloc(text, n + 2);
    if (!line) {
        free(text);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    line[n] = '\n';
    bool sent = net_write_all(c->fd, line, n + 1, timeout_ms > 0 ? timeout_ms : 30000);
    free(line);
    if (!sent) {
        snprintf(err, errlen, "cannot send the request (connection lost)");
        return NULL;
    }
    for (;;) {
        JsonValue *msg = ctl_read_message(c, timeout_ms, err, errlen);
        if (!msg) return NULL;
        const JsonValue *rid = json_get(msg, "id");
        if (json_is_integer(rid) && (long long)rid->u.number == id) return msg;
        if (rid && rid->type == JSON_NULL && json_get(msg, "error")) return msg; /* connection-level error */
        json_free(msg);
    }
}
