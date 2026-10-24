/* xml.c - strict XML subset reader (see xml.h) */
#include "xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct P {
    const char *s;
    size_t n, i;
    int line;
    char *err;
    size_t errlen;
    bool failed;
} P;

static void fail(P *p, const char *msg) {
    if (!p->failed) snprintf(p->err, p->errlen, "line %d: %s", p->line, msg);
    p->failed = true;
}

static bool at(P *p, const char *lit) {
    size_t k = strlen(lit);
    return p->i + k <= p->n && !memcmp(p->s + p->i, lit, k);
}

static void adv(P *p, size_t k) {
    for (size_t j = 0; j < k && p->i < p->n; j++, p->i++)
        if (p->s[p->i] == '\n') p->line++;
}

static void skip_ws(P *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\r' || p->s[p->i] == '\n')) adv(p, 1);
}

static bool name_char(char c, bool first) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || (unsigned char)c >= 0x80 ||
           (!first && ((c >= '0' && c <= '9') || c == '-' || c == '.'));
}

static char *read_name(P *p) {
    size_t st = p->i;
    if (p->i >= p->n || !name_char(p->s[p->i], true)) {
        fail(p, "expected a name");
        return NULL;
    }
    while (p->i < p->n && name_char(p->s[p->i], false)) p->i++;
    char *r = malloc(p->i - st + 1);
    if (!r) {
        fail(p, "out of memory");
        return NULL;
    }
    memcpy(r, p->s + st, p->i - st);
    r[p->i - st] = 0;
    return r;
}

/* decodes entities of s[0..len) into a new string */
static char *decode(P *p, const char *s, size_t len) {
    char *out = malloc(len + 1);
    if (!out) {
        fail(p, "out of memory");
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (s[i] != '&') {
            out[o++] = s[i++];
            continue;
        }
        const char *semi = memchr(s + i, ';', len - i);
        if (!semi) {
            fail(p, "unterminated entity reference");
            free(out);
            return NULL;
        }
        size_t el = (size_t)(semi - (s + i)) + 1;
        if (!strncmp(s + i, "&lt;", el) && el == 4) out[o++] = '<';
        else if (el == 4 && !strncmp(s + i, "&gt;", 4)) out[o++] = '>';
        else if (el == 5 && !strncmp(s + i, "&amp;", 5)) out[o++] = '&';
        else if (el == 6 && !strncmp(s + i, "&quot;", 6)) out[o++] = '"';
        else if (el == 6 && !strncmp(s + i, "&apos;", 6)) out[o++] = '\'';
        else if (el > 3 && s[i + 1] == '#') {
            unsigned long cp = s[i + 2] == 'x' ? strtoul(s + i + 3, NULL, 16) : strtoul(s + i + 2, NULL, 10);
            if (cp == 0 || cp > 0x10FFFF) {
                fail(p, "invalid character reference");
                free(out);
                return NULL;
            }
            if (cp < 0x80) out[o++] = (char)cp; /* encoding shrinks: at most 4 bytes for >= 6 source bytes */
            else if (cp < 0x800) out[o++] = (char)(0xC0 | (cp >> 6)), out[o++] = (char)(0x80 | (cp & 0x3F));
            else if (cp < 0x10000)
                out[o++] = (char)(0xE0 | (cp >> 12)), out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)), out[o++] = (char)(0x80 | (cp & 0x3F));
            else
                out[o++] = (char)(0xF0 | (cp >> 18)), out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)), out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)),
                out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            fail(p, "unknown entity (custom entities are not supported)");
            free(out);
            return NULL;
        }
        i += el;
    }
    out[o] = 0;
    return out;
}

static bool add_child(P *p, XmlNode *parent, XmlNode *c) {
    if (parent->nchildren == parent->cap) {
        int nc = parent->cap ? 2 * parent->cap : 4;
        XmlNode **a = realloc(parent->children, (size_t)nc * sizeof *a);
        if (!a) {
            fail(p, "out of memory");
            return false;
        }
        parent->children = a, parent->cap = nc;
    }
    parent->children[parent->nchildren++] = c;
    c->parent = parent;
    return true;
}

