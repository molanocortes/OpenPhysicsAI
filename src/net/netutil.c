/* netutil.c - socket helpers */
#include "netutil.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

long long net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void net_set_cloexec(int fd) {
    int f = fcntl(fd, F_GETFD);
    if (f >= 0) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

void net_socket_setup(int fd) {
    net_set_cloexec(fd);
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

bool net_write_all(int fd, const char *data, size_t n, int timeout_ms) {
    size_t off = 0;
    long long deadline = net_now_ms() + timeout_ms;
    while (off < n) {
        ssize_t w = send(fd, data + off, n - off, MSG_NOSIGNAL);
        if (w < 0 && errno == ENOTSOCK) w = write(fd, data + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            long long left = deadline - net_now_ms();
            if (left <= 0) return false;
            struct pollfd p = {fd, POLLOUT, 0};
            poll(&p, 1, left > 1000 ? 1000 : (int)left);
            continue;
        }
        return false;
    }
    return true;
}

bool net_peer_is_same_user(int fd) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    uid_t uid;
    gid_t gid;
    return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#elif defined(__linux__)
    struct ucred cred;
    socklen_t len = sizeof cred;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == getuid();
#else
    (void)fd;
    return false;
#endif
}

bool net_addr_is_loopback(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
    }
    return false;
}

void net_random_hex(char *out, int nbytes) {
    unsigned char b[64] = {0};
    if (nbytes > 64) nbytes = 64;
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, (size_t)nbytes) : -1;
    if (fd >= 0) close(fd);
    if (got != nbytes) {
        /* fall back to a time/pid mix; only reached when /dev/urandom is unavailable */
        unsigned long long x = (unsigned long long)net_now_ms() ^ ((unsigned long long)getpid() << 32);
        for (int i = 0; i < nbytes; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            b[i] = (unsigned char)x;
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < nbytes; i++) out[2 * i] = hex[b[i] >> 4], out[2 * i + 1] = hex[b[i] & 15];
    out[2 * nbytes] = 0;
}

bool net_write_private_file(const char *path, const char *text, char *err, size_t errlen) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        snprintf(err, errlen, "cannot write %s: %s", path, strerror(errno));
        return false;
    }
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t)n && fchmod(fd, 0600) == 0;
    close(fd);
    if (!ok) snprintf(err, errlen, "cannot write %s", path);
    return ok;
}

void line_reader_init(LineReader *r, int fd, size_t limit) {
    memset(r, 0, sizeof *r);
    r->fd = fd;
    r->limit = limit;
}

void line_reader_free(LineReader *r) {
    free(r->buf);
    free(r->out);
    memset(r, 0, sizeof *r);
}

int line_reader_next(LineReader *r, int timeout_ms, char **line, size_t *n) {
    long long deadline = net_now_ms() + (timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        char *nl = r->len ? memchr(r->buf, '\n', r->len) : NULL;
        if (nl) {
            size_t L = (size_t)(nl - r->buf);
            if (r->discarding) {
                /* end of an oversized line: drop its tail and report it once */
                memmove(r->buf, nl + 1, r->len - L - 1);
                r->len -= L + 1;
                r->discarding = false;
                return -2;
            }
            if (L + 1 > r->outcap) {
                char *no = realloc(r->out, L + 1);
                if (!no) return -1;
                r->out = no;
                r->outcap = L + 1;
            }
            size_t eff = L;
            memcpy(r->out, r->buf, L);
            if (eff && r->out[eff - 1] == '\r') eff--;
            r->out[eff] = 0;
            memmove(r->buf, nl + 1, r->len - L - 1);
            r->len -= L + 1;
            *line = r->out;
            *n = eff;
            return 1;
        }
        if (r->discarding) r->len = 0; /* keep dropping until the oversized line ends */
        else if (r->len > r->limit) r->discarding = true, r->len = 0;
        if (r->eof) {
            if (r->len && !r->discarding) {
                /* a final line without a newline still counts */
                if (r->len == r->cap) {
                    char *nb = realloc(r->buf, r->cap + 1);
                    if (!nb) return -1;
                    r->buf = nb;
                    r->cap++;
                }
                r->buf[r->len++] = '\n';
                continue;
            }
            if (r->discarding) {
                r->discarding = false;
                return -2;
            }
            return -1;
        }
        if (r->len == r->cap) {
            size_t ncap = r->cap ? r->cap * 2 : 65536;
            if (ncap > r->limit + 2) ncap = r->limit + 2;
            if (ncap <= r->cap) {
                r->discarding = true; /* the buffer is at its limit without a newline */
                r->len = 0;
                continue;
            }
            char *nb = realloc(r->buf, ncap);
            if (!nb) return -1;
            r->buf = nb;
            r->cap = ncap;
        }
        long long left = deadline - net_now_ms();
        struct pollfd p = {r->fd, POLLIN, 0};
        int pr = poll(&p, 1, timeout_ms < 0 ? -1 : (left > 0 ? (int)left : 0));
        if (pr == 0) return 0;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ssize_t got = read(r->fd, r->buf + r->len, r->cap - r->len);
        if (got == 0) {
            r->eof = true;
            continue;
        }
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        r->len += (size_t)got;
    }
}
