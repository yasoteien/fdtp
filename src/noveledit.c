// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _XOPEN_SOURCE 700
/* Novel editor screen (docs/FDTP_NOVEL.md). The text lives in novel_doc.c,
 * one line is folded by novel_layout.c (ruby from novel_ruby.c) only when it
 * is near the visible part, glyphs come from novel_glyph.c, input from ime.h. */
#include "noveledit.h"
#include "i18n.h"
#include "app.h"
#include "ime.h"
#include "novel_doc.h"
#include "novel_docx.h"
#include "novel_format.h"
#include "novel_glyph.h"
#include "novel_layout.h"
#include "font24.h"
#include "raster.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NOVEL_MAX_BYTES (16u * 1024 * 1024)
#define AUTOSAVE_SECONDS 30.0
#define STATE_ENTRIES 500
#define INPUT_CAP 1024
#define DEBUG_GLYPHS 2048
/* Geometry (docs/FDTP_NOVEL.md 2, revision 1 3.16 and item 29). The screen's
 * frame (cells.h frame_open) as every full screen: the name in its top line,
 * the place (行・字), the writing direction in its bottom line; the text in
 * its body, x BODY_LEFT .. BODY_RIGHT, y 24 .. BODY_BOTTOM (528 at 25 rows).
 * Horizontal rows start at y 24 (ruby 12 + body 24 per row): 14 rows of 39
 * characters at 80 x 25. Vertical columns run from the right of the body to
 * its left (26 of 21 characters at 80 x 25, 27 at 85 x 25).
 * Line numbers (C-l): horizontal, at least four digits in columns 1 ..
 * digits, a line down column digits + 1 joining the frame's top and bottom
 * lines, the text after it; vertical, the number above each column in the
 * 12-dot font, a line across the frame (y 40-41) and the text from y 44. */
#define BODY_LEFT (CANVAS_GRID_X + 12)
#define BODY_RIGHT (CANVAS_GRID_X + FD_W - 12)
#define BODY_BOTTOM (FRAME_BOTTOM * 24)
#define H_TOP 24
#define H_ROWS ((BODY_BOTTOM - H_TOP) / 36)
#define V_RIGHT BODY_RIGHT
#define V_TOP 24
#define V_NUM_Y 26
#define V_NUM_RULE_Y 40
#define V_NUM_TOP 44

typedef enum {
    P_NONE, P_SEARCH, P_REPLACE_FROM, P_REPLACE_TO, P_REPLACE_ASK, P_GOTO,
    P_CLOSE, P_UTF8, P_UTF8_CLOSE, P_RECOVER, P_UTF8_AS
} PromptKind;
/* Save as (revision 1 3.18): the name box, then the overwrite question
 * (both App widgets, boxes over the text). F2 (revision 1 4): the choice of
 * conversion, then its y/n question. */
enum { ASK_NONE, ASK_NAME, ASK_OVERWRITE, ASK_CONVERT, ASK_CONVERT_YN, ASK_LOSSY };
enum { CONVERT_FORMAT, CONVERT_MANUSCRIPT, CONVERT_WEB, CONVERT_WORD, CONVERTS };
enum { EDIT_NONE, EDIT_TYPE, EDIT_BS, EDIT_DEL };
typedef struct { uint32_t code; int x, y, w, h, color, kind; } DebugGlyph;

typedef struct {
    Screen screen;
    NovelDoc doc;
    char *path, *name, *autosave, *state;
    NPos cur, anchor;
    int has_anchor, vertical, numbers, number_digits, goal, last_edit;
    NPos last_end;
    size_t top_line, top_row;
    Ime *ime;
    NLayout lay;
    char *scratch; size_t scratch_cap;
    PromptKind prompt;
    char input[INPUT_CAP]; size_t input_cur;
    int input_fresh; /* prefilled text: the first character typed replaces it */
    char query[INPUT_CAP], replacement[INPUT_CAP];
    NPos replace_origin, replace_from; int replace_wrapped, replace_count;
    int autosave_pending; double change_time;
    int ask;      /* ASK_*: which App widget is ours */
    char *target; /* save as: the chosen path until it is saved */
    char *asked;  /* save as: the name typed last (N goes back to it) */
    int convert;  /* F2: the CONVERT_* chosen, until its question is answered */
    PromptKind loss_kind; /* continuation after the lossy-save question */
    int loss_utf8, loss_ready; /* approval is consumed by one save attempt */
    int caret_x, caret_y, caret_visible;
    int field_x, field_y; /* caret in the name box, for the candidate list */
    DebugGlyph dbg[DEBUG_GLYPHS]; size_t ndbg;
} Novel;

/* The application's own clipboard, shared by every editor (FDTP_NOVEL.md 4). */
static char *clipboard;
static size_t clipboard_len;

static uint32_t rgb(int color) { return fd_palette[color]; }
static int ctrl(const Key *k, uint32_t ch)
{
    uint32_t c = k->ch >= 'A' && k->ch <= 'Z' ? k->ch + 32 : k->ch;
    return k->key == KCHAR && c == ch && (k->mods & (MOD_CTRL | MOD_ALT)) == MOD_CTRL;
}
/* Keep the largest document-wide width reached while this editor is open. */
static void update_number_digits(Novel *v)
{
    int digits = 1;
    for (size_t n = v->doc.count; n >= 10; n /= 10) ++digits;
    if (digits < 4) digits = 4;
    if (digits > v->number_digits) v->number_digits = digits;
}
/* The numbers and the line after them (columns 1 .. digits + 1). */
static int number_width(const Novel *v) { return v->number_digits * 12 + 12; }
static int column_pitch(const Novel *v)
{
    int w = v->numbers ? v->number_digits * 6 : 0;
    return w > 36 ? w : 36;
}
static int page(const Novel *v) { return v->vertical ? (BODY_RIGHT - BODY_LEFT) / column_pitch(v) : H_ROWS; }
static int body_x(const Novel *v) { return BODY_LEFT + (v->numbers ? number_width(v) : 0); }
static int text_top(const Novel *v) { return v->vertical && v->numbers ? V_NUM_TOP : V_TOP; }
/* Characters of a vertical column: what fits between its top and the body's bottom. */
static int v_chars(const Novel *v) { return (BODY_BOTTOM - text_top(v)) / 24; }
static int row_width(const Novel *v)
{
    return v->vertical ? v_chars(v) * 24 : BODY_RIGHT - body_x(v);
}

/* ---- files: novel-state and autosave ---------------------------------- */
static char *join(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", a, b);
    return p;
}
static void make_parents(const char *path)
{
    char buffer[PATH_MAX];
    if (strlen(path) >= sizeof(buffer)) return;
    strcpy(buffer, path);
    for (char *p = buffer + 1; *p; ++p)
        if (*p == '/') { *p = 0; mkdir(buffer, 0700); *p = '/'; }
}
static char *home_file(const char *env, const char *name)
{
    const char *set = getenv(env);
    if (set && *set) return strdup(set);
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char *dir = join(home, ".fdtp");
    char *p = dir ? join(dir, name) : NULL;
    free(dir);
    return p;
}
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *p = malloc(cap);
    while (p) {
        size_t got = fread(p + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0) break;
        if (n + 1 == cap) {
            if (cap > NOVEL_MAX_BYTES * 2) { free(p); p = NULL; break; }
            char *q = realloc(p, cap * 2);
            if (!q) { free(p); p = NULL; break; }
            p = q; cap *= 2;
        }
    }
    if (p && ferror(f)) { free(p); p = NULL; }
    fclose(f);
    if (p) { p[n] = 0; *len = n; }
    return p;
}
static int write_file(const char *path, const char *data, size_t n)
{
    make_parents(path);
    size_t pl = strlen(path);
    char *tmp = malloc(pl + 8);
    if (!tmp) return -1;
    snprintf(tmp, pl + 8, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int rc = -1;
    if (f) {
        rc = fwrite(data, 1, n, f) == n ? 0 : -1;
        if (fclose(f)) rc = -1;
        if (!rc) rc = rename(tmp, path);
        if (rc) unlink(tmp);
    }
    free(tmp);
    return rc;
}
/* novel-state: lines "h<TAB>/abs/path" or "v<TAB>/abs/path", newest last;
 * an 'n' after the h or v means line numbers on ("vn<TAB>/abs/path"). */
enum { STATE_VERTICAL = 1, STATE_NUMBERS = 2 };
static const char *state_line(const char *line, int *flags)
{
    if (line[0] != 'h' && line[0] != 'v') return NULL;
    *flags = line[0] == 'v' ? STATE_VERTICAL : 0;
    size_t at = 1;
    if (line[at] == 'n') { *flags |= STATE_NUMBERS; ++at; }
    return line[at] == '\t' ? line + at + 1 : NULL;
}
static int state_get(const char *file, const char *path)
{
    size_t n;
    char *text = file ? read_all(file, &n) : NULL;
    if (!text) return -1;
    int found = -1;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        int flags;
        const char *p = state_line(line, &flags);
        if (p && !strcmp(p, path)) found = flags;
        line = end ? end + 1 : NULL;
    }
    free(text);
    return found;
}
static void state_put(const char *file, const char *path, int vertical, int numbers)
{
    if (!file || strchr(path, '\n')) return;
    size_t n = 0, kept = 0;
    char *text = read_all(file, &n);
    char **lines = calloc(STATE_ENTRIES + 1, sizeof(char *));
    if (!lines) { free(text); return; }
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        int flags;
        const char *p = state_line(line, &flags);
        if (p && strcmp(p, path)) {
            if (kept == STATE_ENTRIES) { memmove(lines, lines + 1, (STATE_ENTRIES - 1) * sizeof(char *)); --kept; }
            lines[kept++] = line;
        }
        line = end ? end + 1 : NULL;
    }
    if (kept == STATE_ENTRIES) { memmove(lines, lines + 1, (STATE_ENTRIES - 1) * sizeof(char *)); --kept; }
    size_t total = strlen(path) + 4;
    for (size_t i = 0; i < kept; ++i) total += strlen(lines[i]) + 1;
    char *out = malloc(total + 1), *p = out;
    if (out) {
        for (size_t i = 0; i < kept; ++i) p += sprintf(p, "%s\n", lines[i]);
        p += sprintf(p, "%c%s\t%s\n", vertical ? 'v' : 'h', numbers ? "n" : "", path);
        write_file(file, out, (size_t)(p - out));
    }
    free(out); free(lines); free(text);
}
static char *autosave_path(const char *path)
{
    const char *set = getenv("FDTP_NOVEL_AUTOSAVE");
    char *dir = NULL;
    if (set && *set) dir = strdup(set);
    else if (getenv("HOME") && *getenv("HOME")) {
        char *fdtp = join(getenv("HOME"), ".fdtp");
        dir = fdtp ? join(fdtp, "autosave") : NULL;
        free(fdtp);
    }
    if (!dir) return NULL;
    unsigned long long hash = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) { hash ^= *p; hash *= 1099511628211ull; }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[128];
    int n = snprintf(name, sizeof(name), "%016llx-", hash);
    for (const char *p = base; *p && n < 100; ++p)
        name[n++] = (*p == '/' || (unsigned char)*p < 32) ? '_' : *p;
    while (n > 17 && ((unsigned char)name[n - 1] & 0xc0) == 0x80) --n; /* whole characters only */
    if (n > 17 && (unsigned char)name[n - 1] >= 0xc0) --n;
    name[n] = 0;
    strcat(name, ".txt");
    char *p = join(dir, name);
    free(dir);
    return p;
}
static void write_autosave(Novel *v)
{
    if (!v->autosave) return;
    size_t n;
    char *text = novel_doc_text(&v->doc, &n);
    if (!text) return;
    size_t skip = v->doc.bom ? 3 : 0;
    write_file(v->autosave, text + skip, n - skip);
    free(text);
}
static void remove_autosave(Novel *v)
{
    if (v->autosave) unlink(v->autosave);
    v->autosave_pending = 0;
}

