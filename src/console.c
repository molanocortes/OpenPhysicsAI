/* console.c - in-app terminal */
#include "console.h"
#include "font.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>

#define CON_LINES 2048
#define CON_LINE_LEN 360
#define CON_HISTORY 128
#define CON_INPUT 512
#define CON_MAX_CMDS 256

typedef struct {
    char text[CON_LINE_LEN];
    uint8_t level;
} ConLine;

static struct {
    pthread_mutex_t mtx;
    ConLine lines[CON_LINES];
    int head, count;
    char input[CON_INPUT];
    int cursor, len;
    char history[CON_HISTORY][CON_INPUT];
    int hist_count, hist_pos;
    char saved[CON_INPUT];
    const Command *cmds[CON_MAX_CMDS];
    int ncmds;
    bool focus;
    float scroll;
    double blink_t0;
} C = {.mtx = PTHREAD_MUTEX_INITIALIZER};

static void push_line(LogLevel level, const char *s, size_t n) {
    ConLine *l = &C.lines[C.head];
    if (n >= CON_LINE_LEN) n = CON_LINE_LEN - 1;
    memcpy(l->text, s, n);
    l->text[n] = 0;
    l->level = (uint8_t)level;
    C.head = (C.head + 1) % CON_LINES;
    if (C.count < CON_LINES) C.count++;
}

void console_log(LogLevel level, const char *msg) {
    static const char *tag[] = {"info", " ok ", "warn", "err!", "data", " >> "};
    fprintf(stderr, "[%s] %s\n", tag[level <= LOG_ECHO ? level : 0], msg);
    pthread_mutex_lock(&C.mtx);
    const char *s = msg;
    for (;;) {
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        push_line(level, s, n);
        if (!nl) break;
        s = nl + 1;
    }
    pthread_mutex_unlock(&C.mtx);
}

void console_init(void) {
    log_set_sink(console_log);
    C.blink_t0 = now_seconds();
}

void console_clear(void) {
    pthread_mutex_lock(&C.mtx);
    C.head = C.count = 0;
    C.scroll = 0;
    pthread_mutex_unlock(&C.mtx);
}

void console_register(const Command *cmds, int n) {
    for (int i = 0; i < n && C.ncmds < CON_MAX_CMDS; i++) C.cmds[C.ncmds++] = &cmds[i];
}

int console_command_count(void) { return C.ncmds; }
const Command *console_command(int i) { return (i >= 0 && i < C.ncmds) ? C.cmds[i] : NULL; }

const Command *console_find(const char *name) {
    for (int i = 0; i < C.ncmds; i++)
        if (str_ieq(C.cmds[i]->name, name)) return C.cmds[i];
    return NULL;
}

const char *complete_from_list(const char *const *list, const char *prefix, int index) {
    int k = 0;
    for (int i = 0; list[i]; i++)
        if (str_starts_with_i(list[i], prefix) && k++ == index) return list[i];
    return NULL;
}

/* splits on whitespace; supports "quoted strings" and backslash-escaped characters */
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *out = p;
        argv[argc++] = out;
        char quote = 0;
        while (*p) {
            if (quote) {
                if (*p == quote) {
                    quote = 0;
                    p++;
                    continue;
                }
            } else if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            } else if (isspace((unsigned char)*p)) {
                break;
            }
            if (*p == '\\' && p[1] && quote != '\'') p++;
            *out++ = *p++;
        }
        if (*p) p++;
        *out = 0;
    }
    return argc;
}

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 31 || lb > 31) return 99;
    int d[32][32];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]);
            int v = d[i - 1][j] + 1;
            v = MINI(v, d[i][j - 1] + 1);
            v = MINI(v, d[i - 1][j - 1] + cost);
            d[i][j] = v;
        }
    return d[la][lb];
}

