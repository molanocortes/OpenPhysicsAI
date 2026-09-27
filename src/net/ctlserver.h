/* ctlserver.h - local control socket for NAVIER-AM
 *
 * Protocol "navier-ctl", version 1 (documented in docs/control-protocol.md): JSON-RPC 2.0 messages, one UTF-8 JSON
 * object per line (newline-delimited JSON). A Unix-domain socket (owner-only permissions, peer uid checked) is the
 * default; an optional TCP listener binds to loopback only and requires a token. Every operation request goes
 * through ops_invoke, exactly like the terminal and MCP. There is no shell or arbitrary file access. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "../ctl/engine.h"

#define CTL_PROTOCOL_NAME "navier-ctl"
#define CTL_PROTOCOL_VERSION 1

typedef struct CtlServerConfig {
    char unix_path[512]; /* "" disables the Unix socket */
    bool tcp_enabled;
    char tcp_host[64];   /* must resolve to loopback addresses only */
    int tcp_port;        /* 0 = ephemeral */
    char token[65];      /* TCP clients authenticate with this token; generated when empty */
    char token_file[512];/* a generated token is written here with 0600 permissions ("" = not written) */
    int max_connections;
    int idle_timeout_ms;    /* close connections that send nothing for this long */
    int message_timeout_ms; /* an incomplete message must be completed within this time */
    int write_timeout_ms;   /* a client must accept a response within this time */
    size_t max_message_bytes;
} CtlServerConfig;

bool ctl_default_socket_path(char *out, size_t cap); /* ~/.navier/run/control.sock */
void ctl_server_config_default(CtlServerConfig *c);

typedef struct CtlServer CtlServer;

CtlServer *ctl_server_start(Engine *e, const CtlServerConfig *cfg, char *err, size_t errlen);
void ctl_server_stop(CtlServer *s); /* closes listeners and connections, waits for their threads */
int ctl_server_tcp_port(const CtlServer *s);
const char *ctl_server_token(const CtlServer *s);
int ctl_server_connection_count(CtlServer *s);

/* Per-connection state used by the message handler. */
typedef struct CtlSession {
    bool auth_required;
    bool authenticated;
    char token[65];
    int auth_failures;
    char client[64];
    const char *transport; /* "socket" */
} CtlSession;

/* One JSON-RPC message in, the response out (NULL for notifications and client responses). */
JsonValue *ctl_handle_message(Engine *e, CtlSession *session, const JsonValue *msg);
JsonValue *ctl_error_response(const JsonValue *id, int code, const char *message, const char *error_code, const char *hint);