/* ---- layout helpers --------------------------------------------------- */
/* Lay out one line. The caret line shows a ruby span under the caret as
 * written and, with preedit, the input method's text at the caret. */
static void lay_line(Novel *v, size_t line, int preedit)
{
    NLine *l = &v->doc.lines[line];
    size_t cursor = line == v->cur.line ? v->cur.off : (size_t)-1;
    v->lay.width = row_width(v);
    ImePreedit pe = {"", 0, 0, 0};
    if (preedit && line == v->cur.line) ime_preedit(v->ime, &pe);
    size_t pn = strlen(pe.text);
    if (!pn) { nl_run(&v->lay, l->s, l->len, v->vertical, cursor, 0, 0, 0, 0); return; }
    size_t need = l->len + pn + 1;
    if (need > v->scratch_cap) {
        char *p = realloc(v->scratch, need);
        if (!p) { nl_run(&v->lay, l->s, l->len, v->vertical, cursor, 0, 0, 0, 0); return; }
        v->scratch = p; v->scratch_cap = need;
    }
    memcpy(v->scratch, l->s, v->cur.off);
    memcpy(v->scratch + v->cur.off, pe.text, pn);
    memcpy(v->scratch + v->cur.off + pn, l->s + v->cur.off, l->len - v->cur.off);
    size_t at = v->cur.off;
    nl_run(&v->lay, v->scratch, l->len + pn, v->vertical, at + pe.caret, at, at + pn,
           at + pe.hl_start, at + pe.hl_end);
}
static size_t rows_of(Novel *v, size_t line)
{
    lay_line(v, line, 0);
    return v->lay.nrow ? v->lay.nrow : 1;
}
static void cursor_row(Novel *v, size_t *row, int *pos)
{
    lay_line(v, v->cur.line, 0);
    nl_locate(&v->lay, v->cur.off, row, pos);
}
/* Move a (line, row) display position by delta rows; returns rows moved. */
static long step(Novel *v, size_t *line, size_t *row, long delta)
{
    long moved = 0;
    size_t rows = delta > 0 ? rows_of(v, *line) : 0;
    while (delta > 0) {
        if (*row + 1 < rows) ++*row;
        else if (*line + 1 < v->doc.count) { ++*line; *row = 0; rows = rows_of(v, *line); }
        else break;
        --delta; ++moved;
    }
    while (delta < 0) {
        if (*row > 0) --*row;
        else if (*line > 0) { --*line; *row = rows_of(v, *line) - 1; }
        else break;
        ++delta; --moved;
    }
    return moved;
}
static void clamp_top(Novel *v)
{
    if (v->top_line >= v->doc.count) { v->top_line = v->doc.count - 1; v->top_row = 0; }
    size_t rows = rows_of(v, v->top_line);
    if (v->top_row >= rows) v->top_row = rows - 1;
}
static void ensure_visible(Novel *v)
{
    update_number_digits(v);
    size_t row; int pos;
    clamp_top(v);
    cursor_row(v, &row, &pos);
    if (v->cur.line < v->top_line || (v->cur.line == v->top_line && row < v->top_row)) {
        v->top_line = v->cur.line; v->top_row = row;
        return;
    }
    size_t line = v->top_line, r = v->top_row;
    for (int count = 0; count < page(v); ++count) {
        if (line == v->cur.line && r == row) return;
        if (line > v->cur.line) break;
        if (!step(v, &line, &r, 1)) break;
    }
    line = v->cur.line; r = row;
    step(v, &line, &r, -(page(v) - 1));
    v->top_line = line; v->top_row = r;
}

/* ---- editing ---------------------------------------------------------- */
static void changed(Novel *v, App *a)
{
    update_number_digits(v);
    if (!v->autosave_pending) { v->autosave_pending = 1; v->change_time = a->now; }
}
static int selection(const Novel *v, NPos *a, NPos *b)
{
    if (!v->has_anchor || !npos_cmp(v->anchor, v->cur)) return 0;
    if (npos_cmp(v->anchor, v->cur) < 0) { *a = v->anchor; *b = v->cur; }
    else { *a = v->cur; *b = v->anchor; }
    return 1;
}
static void delete_selection(Novel *v)
{
    NPos a, b;
    if (selection(v, &a, &b)) v->cur = novel_doc_delete(&v->doc, a, b, NULL);
    v->has_anchor = 0;
}
static void insert(Novel *v, App *a, const char *text, size_t n, int kind)
{
    NPos sa, sb;
    int sel = selection(v, &sa, &sb);
    if (sel || kind == EDIT_NONE || kind != v->last_edit || npos_cmp(v->cur, v->last_end)) novel_doc_group(&v->doc);
    delete_selection(v);
    int ok;
    v->cur = novel_doc_insert(&v->doc, v->cur, text, n, &ok);
    if (!ok) app_notice(a, T("メモリが足りません"));
    v->last_edit = kind; v->last_end = v->cur;
    v->goal = -1;
    changed(v, a);
}
static void erase(Novel *v, App *a, int forward)
{
    NPos sa, sb;
    if (selection(v, &sa, &sb)) {
        novel_doc_group(&v->doc);
        delete_selection(v);
        v->last_edit = EDIT_NONE;
    } else {
        NPos from = v->cur, to = v->cur;
        NLine *l = &v->doc.lines[v->cur.line];
        if (forward) {
            if (to.off < l->len) to.off = novel_next(l->s, l->len, to.off);
            else if (to.line + 1 < v->doc.count) { ++to.line; to.off = 0; }
        } else {
            if (from.off) from.off = novel_prev(l->s, from.off);
            else if (from.line) { --from.line; from.off = v->doc.lines[from.line].len; }
        }
        if (!npos_cmp(from, to)) return;
        int kind = forward ? EDIT_DEL : EDIT_BS;
        if (kind != v->last_edit || npos_cmp(v->cur, v->last_end)) novel_doc_group(&v->doc);
        v->cur = novel_doc_delete(&v->doc, from, to, NULL);
        v->last_edit = kind; v->last_end = v->cur;
    }
    v->goal = -1;
    changed(v, a);
}
static void move_to(Novel *v, NPos p, int shift)
{
    if (shift) { if (!v->has_anchor) { v->anchor = v->cur; v->has_anchor = 1; } }
    else v->has_anchor = 0;
    v->cur = novel_doc_clamp(&v->doc, p);
    v->last_edit = EDIT_NONE;
}
static void move_chars(Novel *v, int forward, int shift)
{
    NPos p = v->cur;
    NLine *l = &v->doc.lines[p.line];
    if (forward) {
        if (p.off < l->len) p.off = novel_next(l->s, l->len, p.off);
        else if (p.line + 1 < v->doc.count) { ++p.line; p.off = 0; }
    } else {
        if (p.off) p.off = novel_prev(l->s, p.off);
        else if (p.line) { --p.line; p.off = v->doc.lines[p.line].len; }
    }
    move_to(v, p, shift);
    v->goal = -1;
}
static void move_rows(Novel *v, long delta, int shift)
{
    size_t row; int pos;
    cursor_row(v, &row, &pos);
    if (v->goal < 0) v->goal = pos;
    size_t line = v->cur.line, r = row;
    long moved = step(v, &line, &r, delta);
    NPos p;
    if (!moved) {
        lay_line(v, line, 0);
        p = (NPos){line, delta < 0 ? nl_row_start(&v->lay, r) : nl_row_end(&v->lay, r)};
        if (delta < 0 && line == 0 && r == 0) p.off = 0;
        if (delta > 0 && line + 1 == v->doc.count) p.off = v->doc.lines[line].len;
    } else {
        lay_line(v, line, 0);
        p = (NPos){line, nl_hit(&v->lay, r, v->goal)};
    }
    int goal = v->goal;
    move_to(v, p, shift);
    v->goal = goal;
}
static void row_edge(Novel *v, int end, int shift)
{
    size_t row; int pos;
    cursor_row(v, &row, &pos);
    move_to(v, (NPos){v->cur.line, end ? nl_row_end(&v->lay, row) : nl_row_start(&v->lay, row)}, shift);
    v->goal = -1;
}
static void page_move(Novel *v, int down, int shift)
{
    long delta = down ? page(v) : -page(v);
    size_t line = v->top_line, r = v->top_row;
    clamp_top(v);
    step(v, &line, &r, delta);
    v->top_line = line; v->top_row = r;
    move_rows(v, delta, shift);
}
static void copy_selection(Novel *v)
{
    NPos a, b;
    if (!selection(v, &a, &b)) return;
    size_t n;
    char *text = novel_doc_copy(&v->doc, a, b, &n);
    if (!text) return;
    free(clipboard);
    clipboard = text; clipboard_len = n;
}
static void undo(Novel *v, App *a, int redo)
{
    NPos p = v->cur;
    int done = redo ? novel_doc_redo(&v->doc, &p) : novel_doc_undo(&v->doc, &p);
    if (!done) { app_notice(a, redo ? T("やり直す操作はありません") : T("取り消す操作はありません")); return; }
    v->cur = novel_doc_clamp(&v->doc, p);
    v->has_anchor = 0; v->last_edit = EDIT_NONE; v->goal = -1;
    changed(v, a);
}
/* Kind of character a ruby base run is made of: 1 kanji, 2 letters and
 * digits (half or full width), 3 katakana (with ー), 0 anything else. */