static void exec_single(char *cmd) {
    char *argv[32];
    int argc = tokenize(cmd, argv, 32);
    if (argc == 0) return;
    const Command *c = console_find(argv[0]);
    if (!c) {
        const Command *best = NULL;
        int bd = 99;
        for (int i = 0; i < C.ncmds; i++) {
            int d = edit_distance(argv[0], C.cmds[i]->name);
            if (str_starts_with_i(C.cmds[i]->name, argv[0])) d = MINI(d, 1);
            if (d < bd) bd = d, best = C.cmds[i];
        }
        if (best && bd <= 2)
            LOGE("unknown command '%s' - did you mean '%s'?  (type 'help')", argv[0], best->name);
        else
            LOGE("unknown command '%s'  (type 'help')", argv[0]);
        return;
    }
    c->fn(argc, argv);
}

void console_exec(const char *line, bool echo) {
    char buf[2048];
    str_copy(buf, sizeof buf, line);
    if (echo) log_msg(LOG_ECHO, "%s", line);
    char *start = buf;
    char quote = 0;
    for (char *p = buf;; p++) {
        if (*p == '"' || *p == '\'') quote = quote == *p ? 0 : (quote ? quote : *p);
        if (*p == 0 || (*p == ';' && !quote)) {
            bool end = *p == 0;
            *p = 0;
            exec_single(start);
            if (end) break;
            start = p + 1;
        }
    }
}

void console_set_focus(bool focus) {
    C.focus = focus;
    C.blink_t0 = now_seconds();
}
bool console_focused(void) { return C.focus; }

void console_set_input(const char *text) {
    str_copy(C.input, sizeof C.input, text);
    C.len = C.cursor = (int)strlen(C.input);
    C.focus = true;
}

void console_scroll(float rows) {
    C.scroll += rows;
    if (C.scroll < 0) C.scroll = 0;
}

static void insert_text(const char *s) {
    size_t n = strlen(s);
    if (C.len + (int)n >= CON_INPUT - 1) return;
    memmove(C.input + C.cursor + n, C.input + C.cursor, (size_t)(C.len - C.cursor + 1));
    memcpy(C.input + C.cursor, s, n);
    C.cursor += (int)n;
    C.len += (int)n;
}

