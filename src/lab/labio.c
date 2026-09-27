/* labio.c - see labio.h. Frames are assembled in memory and written with one fwrite, so a frame is either complete in
 * the file or absent. */
#include "labio.h"

#include <stdlib.h>
#include <string.h>

static const char LAB_MAGIC[8] = {'O', 'P', 'L', 'A', 'B', '0', '1', '\n'};

/* ---- a growable byte buffer ---- */
typedef struct Buf {
    unsigned char *p;
    size_t len, cap;
    bool bad;
} Buf;

static void buf_put(Buf *b, const void *data, size_t n) {
    if (b->bad || n == 0) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        unsigned char *q = realloc(b->p, cap);
        if (!q) {
            b->bad = true;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}
static void buf_i32(Buf *b, int32_t v) { buf_put(b, &v, 4); }
static void buf_u64(Buf *b, uint64_t v) { buf_put(b, &v, 8); }
static void buf_f64(Buf *b, double v) { buf_put(b, &v, 8); }
static void buf_name(Buf *b, const char *s) {
    char name[48] = {0};
    if (s) strncpy(name, s, sizeof name - 1);
    buf_put(b, name, sizeof name);
}

/* ---- writer ---- */
struct LabWriter {
    FILE *fp;
    Buf frame;
    bool in_frame, failed;
    int frames;
    /* the part being built: its fields are counted into a patchable slot */
    size_t part_nfields_at; /* offset of the current part's field count, 0 when there is no part */
    int part_nfields;
};

LabWriter *lab_create(const char *path, const char *header_json, char *err, size_t errlen) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        snprintf(err, errlen, "cannot create %s", path);
        return NULL;
    }
    LabWriter *w = calloc(1, sizeof *w);
    if (!w) {
        fclose(fp);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    w->fp = fp;
    const char *h = header_json ? header_json : "{}";
    uint32_t hl = (uint32_t)strlen(h);
    if (fwrite(LAB_MAGIC, 1, 8, fp) != 8 || fwrite(&hl, 4, 1, fp) != 1 || fwrite(h, 1, hl, fp) != hl) w->failed = true;
    fflush(fp);
    return w;
}

static void close_part(LabWriter *w) {
    if (w->part_nfields_at) {
        int32_t n = w->part_nfields;
        if (!w->frame.bad) memcpy(w->frame.p + w->part_nfields_at, &n, 4);
    }
    w->part_nfields_at = 0;
    w->part_nfields = 0;
}

void lab_frame_begin(LabWriter *w, double time) {
    w->frame.len = 0;
    w->frame.bad = false;
    w->in_frame = true;
    w->part_nfields_at = 0;
    buf_f64(&w->frame, time);
}

static void begin_part(LabWriter *w, int kind, const char *name) {
    close_part(w);
    buf_put(&w->frame, "PART", 4);
    buf_i32(&w->frame, kind);
    buf_name(&w->frame, name);
}

static void open_fields(LabWriter *w) {
    w->part_nfields_at = w->frame.len;
    w->part_nfields = 0;
    buf_i32(&w->frame, 0);
}

void lab_part_blocks(LabWriter *w, const char *name, int nblocks, const LabBlock *blocks) {
    begin_part(w, LAB_BLOCKS, name);
    buf_i32(&w->frame, nblocks);
    for (int i = 0; i < nblocks; i++) {
        const LabBlock *b = &blocks[i];
        for (int k = 0; k < 3; k++) buf_i32(&w->frame, b->n[k]);
        buf_i32(&w->frame, b->level);
        buf_i32(&w->frame, b->plane);
        for (int k = 0; k < 3; k++) buf_f64(&w->frame, b->origin[k]);
        for (int k = 0; k < 3; k++) buf_f64(&w->frame, b->dx[k]);
    }
    open_fields(w);
}

void lab_part_points(LabWriter *w, const char *name, int npoints, const double *xyz) {
    begin_part(w, LAB_POINTS, name);
    buf_i32(&w->frame, npoints);
    buf_put(&w->frame, xyz, (size_t)npoints * 3 * sizeof(double));
    open_fields(w);
}

