/* png.c - PNG encoding: adaptive scanline filters, deflate (LZ77 + fixed Huffman), CRC-32, Adler-32 */
#include "png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *buf;
    size_t len, cap;
    uint32_t bits;
    int nbits;
    bool oom;
} BitWriter;

static void bw_byte(BitWriter *w, unsigned char b) {
    if (w->len == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 65536;
        unsigned char *nb = realloc(w->buf, cap);
        if (!nb) {
            w->oom = true;
            return;
        }
        w->buf = nb;
        w->cap = cap;
    }
    w->buf[w->len++] = b;
}

/* LSB-first bit packing as required by deflate */
static void bw_bits(BitWriter *w, uint32_t value, int n) {
    w->bits |= value << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        bw_byte(w, (unsigned char)(w->bits & 0xFF));
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

static void bw_flush(BitWriter *w) {
    if (w->nbits > 0) bw_byte(w, (unsigned char)(w->bits & 0xFF));
    w->bits = 0;
    w->nbits = 0;
}

/* Huffman codes are defined MSB-first: reverse them before packing */
static void bw_code(BitWriter *w, uint32_t code, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1u) << (n - 1 - i);
    bw_bits(w, r, n);
}

static void put_literal(BitWriter *w, int lit) {
    if (lit <= 143) bw_code(w, 0x30 + (uint32_t)lit, 8);
    else if (lit <= 255) bw_code(w, 0x190 + (uint32_t)(lit - 144), 9);
    else if (lit <= 279) bw_code(w, (uint32_t)(lit - 256), 7);
    else bw_code(w, 0xC0 + (uint32_t)(lit - 280), 8);
}

static const int LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const int DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(BitWriter *w, int len, int dist) {
    int li = 28;
    while (li > 0 && LEN_BASE[li] > len) li--;
    put_literal(w, 257 + li);
    if (LEN_EXTRA[li]) bw_bits(w, (uint32_t)(len - LEN_BASE[li]), LEN_EXTRA[li]);
    int di = 29;
    while (di > 0 && DIST_BASE[di] > dist) di--;
    bw_code(w, (uint32_t)di, 5);
    if (DIST_EXTRA[di]) bw_bits(w, (uint32_t)(dist - DIST_BASE[di]), DIST_EXTRA[di]);
}

enum { WSIZE = 32768, WMASK = WSIZE - 1, HBITS = 15, HSIZE = 1 << HBITS, MAX_CHAIN = 48, MIN_MATCH = 3, MAX_MATCH = 258 };

static void deflate_fixed(BitWriter *w, const unsigned char *d, size_t n) {
    bw_bits(w, 1, 1); /* BFINAL */
    bw_bits(w, 1, 2); /* BTYPE = 01, fixed Huffman */
    int32_t *head = malloc(HSIZE * sizeof(int32_t));
    int32_t *prev = malloc(WSIZE * sizeof(int32_t));
    if (!head || !prev) {
        free(head), free(prev);
        /* without match tables, emit literals only (still valid deflate) */
        for (size_t i = 0; i < n; i++) put_literal(w, d[i]);
        put_literal(w, 256);
        return;
    }
    for (int i = 0; i < HSIZE; i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + MIN_MATCH <= n) {
            uint32_t h = ((uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2]) * 2654435761u >> (32 - HBITS);
            int32_t cand = head[h];
            int chain = 0;
            size_t maxlen = n - i < MAX_MATCH ? n - i : MAX_MATCH;
            while (cand >= 0 && i - (size_t)cand <= WSIZE - 1 && chain++ < MAX_CHAIN) {
                size_t l = 0;
                while (l < maxlen && d[cand + l] == d[i + l]) l++;
                if ((int)l > best_len) {
                    best_len = (int)l;
                    best_dist = (int)(i - (size_t)cand);
                    if (l == maxlen) break;
                }
                int32_t nx = prev[cand & WMASK];
                if (nx >= cand) break;
                cand = nx;
            }
            prev[i & WMASK] = head[h];
            head[h] = (int32_t)i;
        }
        if (best_len >= MIN_MATCH) {
            put_match(w, best_len, best_dist);
            /* index the skipped positions so later matches can refer to them */
            for (size_t k = i + 1; k < i + (size_t)best_len && k + MIN_MATCH <= n; k++) {
                uint32_t h = ((uint32_t)d[k] << 16 | (uint32_t)d[k + 1] << 8 | d[k + 2]) * 2654435761u >> (32 - HBITS);
                prev[k & WMASK] = head[h];
                head[h] = (int32_t)k;
            }
            i += (size_t)best_len;
        } else {
            put_literal(w, d[i]);
            i++;
        }
    }
    put_literal(w, 256);
    free(head), free(prev);
}

