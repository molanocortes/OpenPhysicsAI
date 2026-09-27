/* json.c - strict JSON parser, DOM and writer */
#include "json.h"
#include "sbuf.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const JsonLimits DEFAULT_LIMITS = {64, (size_t)64 << 20, 4096, (size_t)16 << 20};

int utf8_sequence_length(const unsigned char *s, size_t avail) {
    if (!avail) return 0;
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c < 0xC2) return 0; /* stray continuation byte or overlong 2-byte form */
    if (c < 0xE0) return avail >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c < 0xF0) {
        if (avail < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;  /* overlong */
        if (c == 0xED && s[1] >= 0xA0) return 0; /* UTF-16 surrogates */
        return 3;
    }
    if (c < 0xF5) {
        if (avail < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;  /* overlong */
        if (c == 0xF4 && s[1] >= 0x90) return 0; /* above U+10FFFF */
        return 4;
    }
    return 0;
}

/* ---- parser ------------------------------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t len, pos;
    JsonLimits lim;
    JsonError *err;
    bool failed;
} Parser;

static void fail(Parser *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Parser *p, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = true;
    if (!p->err) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err->message, sizeof p->err->message, fmt, ap);
    va_end(ap);
    p->err->offset = p->pos;
    int line = 1, col = 1;
    for (size_t i = 0; i < p->pos && i < p->len; i++) {
        if (p->s[i] == '\n') line++, col = 1;
        else if (((unsigned char)p->s[i] & 0xC0) != 0x80) col++;
    }
    p->err->line = line;
    p->err->column = col;
}

static void skip_ws(Parser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static JsonValue *new_value(JsonType t) {
    JsonValue *v = calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* p->s[p->pos] is the opening quote. On success *out is malloc'd and NUL-terminated. */
static bool parse_string_raw(Parser *p, char **out, size_t *outlen) {
    size_t start = p->pos + 1, i = start;
    for (;;) {
        if (i >= p->len) {
            p->pos = p->len;
            fail(p, "unterminated string");
            return false;
        }
        unsigned char c = (unsigned char)p->s[i];
        if (c == '"') break;
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c < 0x20) {
            p->pos = i;
            fail(p, "unescaped control character in string");
            return false;
        }
        if (c >= 0x80) {
            int n = utf8_sequence_length((const unsigned char *)p->s + i, p->len - i);
            if (!n) {
                p->pos = i;
                fail(p, "invalid UTF-8 in string");
                return false;
            }
            i += (size_t)n;
            continue;
        }
        i++;
    }
    size_t raw = i - start;
    if (raw > p->lim.max_string) {
        p->pos = start;
        fail(p, "string longer than %zu bytes", p->lim.max_string);
        return false;
    }
    char *buf = malloc(raw + 1);
    if (!buf) {
        fail(p, "out of memory");
        return false;
    }
    size_t o = 0;
    for (size_t k = start; k < i;) {
        char c = p->s[k];
        if (c != '\\') {
            buf[o++] = c;
            k++;
            continue;
        }
        char e = p->s[k + 1];
        size_t esc_at = k;
        k += 2;
        switch (e) {
        case '"': buf[o++] = '"'; break;
        case '\\': buf[o++] = '\\'; break;
        case '/': buf[o++] = '/'; break;
        case 'b': buf[o++] = '\b'; break;
        case 'f': buf[o++] = '\f'; break;
        case 'n': buf[o++] = '\n'; break;
        case 'r': buf[o++] = '\r'; break;
        case 't': buf[o++] = '\t'; break;
        case 'u': {
            int cp = k + 4 <= i ? hex4(p->s + k) : -1;
            if (cp < 0) {
                p->pos = esc_at;
                free(buf);
                fail(p, "invalid \\u escape");
                return false;
            }
            k += 4;
            uint32_t u = (uint32_t)cp;
            if (u >= 0xD800 && u <= 0xDBFF) {
                int lo = (k + 6 <= i && p->s[k] == '\\' && p->s[k + 1] == 'u') ? hex4(p->s + k + 2) : -1;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    p->pos = esc_at;
                    free(buf);
                    fail(p, "unpaired UTF-16 surrogate in \\u escape");
                    return false;
                }
                u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                k += 6;
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                p->pos = esc_at;
                free(buf);
                fail(p, "unpaired UTF-16 surrogate in \\u escape");
                return false;
            }
            if (u == 0) {
                p->pos = esc_at;
                free(buf);
                fail(p, "\\u0000 is not allowed in strings");
                return false;
            }
            o += utf8_encode(u, buf + o);
            break;
        }
        default:
            p->pos = esc_at;
            free(buf);
            fail(p, "invalid escape sequence");
            return false;
        }
    }
    buf[o] = 0;
    *out = buf;
    *outlen = o;
    p->pos = i + 1;
    return true;
}

