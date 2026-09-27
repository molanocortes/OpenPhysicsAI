/* mcp.h - Model Context Protocol server core (transport independent)
 *
 * Implements the MCP lifecycle (initialize with version negotiation, notifications/initialized, ping), tool discovery
 * (tools/list with pagination) and tool invocation (tools/call) on top of a backend that lists and runs NAVIER-AM
 * operations: either an in-process Engine or a bridge to a running navier-ctl server. Supported protocol revisions:
 * 2025-11-25 (latest), 2025-06-18, 2025-03-26 and 2024-11-05; responses adapt to the negotiated revision
 * (structuredContent and titles from 2025-06-18, tool annotations from 2025-03-26, batches only before 2025-06-18).
 * Operation failures are tool execution errors (isError: true) so the model can correct its input; malformed
 * protocol messages and unknown tools are JSON-RPC errors. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"

#define MCP_LATEST_VERSION "2025-11-25"

typedef struct McpBackend {
    /* array of {name, title, description, kind, idempotent, destructive, input_schema}; NULL + err on failure */
    JsonValue *(*list_ops)(void *ctx, char *err, size_t errlen);
    /* {ok, revision, value | error, images?: [{mime_type, name, data}]}; NULL + err when the backend is unreachable */
    JsonValue *(*call_op)(void *ctx, const char *name, const JsonValue *arguments, const char *client, char *err, size_t errlen);
    void *ctx;
    const char *server_title;
    const char *instructions;
} McpBackend;

typedef struct McpSession McpSession;

McpSession *mcp_session_create(const McpBackend *backend);
void mcp_session_destroy(McpSession *s);
/* one decoded JSON-RPC message (object or batch array) -> response to send, or NULL when nothing is sent */
JsonValue *mcp_handle(McpSession *s, const JsonValue *msg);
/* raw message text -> serialised response (malloc'd, no trailing newline), or NULL when nothing is sent */
char *mcp_handle_text(McpSession *s, const char *text, size_t len, size_t *out_len);
bool mcp_session_initialized(const McpSession *s);
const char *mcp_session_version(const McpSession *s); /* "" before initialize */
bool mcp_version_supported(const char *v);
const char *mcp_default_instructions(void);
