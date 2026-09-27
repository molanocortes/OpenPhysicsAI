/* ctlclient.h - client for the navier-ctl control socket (used by navier-ctl, the MCP bridge and tests) */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"

typedef struct CtlClient CtlClient;

CtlClient *ctl_connect_unix(const char *path, char *err, size_t errlen);
/* connects to a loopback TCP listener and authenticates with token (when non-NULL) */
CtlClient *ctl_connect_tcp(const char *host, int port, const char *token, int timeout_ms, char *err, size_t errlen);
void ctl_close(CtlClient *c);
int ctl_fd(const CtlClient *c);

/* Sends {"jsonrpc":"2.0","id":N,"method":method,"params":params} and waits for the response with that id.
 * Returns the whole response object (with "result" or "error"); NULL on transport failure or timeout. */
JsonValue *ctl_request(CtlClient *c, const char *method, const JsonValue *params, int timeout_ms, char *err, size_t errlen);
/* Raw access for protocol tests and tools */
bool ctl_send_raw(CtlClient *c, const char *data, size_t n, int timeout_ms);
/* next message (any id); NULL with err on timeout/EOF/parse failure */
JsonValue *ctl_read_message(CtlClient *c, int timeout_ms, char *err, size_t errlen);