static bool parse_number(Parser *p, double *out) {
    const char *s = p->s;
    size_t start = p->pos, i = start, n = p->len;
    if (i < n && s[i] == '-') i++;
    if (i >= n) goto bad;
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    } else {
        goto bad;
    }
    if (i < n && s[i] == '.') {
        size_t d = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) goto bad;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        size_t d = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) goto bad;
    }
    if (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) goto bad; /* e.g. leading zeros "012" */
    size_t len = i - start;
    if (len > 400) {
        fail(p, "number literal too long");
        return false;
    }
    char buf[512];
    memcpy(buf, s + start, len);
    buf[len] = 0;
    char dp = localeconv()->decimal_point[0];
    if (dp != '.')
        for (char *q = buf; *q; q++)
            if (*q == '.') *q = dp;
    char *end;
    errno = 0;
    double v = strtod(buf, &end);
    if (end != buf + len) goto bad;
    if (isinf(v)) {
        fail(p, "number out of range");
        return false;
    }
    *out = v;
    p->pos = i;
    return true;
bad:
    p->pos = start;
    fail(p, "invalid number");
    return false;
}

static bool arr_append(JsonValue *a, JsonValue *v) {
    if (a->u.array.len == a->u.array.cap) {
        size_t cap = a->u.array.cap ? a->u.array.cap * 2 : 8;
        JsonValue **items = realloc(a->u.array.items, cap * sizeof *items);
        if (!items) return false;
        a->u.array.items = items;
        a->u.array.cap = cap;
    }
    a->u.array.items[a->u.array.len++] = v;
    return true;
}

static bool obj_append(JsonValue *o, char *key, JsonValue *v) {
    if (o->u.object.len == o->u.object.cap) {
        size_t cap = o->u.object.cap ? o->u.object.cap * 2 : 8;
        char **keys = realloc(o->u.object.keys, cap * sizeof *keys);
        if (!keys) return false;
        o->u.object.keys = keys;
        JsonValue **vals = realloc(o->u.object.values, cap * sizeof *vals);
        if (!vals) return false;
        o->u.object.values = vals;
        o->u.object.cap = cap;
    }
    o->u.object.keys[o->u.object.len] = key;
    o->u.object.values[o->u.object.len++] = v;
    return true;
}

static JsonValue *parse_value(Parser *p, int depth);

static JsonValue *parse_array(Parser *p, int depth) {
    if (depth > p->lim.max_depth) {
        fail(p, "nesting deeper than %d levels", p->lim.max_depth);
        return NULL;
    }
    p->pos++;
    JsonValue *arr = new_value(JSON_ARRAY);
    if (!arr) {
        fail(p, "out of memory");
        return NULL;
    }
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return arr;
    }
    for (;;) {
        if (arr->u.array.len >= p->lim.max_items) {
            fail(p, "array longer than %zu elements", p->lim.max_items);
            break;
        }
        JsonValue *v = parse_value(p, depth);
        if (!v) break;
        if (!arr_append(arr, v)) {
            json_free(v);
            fail(p, "out of memory");
            break;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == ']') {
            p->pos++;
            return arr;
        }
        fail(p, "expected ',' or ']' in array");
        break;
    }
    json_free(arr);
    return NULL;
}

