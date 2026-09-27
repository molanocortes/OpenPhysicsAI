/* base64.c - base64 encoding and strict decoding */
#include "base64.h"

#include <stdlib.h>

static const char ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const unsigned char *d, size_t n, size_t *out_len) {
    if (n > ((size_t)-1 / 4) * 3 - 3) return NULL;
    size_t m = 4 * ((n + 2) / 3);
    char *o = malloc(m + 1);
    if (!o) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o[j++] = ENC[(v >> 18) & 63];
        o[j++] = ENC[(v >> 12) & 63];
        o[j++] = i + 1 < n ? ENC[(v >> 6) & 63] : '=';
        o[j++] = i + 2 < n ? ENC[v & 63] : '=';
    }
    o[j] = 0;
    if (out_len) *out_len = j;
    return o;
}

static int dec(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

unsigned char *base64_decode(const char *s, size_t n, size_t *out_len) {
    if (n % 4) return NULL;
    size_t pad = n >= 1 && s[n - 1] == '=' ? (n >= 2 && s[n - 2] == '=' ? 2 : 1) : 0;
    size_t m = n / 4 * 3 - pad;
    unsigned char *o = malloc(m ? m : 1);
    if (!o) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = dec(s[i]), b = dec(s[i + 1]);
        int c = s[i + 2] == '=' && i + 4 == n ? 0 : dec(s[i + 2]);
        int d = s[i + 3] == '=' && i + 4 == n ? 0 : dec(s[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (s[i + 2] == '=' && s[i + 3] != '=')) {
            free(o);
            return NULL;
        }
        unsigned v = (unsigned)a << 18 | (unsigned)b << 12 | (unsigned)c << 6 | (unsigned)d;
        o[j++] = (unsigned char)(v >> 16);
        if (j < m) o[j++] = (unsigned char)(v >> 8);
        if (j < m) o[j++] = (unsigned char)v;
    }
    if (out_len) *out_len = m;
    return o;
}