static int ruby_base_kind(uint32_t c)
{
    if (novel_is_kanji(c)) return 1;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0xff10 && c <= 0xff19) || (c >= 0xff21 && c <= 0xff3a) || (c >= 0xff41 && c <= 0xff5a)) return 2;
    if ((c >= 0x30a1 && c <= 0x30fa) || c == 0x30fc || (c >= 0xff66 && c <= 0xff9d)) return 3;
    return 0;
}
/* C-r: ｜ before the selection (or the run right before the caret) and
 * 《》 after it, the caret between 《 and 》. */
static void ruby_input(Novel *v, App *a)
{
    NPos sa, sb;
    novel_doc_group(&v->doc);
    if (selection(v, &sa, &sb)) {
        if (sa.line != sb.line) { app_notice(a, T("ルビは1行の中で付けてください")); return; }
        novel_doc_insert(&v->doc, sb, "《》", 6, NULL); /* i18n:keep */
        novel_doc_insert(&v->doc, sa, "｜", 3, NULL); /* i18n:keep */
        v->cur = (NPos){sb.line, sb.off + 6};
    } else {
        /* The run right before the caret of one kind: kanji, letters and
         * digits (half or full width), or katakana. Kanji only left a
         * Latin word outside the base (｜ after it, an empty base). */
        NLine *l = &v->doc.lines[v->cur.line];
        size_t start = v->cur.off;
        int kind = 0;
        while (start) {
            size_t prev = novel_prev(l->s, start);
            int k = ruby_base_kind(novel_decode(l->s, l->len, prev, NULL));
            if (!k || (kind && k != kind)) break;
            kind = k;
            start = prev;
        }
        NPos at = v->cur;
        novel_doc_insert(&v->doc, at, "《》", 6, NULL); /* i18n:keep */
        novel_doc_insert(&v->doc, (NPos){at.line, start}, "｜", 3, NULL); /* i18n:keep */
        v->cur = (NPos){at.line, start < at.off ? at.off + 6 : at.off + 3};
    }
    v->has_anchor = 0; v->last_edit = EDIT_NONE; v->goal = -1;
    novel_doc_group(&v->doc);
    changed(v, a);
}

/* ---- prompts on row 23 ------------------------------------------------ */
static void open_prompt(Novel *v, PromptKind kind, const char *initial)
{
    v->prompt = kind;
    snprintf(v->input, sizeof(v->input), "%s", initial ? initial : "");
    v->input_cur = strlen(v->input);
    v->input_fresh = v->input[0] != 0;
}
static void input_insert(Novel *v, const char *s, size_t n)
{
    if (v->input_fresh) { v->input[0] = 0; v->input_cur = 0; v->input_fresh = 0; }
    size_t len = strlen(v->input);
    if (len + n + 1 > sizeof(v->input)) return;
    for (size_t i = 0; i < n; ++i) if (s[i] == '\n' || s[i] == '\r') return;
    memmove(v->input + v->input_cur + n, v->input + v->input_cur, len - v->input_cur + 1);
    memcpy(v->input + v->input_cur, s, n);
    v->input_cur += n;
}
static int text_prompt(PromptKind k) { return k == P_SEARCH || k == P_REPLACE_FROM || k == P_REPLACE_TO || k == P_GOTO; }
static const char *prompt_label(const Novel *v)
{
    switch (v->prompt) {
    case P_SEARCH: return T("検索");
    case P_REPLACE_FROM: return T("置換する文字");
    case P_REPLACE_TO: return T("置換後の文字");
    case P_GOTO: return T("行へ移動");
    case P_REPLACE_ASK: return T("置換しますか？ (Y:置換 N:次へ A:全部 Esc:やめる)");
    case P_CLOSE: return T("保存しますか？ (Y/N/Esc)");
    case P_UTF8: case P_UTF8_CLOSE: case P_UTF8_AS: return T("この文字コードで表せない字があります。UTF-8 で保存しますか？ (Y/N)");
    case P_RECOVER: return T("前回の続きがあります。開きますか？ (Y/N)");
    default: return "";
    }
}
/* Write the text to path in its encoding and line ends. -2 from the
 * document (a character the encoding cannot hold) opens the ask prompt;
 * a failure (no such directory, no permission) is an error box. */
static int save_to(Novel *v, App *a, const char *path, int utf8, PromptKind ask)
{
    if (v->doc.lossy && !v->loss_ready) {
        v->loss_kind = ask;
        v->loss_utf8 = utf8;
        widgets_confirm(&a->widgets, T("元のバイト列には戻りません。保存しますか"));
        v->ask = ASK_LOSSY;
        return 0;
    }
    v->loss_ready = 0;
    int rc = novel_doc_save(&v->doc, path, utf8);
    if (rc == -2) { open_prompt(v, ask, NULL); return 0; }
    if (rc) {
        int e = errno;
        char message[256];
        snprintf(message, sizeof(message), T("保存できません: %s"),
                 e == ENOENT ? T("そのディレクトリはありません") : e == EACCES ? T("書き込めません") :
                 e == EISDIR ? T("ディレクトリです") : strerror(e));
        app_error(a, message);
        return 0;
    }
    remove_autosave(v);
    return 1;
}
static int save(Novel *v, App *a, int utf8, PromptKind ask)
{
    if (!save_to(v, a, v->path, utf8, ask)) return 0;
    char message[96];
    snprintf(message, sizeof(message), T("保存しました（%s）"), jis_encoding_name(v->doc.encoding));
    app_notice(a, message);
    return 1;
}
/* ---- save as (revision 1 3.18) ----------------------------------------- */
static char *absolute(const char *path);
/* The directory of the file being edited, "/" for a file in the root. */
static char *directory_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return strdup(".");
    size_t n = slash == path ? 1 : (size_t)(slash - path);
    char *dir = malloc(n + 1);
    if (dir) { memcpy(dir, path, n); dir[n] = 0; }
    return dir;
}
static void open_name_box(Novel *v, App *a, const char *initial)
{
    char *dir = directory_of(v->path);
    widgets_input_history(&a->widgets, T("名前を付けて保存"), initial, dir ? dir : ".", WIDGET_HISTORY_NAME);
    free(dir);
    v->ask = ASK_NAME;
}
/* A relative name is in the directory of the file being edited; ~/ is $HOME. */
static char *resolve(const Novel *v, const char *input)
{
    if (input[0] == '/') return strdup(input);
    const char *home = getenv("HOME");
    if (input[0] == '~' && input[1] == '/' && home && *home) return join(home, input + 2);
    char *dir = directory_of(v->path);
    char *p = NULL;
    if (dir && !strcmp(dir, "/")) {
        size_t n = strlen(input) + 2;
        p = malloc(n);
        if (p) snprintf(p, n, "/%s", input);
    } else if (dir) p = join(dir, input);
    free(dir);
    return p;
}
/* The file just saved under another name is the one edited from now on: the
 * title, the remembered direction and line numbers, the autosave copy. */