static JsonValue *parse_object(Parser *p, int depth) {
    if (depth > p->lim.max_depth) {
        fail(p, "nesting deeper than %d levels", p->lim.max_depth);
        return NULL;
    }
    p->pos++;
    JsonValue *obj = new_value(JSON_OBJECT);
    if (!obj) {
        fail(p, "out of memory");
        return NULL;
    }
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return obj;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != '"') {
            fail(p, "expected a string key in object");
            break;
        }
        size_t key_pos = p->pos;
        char *key;
        size_t klen;
        if (!parse_string_raw(p, &key, &klen)) break;
        bool dup = false;
        for (size_t k = 0; k < obj->u.object.len && !dup; k++) dup = strcmp(obj->u.object.keys[k], key) == 0;
        if (dup || obj->u.object.len >= p->lim.max_members) {
            p->pos = key_pos;
            if (dup) fail(p, "duplicate key \"%.60s\"", key);
            else fail(p, "object has more than %zu keys", p->lim.max_members);
            free(key);
            break;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            free(key);
            fail(p, "expected ':' after object key");
            break;
        }
        p->pos++;
        JsonValue *v = parse_value(p, depth);
        if (!v) {
            free(key);
            break;
        }
        if (!obj_append(obj, key, v)) {
            free(key);
            json_free(v);
            fail(p, "out of memory");
            break;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            return obj;
        }
        fail(p, "expected ',' or '}' in object");
        break;
    }
    json_free(obj);
    return NULL;
}

static bool match_literal(Parser *p, const char *lit) {
    size_t n = strlen(lit);
    if (p->len - p->pos < n || memcmp(p->s + p->pos, lit, n) != 0) {
        fail(p, "invalid literal (expected %s)", lit);
        return false;
    }
    p->pos += n;
    return true;
}

static JsonValue *parse_value(Parser *p, int depth) {
    skip_ws(p);
    if (p->pos >= p->len) {
        fail(p, "unexpected end of input");
        return NULL;
    }
    char c = p->s[p->pos];
    JsonValue *v = NULL;
    switch (c) {
    case '{': return parse_object(p, depth + 1);
    case '[': return parse_array(p, depth + 1);
    case '"': {
        char *str;
        size_t n;
        if (!parse_string_raw(p, &str, &n)) return NULL;
        v = new_value(JSON_STRING);
        if (!v) {
            free(str);
            fail(p, "out of memory");
            return NULL;
        }
        v->u.string.ptr = str;
        v->u.string.len = n;
        return v;
    }
    case 't':
    case 'f':
        if (!match_literal(p, c == 't' ? "true" : "false")) return NULL;
        v = new_value(JSON_BOOL);
        if (v) v->u.boolean = c == 't';
        break;
    case 'n':
        if (!match_literal(p, "null")) return NULL;
        v = new_value(JSON_NULL);
        break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            double d;
            if (!parse_number(p, &d)) return NULL;
            v = new_value(JSON_NUMBER);
            if (v) v->u.number = d;
            break;
        }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) fail(p, "unexpected character '%c'", c);
        else fail(p, "unexpected byte 0x%02x", (unsigned char)c);
        return NULL;
    }
    if (!v) fail(p, "out of memory");
    return v;
}

JsonValue *json_parse(const char *text, size_t len, const JsonLimits *limits, JsonError *err) {
    Parser p = {text, len, 0, limits ? *limits : DEFAULT_LIMITS, err, false};
    if (p.lim.max_depth <= 0) p.lim.max_depth = DEFAULT_LIMITS.max_depth;
    if (!p.lim.max_string) p.lim.max_string = DEFAULT_LIMITS.max_string;
    if (!p.lim.max_members) p.lim.max_members = DEFAULT_LIMITS.max_members;
    if (!p.lim.max_items) p.lim.max_items = DEFAULT_LIMITS.max_items;
    if (err) memset(err, 0, sizeof *err);
    if (!text) {
        fail(&p, "no input");
        return NULL;
    }
    /* a UTF-8 byte order mark is not JSON; say so explicitly */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        fail(&p, "byte order mark is not allowed");
        return NULL;
    }
    JsonValue *v = parse_value(&p, 0);
    if (!v) return NULL;
    skip_ws(&p);
    if (p.pos != p.len) {
        fail(&p, "trailing characters after JSON value");
        json_free(v);
        return NULL;
    }
    return v;
}