static int prev_char(int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)C.input[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static int next_char(int pos) {
    if (pos >= C.len) return C.len;
    pos++;
    while (pos < C.len && ((unsigned char)C.input[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static void delete_range(int a, int b) {
    if (b <= a) return;
    memmove(C.input + a, C.input + b, (size_t)(C.len - b + 1));
    C.len -= b - a;
    C.cursor = a;
}

static const char *path_completion(const char *prefix, int index) {
    static char out[1024];
    char dir[1024], base[512];
    const char *slash = strrchr(prefix, '/');
    if (slash) {
        size_t dl = (size_t)(slash - prefix) + 1;
        if (dl >= sizeof dir) return NULL;
        memcpy(dir, prefix, dl);
        dir[dl] = 0;
        str_copy(base, sizeof base, slash + 1);
    } else {
        dir[0] = 0;
        str_copy(base, sizeof base, prefix);
    }
    char open_dir[1024];
    if (dir[0] == '~') snprintf(open_dir, sizeof open_dir, "%s%s", getenv("HOME") ? getenv("HOME") : "", dir + 1);
    else snprintf(open_dir, sizeof open_dir, "%s", dir[0] ? dir : ".");
    DIR *d = opendir(open_dir);
    if (!d) return NULL;
    int k = 0;
    const char *res = NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && base[0] != '.') continue;
        if (!str_starts_with_i(e->d_name, base)) continue;
        bool is_dir = e->d_type == DT_DIR;
        const char *ext = strrchr(e->d_name, '.');
        if (!is_dir && !(ext && (str_ieq(ext, ".stl") || str_ieq(ext, ".nav") || str_ieq(ext, ".txt")))) continue;
        if (k++ == index) {
            snprintf(out, sizeof out, "%s%s%s", dir, e->d_name, is_dir ? "/" : "");
            res = out;
            break;
        }
    }
    closedir(d);
    return res;
}

/* candidates for the token under the cursor */
static const char *candidate(int argi, const char *cmdname, const char *prefix, int index) {
    if (argi == 0) {
        int k = 0;
        for (int i = 0; i < C.ncmds; i++)
            if (str_starts_with_i(C.cmds[i]->name, prefix) && k++ == index) return C.cmds[i]->name;
        return NULL;
    }
    const Command *c = console_find(cmdname);
    if (!c) return NULL;
    if (c->complete) return c->complete(argi, prefix, index);
    if (c->args && strstr(c->args, "file")) return path_completion(prefix, index);
    return NULL;
}

static void complete(void) {
    /* locate the token under the cursor */
    int start = C.cursor;
    while (start > 0 && !isspace((unsigned char)C.input[start - 1])) start--;
    char prefix[CON_INPUT];
    memcpy(prefix, C.input + start, (size_t)(C.cursor - start));
    prefix[C.cursor - start] = 0;
    int argi = 0;
    char cmdname[64] = {0};
    for (int i = 0, in_tok = 0; i < start; i++) {
        if (!isspace((unsigned char)C.input[i]) && !in_tok) {
            in_tok = 1;
            if (argi == 0) {
                int j = 0;
                while (i + j < C.len && !isspace((unsigned char)C.input[i + j]) && j < 63) cmdname[j] = C.input[i + j], j++;
            }
        } else if (isspace((unsigned char)C.input[i]) && in_tok) {
            in_tok = 0;
            argi++;
        }
    }
    if (start > 0 && argi == 0) argi = 1;
    const char *first = candidate(argi, cmdname, prefix, 0);
    if (!first) return;
    const char *second = candidate(argi, cmdname, prefix, 1);
    char common[CON_INPUT];
    str_copy(common, sizeof common, first);
    if (second) {
        char list[1024] = {0};
        size_t used = 0;
        for (int i = 0; i < 64; i++) {
            const char *c = candidate(argi, cmdname, prefix, i);
            if (!c) break;
            size_t k = 0;
            while (common[k] && c[k] && tolower((unsigned char)common[k]) == tolower((unsigned char)c[k])) k++;
            common[k] = 0;
            const char *disp = strrchr(c, '/');
            disp = (disp && disp[1]) ? disp + 1 : c;
            int w = snprintf(list + used, sizeof list - used, "%s  ", disp);
            if (w > 0 && used + (size_t)w < sizeof list) used += (size_t)w;
        }
        log_msg(LOG_DATA, "%s", list);
    }
    delete_range(start, C.cursor);
    insert_text(common);
    if (!second && common[strlen(common) - 1] != '/') insert_text(" ");
}

static void history_push(const char *line) {
    if (!line[0]) return;
    if (C.hist_count > 0 && strcmp(C.history[C.hist_count - 1], line) == 0) return;
    if (C.hist_count == CON_HISTORY) {
        memmove(C.history, C.history + 1, sizeof(C.history[0]) * (CON_HISTORY - 1));
        C.hist_count--;
    }
    str_copy(C.history[C.hist_count++], CON_INPUT, line);
}

bool console_handle_event(const PlatformEvent *e) {
    if (!C.focus) return false;
    C.blink_t0 = now_seconds();
    if (e->type == EV_TEXT) {
        if (e->mods & (MOD_CMD | MOD_CTRL)) return true;
        char utf[5] = {0};
        uint32_t cp = e->codepoint;
        if (cp < 0x80) utf[0] = (char)cp;
        else if (cp < 0x800) utf[0] = (char)(0xC0 | (cp >> 6)), utf[1] = (char)(0x80 | (cp & 63));
        else if (cp < 0x10000) utf[0] = (char)(0xE0 | (cp >> 12)), utf[1] = (char)(0x80 | ((cp >> 6) & 63)), utf[2] = (char)(0x80 | (cp & 63));
        else utf[0] = (char)(0xF0 | (cp >> 18)), utf[1] = (char)(0x80 | ((cp >> 12) & 63)), utf[2] = (char)(0x80 | ((cp >> 6) & 63)), utf[3] = (char)(0x80 | (cp & 63));
        insert_text(utf);
        return true;
    }
    if (e->type != EV_KEY_DOWN) return false;
    bool alt = e->mods & MOD_ALT, ctrl = e->mods & MOD_CTRL, cmd = e->mods & MOD_CMD;
    switch (e->key) {
    case KEY_ENTER: {
        char line[CON_INPUT];
        str_copy(line, sizeof line, C.input);
        history_push(line);
        C.hist_pos = C.hist_count;
        C.input[0] = 0;
        C.len = C.cursor = 0;
        C.scroll = 0;
        if (line[0]) console_exec(line, true);
        return true;
    }
    case KEY_BACKSPACE:
        if (alt || cmd) {
            int p = C.cursor;
            while (p > 0 && isspace((unsigned char)C.input[p - 1])) p--;
            while (p > 0 && !isspace((unsigned char)C.input[p - 1])) p--;
            delete_range(cmd ? 0 : p, C.cursor);
        } else {
            delete_range(prev_char(C.cursor), C.cursor);
        }
        return true;
    case KEY_DELETE: delete_range(C.cursor, next_char(C.cursor)); return true;
    case KEY_LEFT:
        if (alt) {
            while (C.cursor > 0 && isspace((unsigned char)C.input[C.cursor - 1])) C.cursor--;
            while (C.cursor > 0 && !isspace((unsigned char)C.input[C.cursor - 1])) C.cursor--;
        } else if (cmd) {
            C.cursor = 0;
        } else {
            C.cursor = prev_char(C.cursor);
        }
        return true;
    case KEY_RIGHT:
        if (alt) {
            while (C.cursor < C.len && isspace((unsigned char)C.input[C.cursor])) C.cursor++;
            while (C.cursor < C.len && !isspace((unsigned char)C.input[C.cursor])) C.cursor++;
        } else if (cmd) {
            C.cursor = C.len;
        } else {
            C.cursor = next_char(C.cursor);
        }
        return true;
    case KEY_HOME: C.cursor = 0; return true;
    case KEY_END: C.cursor = C.len; return true;
    case KEY_UP:
        if (C.hist_count == 0) return true;
        if (C.hist_pos == C.hist_count) str_copy(C.saved, sizeof C.saved, C.input);
        if (C.hist_pos > 0) C.hist_pos--;
        console_set_input(C.history[C.hist_pos]);
        return true;
    case KEY_DOWN:
        if (C.hist_pos < C.hist_count) C.hist_pos++;
        console_set_input(C.hist_pos == C.hist_count ? C.saved : C.history[C.hist_pos]);
        return true;
    case KEY_TAB: complete(); return true;
    case KEY_PAGEUP: console_scroll(10); return true;
    case KEY_PAGEDOWN: console_scroll(-10); return true;
    case KEY_ESCAPE:
        if (C.len) {
            C.input[0] = 0;
            C.len = C.cursor = 0;
        } else {
            C.focus = false;
        }
        return true;
    default: break;
    }
    if (cmd && e->key == 'v') {
        char clip[CON_INPUT];
        if (platform_clipboard_get(clip, sizeof clip)) {
            for (char *p = clip; *p; p++)
                if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            insert_text(clip);
        }
        return true;
    }
    if (cmd && e->key == 'c') {
        platform_clipboard_set(C.input);
        return true;
    }
    if (cmd) return false; /* other Cmd shortcuts (open, screenshot, ...) stay global while typing */
    if (ctrl) {
        switch (e->key) {
        case 'a': C.cursor = 0; return true;
        case 'e': C.cursor = C.len; return true;
        case 'u': delete_range(0, C.cursor); return true;
        case 'k': C.input[C.cursor] = 0, C.len = C.cursor; return true;
        case 'l': console_clear(); return true;
        case 'c':
            if (C.len) log_msg(LOG_ECHO, "%s^C", C.input);
            C.input[0] = 0;
            C.len = C.cursor = 0;
            return true;
        default: break;
        }
    }
    return e->key < 256 || e->key == KEY_ESCAPE; /* swallow printable keys so hotkeys don't fire */
}

static uint32_t level_color(int level) {
    switch (level) {
    case LOG_OK: return 0x52F5A8FFu;
    case LOG_WARN: return 0xFFBE55FFu;
    case LOG_ERROR: return 0xFF5C70FFu;
    case LOG_DATA: return 0x7FD8F0FFu;
    case LOG_ECHO: return 0xEAF6FFFFu;
    default: return 0xAFC3D2FFu;
    }
}

static int utf8_advance_cols(const char *s, int cols) {
    const char *p = s;
    for (int i = 0; i < cols && *p; i++) utf8_decode(&p);
    return (int)(p - s);
}

static int utf8_count(const char *s) {
    int n = 0;
    while (*s) utf8_decode(&s), n++;
    return n;
}

/* splits text into rows of at most cols codepoints, breaking after a space when one is close to the edge */
static int wrap_rows(const char *text, int cols, int *starts, int *lens, int max_rows) {
    int rows = 0;
    const char *p = text;
    while (*p && rows < max_rows) {
        const char *q = p, *last_space = NULL;
        int count = 0;
        while (*q && count < cols) {
            if (*q == ' ') last_space = q;
            utf8_decode(&q);
            count++;
        }
        const char *end = q;
        if (*q && last_space && (last_space - p) > (q - p) / 2) end = last_space + 1;
        starts[rows] = (int)(p - text);
        lens[rows] = (int)(end - p);
        rows++;
        p = end;
    }
    if (rows == 0) {
        starts[0] = lens[0] = 0;
        rows = 1;
    }
    return rows;
}

void console_draw(Ui *ui, float x, float y, float w, float h, double time) {
    const float lh = ui_line_height(ui, FONT_MONO);
    const float cw = ui_char_width(ui, FONT_MONO);
    const float pad = 10;
    const float input_h = lh + 14;
    const float log_h = h - input_h;
    int cols = MAXI(8, (int)((w - 2 * pad) / cw));

    pthread_mutex_lock(&C.mtx);
    /* count wrapped rows to clamp scrolling */
    int total_rows = 0;
    int starts[64], lens[64];
    for (int i = 0; i < C.count; i++) {
        const ConLine *l = &C.lines[(C.head - 1 - i + CON_LINES) % CON_LINES];
        total_rows += wrap_rows(l->text, l->level == LOG_ECHO ? cols - 2 : cols, starts, lens, 64);
        if (total_rows > 4000) break;
    }
    int visible = (int)(log_h / lh);
    float max_scroll = (float)MAXI(0, total_rows - visible);
    if (C.scroll > max_scroll) C.scroll = max_scroll;

    ui_push_clip(ui, x, y, w, log_h);
    float cy = y + log_h - 6 - lh + C.scroll * lh;
    for (int i = 0; i < C.count && cy > y - lh; i++) {
        const ConLine *l = &C.lines[(C.head - 1 - i + CON_LINES) % CON_LINES];
        const bool echo = l->level == LOG_ECHO;
        const float indent = echo ? 2 * cw : 0;
        int rows = wrap_rows(l->text, echo ? cols - 2 : cols, starts, lens, 64);
        float row_y = cy - (rows - 1) * lh;
        uint32_t col = level_color(l->level);
        for (int r = 0; r < rows; r++) {
            float ry = row_y + (float)r * lh;
            if (ry >= y + log_h || ry <= y - lh) continue;
            char seg[CON_LINE_LEN + 8];
            memcpy(seg, l->text + starts[r], (size_t)lens[r]);
            seg[lens[r]] = 0;
            if (echo && r == 0) ui_text(ui, FONT_MONO, x + pad, ry, 0x38E1FFFFu, "\xE2\x96\xB8");
            ui_text(ui, FONT_MONO, x + pad + indent, ry, col, seg);
        }
        cy -= rows * lh;
    }
    ui_pop_clip(ui);
    ui_rect_grad(ui, x, y, w, 18, 0x080E16F8u, 0x080E1600u, 0); /* soften rows cut at the top */
    if (C.scroll > 0) ui_text_right(ui, FONT_SMALL, x + w - pad, y + 4, 0x6F8699FFu, "\xE2\x86\x91 scrolled  (PgDn)");
    pthread_mutex_unlock(&C.mtx);

    /* input line */
    float iy = y + log_h;
    ui_rect(ui, x + 6, iy + 1, w - 12, input_h - 6, C.focus ? 0x0E1C28F0u : 0x0B141CF0u, 6);
    ui_rect_outline(ui, x + 6, iy + 1, w - 12, input_h - 6, C.focus ? 0x38E1FF70u : 0x2A4A6240u, 6, 1);
    float ty = iy + (input_h - 6 - lh) * 0.5f + 1;
    float px = x + pad + 4;
    px += ui_text(ui, FONT_BOLD, px, ty, C.focus ? 0x38E1FFFFu : 0x4F7A90FFu, "navier");
    px += ui_text(ui, FONT_MONO, px, ty, 0x6F8699FFu, " \xE2\x96\xB8 ");
    float avail = x + w - pad - 4 - px;
    int avail_cols = MAXI(4, (int)(avail / cw));
    /* horizontal scroll of the input so the cursor stays visible */
    int cursor_col = utf8_count(C.input) - utf8_count(C.input + C.cursor);
    int first_col = cursor_col > avail_cols - 2 ? cursor_col - (avail_cols - 2) : 0;
    const char *vis = C.input + utf8_advance_cols(C.input, first_col);
    char shown[CON_INPUT];
    int nb = utf8_advance_cols(vis, avail_cols);
    memcpy(shown, vis, (size_t)nb);
    shown[nb] = 0;
    ui_text(ui, FONT_MONO, px, ty, 0xEAF6FFFFu, shown);

    /* ghost completion / argument hint */
    if (C.focus && C.len > 0 && C.cursor == C.len) {
        char first[64] = {0};
        int k = 0;
        while (k < C.len && k < 63 && !isspace((unsigned char)C.input[k])) first[k] = C.input[k], k++;
        float gx = px + (float)utf8_count(shown) * cw;
        if (k == C.len) {
            for (int i = 0; i < C.ncmds; i++)
                if (str_starts_with_i(C.cmds[i]->name, first) && strlen(C.cmds[i]->name) > (size_t)k) {
                    char ghost[256];
                    snprintf(ghost, sizeof ghost, "%s %s", C.cmds[i]->name + k, C.cmds[i]->args ? C.cmds[i]->args : "");
                    ui_text(ui, FONT_MONO, gx, ty, 0x3E5263FFu, ghost);
                    break;
                }
        } else {
            const Command *c = console_find(first);
            if (c && c->args && C.input[C.len - 1] == ' ') ui_text(ui, FONT_MONO, gx, ty, 0x3E5263FFu, c->args);
        }
    }
    if (C.focus) {
        double t = time - C.blink_t0;
        bool on = fmod(t, 1.06) < 0.62;
        float cx = px + (float)(cursor_col - first_col) * cw;
        if (on) ui_rect(ui, cx, ty + 1, cw, lh - 2, 0x38E1FFC0u, 1);
    } else if (C.len == 0) {
        ui_text(ui, FONT_MONO, px, ty, 0x3E5263FFu, "click or press \xE2\x8F\x8E to type a command \xC2\xB7 'help'");
    }
}