static void adopt(Novel *v, App *a, char *target)
{
    char *path = absolute(target);
    free(target);
    const char *base = path ? strrchr(path, '/') : NULL;
    char *name = path ? strdup(base ? base + 1 : path) : NULL;
    if (!path || !name) { free(path); free(name); app_error(a, T("メモリが足りません")); return; }
    free(v->path); free(v->name);
    v->path = path; v->name = name;
    free(v->autosave);
    v->autosave = autosave_path(v->path);
    state_put(v->state, v->path, v->vertical, v->numbers);
    char message[512];
    snprintf(message, sizeof(message), T("%s に保存しました（%s）"), v->name, jis_encoding_name(v->doc.encoding));
    app_notice(a, message);
}
/* target is owned; kept in v->target while the UTF-8 question is open. */
static void save_as(Novel *v, App *a, char *target, int utf8)
{
    free(v->target);
    v->target = target;
    if (save_to(v, a, target, utf8, P_UTF8_AS)) {
        v->target = NULL;
        adopt(v, a, target);
    } else if (v->prompt != P_UTF8_AS && v->ask != ASK_LOSSY) { free(v->target); v->target = NULL; }
}
static void name_chosen(Novel *v, App *a, const char *input)
{
    if (!*input) return;
    if (input[strlen(input) - 1] == '/') { app_error(a, T("ファイルの名前がありません")); return; }
    char *target = resolve(v, input);
    if (!target) { app_error(a, T("メモリが足りません")); return; }
    struct stat st, now;
    if (!stat(target, &st)) {
        if (S_ISDIR(st.st_mode)) { free(target); app_error(a, T("ディレクトリには保存できません")); return; }
        if (!stat(v->path, &now) && st.st_dev == now.st_dev && st.st_ino == now.st_ino) {
            free(target);
            save(v, a, 0, P_UTF8);   /* the same file: a plain save */
            return;
        }
        const char *base = strrchr(target, '/');
        char message[512];
        snprintf(message, sizeof(message), T("%s は既にあります。上書きしますか？"), base ? base + 1 : target);
        free(v->target);
        v->target = target;
        widgets_confirm(&a->widgets, message);
        v->ask = ASK_OVERWRITE;
        return;
    }
    save_as(v, a, target, 0);
}
/* The name box takes Japanese input like the editor: committed text goes
 * into the field at its cursor (widgets_key inserts ASCII only). */
static void field_insert(Widgets *w, const char *s)
{
    size_t len = strlen(w->text), n = strlen(s);
    if (len + n + 1 > sizeof(w->text) || w->cursor > len) return;
    for (size_t i = 0; i < n; ++i) if (s[i] == '\n' || s[i] == '\r') return;
    memmove(w->text + w->cursor + n, w->text + w->cursor, len - w->cursor + 1);
    memcpy(w->text + w->cursor, s, n);
    w->cursor += n;
    w->edited = 1;
    w->list_selected = w->list_top = 0;
}
static void convert_chosen(Novel *v, App *a, int which);
static void convert_run(Novel *v, App *a);
static void ask_key(Novel *v, App *a, const Key *k)
{
    Widgets *w = &a->widgets;
    if (v->ask == ASK_CONVERT || v->ask == ASK_CONVERT_YN) {
        int ask = v->ask;
        WidgetResult r = widgets_key(w, k);
        if (r == WIDGET_ACCEPT) {
            v->ask = ASK_NONE;
            if (ask == ASK_CONVERT) convert_chosen(v, a, w->selected);
            else convert_run(v, a);
        } else if (r == WIDGET_CANCEL) v->ask = ASK_NONE;
        return;
    }
    if (v->ask == ASK_NAME) {
        int consumed = ime_key(v->ime, k);
        const char *commit = ime_commit(v->ime);
        if (*commit) { field_insert(w, commit); ime_commit_done(v->ime); }
        if (consumed || (k->key == KCHAR && k->ch == IME_CH_ZENKAKU_HANKAKU)) return;
        WidgetResult r = widgets_key(w, k);
        if (r == WIDGET_ACCEPT) {
            v->ask = ASK_NONE;
            free(v->asked);
            v->asked = strdup(w->text);
            if (v->asked) name_chosen(v, a, v->asked);
        } else if (r == WIDGET_CANCEL) v->ask = ASK_NONE;
        return;
    }
    WidgetResult r = widgets_key(w, k);
    if (r == WIDGET_ACCEPT) {
        v->ask = ASK_NONE;
        char *target = v->target;
        v->target = NULL;
        if (target) save_as(v, a, target, 0);
    } else if (r == WIDGET_CANCEL) {
        v->ask = ASK_NONE;
        free(v->target); v->target = NULL;
        /* N: choose another name; Esc: back to the text. */
        if (k->key != KESC) open_name_box(v, a, v->asked ? v->asked : v->name);
    }
}
/* F1: the help box (revision 1 item 25, the same as the filer's h). */
static const WidgetHelpItem help_items[] = {
    {NULL, N_("ファイル")},
    {"Ctrl+s", N_("保存")},
    {"Ctrl+Shift+s", N_("名前を付けて保存")},
    {"", N_("（同じ名前の物があれば上書きの確認）")},
    {"Esc", N_("終了（変更があれば保存の確認）")},
    {NULL, N_("検索・移動")},
    {"Ctrl+f", N_("検索")},
    {"Ctrl+h", N_("置換")},
    {"Ctrl+g", N_("行へ移動")},
    {"Ctrl+Home", N_("先頭へ")},
    {"Ctrl+End", N_("末尾へ")},
    {NULL, N_("編集")},
    {N_("Shift+矢印"), N_("選ぶ")},
    {"Ctrl+a", N_("全部を選ぶ")},
    {"Ctrl+c/x/v", N_("写す・切り取る・貼り付ける")},
    {"Ctrl+z/y", N_("取り消す・やり直す")},
    {"Ctrl+r", N_("ルビ（｜《》）を入れる")},
    {N_("半角/全角"), N_("日本語入力の入・切（Alt+Space も）")},
    {NULL, N_("表示")},
    {"Ctrl+t", N_("縦書き・横書き")},
    {"Ctrl+l", N_("行番号")},
    {NULL, N_("変換（F2。どれも y/n で確かめてから）")},
    {"F2", N_("出版社の書式に整形")},
    {"", N_("原稿形式へ・Web 形式へ")},
    {"", N_("Word 縦書きで書き出す")},
    {NULL, N_("その他")},
    {"F1", N_("このヘルプ")},
};
const WidgetHelpItem *noveledit_help(int *count)
{
    *count = (int)(sizeof(help_items) / sizeof(help_items[0]));
    static WidgetHelpItem translated[sizeof(help_items) / sizeof(help_items[0])];
    for (int i = 0; i < *count; ++i) {
        translated[i].key = help_items[i].key ? T(help_items[i].key) : NULL;
        translated[i].text = T(help_items[i].text);
    }
    return translated;
}
static void show_help(App *a)
{
    int count;
    const WidgetHelpItem *items = noveledit_help(&count);
    widgets_help(&a->widgets, T("小説エディタのキー"), items, count);
}
/* ---- F2: conversions (revision 1 4) --------------------------------------
 * Nothing runs without the key and a y/n box (never on save). The text
 * changes are one undo group each; the Word export writes a file, so its
 * box says it cannot be undone. */
static const char *const convert_options[CONVERTS] = {
    N_("出版社の書式に整形"), N_("原稿形式へ（空行を除く）"), N_("Web 形式へ（空行を入れる）"), N_("Word 縦書きで書き出す")
};
static void open_convert(Novel *v, App *a)
{
    static const char *options[CONVERTS];
    for (int i = 0; i < CONVERTS; ++i) options[i] = T(convert_options[i]);
    widgets_choice(&a->widgets, T("変換"), options, CONVERTS);
    v->ask = ASK_CONVERT;
}
/* $FDTP_WORD_TEMPLATE, else ~/.fdtp/template/本文フォーマット.docx (or the
 * name of the user's own file, 本文フォ－マット.docx with U+FF0D). *missing
 * is set when neither exists; the export then uses the built-in template
 * (novel_docx.h, revision 1 4.3). */
static char *word_template(int *missing)
{
    const char *set = getenv("FDTP_WORD_TEMPLATE");
    struct stat st;
    *missing = 0;
    if (set && *set) {
        *missing = stat(set, &st) != 0;
        return strdup(set);
    }
    char *first = home_file("FDTP_WORD_TEMPLATE", "template/本文フォーマット.docx"); /* i18n:keep */
    if (!first || !stat(first, &st)) return first;
    char *second = home_file("FDTP_WORD_TEMPLATE", "template/本文フォ－マット.docx"); /* i18n:keep */
    if (second && !stat(second, &st)) { free(first); return second; }
    free(second);
    *missing = 1;
    return first;
}
/* The same directory, the name without its extension, .docx. */
static char *word_destination(const Novel *v)
{
    const char *slash = strrchr(v->path, '/');
    const char *base = slash ? slash + 1 : v->path;
    const char *dot = strrchr(base, '.');
    size_t keep = (size_t)((dot && dot != base ? dot : base + strlen(base)) - v->path);
    char *p = malloc(keep + 6);
    if (p) { memcpy(p, v->path, keep); memcpy(p + keep, ".docx", 6); }
    return p;
}
static void convert_chosen(Novel *v, App *a, int which)
{
    char message[256];
    v->convert = which;
    if (which == CONVERT_WORD) {
        int missing;
        char *template = word_template(&missing);
        char *dest = word_destination(v);
        (void)missing; /* no template: the built-in one (convert_run) */
        if (!template || !dest) { free(template); free(dest); app_error(a, T("メモリが足りません")); return; }
        struct stat st;
        const char *base = strrchr(dest, '/');
        base = base ? base + 1 : dest;
        if (!stat(dest, &st))
            snprintf(message, sizeof(message), T("%s は既にあります。上書きして書き出しますか。\nこの処理は取り消し（Undo）が効きません"), base);
        else snprintf(message, sizeof(message), T("Word 縦書きで %s に書き出しますか。\nこの処理は取り消し（Undo）が効きません"), base);
        free(template); free(dest);
    } else snprintf(message, sizeof(message), T("%sしますか"),
                    which == CONVERT_FORMAT ? T("出版社の書式に整形") : which == CONVERT_MANUSCRIPT ? T("原稿形式に（空行を除く）") : T("Web 形式に（空行を入れる）"));
    widgets_confirm(&a->widgets, message);
    v->ask = ASK_CONVERT_YN;
}
/* The list of changes: 「N行目: 内容」 for every change (the box scrolls,
 * revision 1 item 25; past WIDGET_INFO_LINES - 2 rows 「ほか M 件」), then
 * where the log went. */