JsonValue *json_read_file(const char *path, size_t max_bytes, JsonError *err) {
    if (err) memset(err, 0, sizeof *err);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err->message, sizeof err->message, "cannot open file: %s", strerror(errno));
        return NULL;
    }
    StrBuf b;
    sb_init(&b);
    char chunk[65536];
    size_t n;
    bool too_big = false;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (b.len + n > max_bytes) {
            too_big = true;
            break;
        }
        sb_append(&b, chunk, n);
    }
    bool ioerr = ferror(f);
    fclose(f);
    if (too_big || ioerr || b.failed) {
        if (err)
            snprintf(err->message, sizeof err->message, "%s", too_big ? "file exceeds the size limit" : "read error");
        sb_free(&b);
        return NULL;
    }
    JsonValue *v = json_parse(b.data ? b.data : "", b.len, NULL, err);
    sb_free(&b);
    return v;
}

/* ---- construction ------------------------------------------------------------------------------ */

JsonValue *json_null(void) { return new_value(JSON_NULL); }

JsonValue *json_bool(bool b) {
    JsonValue *v = new_value(JSON_BOOL);
    if (v) v->u.boolean = b;
    return v;
}

JsonValue *json_number(double d) {
    JsonValue *v = new_value(JSON_NUMBER);
    if (v) v->u.number = d;
    return v;
}

JsonValue *json_stringn(const char *s, size_t n) {
    if (!s) return json_null();
    JsonValue *v = new_value(JSON_STRING);
    if (!v) return NULL;
    v->u.string.ptr = malloc(n + 1);
    if (!v->u.string.ptr) {
        free(v);
        return NULL;
    }
    memcpy(v->u.string.ptr, s, n);
    v->u.string.ptr[n] = 0;
    v->u.string.len = strnlen(v->u.string.ptr, n); /* a C string never carries an embedded NUL */
    return v;
}

JsonValue *json_string(const char *s) { return s ? json_stringn(s, strlen(s)) : json_null(); }