void lab_part_cells(LabWriter *w, const char *name, int nnodes, const double *xyz, int ncells, int cell_type, const int *conn) {
    begin_part(w, LAB_CELLS, name);
    buf_i32(&w->frame, nnodes);
    buf_put(&w->frame, xyz, (size_t)nnodes * 3 * sizeof(double));
    buf_i32(&w->frame, ncells);
    buf_i32(&w->frame, cell_type);
    buf_put(&w->frame, conn, (size_t)ncells * (size_t)cell_type * sizeof(int32_t));
    open_fields(w);
}

void lab_field(LabWriter *w, const char *name, int location, size_t count, const float *data) {
    if (!w->part_nfields_at) return; /* no part: ignored, the reader would not know where it belongs */
    buf_put(&w->frame, "FELD", 4);
    buf_name(&w->frame, name);
    buf_i32(&w->frame, location);
    buf_u64(&w->frame, count);
    buf_put(&w->frame, data, count * sizeof(float));
    w->part_nfields++;
}

void lab_field_d(LabWriter *w, const char *name, int location, size_t count, const double *data) {
    float *f = malloc((count ? count : 1) * sizeof(float));
    if (!f) {
        w->frame.bad = true;
        return;
    }
    for (size_t i = 0; i < count; i++) f[i] = (float)data[i];
    lab_field(w, name, location, count, f);
    free(f);
}

bool lab_frame_end(LabWriter *w) {
    if (!w->in_frame) return false;
    close_part(w);
    w->in_frame = false;
    if (w->frame.bad) {
        w->failed = true;
        return false;
    }
    uint64_t n = w->frame.len;
    bool ok = fwrite("FRAM", 1, 4, w->fp) == 4 && fwrite(&n, 8, 1, w->fp) == 1 && fwrite(w->frame.p, 1, n, w->fp) == n &&
              fflush(w->fp) == 0;
    if (!ok) {
        w->failed = true;
        return false;
    }
    w->frames++;
    return true;
}

int lab_frames_written(const LabWriter *w) { return w->frames; }

bool lab_close(LabWriter *w) {
    if (!w) return false;
    bool ok = !w->failed;
    if (fclose(w->fp) != 0) ok = false;
    free(w->frame.p);
    free(w);
    return ok;
}

/* ---- reader ---- */
struct LabFile {
    FILE *fp;
    char *header;
    int nframes, cap;
    uint64_t *offset, *size;
    double *time;
};

LabFile *lab_open(const char *path, char *err, size_t errlen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "cannot open %s", path);
        return NULL;
    }
    char magic[8];
    uint32_t hl = 0;
    if (fread(magic, 1, 8, fp) != 8 || memcmp(magic, LAB_MAGIC, 8) != 0 || fread(&hl, 4, 1, fp) != 1 || hl > (64u << 20)) {
        snprintf(err, errlen, "%s is not a lab result (OPLAB01)", path);
        fclose(fp);
        return NULL;
    }
    LabFile *f = calloc(1, sizeof *f);
    char *h = malloc((size_t)hl + 1);
    if (!f || !h || fread(h, 1, hl, fp) != hl) {
        snprintf(err, errlen, "cannot read the header of %s", path);
        free(f);
        free(h);
        fclose(fp);
        return NULL;
    }
    h[hl] = 0;
    f->fp = fp;
    f->header = h;
    long body = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, body, SEEK_SET);
    for (;;) {
        char tag[4];
        uint64_t n;
        if (fread(tag, 1, 4, fp) != 4 || memcmp(tag, "FRAM", 4) != 0 || fread(&n, 8, 1, fp) != 1) break;
        long at = ftell(fp);
        double t;
        if (n < 8 || fread(&t, 8, 1, fp) != 1) break;
        /* a truncated last frame (a run stopped while writing) is left out */
        if (at + (long)n > fsize || fseek(fp, at + (long)n, SEEK_SET) != 0) break;
        if (f->nframes == f->cap) {
            int cap = f->cap ? 2 * f->cap : 64;
            uint64_t *o = realloc(f->offset, (size_t)cap * sizeof *o);
            if (o) f->offset = o;
            uint64_t *s = realloc(f->size, (size_t)cap * sizeof *s);
            if (s) f->size = s;
            double *tt = realloc(f->time, (size_t)cap * sizeof *tt);
            if (tt) f->time = tt;
            if (!o || !s || !tt) break;
            f->cap = cap;
        }
        f->offset[f->nframes] = (uint64_t)at;
        f->size[f->nframes] = n;
        f->time[f->nframes] = t;
        f->nframes++;
    }
    return f;
}

