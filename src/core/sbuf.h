/* sbuf.h - growable byte/string buffer (always NUL-terminated) */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct StrBuf {
    char *data;
    size_t len, cap;
    bool failed; /* an allocation failed; contents are truncated */
} StrBuf;

void sb_init(StrBuf *b);
void sb_free(StrBuf *b);
void sb_clear(StrBuf *b);
bool sb_reserve(StrBuf *b, size_t extra);
void sb_append(StrBuf *b, const char *s, size_t n);
void sb_puts(StrBuf *b, const char *s);
void sb_putc(StrBuf *b, char c);
void sb_printf(StrBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Hands the buffer to the caller (malloc'd, NUL-terminated) and resets b. NULL if an allocation failed. */
char *sb_steal(StrBuf *b, size_t *len);