JsonValue *json_stringf(const char *fmt, ...) {
    StrBuf b;
    sb_init(&b);
    va_list ap;
    va_start(ap, fmt);
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    JsonValue *v = NULL;
    if (n >= 0 && (size_t)n < sizeof small) {
        v = json_stringn(small, (size_t)n);
    } else if (n >= 0) {
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            v = json_stringn(big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
    sb_free(&b);
    return v;
}

JsonValue *json_array(void) { return new_value(JSON_ARRAY); }
JsonValue *json_object(void) { return new_value(JSON_OBJECT); }

bool json_push(JsonValue *a, JsonValue *v) {
    if (!v) return false;
    if (!a || a->type != JSON_ARRAY || !arr_append(a, v)) {
        json_free(v);
        return false;
    }
    return true;
}

bool json_set(JsonValue *o, const char *key, JsonValue *v) {
    if (!v) return false;
    if (!o || o->type != JSON_OBJECT || !key) {
        json_free(v);
        return false;
    }
    for (size_t i = 0; i < o->u.object.len; i++) {
        if (strcmp(o->u.object.keys[i], key) == 0) {
            json_free(o->u.object.values[i]);
            o->u.object.values[i] = v;
            return true;
        }
    }
    size_t n = strlen(key);
    char *k = malloc(n + 1);
    if (!k) {
        json_free(v);
        return false;
    }
    memcpy(k, key, n + 1);
    if (!obj_append(o, k, v)) {
        free(k);
        json_free(v);
        return false;
    }
    return true;
}

bool json_set_number(JsonValue *o, const char *k, double v) { return json_set(o, k, json_number(v)); }
bool json_set_int(JsonValue *o, const char *k, long long v) { return json_set(o, k, json_number((double)v)); }
bool json_set_string(JsonValue *o, const char *k, const char *s) { return json_set(o, k, json_string(s)); }
bool json_set_bool(JsonValue *o, const char *k, bool b) { return json_set(o, k, json_bool(b)); }

JsonValue *json_set_object(JsonValue *o, const char *k) {
    JsonValue *c = json_object();
    return json_set(o, k, c) ? c : NULL;
}

JsonValue *json_set_array(JsonValue *o, const char *k) {
    JsonValue *c = json_array();
    return json_set(o, k, c) ? c : NULL;
}

JsonValue *json_vec3(double x, double y, double z) {
    double v[3] = {x, y, z};
    return json_numbers(v, 3);
}

JsonValue *json_numbers(const double *v, size_t n) {
    JsonValue *a = json_array();
    if (!a) return NULL;
    for (size_t i = 0; i < n; i++)
        if (!json_push(a, json_number(v[i]))) {
            json_free(a);
            return NULL;
        }
    return a;
}

/* ---- access ------------------------------------------------------------------------------------ */

JsonValue *json_get(const JsonValue *o, const char *key) {
    if (!o || o->type != JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < o->u.object.len; i++)
        if (strcmp(o->u.object.keys[i], key) == 0) return o->u.object.values[i];
    return NULL;
}

JsonValue *json_at(const JsonValue *a, size_t i) {
    if (!a || a->type != JSON_ARRAY || i >= a->u.array.len) return NULL;
    return a->u.array.items[i];
}

JsonValue *json_take(JsonValue *o, const char *key) {
    if (!o || o->type != JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < o->u.object.len; i++) {
        if (strcmp(o->u.object.keys[i], key) == 0) {
            JsonValue *v = o->u.object.values[i];
            free(o->u.object.keys[i]);
            size_t rest = o->u.object.len - i - 1;
            memmove(o->u.object.keys + i, o->u.object.keys + i + 1, rest * sizeof(char *));
            memmove(o->u.object.values + i, o->u.object.values + i + 1, rest * sizeof(JsonValue *));
            o->u.object.len--;
            return v;
        }
    }
    return NULL;
}

bool json_remove(JsonValue *o, const char *key) {
    JsonValue *v = json_take(o, key);
    if (!v) return false;
    json_free(v);
    return true;
}

size_t json_len(const JsonValue *v) {
    if (!v) return 0;
    if (v->type == JSON_ARRAY) return v->u.array.len;
    if (v->type == JSON_OBJECT) return v->u.object.len;
    return 0;
}

const char *json_key_at(const JsonValue *o, size_t i) {
    return o && o->type == JSON_OBJECT && i < o->u.object.len ? o->u.object.keys[i] : NULL;
}

JsonValue *json_value_at(const JsonValue *o, size_t i) {
    return o && o->type == JSON_OBJECT && i < o->u.object.len ? o->u.object.values[i] : NULL;
}

const char *json_str(const JsonValue *v) { return v && v->type == JSON_STRING ? v->u.string.ptr : NULL; }

const char *json_get_str(const JsonValue *o, const char *k, const char *def) {
    const char *s = json_str(json_get(o, k));
    return s ? s : def;
}

double json_get_num(const JsonValue *o, const char *k, double def) {
    const JsonValue *v = json_get(o, k);
    return v && v->type == JSON_NUMBER ? v->u.number : def;
}

long long json_get_int(const JsonValue *o, const char *k, long long def) {
    const JsonValue *v = json_get(o, k);
    return json_is_integer(v) ? (long long)v->u.number : def;
}

bool json_get_bool(const JsonValue *o, const char *k, bool def) {
    const JsonValue *v = json_get(o, k);
    return v && v->type == JSON_BOOL ? v->u.boolean : def;
}

bool json_is_integer(const JsonValue *v) {
    return v && v->type == JSON_NUMBER && isfinite(v->u.number) && v->u.number == floor(v->u.number) &&
           fabs(v->u.number) <= 9007199254740992.0;
}

bool json_get_numbers(const JsonValue *a, double *out, size_t n) {
    if (!a || a->type != JSON_ARRAY || a->u.array.len != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (a->u.array.items[i]->type != JSON_NUMBER) return false;
        out[i] = a->u.array.items[i]->u.number;
    }
    return true;
}

JsonValue *json_clone(const JsonValue *v) {
    if (!v) return NULL;
    switch (v->type) {
    case JSON_NULL: return json_null();
    case JSON_BOOL: return json_bool(v->u.boolean);
    case JSON_NUMBER: return json_number(v->u.number);
    case JSON_STRING: return json_stringn(v->u.string.ptr, v->u.string.len);
    case JSON_ARRAY: {
        JsonValue *a = json_array();
        for (size_t i = 0; a && i < v->u.array.len; i++) {
            if (!json_push(a, json_clone(v->u.array.items[i]))) {
                json_free(a);
                return NULL;
            }
        }
        return a;
    }
    case JSON_OBJECT: {
        JsonValue *o = json_object();
        for (size_t i = 0; o && i < v->u.object.len; i++) {
            if (!json_set(o, v->u.object.keys[i], json_clone(v->u.object.values[i]))) {
                json_free(o);
                return NULL;
            }
        }
        return o;
    }
    }
    return NULL;
}

bool json_equal(const JsonValue *a, const JsonValue *b) {
    if (a == b) return true;
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
    case JSON_NULL: return true;
    case JSON_BOOL: return a->u.boolean == b->u.boolean;
    case JSON_NUMBER: return a->u.number == b->u.number;
    case JSON_STRING: return a->u.string.len == b->u.string.len && memcmp(a->u.string.ptr, b->u.string.ptr, a->u.string.len) == 0;
    case JSON_ARRAY:
        if (a->u.array.len != b->u.array.len) return false;
        for (size_t i = 0; i < a->u.array.len; i++)
            if (!json_equal(a->u.array.items[i], b->u.array.items[i])) return false;
        return true;
    case JSON_OBJECT:
        if (a->u.object.len != b->u.object.len) return false;
        for (size_t i = 0; i < a->u.object.len; i++) {
            const JsonValue *bv = json_get(b, a->u.object.keys[i]);
            if (!bv || !json_equal(a->u.object.values[i], bv)) return false;
        }
        return true;
    }
    return false;
}

void json_free(JsonValue *v) {
    if (!v) return;
    switch (v->type) {
    case JSON_STRING: free(v->u.string.ptr); break;
    case JSON_ARRAY:
        for (size_t i = 0; i < v->u.array.len; i++) json_free(v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < v->u.object.len; i++) {
            free(v->u.object.keys[i]);
            json_free(v->u.object.values[i]);
        }
        free(v->u.object.keys);
        free(v->u.object.values);
        break;
    default: break;
    }
    free(v);
}

const char *json_type_name(JsonType t) {
    switch (t) {
    case JSON_NULL: return "null";
    case JSON_BOOL: return "boolean";
    case JSON_NUMBER: return "number";
    case JSON_STRING: return "string";
    case JSON_ARRAY: return "array";
    case JSON_OBJECT: return "object";
    }
    return "?";
}

/* ---- writer ------------------------------------------------------------------------------------ */

static void write_string(StrBuf *b, const char *s, size_t n) {
    sb_putc(b, '"');
    size_t run = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[8];
        size_t step = 1;
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) {
                snprintf(ubuf, sizeof ubuf, "\\u%04x", c);
                esc = ubuf;
            } else if (c >= 0x80) {
                int len = utf8_sequence_length((const unsigned char *)s + i, n - i);
                if (len) step = (size_t)len;
                else esc = "\xEF\xBF\xBD"; /* replacement character */
            }
        }
        if (esc) {
            sb_append(b, s + run, i - run);
            sb_puts(b, esc);
            run = i + 1;
        }
        i += step;
    }
    sb_append(b, s + run, n - run);
    sb_putc(b, '"');
}

