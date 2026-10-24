/* sbuf.c - growable string buffer */
#include "sbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(StrBuf *b) { memset(b, 0, sizeof *b); }

void sb_free(StrBuf *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

void sb_clear(StrBuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

bool sb_reserve(StrBuf *b, size_t extra) {
    if (b->failed) return false;
    if (extra > ((size_t)-1) / 2 - b->len) {
        b->failed = true;
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) {
        b->failed = true;
        return false;
    }
    b->data = p;
    b->cap = cap;
    return true;
}

void sb_append(StrBuf *b, const char *s, size_t n) {
    if (!n || !sb_reserve(b, n)) {
        if (b->data && !b->failed) b->data[b->len] = 0;
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void sb_puts(StrBuf *b, const char *s) { sb_append(b, s, strlen(s)); }

void sb_putc(StrBuf *b, char c) {
    if (!sb_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

void sb_printf(StrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)n < sizeof small) {
        sb_append(b, small, (size_t)n);
    } else if (sb_reserve(b, (size_t)n)) {
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

char *sb_steal(StrBuf *b, size_t *len) {
    if (b->failed) {
        sb_free(b);
        if (len) *len = 0;
        return NULL;
    }
    if (!b->data && !sb_reserve(b, 0)) return NULL;
    char *d = b->data;
    if (len) *len = b->len;
    memset(b, 0, sizeof *b);
    return d;
}