static void show_changes(App *a, const char *title, const NFLog *log, const char *log_path, int log_ok)
{
    enum { SHOWN = WIDGET_INFO_LINES - 2 };
    static char names[SHOWN + 2][64], values[SHOWN + 2][512];
    static const char *np[SHOWN + 2], *vp[SHOWN + 2];
    int n = 0;
    for (size_t i = 0; i < log->count && n < SHOWN; ++i, ++n) {
        const char *t = log->items[i].text;
        if (!strncmp(t, "の", 3) || !strncmp(t, "は", 3) || !strncmp(t, "で", 3)) t += 3; /* i18n:keep */
        snprintf(names[n], sizeof(names[n]), T("%zu行目"), log->items[i].line);
        snprintf(values[n], sizeof(values[n]), "%s", t);
    }
    if (log->count > SHOWN) {
        snprintf(names[n], sizeof(names[n]), "%s", T("ほか"));
        snprintf(values[n], sizeof(values[n]), T("%zu 件"), log->count - SHOWN);
        ++n;
    }
    snprintf(names[n], sizeof(names[n]), "%s", T("記録"));
    snprintf(values[n], sizeof(values[n]), "%s%s", log_ok ? "" : T("書けません: "), log_path ? log_path : "");
    ++n;
    for (int i = 0; i < n; ++i) { np[i] = names[i]; vp[i] = values[i]; }
    char heading[128];
    snprintf(heading, sizeof(heading), T("%s（%zu 件）"), title, log->count);
    widgets_info(&a->widgets, heading, np, vp, n);
}
static void convert_run(Novel *v, App *a)
{
    if (v->convert == CONVERT_WORD) {
        int missing;
        char *template = word_template(&missing);
        char *dest = word_destination(v);
        char err[512] = "", message[768];
        if (!template || !dest) app_error(a, T("メモリが足りません"));
        else if (novel_docx_write(&v->doc, missing ? NULL : template, dest, a->settings.novel_word_ruby, err, sizeof(err))) {
            snprintf(message, sizeof(message), T("書き出せません: %s"), err);
            app_error(a, message);
        } else {
            const char *base = strrchr(dest, '/');
            const char *ruby = a->settings.novel_word_ruby ? T("Word のルビ") : T("記法のまま");
            /* No template of the user's: say so (revision 1 4.3). */
            if (missing) snprintf(message, sizeof(message), T("%s に既定の雛形で書き出しました（ルビ: %s）"), base ? base + 1 : dest, ruby);
            else snprintf(message, sizeof(message), T("%s に書き出しました（ルビ: %s）"), base ? base + 1 : dest, ruby);
            app_notice(a, message);
        }
        free(template); free(dest);
        return;
    }
    int rules[NOVEL_FORMAT_COUNT];
    for (int i = 0; i < NOVEL_FORMAT_COUNT; ++i) rules[i] = a->settings.novel_format[i] != 0;
    NFLog log = {0};
    size_t line = v->cur.line;
    size_t undo_from = v->doc.undo_n;
    long r = v->convert == CONVERT_FORMAT ? novel_format_doc(&v->doc, rules, &log, &line)
           : v->convert == CONVERT_MANUSCRIPT ? novel_form_manuscript(&v->doc, rules[NF_SCENE], &log, &line)
           : novel_form_web(&v->doc, &log, &line);
    /* The publisher format is the manuscript form too (revision 1 4.1). */
    if (v->convert == CONVERT_FORMAT && r >= 0 && rules[NF_BLANK]) {
        long m = novel_form_manuscript(&v->doc, rules[NF_SCENE], &log, &line);
        if (v->doc.undo_n > undo_from) novel_doc_join(&v->doc, undo_from);
        if (m < 0) { /* leave the document as before, the format included */
            NPos p;
            if (r > 0) { novel_doc_group(&v->doc); novel_doc_undo(&v->doc, &p); }
            r = -1;
        } else r += m;
    }
    if (r < 0 || log.failed) {
        nf_log_free(&log);
        app_error(a, r < 0 ? T("メモリが足りません（変えていません）") : T("メモリが足りません（直した所の一覧は不完全です）"));
        if (r < 0) return;
    }
    static const char *const commands[] = {N_("出版社の書式に整形"), N_("原稿形式へ（空行を除く）"), N_("Web 形式へ（空行を入れる）")};
    char *log_path = home_file("FDTP_FORMAT_LOG", "format.log");
    int log_ok = log_path && !novel_format_log_append(log_path, T(commands[v->convert]), v->path, &log);
    if (r > 0) {
        if (line >= v->doc.count) line = v->doc.count - 1;
        v->cur = (NPos){line, 0};
        v->has_anchor = 0; v->last_edit = EDIT_NONE; v->goal = -1;
        changed(v, a);
        show_changes(a, T(commands[v->convert]), &log, log_path, log_ok);
    } else if (!log.failed) app_notice(a, T("直す所はありませんでした"));
    free(log_path);
    nf_log_free(&log);
}
static void close_normally(Novel *v)
{
    remove_autosave(v);
    state_put(v->state, v->path, v->vertical, v->numbers);
}
static void select_range(Novel *v, NPos a, NPos b)
{
    v->anchor = a; v->cur = b; v->has_anchor = 1; v->goal = -1; v->last_edit = EDIT_NONE;
}
static void search(Novel *v, App *a)
{
    NPos sa, sb, from = v->cur, s, e;
    if (selection(v, &sa, &sb)) from = sb;
    int wrapped;
    if (!novel_doc_find(&v->doc, from, v->query, strlen(v->query), &s, &e, &wrapped)) {
        app_notice(a, T("見つかりません"));
        return;
    }
    select_range(v, s, e);
    if (wrapped) app_notice(a, T("先頭に戻って見つけました"));
}
/* Interactive replace: from the caret to the end, then from the top back to
 * where it started. */
static void replace_finish(Novel *v, App *a)
{
    char message[96];
    v->prompt = P_NONE;
    snprintf(message, sizeof(message), T("%d 個置換しました"), v->replace_count);
    app_notice(a, message);
}
static int replace_find(Novel *v, NPos *s, NPos *e)
{
    int wrapped;
    if (!novel_doc_find(&v->doc, v->replace_from, v->query, strlen(v->query), s, e, &wrapped)) return 0;
    if (npos_cmp(*s, v->replace_from) < 0) v->replace_wrapped = 1;
    if (v->replace_wrapped && npos_cmp(*s, v->replace_origin) >= 0) return 0;
    return 1;
}
static void replace_one(Novel *v, NPos s, NPos e)
{
    size_t qn = strlen(v->query), rn = strlen(v->replacement);
    novel_doc_delete(&v->doc, s, e, NULL);
    NPos end = novel_doc_insert(&v->doc, s, v->replacement, rn, NULL);
    if (v->replace_wrapped && s.line == v->replace_origin.line && s.off < v->replace_origin.off)
        v->replace_origin.off = v->replace_origin.off + rn - qn;
    v->replace_from = end;
    v->cur = end;
    ++v->replace_count;
}
static void replace_next(Novel *v, App *a)
{
    NPos s, e;
    if (!replace_find(v, &s, &e)) { v->has_anchor = 0; replace_finish(v, a); return; }
    select_range(v, s, e);
    v->prompt = P_REPLACE_ASK;
}
static int prompt_key(Novel *v, App *a, const Key *k)
{
    PromptKind kind = v->prompt;
    if (text_prompt(kind)) {
        int consumed = ime_key(v->ime, k);
        const char *commit = ime_commit(v->ime);
        if (*commit) { input_insert(v, commit, strlen(commit)); ime_commit_done(v->ime); }
        if (consumed) return 0;
        size_t len = strlen(v->input);
        if (k->key == KESC) { v->prompt = P_NONE; return 0; }
        if (k->key != KCHAR && k->key != KRET) v->input_fresh = 0;
        if (k->key == KLEFT) v->input_cur = novel_prev(v->input, v->input_cur);
        else if (k->key == KRIGHT) v->input_cur = novel_next(v->input, len, v->input_cur);
        else if (k->key == KHOME) v->input_cur = 0;
        else if (k->key == KEND) v->input_cur = len;
        else if (k->key == KBS && v->input_cur) {
            size_t at = novel_prev(v->input, v->input_cur);
            memmove(v->input + at, v->input + v->input_cur, len - v->input_cur + 1);
            v->input_cur = at;
        } else if (k->key == KDEL && v->input_cur < len) {
            size_t next = novel_next(v->input, len, v->input_cur);
            memmove(v->input + v->input_cur, v->input + next, len - next + 1);
        } else if (k->key == KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) && k->ch >= 32 && k->ch != 127 &&
                   k->ch != IME_CH_ZENKAKU_HANKAKU) {
            char buffer[4];
            input_insert(v, buffer, novel_encode(k->ch, buffer));
        } else if (k->key == KRET) {
            v->prompt = P_NONE;
            if (kind == P_SEARCH) {
                if (!v->input[0]) return 0;
                strcpy(v->query, v->input);
                search(v, a);
            } else if (kind == P_REPLACE_FROM) {
                if (!v->input[0]) return 0;
                strcpy(v->query, v->input);
                open_prompt(v, P_REPLACE_TO, v->replacement);
            } else if (kind == P_REPLACE_TO) {
                strcpy(v->replacement, v->input);
                NPos sa, sb;
                v->replace_origin = selection(v, &sa, &sb) ? sa : v->cur;
                v->replace_from = v->replace_origin;
                v->replace_wrapped = 0; v->replace_count = 0;
                replace_next(v, a);
            } else if (kind == P_GOTO) {
                char *end;
                errno = 0;
                unsigned long n = strtoul(v->input, &end, 10);
                if (errno || *end || !n || v->input[0] == '-') { app_notice(a, T("行の番号を入力してください")); return 0; }
                if (n > v->doc.count) n = v->doc.count;
                move_to(v, (NPos){n - 1, 0}, 0);
                v->goal = -1;
            }
        }
        return 0;
    }
    uint32_t c = k->key == KCHAR ? (k->ch >= 'A' && k->ch <= 'Z' ? k->ch + 32 : k->ch) : 0;
    int yes = c == 'y', no = c == 'n', esc = k->key == KESC;
    switch (kind) {
    case P_REPLACE_ASK: {
        NPos s, e;
        if (esc) { v->has_anchor = 0; replace_finish(v, a); return 0; }
        if (!selection(v, &s, &e)) { replace_finish(v, a); return 0; }
        if (yes) { novel_doc_group(&v->doc); replace_one(v, s, e); changed(v, a); v->has_anchor = 0; replace_next(v, a); }
        else if (no) { v->replace_from = e; v->has_anchor = 0; replace_next(v, a); }
        else if (c == 'a') {
            novel_doc_group(&v->doc);
            do replace_one(v, s, e); while (replace_find(v, &s, &e));
            changed(v, a);
            v->has_anchor = 0;
            replace_finish(v, a);
        }
        return 0;
    }
    case P_CLOSE:
        if (esc) { v->prompt = P_NONE; return 0; }
        if (no) { v->prompt = P_NONE; close_normally(v); return 1; }
        if (yes) {
            v->prompt = P_NONE;
            if (save(v, a, 0, P_UTF8_CLOSE)) { close_normally(v); return 1; }
        }
        return 0;
    case P_UTF8_AS:
        v->prompt = P_NONE;
        if (yes && v->target) { char *t = v->target; v->target = NULL; save_as(v, a, t, 1); }
        else if (no || esc) { free(v->target); v->target = NULL; }
        else v->prompt = P_UTF8_AS;
        return 0;
    case P_UTF8: case P_UTF8_CLOSE:
        if (yes) {
            v->prompt = P_NONE;
            if (save(v, a, 1, kind) && kind == P_UTF8_CLOSE) { close_normally(v); return 1; }
        } else if (no || esc) v->prompt = P_NONE;
        return 0;
    case P_RECOVER:
        if (yes) {
            size_t n;
            char *text = read_all(v->autosave, &n);
            v->prompt = P_NONE;
            if (!text || novel_doc_set_text(&v->doc, text, n)) app_notice(a, T("前回の続きを読めません"));
            else { novel_doc_mark_dirty(&v->doc); v->cur = (NPos){0, 0}; v->top_line = v->top_row = 0; }
            free(text);
        } else if (no || esc) v->prompt = P_NONE;
        return 0;
    default:
        v->prompt = P_NONE;
        return 0;
    }
}