void lab_close_file(LabFile *f) {
    if (!f) return;
    fclose(f->fp);
    free(f->header);
    free(f->offset);
    free(f->size);
    free(f->time);
    free(f);
}

const char *lab_header(const LabFile *f) { return f->header; }
int lab_frame_count(const LabFile *f) { return f->nframes; }
double lab_frame_time(const LabFile *f, int i) { return (i >= 0 && i < f->nframes) ? f->time[i] : 0.0; }

typedef struct Cur {
    const unsigned char *p;
    size_t len, at;
    bool bad;
} Cur;

static bool cur_get(Cur *c, void *out, size_t n) {
    if (c->bad || c->at + n > c->len) {
        c->bad = true;
        return false;
    }
    memcpy(out, c->p + c->at, n);
    c->at += n;
    return true;
}
static int32_t cur_i32(Cur *c) {
    int32_t v = 0;
    cur_get(c, &v, 4);
    return v;
}
static void *cur_array(Cur *c, size_t n) {
    if (c->bad || c->at + n > c->len) {
        c->bad = true;
        return NULL;
    }
    void *q = malloc(n ? n : 1);
    if (!q) {
        c->bad = true;
        return NULL;
    }
    memcpy(q, c->p + c->at, n);
    c->at += n;
    return q;
}

void lab_frame_free(LabFrame *fr) {
    if (!fr) return;
    for (int i = 0; i < fr->nparts; i++) {
        LabPart *p = &fr->parts[i];
        free(p->blocks);
        free(p->xyz);
        free(p->conn);
        for (int k = 0; k < p->nfields; k++) free(p->fields[k].data);
        free(p->fields);
    }
    free(fr->parts);
    memset(fr, 0, sizeof *fr);
}