static void append_text(P *p, XmlNode *n, const char *s, size_t len) {
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    if (a == b) return;
    char *d = decode(p, s + a, b - a);
    if (!d) return;
    size_t old = n->text ? strlen(n->text) : 0, add = strlen(d);
    char *t = realloc(n->text, old + add + 2);
    if (!t) {
        free(d);
        fail(p, "out of memory");
        return;
    }
    if (old) t[old++] = ' ';
    memcpy(t + old, d, add + 1);
    n->text = t;
    free(d);
}

static bool skip_misc(P *p) {
    for (;;) {
        skip_ws(p);
        if (at(p, "<!--")) {
            const char *e = NULL;
            for (size_t j = p->i + 4; j + 2 < p->n; j++)
                if (!memcmp(p->s + j, "-->", 3)) {
                    e = p->s + j;
                    break;
                }
            if (!e) {
                fail(p, "unterminated comment");
                return false;
            }
            adv(p, (size_t)(e - (p->s + p->i)) + 3);
        } else if (at(p, "<?")) {
            const char *e = NULL;
            for (size_t j = p->i + 2; j + 1 < p->n; j++)
                if (!memcmp(p->s + j, "?>", 2)) {
                    e = p->s + j;
                    break;
                }
            if (!e) {
                fail(p, "unterminated processing instruction");
                return false;
            }
            adv(p, (size_t)(e - (p->s + p->i)) + 2);
        } else if (at(p, "<!DOCTYPE") || at(p, "<!ENTITY")) {
            fail(p, "DOCTYPE and entity declarations are not supported");
            return false;
        } else
            return true;
    }
}