/* ---- keys ------------------------------------------------------------- */
static int novel_key(Screen *s, App *a, const Key *k)
{
    Novel *v = s->self;
    if (v->ask != ASK_NONE) {
        if (v->ask == ASK_LOSSY && a->widgets.kind == WIDGET_CONFIRM) {
            WidgetResult r = widgets_key(&a->widgets, k);
            if (r == WIDGET_ACCEPT) {
                v->ask = ASK_NONE;
                v->loss_ready = 1;
                if (v->loss_kind == P_UTF8_AS) {
                    char *target = v->target;
                    v->target = NULL;
                    if (target) save_as(v, a, target, v->loss_utf8);
                    else v->loss_ready = 0;
                } else if (save(v, a, v->loss_utf8, v->loss_kind) && v->loss_kind == P_UTF8_CLOSE) {
                    close_normally(v);
                    return 1;
                }
            } else if (r == WIDGET_CANCEL) {
                v->ask = ASK_NONE;
                free(v->target); v->target = NULL;
            }
            ensure_visible(v);
            return 0;
        }
        if (a->widgets.kind == WIDGET_INPUT || a->widgets.kind == WIDGET_CONFIRM || a->widgets.kind == WIDGET_CHOICE) {
            ask_key(v, a, k);
            ensure_visible(v);
            return 0;
        }
        v->ask = ASK_NONE;
    }
    if (v->prompt != P_NONE) {
        int close = prompt_key(v, a, k);
        if (!close) ensure_visible(v);
        return close;
    }
    int consumed = ime_key(v->ime, k);
    const char *commit = ime_commit(v->ime);
    if (*commit) {
        insert(v, a, commit, strlen(commit), EDIT_TYPE);
        ime_commit_done(v->ime);
    }
    if (consumed) { ensure_visible(v); return 0; }
    int shift = (k->mods & MOD_SHIFT) != 0, c = (k->mods & MOD_CTRL) != 0;
    int vertical = v->vertical;
    if (k->key == KCHAR && (k->mods & (MOD_CTRL | MOD_ALT)) == MOD_CTRL) {
        if (ctrl(k, 's') && (k->mods & MOD_SHIFT)) open_name_box(v, a, v->name);
        else if (ctrl(k, 's')) save(v, a, 0, P_UTF8);
        else if (ctrl(k, 'f')) open_prompt(v, P_SEARCH, v->query);
        else if (ctrl(k, 'h')) open_prompt(v, P_REPLACE_FROM, v->query);
        else if (ctrl(k, 'g')) open_prompt(v, P_GOTO, "");
        else if (ctrl(k, 't')) {
            v->vertical = !v->vertical;
            state_put(v->state, v->path, v->vertical, v->numbers);
            v->top_row = 0; v->goal = -1;
        } else if (ctrl(k, 'l')) {
            /* The rows change width: keep the top line, refold from its start. */
            v->numbers = !v->numbers;
            state_put(v->state, v->path, v->vertical, v->numbers);
            v->top_row = 0; v->goal = -1;
        } else if (ctrl(k, 'r')) ruby_input(v, a);
        else if (ctrl(k, 'c')) copy_selection(v);
        else if (ctrl(k, 'x')) {
            NPos sa, sb;
            if (selection(v, &sa, &sb)) { copy_selection(v); novel_doc_group(&v->doc); delete_selection(v); v->last_edit = EDIT_NONE; changed(v, a); }
        } else if (ctrl(k, 'v')) { if (clipboard) insert(v, a, clipboard, clipboard_len, EDIT_NONE); }
        else if (ctrl(k, 'z')) undo(v, a, 0);
        else if (ctrl(k, 'y')) undo(v, a, 1);
        else if (ctrl(k, 'a')) select_range(v, (NPos){0, 0}, novel_doc_end(&v->doc));
        ensure_visible(v);
        return 0;
    }
    switch (k->key) {
    case KESC:
        if (novel_doc_modified(&v->doc)) { open_prompt(v, P_CLOSE, NULL); return 0; }
        close_normally(v);
        return 1;
    case KLEFT: if (vertical) move_rows(v, 1, shift); else move_chars(v, 0, shift); break;
    case KRIGHT: if (vertical) move_rows(v, -1, shift); else move_chars(v, 1, shift); break;
    case KUP: if (vertical) move_chars(v, 0, shift); else move_rows(v, -1, shift); break;
    case KDOWN: if (vertical) move_chars(v, 1, shift); else move_rows(v, 1, shift); break;
    case KHOME:
        if (c) { move_to(v, (NPos){0, 0}, shift); v->goal = -1; }
        else row_edge(v, 0, shift);
        break;
    case KEND:
        if (c) { move_to(v, novel_doc_end(&v->doc), shift); v->goal = -1; }
        else row_edge(v, 1, shift);
        break;
    case KPGUP: page_move(v, 0, shift); break;
    case KPGDN: page_move(v, 1, shift); break;
    case KF1: show_help(a); break;
    case KF2: open_convert(v, a); break;
    case KRET: insert(v, a, "\n", 1, EDIT_NONE); break;
    case KTAB: insert(v, a, "\t", 1, EDIT_TYPE); break;
    case KBS: erase(v, a, 0); break;
    case KDEL: erase(v, a, 1); break;
    case KCHAR:
        if (!(k->mods & (MOD_CTRL | MOD_ALT)) && k->ch >= 32 && k->ch != 127 && k->ch != IME_CH_ZENKAKU_HANKAKU) {
            char buffer[4];
            insert(v, a, buffer, novel_encode(k->ch, buffer), EDIT_TYPE);
        }
        break;
    default: break;
    }
    ensure_visible(v);
    return 0;
}
static void novel_tick(Screen *s, App *a, double now)
{
    Novel *v = s->self;
    if (v->autosave_pending && now - v->change_time >= AUTOSAVE_SECONDS) {
        write_autosave(v);
        v->autosave_pending = 0;
        a->changed = 1;
    }
}

/* ---- drawing ---------------------------------------------------------- */
static void debug_glyph(Novel *v, uint32_t code, int x, int y, int w, int h, int color, int kind)
{
    if (v->ndbg < DEBUG_GLYPHS) v->dbg[v->ndbg++] = (DebugGlyph){code, x, y, w, h, color, kind};
}
/* The number of a line (from 1) on the first row of it only: CYAN, right
 * aligned in the document-wide gutter left of a horizontal row, centred
 * above a vertical column in the 12-dot font. */