bool zlib_compress(const unsigned char *data, size_t n, unsigned char **out, size_t *out_len) {
    BitWriter w = {0};
    bw_byte(&w, 0x78);
    bw_byte(&w, 0x01);
    deflate_fixed(&w, data, n);
    bw_flush(&w);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; k--) bw_byte(&w, (unsigned char)(adler >> (8 * k)));
    if (w.oom) {
        free(w.buf);
        return false;
    }
    *out = w.buf;
    *out_len = w.len;
    return true;
}

unsigned long crc32_update(unsigned long crc, const unsigned char *data, size_t n) {
    static uint32_t table[256];
    static int ready = 0;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t c = (uint32_t)crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static bool chunk(BitWriter *w, const char *type, const unsigned char *data, size_t n) {
    for (int k = 3; k >= 0; k--) bw_byte(w, (unsigned char)(n >> (8 * k)));
    size_t start = w->len;
    for (int k = 0; k < 4; k++) bw_byte(w, (unsigned char)type[k]);
    for (size_t i = 0; i < n; i++) bw_byte(w, data[i]);
    if (w->oom) return false;
    unsigned long crc = crc32_update(0, w->buf + start, n + 4);
    for (int k = 3; k >= 0; k--) bw_byte(w, (unsigned char)(crc >> (8 * k)));
    return !w->oom;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

bool png_encode(const unsigned char *px, int width, int height, int ch, unsigned char **out, size_t *out_len) {
    if (width <= 0 || height <= 0 || (ch != 3 && ch != 4)) return false;
    size_t stride = (size_t)width * (size_t)ch, rowlen = stride + 1;
    unsigned char *raw = malloc(rowlen * (size_t)height);
    unsigned char *cand = malloc(rowlen * 4);
    if (!raw || !cand) {
        free(raw), free(cand);
        return false;
    }
    for (int y = 0; y < height; y++) {
        const unsigned char *row = px + stride * (size_t)y, *up = y ? px + stride * (size_t)(y - 1) : NULL;
        size_t best = 0;
        unsigned long best_sum = ~0ul;
        for (int f = 0; f < 4; f++) {
            unsigned char *o = cand + rowlen * (size_t)f;
            static const int FILTERS[4] = {0, 1, 2, 4}; /* none, sub, up, paeth */
            o[0] = (unsigned char)FILTERS[f];
            unsigned long sum = 0;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)ch ? row[x - (size_t)ch] : 0, b = up ? up[x] : 0, c = (up && x >= (size_t)ch) ? up[x - (size_t)ch] : 0;
                int pred = FILTERS[f] == 0 ? 0 : FILTERS[f] == 1 ? a : FILTERS[f] == 2 ? b : paeth(a, b, c);
                unsigned char v = (unsigned char)(row[x] - pred);
                o[x + 1] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (sum < best_sum) best_sum = sum, best = (size_t)f;
        }
        memcpy(raw + rowlen * (size_t)y, cand + rowlen * best, rowlen);
    }
    free(cand);
    unsigned char *z;
    size_t zlen;
    bool ok = zlib_compress(raw, rowlen * (size_t)height, &z, &zlen);
    free(raw);
    if (!ok) return false;
    BitWriter w = {0};
    static const unsigned char SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (int k = 0; k < 8; k++) bw_byte(&w, SIG[k]);
    unsigned char ihdr[13] = {(unsigned char)(width >> 24), (unsigned char)(width >> 16), (unsigned char)(width >> 8), (unsigned char)width,
                              (unsigned char)(height >> 24), (unsigned char)(height >> 16), (unsigned char)(height >> 8), (unsigned char)height,
                              8, ch == 4 ? 6 : 2, 0, 0, 0};
    ok = chunk(&w, "IHDR", ihdr, 13) && chunk(&w, "IDAT", z, zlen) && chunk(&w, "IEND", NULL, 0);
    free(z);
    if (!ok) {
        free(w.buf);
        return false;
    }
    *out = w.buf;
    *out_len = w.len;
    return true;
}

bool png_write_file(const char *path, const unsigned char *px, int width, int height, int ch) {
    unsigned char *png;
    size_t n;
    if (!png_encode(px, width, height, ch, &png, &n)) return false;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(png, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = false;
    free(png);
    return ok;
}