XmlNode *xml_parse(const char *text, size_t len, char *err, size_t errlen) {
    char dummy[8];
    P p = {text, len, 0, 1, err ? err : dummy, err ? errlen : sizeof dummy, false};
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) p.i = 3;
    if (!skip_misc(&p)) return NULL;
    if (!at(&p, "<")) {
        fail(&p, "expected the root element");
        return NULL;
    }
    XmlNode *root = NULL, *cur = NULL;
    int depth = 0;
    while (!p.failed) {
        if (!cur) {
            if (root) {
                if (!skip_misc(&p)) break;
                if (p.i < p.n) fail(&p, "content after the root element");
                break;
            }
        }
        if (p.i >= p.n) {
            fail(&p, cur ? "unexpected end of file (unclosed element)" : "empty document");
            break;
        }
        if (cur && p.s[p.i] != '<') {
            size_t st = p.i;
            while (p.i < p.n && p.s[p.i] != '<') adv(&p, 1);
            append_text(&p, cur, p.s + st, p.i - st);
            continue;
        }
        if (at(&p, "<!--") || at(&p, "<?")) {
            if (!skip_misc(&p)) break;
            continue;
        }
        if (at(&p, "<![CDATA[")) {
            size_t st = p.i + 9;
            const char *e = NULL;
            for (size_t j = st; j + 2 < p.n; j++)
                if (!memcmp(p.s + j, "]]>", 3)) {
                    e = p.s + j;
                    break;
                }
            if (!e || !cur) {
                fail(&p, "invalid CDATA section");
                break;
            }
            size_t clen = (size_t)(e - (p.s + st));
            char *t = realloc(cur->text, (cur->text ? strlen(cur->text) : 0) + clen + 2);
            if (!t) {
                fail(&p, "out of memory");
                break;
            }
            size_t old = cur->text ? strlen(t) : 0;
            if (!cur->text) t[0] = 0;
            memcpy(t + old, p.s + st, clen);
            t[old + clen] = 0;
            cur->text = t;
            adv(&p, (size_t)(e - (p.s + p.i)) + 3);
            continue;
        }
        if (at(&p, "</")) {
            adv(&p, 2);
            char *nm = read_name(&p);
            if (!nm) break;
            skip_ws(&p);
            if (!cur || strcmp(nm, cur->name) || !at(&p, ">")) {
                fail(&p, cur ? "mismatched closing tag" : "closing tag without an open element");
                free(nm);
                break;
            }
            free(nm);
            adv(&p, 1);
            cur = cur->parent;
            depth--;
            continue;
        }
        /* start tag */
        adv(&p, 1);
        XmlNode *node = calloc(1, sizeof *node);
        if (!node) {
            fail(&p, "out of memory");
            break;
        }
        node->line = p.line;
        node->name = read_name(&p);
        if (!node->name) {
            xml_free(node);
            break;
        }
        if (cur) {
            if (!add_child(&p, cur, node)) {
                xml_free(node);
                break;
            }
        } else if (!root)
            root = node;
        else {
            xml_free(node);
            fail(&p, "multiple root elements");
            break;
        }
        bool closed = false;
        for (;;) {
            skip_ws(&p);
            if (at(&p, "/>")) {
                adv(&p, 2);
                closed = true;
                break;
            }
            if (at(&p, ">")) {
                adv(&p, 1);
                break;
            }
            char *an = read_name(&p);
            if (!an) break;
            skip_ws(&p);
            if (!at(&p, "=")) {
                free(an);
                fail(&p, "expected '=' after an attribute name");
                break;
            }
            adv(&p, 1);
            skip_ws(&p);
            if (p.i >= p.n || (p.s[p.i] != '"' && p.s[p.i] != '\'')) {
                free(an);
                fail(&p, "attribute values must be quoted");
                break;
            }
            char q = p.s[p.i];
            adv(&p, 1);
            size_t st = p.i;
            while (p.i < p.n && p.s[p.i] != q) {
                if (p.s[p.i] == '<') break;
                adv(&p, 1);
            }
            if (p.i >= p.n || p.s[p.i] != q) {
                free(an);
                fail(&p, "unterminated attribute value");
                break;
            }
            char *val = decode(&p, p.s + st, p.i - st);
            adv(&p, 1);
            if (!val) {
                free(an);
                break;
            }
            for (int k = 0; k < node->nattrs; k++)
                if (!strcmp(node->attrs[k].name, an)) fail(&p, "duplicate attribute");
            XmlAttr *na = realloc(node->attrs, (size_t)(node->nattrs + 1) * sizeof *na);
            if (!na || p.failed) {
                free(an), free(val);
                if (!na) fail(&p, "out of memory");
                break;
            }
            node->attrs = na;
            node->attrs[node->nattrs].name = an;
            node->attrs[node->nattrs].value = val;
            node->nattrs++;
        }
        if (p.failed) break;
        if (!closed) {
            cur = node;
            if (++depth > 64) fail(&p, "elements nested deeper than 64 levels");
        } else if (!cur) {
            /* self-closing root */
            if (!skip_misc(&p)) break;
            if (p.i < p.n) fail(&p, "content after the root element");
            break;
        }
    }
    if (p.failed) {
        xml_free(root);
        return NULL;
    }
    return root;
}

XmlNode *xml_read_file(const char *path, size_t max_bytes, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (max_bytes && (size_t)sz > max_bytes)) {
        fclose(f);
        snprintf(err, errlen, "%s is larger than the %zu byte limit", path, max_bytes);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    XmlNode *n = xml_parse(buf, got, err, errlen);
    free(buf);
    return n;
}

void xml_free(XmlNode *n) {
    if (!n) return;
    for (int i = 0; i < n->nchildren; i++) xml_free(n->children[i]);
    for (int i = 0; i < n->nattrs; i++) free(n->attrs[i].name), free(n->attrs[i].value);
    free(n->children);
    free(n->attrs);
    free(n->name);
    free(n->text);
    free(n);
}

const char *xml_attr(const XmlNode *n, const char *name) {
    for (int i = 0; n && i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

XmlNode *xml_child(const XmlNode *n, const char *name) {
    for (int i = 0; n && i < n->nchildren; i++)
        if (!strcmp(n->children[i]->name, name)) return n->children[i];
    return NULL;
}