static void draw_number(Novel *v, Canvas *c, size_t line, int slot)
{
    char number[3 * sizeof(size_t) + 1];
    int n = snprintf(number, sizeof(number), "%zu", line + 1);
    if (v->vertical) {
        int x = V_RIGHT - (slot + 1) * column_pitch(v) + (column_pitch(v) - n * 6) / 2;
        for (int i = 0; i < n; ++i) ng_ruby(c, x + i * 6, V_NUM_Y, (uint32_t)number[i], rgb(CYAN), NG_UPRIGHT);
        debug_glyph(v, (uint32_t)(line + 1), x, V_NUM_Y, n * 6, 12, CYAN, 5);
    } else {
        int x = BODY_LEFT + (v->number_digits - n) * 12, y = H_TOP + slot * 36 + 12;
        ng_text(c, x, y, number, rgb(CYAN), -1);
        debug_glyph(v, (uint32_t)(line + 1), x, y, n * 12, 24, CYAN, 5);
    }
}
static void draw_row(Novel *v, Canvas *c, size_t line, size_t r, int slot)
{
    const NLayout *L = &v->lay;
    const NRow *row = &L->row[r];
    NPos sa, sb;
    size_t s0 = 1, s1 = 0;
    if (selection(v, &sa, &sb) && line >= sa.line && line <= sb.line && !ime_busy(v->ime)) {
        s0 = line == sa.line ? sa.off : 0;
        s1 = line == sb.line ? sb.off : (size_t)-1;
    }
    int bx, by, rx, ry; /* body and ruby origins; positions run along x or y */
    if (v->vertical) { bx = V_RIGHT - (slot + 1) * column_pitch(v); by = text_top(v); rx = bx + 24; ry = by; }
    else { bx = body_x(v); by = H_TOP + slot * 36 + 12; rx = bx; ry = H_TOP + slot * 36; }
    if (v->numbers && r == 0) draw_number(v, c, line, slot);
    for (size_t k = row->unit0; k < row->unit0 + row->units; ++k) {
        const NUnit *u = &L->u[k];
        if (u->flags & NU_HIDDEN) continue;
        int half = (u->flags & NU_HALF) != 0, x, y, w, h;
        if (v->vertical) {
            x = bx + u->cross; y = by + u->pos;
            w = half && u->form != NG_ROTATE ? 12 : 24;
            h = half && u->form == NG_ROTATE ? 12 : 24;
        } else { x = bx + u->pos; y = by; w = half ? 12 : 24; h = 24; }
        if (u->code == ' ' && u->adv == 48) { if (v->vertical) h = 48; else w = 48; }
        int selected = u->off >= s0 && u->off < s1;
        int reverse = selected || (u->flags & NU_PREEDIT_HL);
        int color = reverse ? BG : u->color;
        if (reverse) ng_fill(c, x, y, w, h, rgb(WHITE));
        ng_body(c, x, y, u->code, rgb(color), u->form);
        debug_glyph(v, u->code, x, y, w, h, color, reverse ? 2 : 0);
        if (u->flags & NU_PREEDIT) {
            if (v->vertical) ng_fill(c, x - 2, y, 2, h, rgb(WHITE));
            else ng_fill(c, x, y + 22, w, 2, rgb(WHITE));
        }
        if (u->flags & NU_EMPHASIS) {
            int dx = v->vertical ? rx : bx + u->pos + u->adv / 2 - 6;
            int dy = v->vertical ? by + u->pos + u->adv / 2 - 6 : ry;
            ng_dot(c, dx, dy, rgb(u->color));
            debug_glyph(v, 0xfe45, dx, dy, 12, 12, u->color, 3);
        }
    }
    for (size_t k = row->ruby0; k < row->ruby0 + row->rubies; ++k) {
        const NRubyGlyph *g = &L->r[k];
        int x = v->vertical ? rx : rx + g->pos, y = v->vertical ? ry + g->pos : ry;
        ng_ruby(c, x, y, g->code, rgb(g->color), g->form);
        debug_glyph(v, g->code, x, y, g->half ? 6 : 12, 12, g->color, 1);
    }
    if (line == v->cur.line) {
        size_t cr; int cp;
        ImePreedit pe;
        ime_preedit(v->ime, &pe);
        nl_locate(L, v->cur.off + pe.caret, &cr, &cp);
        if (cr == r) {
            if (v->vertical) { v->caret_x = bx; v->caret_y = by + cp; ng_fill(c, bx, by + cp - 1, 24, 3, rgb(YELLOW)); }
            else { v->caret_x = bx + cp; v->caret_y = by; ng_fill(c, bx + cp - 1, by, 3, 24, rgb(YELLOW)); }
            v->caret_visible = 1;
        }
    }
}
static void draw_cells_row(Canvas *c, const Cells *cells, int row)
{
    for (int col = 0; col < FD_COLS; ++col) {
        const Cell *cell = &cells->data[row][col];
        if (cell->part == 2) continue;
        int w = cell->part == 1 ? 24 : 12;
        ng_fill(c, CANVAS_GRID_X + col * 12, row * 24, w, 24, fd_palette[cell->bg]);
        ng_body(c, CANVAS_GRID_X + col * 12, row * 24, cell->code, fd_palette[cell->fg], NG_UPRIGHT);
    }
}
static size_t char_index(const NLine *l, size_t off)
{
    size_t n = 0;
    for (size_t i = 0; i < off && i < l->len; i = novel_next(l->s, l->len, i)) ++n;
    return n;
}
static void draw_bands(Novel *v, App *a, Canvas *c, Cells *cells)
{
    cells_init(cells);
    frame_open(cells);
    /* The name in the top line (after the numbers' line), the place in the bottom line. */
    char status[128];
    snprintf(status, sizeof(status), T("%s  行 %zu  字 %zu"), v->vertical ? T("縦書き") : T("横書き"), v->cur.line + 1,
             char_index(&v->doc.lines[v->cur.line], v->cur.off) + 1);
    int title = v->numbers && !v->vertical ? v->number_digits + 1 + FRAME_TITLE_COL : FRAME_TITLE_COL;
    frame_title(cells, title, v->name, FRAME_STATUS_END - title - 9, novel_doc_modified(&v->doc) ? T("変更あり") : NULL, MAGENTA);
    frame_status(cells, FRAME_STATUS_END, status, FD_COLS - 8);
    if (v->numbers && !v->vertical) frame_vline(cells, v->number_digits + 1, 0, FRAME_BOTTOM);
    /* Row 24: input state, then the commands. */
    char label[32];
    snprintf(label, sizeof(label), "[%s]", ime_label(v->ime));
    cells_text(cells, 0, FD_BAND, label, YELLOW, BG, 6);
    /* S^S is Ctrl+Shift+s (save as); F1 lists every key. */
    const FrameKey keys[] = {{"^S", T("保存")}, {"S^S", T("別名")}, {"^F", T("検索")}, {"^H", T("置換")}, {"^G", i18n_lang() == I18N_EN ? "Go to" : "移動"}, /* i18n:keep */
                                    {"^T", T("縦横")}, {"^L", T("番号")}, {"^R", T("ルビ")}, {"Esc", T("終了")}};
    frame_keys(cells, 6, keys, 9);
    int row23 = 0;
    if (v->prompt != P_NONE) {
        row23 = 1;
        for (int x = 0; x < FD_COLS; ++x) cells_put(cells, x, FD_LINE, ' ', WHITE, BG);
        if (text_prompt(v->prompt)) {
            int at = 2 + cells_text(cells, 2, FD_LINE, prompt_label(v), CYAN, BG, 40);
            at += cells_text(cells, at, FD_LINE, ": ", CYAN, BG, 2);
            ImePreedit pe;
            ime_preedit(v->ime, &pe);
            char before[INPUT_CAP];
            memcpy(before, v->input, v->input_cur); before[v->input_cur] = 0;
            at += cells_text(cells, at, FD_LINE, before, WHITE, BG, FD_COLS - at);
            at += cells_text(cells, at, FD_LINE, pe.text, BG, CYAN, FD_COLS - at);
            const char *rest = v->input + v->input_cur;
            const char *p = rest;
            uint32_t ch = *p ? font24_decode(&p) : ' ';
            if (at + font24_width(ch) <= FD_COLS) cells_put(cells, at, FD_LINE, ch, BG, WHITE);
            if (*rest) cells_text(cells, at + font24_width(ch), FD_LINE, p, WHITE, BG, FD_COLS - at - font24_width(ch));
            v->caret_x = CANVAS_GRID_X + at * 12; v->caret_y = FD_LINE * 24;
        } else cells_text(cells, 2, FD_LINE, prompt_label(v), YELLOW, BG, FD_COLS - 2);
    }
    if (a->widgets.kind != WIDGET_NONE) { widgets_draw(&a->widgets, cells); if (widgets_row23(&a->widgets)) row23 = 1; }
    if (v->ask == ASK_NAME && a->widgets.kind == WIDGET_INPUT) {
        /* The name box (widgets.c: the field on row 11, its text in columns
         * 8 to 71): the preedit at the field's cursor, the rest after it. */
        int FIELD_ROW, FIELD_START, FIELD_END;
        widgets_input_field(&FIELD_ROW, &FIELD_START, &FIELD_END);
        const Widgets *w = &a->widgets;
        size_t from = w->scroll <= w->cursor ? w->scroll : w->cursor;
        char before[WIDGET_CAP];
        memcpy(before, w->text + from, w->cursor - from);
        before[w->cursor - from] = 0;
        int col = FIELD_START + cells_width(before);
        if (col > FIELD_END) col = FIELD_END;
        v->field_x = CANVAS_GRID_X + col * 12; v->field_y = FIELD_ROW * 24;
        ImePreedit pe;
        ime_preedit(v->ime, &pe);
        if (*pe.text && col < FIELD_END) {
            int at = col + cells_text(cells, col, FIELD_ROW, pe.text, BG, CYAN, FIELD_END - col);
            if (at < FIELD_END) cells_text(cells, at, FIELD_ROW, w->text + w->cursor, BG, WHITE, FIELD_END - at);
        }
    }
    /* The frame, its title and status, the line between the numbers and the
     * text (raster_boxes: the frame's border cells and lines, then the boxes). */
    (void)row23;
    draw_cells_row(c, cells, FD_LINE);
    draw_cells_row(c, cells, FD_BAND);
    raster_boxes(c, cells);
    if (v->numbers && v->vertical) raster_frame_line(c, cells, V_NUM_RULE_Y);
}
static void draw_candidates(Novel *v, Canvas *c)
{
    ImeCandidates cand;
    ime_candidates(v->ime, &cand);
    if (!cand.active || !cand.shown) return;
    char footer[32];
    snprintf(footer, sizeof(footer), "[%d/%d]", cand.index < 0 ? 0 : cand.index + 1, cand.count);
    int width = ng_text_width(footer);
    for (int i = 0; i < cand.shown; ++i) {
        int w = 36 + ng_text_width(cand.items[i]);
        if (w > width) width = w;
    }
    int w = width + 16, h = (cand.shown + 1) * 24 + 8, x, y;
    if (v->ask == ASK_NAME) { x = v->field_x; y = v->field_y + 48; }   /* under the box and its shadow */
    else if (v->prompt != P_NONE) { x = v->caret_x; y = FD_LINE * 24 - h; }
    else if (v->vertical) {
        x = v->caret_x - w - 2; y = v->caret_y;
        if (x < 0) x = v->caret_x + 38;
    } else {
        x = v->caret_x; y = v->caret_y + 24;
        if (y + h > FD_BAND * 24) y = v->caret_y - 12 - h;
    }
    if (x + w > CANVAS_W) x = CANVAS_W - w;
    if (x < 0) x = 0;
    if (y + h > FD_BAND * 24) y = FD_BAND * 24 - h;
    if (y < 24) y = 24;
    ng_fill(c, x, y, w, h, rgb(CYAN));
    ng_fill(c, x + 2, y + 2, w - 4, h - 4, rgb(BG));
    for (int i = 0; i < cand.shown; ++i) {
        int selected = cand.first + i == cand.index;
        int ty = y + 4 + i * 24;
        if (selected) ng_fill(c, x + 4, ty, w - 8, 24, rgb(WHITE));
        char number[8];
        snprintf(number, sizeof(number), "%d", (i + 1) % 10);
        ng_text(c, x + 8, ty, number, rgb(selected ? BG : YELLOW), -1);
        ng_text(c, x + 8 + 24, ty, cand.items[i], rgb(selected ? BG : WHITE), w - 40);
        debug_glyph(v, 0, x + 8 + 24, ty, ng_text_width(cand.items[i]), 24, selected ? BG : WHITE, 4);
    }
    ng_text(c, x + 8, y + 4 + cand.shown * 24, footer, rgb(CYAN), -1);
}
static void novel_draw(Screen *s, App *a, Canvas *c, Cells *cells)
{
    Novel *v = s->self;
    canvas_clear(c, fd_palette[BG]);
    v->ndbg = 0; v->caret_visible = 0;
    clamp_top(v);
    size_t line = v->top_line, r = v->top_row;
    for (int slot = 0; slot < page(v) && line < v->doc.count; ++line, r = 0) {
        lay_line(v, line, 1);
        for (; r < v->lay.nrow && slot < page(v); ++r, ++slot) draw_row(v, c, line, r, slot);
    }
    draw_bands(v, a, c, cells);
    draw_candidates(v, c);
    c->full_dirty = 1;
}
static void novel_destroy(Screen *s)
{
    Novel *v = s->self;
    novel_doc_free(&v->doc);
    nl_free(&v->lay);
    ime_close(v->ime);
    free(v->scratch); free(v->path); free(v->name); free(v->autosave); free(v->state);
    free(v->target); free(v->asked);
    free(v);
    /* The clipboard outlives the editors: copy in one file, paste in the next. */
}