static void write_number(StrBuf *b, double v, bool *nonfinite) {
    if (!isfinite(v)) {
        sb_puts(b, "null");
        if (nonfinite) *nonfinite = true;
        return;
    }
    if (v == floor(v) && fabs(v) < 9007199254740992.0) {
        sb_printf(b, "%lld", (long long)v);
        return;
    }
    char buf[48];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    char dp = localeconv()->decimal_point[0];
    if (dp != '.')
        for (char *q = buf; *q; q++)
            if (*q == dp) *q = '.';
    sb_puts(b, buf);
}

static int cmp_keys(const void *a, const void *b) {
    const char *const *ka = a, *const *kb = b;
    return strcmp(*ka, *kb);
}

static void indent(StrBuf *b, int depth) {
    sb_putc(b, '\n');
    for (int i = 0; i < depth; i++) sb_append(b, "  ", 2);
}

static void write_value(StrBuf *b, const JsonValue *v, int flags, int depth, bool *nonfinite) {
    if (!v) {
        sb_puts(b, "null");
        return;
    }
    bool pretty = flags & JSON_PRETTY;
    switch (v->type) {
    case JSON_NULL: sb_puts(b, "null"); break;
    case JSON_BOOL: sb_puts(b, v->u.boolean ? "true" : "false"); break;
    case JSON_NUMBER: write_number(b, v->u.number, nonfinite); break;
    case JSON_STRING: write_string(b, v->u.string.ptr, v->u.string.len); break;
    case JSON_ARRAY: {
        sb_putc(b, '[');
        /* short arrays of scalars stay on one line even when pretty-printing */
        bool inline_arr = true;
        for (size_t i = 0; i < v->u.array.len && inline_arr; i++)
            inline_arr = v->u.array.items[i]->type != JSON_ARRAY && v->u.array.items[i]->type != JSON_OBJECT;
        inline_arr = inline_arr && v->u.array.len <= 16;
        for (size_t i = 0; i < v->u.array.len; i++) {
            if (i) sb_putc(b, ',');
            if (pretty && !inline_arr) indent(b, depth + 1);
            else if (pretty && i) sb_putc(b, ' ');
            write_value(b, v->u.array.items[i], flags, depth + 1, nonfinite);
        }
        if (pretty && !inline_arr && v->u.array.len) indent(b, depth);
        sb_putc(b, ']');
        break;
    }
    case JSON_OBJECT: {
        sb_putc(b, '{');
        size_t n = v->u.object.len;
        size_t *order = NULL;
        if ((flags & JSON_SORTED) && n > 1) {
            const char **keys = malloc(n * sizeof *keys);
            order = malloc(n * sizeof *order);
            if (keys && order) {
                for (size_t i = 0; i < n; i++) keys[i] = v->u.object.keys[i];
                qsort(keys, n, sizeof *keys, cmp_keys);
                for (size_t i = 0; i < n; i++)
                    for (size_t j = 0; j < n; j++)
                        if (keys[i] == v->u.object.keys[j]) {
                            order[i] = j;
                            break;
                        }
            } else {
                free(order);
                order = NULL;
            }
            free(keys);
        }
        for (size_t i = 0; i < n; i++) {
            size_t k = order ? order[i] : i;
            if (i) sb_putc(b, ',');
            if (pretty) indent(b, depth + 1);
            write_string(b, v->u.object.keys[k], strlen(v->u.object.keys[k]));
            sb_putc(b, ':');
            if (pretty) sb_putc(b, ' ');
            write_value(b, v->u.object.values[k], flags, depth + 1, nonfinite);
        }
        free(order);
        if (pretty && n) indent(b, depth);
        sb_putc(b, '}');
        break;
    }
    }
}

char *json_dump(const JsonValue *v, int flags, size_t *len, bool *nonfinite) {
    StrBuf b;
    sb_init(&b);
    if (nonfinite) *nonfinite = false;
    write_value(&b, v, flags, 0, nonfinite);
    return sb_steal(&b, len);
}

bool json_write_file(const char *path, const JsonValue *v, int flags) {
    size_t n;
    char *s = json_dump(v, flags, &n, NULL);
    if (!s) return false;
    /* write to a temporary name and rename, so a crash never leaves a truncated file behind */
    char tmp[4096];
    int w = snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    bool ok = w > 0 && (size_t)w < sizeof tmp;
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    ok = f && fwrite(s, 1, n, f) == n && fputc('\n', f) != EOF;
    if (f && fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok && f) remove(tmp);
    free(s);
    return ok;
}
