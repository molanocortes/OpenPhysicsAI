/* netutil.h - small POSIX socket helpers shared by the control server, its client and the MCP transports */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

long long net_now_ms(void);
void net_set_cloexec(int fd);
/* non-blocking, close-on-exec, no SIGPIPE on write */
void net_socket_setup(int fd);
/* writes everything, waiting up to timeout_ms in total when the peer is slow; false on error or timeout */
bool net_write_all(int fd, const char *data, size_t n, int timeout_ms);
/* Unix-domain peer runs as the same user as this process */
bool net_peer_is_same_user(int fd);
bool net_addr_is_loopback(const struct sockaddr *sa);
void net_random_hex(char *out, int nbytes); /* 2*nbytes hex characters + NUL */
bool net_write_private_file(const char *path, const char *text, char *err, size_t errlen); /* mode 0600 */

/* Line reader over a file descriptor: newline-delimited messages with a size limit. */
typedef struct LineReader {
    int fd;
    char *buf; /* unconsumed input */
    size_t len, cap, limit;
    char *out; /* the line handed to the caller */
    size_t outcap;
    bool eof;
    bool discarding; /* inside a line that exceeded the limit */
} LineReader;

void line_reader_init(LineReader *r, int fd, size_t limit);
void line_reader_free(LineReader *r);
/* Returns 1 with *line (NUL-terminated, a trailing '\r' removed, valid until the next call on this reader) and *n
 * when a complete line is available; 0 on timeout; -1 on EOF or error; -2 once for each line that exceeded the limit
 * (its bytes are discarded). timeout_ms < 0 waits indefinitely. */
int line_reader_next(LineReader *r, int timeout_ms, char **line, size_t *n);