static char *absolute(const char *path)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return strdup(resolved);
    if (path[0] == '/') return strdup(path);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return strdup(path);
    return join(cwd, path);
}
Screen *noveledit_create(App *app, const char *path)
{
    if (!path || !*path) { app_notice(app, T("ファイル名がありません")); return NULL; }
    Novel *v = calloc(1, sizeof(*v));
    if (!v) { app_notice(app, T("メモリが足りません")); return NULL; }
    v->screen = (Screen){v, novel_key, novel_draw, 1, novel_tick, novel_destroy};
    novel_doc_init(&v->doc);
    nl_init(&v->lay);
    v->goal = -1;
    v->path = absolute(path);
    const char *base = v->path ? strrchr(v->path, '/') : NULL;
    v->name = strdup(base ? base + 1 : path);
    v->ime = ime_open();
    if (!v->path || !v->name || !v->ime) {
        app_notice(app, T("メモリが足りません"));
        novel_destroy(&v->screen);
        return NULL;
    }
    int fresh = 0;
    if (novel_doc_load(&v->doc, v->path, NOVEL_MAX_BYTES)) {
        if (errno == ENOENT) { fresh = novel_doc_set_text(&v->doc, "", 0) == 0; }
        if (!fresh) {
            char message[256];
            snprintf(message, sizeof(message), errno == EFBIG ? T("大きすぎて開けません（16 MB まで）") : T("開けません: %s"), strerror(errno));
            app_notice(app, message);
            novel_destroy(&v->screen);
            return NULL;
        }
    }
    update_number_digits(v);
    v->state = home_file("FDTP_NOVEL_STATE", "novel-state");
    v->autosave = autosave_path(v->path);
    int mode = state_get(v->state, v->path);
    v->vertical = mode >= 0 && (mode & STATE_VERTICAL);
    v->numbers = mode >= 0 && (mode & STATE_NUMBERS);
    struct stat st;
    if (v->autosave && !stat(v->autosave, &st) && S_ISREG(st.st_mode)) open_prompt(v, P_RECOVER, NULL);
    else if (fresh) app_notice(app, T("新しいファイルです"));
    else if (v->doc.lossy) app_notice(app, T("読めない字を〓（U+FFFD）にしました。保存すると元に戻りません"));
    return &v->screen;
}

static size_t json_escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 8 < cap; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[n++] = '\\'; out[n++] = (char)ch; }
        else if (ch < 32) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", ch);
        else out[n++] = (char)ch;
    }
    out[n] = 0;
    return n;
}
size_t noveledit_debug(Screen *screen, char *out, size_t cap)
{
    Novel *v = screen->self;
    char name[512], title[512], query[INPUT_CAP * 2], input[INPUT_CAP * 2];
    json_escape(name, sizeof(name), v->path);
    json_escape(title, sizeof(title), v->name);
    json_escape(query, sizeof(query), v->query);
    json_escape(input, sizeof(input), v->input);
    NPos sa = v->cur, sb = v->cur;
    int sel = selection(v, &sa, &sb);
    size_t n = (size_t)snprintf(out, cap,
        "{\"path\":\"%s\",\"name\":\"%s\",\"ask\":%d,\"line\":%zu,\"off\":%zu,\"selection\":%s,\"sel\":[%zu,%zu,%zu,%zu],"
        "\"vertical\":%d,\"numbers\":%d,\"number_digits\":%d,\"modified\":%d,\"encoding\":\"%s\",\"lines\":%zu,\"top\":[%zu,%zu],"
        "\"prompt\":%d,\"input\":\"%s\",\"query\":\"%s\",\"ime\":\"%s\",\"caret\":[%d,%d,%d],\"glyphs\":[",
        name, title, v->ask, v->cur.line, v->cur.off, sel ? "true" : "false", sa.line, sa.off, sb.line, sb.off,
        v->vertical, v->numbers, v->number_digits, novel_doc_modified(&v->doc), jis_encoding_name(v->doc.encoding), v->doc.count,
        v->top_line, v->top_row, (int)v->prompt, input, query, ime_label(v->ime),
        v->caret_visible, v->caret_x, v->caret_y);
    for (size_t i = 0; i < v->ndbg && n + 64 < cap; ++i) {
        if (n + 64 + 8 * 4096 + 64 >= cap) break;
        const DebugGlyph *g = &v->dbg[i];
        n += (size_t)snprintf(out + n, cap - n, "%s[%u,%d,%d,%d,%d,%d,%d]", i ? "," : "",
                              g->code, g->x, g->y, g->w, g->h, g->color, g->kind);
    }
    /* The first 4096 bytes of the text (whole characters), '\n' between lines. */
    size_t len;
    char *text = novel_doc_copy(&v->doc, (NPos){0, 0}, novel_doc_end(&v->doc), &len);
    if (text) {
        if (len > 4096) { len = 4096; while (len && ((unsigned char)text[len] & 0xc0) == 0x80) --len; }
        text[len] = 0;
        char *escaped = malloc(len * 6 + 1);
        if (escaped && n + len * 6 + 32 < cap) {
            json_escape(escaped, len * 6 + 1, text);
            n += (size_t)snprintf(out + n, cap - n, "],\"text\":\"%s\"}", escaped);
        } else if (n + 3 < cap) n += (size_t)snprintf(out + n, cap - n, "]}");
        free(escaped);
        free(text);
    } else if (n + 3 < cap) n += (size_t)snprintf(out + n, cap - n, "]}");
    return n < cap ? n : cap - 1;
}