bool lab_read_frame(LabFile *f, int i, LabFrame *fr, char *err, size_t errlen) {
    memset(fr, 0, sizeof *fr);
    if (i < 0 || i >= f->nframes) {
        snprintf(err, errlen, "frame %d out of range (0..%d)", i, f->nframes - 1);
        return false;
    }
    unsigned char *raw = malloc(f->size[i]);
    if (!raw || fseek(f->fp, (long)f->offset[i], SEEK_SET) != 0 || fread(raw, 1, f->size[i], f->fp) != f->size[i]) {
        free(raw);
        snprintf(err, errlen, "cannot read frame %d", i);
        return false;
    }
    Cur c = {raw, f->size[i], 0, false};
    cur_get(&c, &fr->time, 8);
    int cap = 0;
    while (!c.bad && c.at < c.len) {
        char tag[4];
        if (!cur_get(&c, tag, 4) || memcmp(tag, "PART", 4) != 0) {
            c.bad = true;
            break;
        }
        if (fr->nparts == cap) {
            cap = cap ? 2 * cap : 4;
            LabPart *np = realloc(fr->parts, (size_t)cap * sizeof *np);
            if (!np) {
                c.bad = true;
                break;
            }
            fr->parts = np;
        }
        LabPart *p = &fr->parts[fr->nparts++];
        memset(p, 0, sizeof *p);
        p->kind = cur_i32(&c);
        cur_get(&c, p->name, 48);
        p->name[47] = 0;
        if (p->kind == LAB_BLOCKS) {
            p->nblocks = cur_i32(&c);
            if (p->nblocks < 0 || p->nblocks > (1 << 24)) {
                c.bad = true;
                break;
            }
            p->blocks = calloc((size_t)(p->nblocks ? p->nblocks : 1), sizeof *p->blocks);
            if (!p->blocks) {
                c.bad = true;
                break;
            }
            for (int b = 0; b < p->nblocks && !c.bad; b++) {
                LabBlock *bl = &p->blocks[b];
                for (int k = 0; k < 3; k++) bl->n[k] = cur_i32(&c);
                bl->level = cur_i32(&c);
                bl->plane = cur_i32(&c);
                cur_get(&c, bl->origin, 24);
                cur_get(&c, bl->dx, 24);
            }
        } else if (p->kind == LAB_POINTS) {
            p->npoints = cur_i32(&c);
            if (p->npoints < 0) c.bad = true;
            else p->xyz = cur_array(&c, (size_t)p->npoints * 24);
        } else if (p->kind == LAB_CELLS) {
            p->npoints = cur_i32(&c);
            if (p->npoints < 0) c.bad = true;
            else p->xyz = cur_array(&c, (size_t)p->npoints * 24);
            p->ncells = cur_i32(&c);
            p->cell_type = cur_i32(&c);
            if (p->ncells < 0 || (p->cell_type != LAB_LINE && p->cell_type != LAB_TRI && p->cell_type != LAB_QUAD && p->cell_type != LAB_HEX))
                c.bad = true;
            else p->conn = cur_array(&c, (size_t)p->ncells * (size_t)p->cell_type * 4);
            for (int k = 0; !c.bad && k < p->ncells * p->cell_type; k++)
                if (p->conn[k] < 0 || p->conn[k] >= p->npoints) c.bad = true;
        } else {
            c.bad = true;
        }
        if (c.bad) break;
        p->nfields = cur_i32(&c);
        if (p->nfields < 0 || p->nfields > 4096) {
            c.bad = true;
            break;
        }
        p->fields = calloc((size_t)(p->nfields ? p->nfields : 1), sizeof *p->fields);
        if (!p->fields) {
            c.bad = true;
            break;
        }
        for (int k = 0; k < p->nfields && !c.bad; k++) {
            LabField *fl = &p->fields[k];
            if (!cur_get(&c, tag, 4) || memcmp(tag, "FELD", 4) != 0) {
                c.bad = true;
                break;
            }
            cur_get(&c, fl->name, 48);
            fl->name[47] = 0;
            fl->location = cur_i32(&c);
            uint64_t n = 0;
            cur_get(&c, &n, 8);
            if (n > (c.len - c.at) / 4) {
                c.bad = true;
                break;
            }
            fl->count = (size_t)n;
            fl->data = cur_array(&c, fl->count * sizeof(float));
        }
    }
    free(raw);
    if (c.bad) {
        lab_frame_free(fr);
        snprintf(err, errlen, "frame %d is malformed", i);
        return false;
    }
    return true;
}

const LabPart *lab_find_part(const LabFrame *fr, const char *name) {
    for (int i = 0; i < fr->nparts; i++)
        if (strcmp(fr->parts[i].name, name) == 0) return &fr->parts[i];
    return NULL;
}

const LabField *lab_find_field(const LabPart *p, const char *name) {
    if (!p) return NULL;
    for (int i = 0; i < p->nfields; i++)
        if (strcmp(p->fields[i].name, name) == 0) return &p->fields[i];
    return NULL;
}

size_t lab_block_cells(const LabBlock *b) { return (size_t)b->n[0] * (size_t)b->n[1] * (size_t)(b->n[2] > 0 ? b->n[2] : 1); }

size_t lab_part_ncells(const LabPart *p) {
    size_t n = 0;
    for (int i = 0; i < p->nblocks; i++) n += lab_block_cells(&p->blocks[i]);
    return n;
}
