// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 5 (docs/FDTP_VIEWERS.md): docx on the FD grid. Paragraphs wrap at
 * the screen width, 80 columns on the Pomera (headings in YELLOW), tables are drawn with ruled lines, page
 * breaks are separator lines, images are [画像]. RET edits the paragraph
 * under the cursor on row 23, o adds one, C-k deletes one, C-s saves.
 * Revision 1 item 38: C-z undoes and C-y redoes one paragraph's change
 * (an edit, an added or a deleted paragraph), UNDO_LEVELS steps. A step is
 * the splice between the main part as doc_build() gave it before and
 * after the change; undoing rebuilds the part and reads it again
 * (doc_reparse), so the bytes come back exactly, after a save too. */
#include "docview.h"
#include "i18n.h"
#include "doc_input.h"
#include "doc_model.h"
#include "doc_undo.h"
#include "app.h"
#include "font24.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Revision 1 item 29: the screen's frame (cells.h frame_open), as the text
 * view. The document is rows 1 .. FRAME_BOTTOM - 1, columns 1 .. FD_COLS - 2
 * (laid out FRAME_BODY_COLS wide); page breaks are lines across the frame.
 * Row FD_LINE: the item under the cursor, or the input line. */
#define TOP 1
#define ROWS FRAME_BODY_ROWS
#define LEFT 1

typedef enum { PROMPT_NONE, PROMPT_EDIT, PROMPT_DELETE, PROMPT_QUIT } Prompt;
typedef struct {
    Screen screen;
    DocFile doc;
    DocLayout lay;
    size_t cur, top;
    Prompt prompt;
    DocInput input;
    size_t edit_item;
    int edit_new;
    Ime *ime;
    int lay_cols; /* FD_COLS when lay was made */
    UndoHist undo;
    int saves;
    /* The change in hand: the main part before it, and the paragraph it is
     * on then (-1: o, which adds one). */
    int change_open;
    unsigned char *change_before;
    size_t change_len;
    long change_item;
} DocView;

static void clamp(DocView *v)
{
    if (!v->lay.n) { v->cur = v->top = 0; return; }
    if (v->cur >= v->lay.n) v->cur = v->lay.n - 1;
    if (v->top > v->cur) v->top = v->cur;
    if (v->cur >= v->top + ROWS) v->top = v->cur - ROWS + 1;
    if (v->lay.n >= (size_t)ROWS && v->top > v->lay.n - ROWS) v->top = v->lay.n - ROWS;
    if (v->lay.n < (size_t)ROWS) v->top = 0;
}
/* Lays the document out again; the cursor goes to the first line of item
 * (or stays at the same line number when item is (size_t)-1). */
static int relayout(DocView *v, App *app, size_t item)
{
    doc_layout_free(&v->lay);
    if (doc_layout(&v->doc, &v->lay, FRAME_BODY_COLS)) { app_notice(app, T("メモリが足りません")); return -1; }
    v->lay_cols = FD_COLS;
    if (item != (size_t)-1) {
        for (size_t i = 0; i < v->lay.n; ++i)
            if (v->lay.lines[i].item >= item) { v->cur = i; break; }
    }
    clamp(v);
    return 0;
}
static const DocLine *current(const DocView *v) { return v->lay.n ? &v->lay.lines[v->cur] : NULL; }

/* ---- drawing ---- */
static void draw_line(DocView *v, Cells *c, int row, const DocLine *ln, int cursor)
{
    int bg = cursor ? WHITE : BG;
    for (int x = LEFT; x < FRAME_RIGHT; ++x) cells_put(c, x, row, ' ', WHITE, bg);
    if (ln->kind == DOC_LINE_SEP) { frame_hline(c, row); return; }
    const DocText *t;
    size_t s, e;
    if (ln->kind == DOC_LINE_TABLE) { t = &v->lay.tlines[ln->off]; s = 0; e = t->n; }
    else { t = &v->doc.items[ln->item].disp; s = ln->off; e = ln->off + ln->len; }
    int col = 0, width = FRAME_BODY_COLS;
    for (size_t k = s; k < e && col < width; ++k) {
        uint32_t code = t->code[k];
        if (code == '\t') { col += 4 - col % 4; continue; }
        if (code < 0x20 || code == 0x7f) continue;
        if (code == 0x2011) code = '-';
        int w = font24_width(code);
        if (col + w > width) break;
        cells_put(c, LEFT + col, row, code, cursor ? BG : t->color[k], bg);
        col += w;
    }
}
static void doc_draw(Screen *screen, App *app, Canvas *canvas, Cells *c)
{
    (void)app; (void)canvas;
    DocView *v = screen->self;
    /* The screen changed width (an X window resized): wrap anew, same line. */
    if (v->lay_cols != FD_COLS && v->prompt != PROMPT_EDIT && !relayout(v, app, (size_t)-1)) v->lay_cols = FD_COLS;
    clamp(v);
    if (frame_open(c) < 0) return;
    const char *name = strrchr(v->doc.path, '/');
    name = name ? name + 1 : v->doc.path;
    frame_title(c, FRAME_TITLE_COL, name, FRAME_STATUS_END - FRAME_TITLE_COL - 9, undo_dirty(&v->undo) ? T("変更あり") : NULL, MAGENTA);
    char status[96];
    snprintf(status, sizeof(status), T("行 %zu/%zu"), v->lay.n ? v->cur + 1 : 0, v->lay.n);
    frame_status(c, FRAME_STATUS_END, status, FD_COLS - 8);
    for (int y = 0; y < ROWS; ++y) {
        size_t i = v->top + (size_t)y;
        if (i >= v->lay.n) break;
        draw_line(v, c, TOP + y, &v->lay.lines[i], i == v->cur);
    }
    /* Row 23: which item the cursor is on, or the input line. */
    if (v->prompt == PROMPT_EDIT) doc_input_draw(&v->input, c);
    else if (current(v)) {
        size_t item = current(v)->item, n = 0, k = 0;
        for (size_t i = 0; i < v->doc.nitems; ++i) {
            if (v->doc.items[i].deleted) continue;
            ++n;
            if (i <= item) k = n;
        }
        const DocItem *it = &v->doc.items[item];
        char info[160];
        snprintf(info, sizeof(info), "%s %zu/%zu%s%s", it->kind == DOC_TABLE ? T("表") : T("段落"), k, n,
                 it->heading ? T("  見出し") : "", it->kind == DOC_PARA && it->refuse ? T("  （直せない段落）") : "");
        cells_text(c, 2, FD_LINE, info, CYAN, BG, FD_COLS - 2);
    }
    /* Row 24: the band, with the key's letter on yellow. */
    static const char *const bands[] = {" Edit ", " Open ", " Kill ", " Save ", "Undo^Z", "Redo^Y", "      ", " Quit "};
    static const int hot[] = {1, 1, 1, 1, 5, 5, -1, 1};
    for (int i = 0; i < 8; ++i) cells_band_item(c, 2 + i * 10, 6, bands[i], hot[i]);
    if (v->prompt == PROMPT_EDIT && v->ime) {
        char label[32];
        snprintf(label, sizeof(label), "[%s]", ime_label(v->ime));
        cells_text(c, 52 * FD_COLS / 80, FD_BAND, label, BG, CYAN, 8);
    }
}

/* ---- undo (revision 1 item 38) ---- */
/* The number of the paragraph item (0-based) as the document has them:
 * deleted ones not counted, as after reading it again. */
static long shown_index(const DocFile *d, size_t item)
{
    long n = 0;
    for (size_t i = 0; i < item && i < d->nitems; ++i)
        if (!d->items[i].deleted) ++n;
    return n;
}
static void change_begin(DocView *v, long item)
{
    free(v->change_before);
    size_t n = 0;
    v->change_before = doc_build(&v->doc, &n, NULL, 0);
    v->change_len = n;
    v->change_item = item;
    v->change_open = 1;
}
/* after: the paragraph the change is on now (-1: it was deleted). A change
 * that left the part as it was makes no step. */
static void change_end(DocView *v, App *app, long after)
{
    if (!v->change_open) return;
    v->change_open = 0;
    size_t n = 0;
    unsigned char *now = doc_build(&v->doc, &n, NULL, 0);
    int same = 0;
    UndoSplice *s = v->change_before && now ? undo_splice_make(v->change_before, v->change_len, now, n, &same) : NULL;
    free(v->change_before);
    v->change_before = NULL;
    free(now);
    if (same) return;
    UndoStep step = {s, v->change_item, after, v->saves};
    if (!s || undo_push(&v->undo, app->settings.undo_levels, &step)) {
        if (!s) undo_lost(&v->undo);
        app_notice(app, T("メモリが足りないため、取り消しの記録を消しました"));
    }
}
static void undo_redo(DocView *v, App *app, int redo)
{
    UndoStep *st = redo ? undo_next_redo(&v->undo) : undo_next_undo(&v->undo);
    if (!st) { app_notice(app, redo ? T("やり直す物はありません") : T("取り消す物はありません")); return; }
    char err[256] = "", message[320];
    size_t n = 0, m = 0;
    unsigned char *cur = doc_build(&v->doc, &n, err, sizeof(err));
    unsigned char *next = cur ? undo_splice_apply(st->data, !redo, cur, n, &m) : NULL;
    free(cur);
    if (!next || doc_reparse(&v->doc, next, m, err, sizeof(err))) {
        undo_clear(&v->undo);
        snprintf(message, sizeof(message), "%s: %s", redo ? T("やり直せません") : T("取り消せません"), err[0] ? err : T("記録が文書と合いません"));
        app_notice(app, message);
        relayout(v, app, (size_t)-1);
        return;
    }
    /* Where the cursor goes: the paragraph the step is about; for an added
     * paragraph undone the one before it, for a deletion redone the one
     * that followed. */
    long here, label;
    if (redo) {
        here = label = st->after >= 0 ? st->after : st->before;
        undo_done_redo(&v->undo);
    } else {
        here = st->before >= 0 ? st->before : st->after - 1;
        label = st->before >= 0 ? st->before : st->after;
        undo_done_undo(&v->undo);
    }
    relayout(v, app, here < 0 ? 0 : (size_t)here);
    snprintf(message, sizeof(message), T("%s（%ld 段落目）"), redo ? T("やり直しました") : T("取り消しました"), label + 1);
    app_notice(app, message);
}

/* ---- editing ---- */
static void begin_edit(DocView *v, App *app, size_t item, int added)
{
    DocItem *it = &v->doc.items[item];
    if (!v->ime) v->ime = ime_open();
    char label[64];
    snprintf(label, sizeof(label), "%s", added ? T("新しい段落") : T("段落"));
    if (doc_input_open(&v->input, v->ime, label, it->edit)) {
        doc_input_close(&v->input);
        app_notice(app, T("メモリが足りません"));
        return;
    }
    v->edit_item = item;
    v->edit_new = added;
    v->prompt = PROMPT_EDIT;
}
static void edit_current(DocView *v, App *app)
{
    const DocLine *ln = current(v);
    if (!ln) return;
    const DocItem *it = &v->doc.items[ln->item];
    if (it->refuse) { app_notice(app, T(it->refuse)); return; }
    change_begin(v, shown_index(&v->doc, ln->item));
    begin_edit(v, app, ln->item, 0);
    if (v->prompt != PROMPT_EDIT) change_end(v, app, -1);
}
static void finish_edit(DocView *v, App *app, int accepted)
{
    char err[256] = "";
    size_t item = v->edit_item;
    if (accepted) {
        char *text = doc_input_text(&v->input);
        if (!text) app_notice(app, T("メモリが足りません"));
        else if (doc_set_text(&v->doc, item, text, err, sizeof(err))) app_notice(app, err);
        free(text);
    } else if (v->edit_new) {
        doc_delete(&v->doc, item, err, sizeof(err));
        if (item) --item;
    }
    /* One step: the edited paragraph, or the added one with its text. */
    change_end(v, app, shown_index(&v->doc, item));
    doc_input_close(&v->input);
    v->prompt = PROMPT_NONE;
    relayout(v, app, item);
}
static void add_paragraph(DocView *v, App *app)
{
    const DocLine *ln = current(v);
    char err[256] = "";
    change_begin(v, -1);
    long at = doc_insert_after(&v->doc, ln ? ln->item : v->doc.nitems, err, sizeof(err));
    if (at < 0) { change_end(v, app, -1); app_notice(app, err); return; }
    if (relayout(v, app, (size_t)at)) { change_end(v, app, shown_index(&v->doc, (size_t)at)); return; }
    begin_edit(v, app, (size_t)at, 1);
    if (v->prompt != PROMPT_EDIT) change_end(v, app, shown_index(&v->doc, (size_t)at));
}
static void ask_delete(DocView *v, App *app)
{
    const DocLine *ln = current(v);
    char err[256] = "";
    if (!ln) return;
    if (doc_can_delete(&v->doc, ln->item, err, sizeof(err))) { app_notice(app, err); return; }
    v->edit_item = ln->item;
    widgets_confirm(&app->widgets, T("この段落を消しますか？"));
    v->prompt = PROMPT_DELETE;
}
static void save(DocView *v, App *app)
{
    if (!undo_dirty(&v->undo)) { app_notice(app, T("変更はありません")); return; }
    char err[256] = "", message[320];
    int backed = 0;
    if (doc_save(&v->doc, 1, &backed, err, sizeof(err))) {
        snprintf(message, sizeof(message), T("保存できません: %s"), err[0] ? err : T("書き込めません"));
        app_notice(app, message);
        return;
    }
    undo_saved(&v->undo);
    v->saves++;
    relayout(v, app, (size_t)-1);
    app_notice(app, backed ? T("保存しました（元のファイルは .bak に残しました）") : T("保存しました"));
}

/* ---- keys ---- */
static void move(DocView *v, long lines)
{
    long n = (long)v->lay.n, c = (long)v->cur + lines;
    if (c >= n) c = n - 1;
    if (c < 0) c = 0;
    v->cur = (size_t)c;
}
static int doc_key(Screen *screen, App *app, const Key *k)
{
    DocView *v = screen->self;
    if (v->prompt == PROMPT_EDIT) {
        int r = doc_input_key(&v->input, k);
        if (v->input.failed) { app_notice(app, T("メモリが足りません")); r = DOC_INPUT_CANCEL; }
        if (r != DOC_INPUT_MORE) finish_edit(v, app, r == DOC_INPUT_ACCEPT);
        return 0;
    }
    if (v->prompt == PROMPT_DELETE || v->prompt == PROMPT_QUIT) {
        WidgetResult r = widgets_key(&app->widgets, k);
        if (r != WIDGET_ACCEPT && r != WIDGET_CANCEL) return 0;
        Prompt p = v->prompt;
        v->prompt = PROMPT_NONE;
        if (r == WIDGET_CANCEL) return 0;
        if (p == PROMPT_QUIT) return 1;
        char err[256] = "";
        size_t item = v->edit_item;
        change_begin(v, shown_index(&v->doc, item));
        if (doc_delete(&v->doc, item, err, sizeof(err))) app_notice(app, err);
        change_end(v, app, -1);
        relayout(v, app, item);
        return 0;
    }
    int ctrl = (k->mods & MOD_CTRL) != 0, alt = (k->mods & MOD_ALT) != 0;
    switch (k->key) {
    case KESC:
        if (undo_dirty(&v->undo)) { widgets_confirm(&app->widgets, T("保存せずに戻りますか？")); v->prompt = PROMPT_QUIT; return 0; }
        return 1;
    case KUP: move(v, -1); return 0;
    case KDOWN: move(v, 1); return 0;
    case KPGUP:
        move(v, -ROWS);
        v->top = v->top >= (size_t)ROWS ? v->top - ROWS : 0;
        return 0;
    case KPGDN:
        move(v, ROWS);
        v->top += ROWS;
        return 0;
    case KHOME: v->cur = v->top = 0; return 0;
    case KEND: move(v, (long)v->lay.n); return 0;
    case KRET: edit_current(v, app); return 0;
    default: break;
    }
    if (k->key != KCHAR || alt) return 0;
    if (ctrl) {
        if (k->ch == 's' || k->ch == 'S') save(v, app);
        else if (k->ch == 'k' || k->ch == 'K') ask_delete(v, app);
        else if (k->ch == 'z' || k->ch == 'Z') undo_redo(v, app, 0);
        else if (k->ch == 'y' || k->ch == 'Y') undo_redo(v, app, 1);
        return 0;
    }
    switch (k->ch) {
    case 'e': case 'E': edit_current(v, app); return 0;
    case 'o': case 'O': add_paragraph(v, app); return 0;
    case 'q': case 'Q': {
        Key esc = {KESC, 0, 0};
        return doc_key(screen, app, &esc);
    }
    case ' ': move(v, ROWS); v->top += ROWS; return 0;
    default: return 0;
    }
}
static void doc_destroy(Screen *screen)
{
    DocView *v = screen->self;
    doc_input_close(&v->input);
    if (v->ime) ime_close(v->ime);
    doc_layout_free(&v->lay);
    doc_free(&v->doc);
    undo_free(&v->undo);
    free(v->change_before);
    free(v);
}

Screen *docview_create(App *app, const char *path)
{
    DocView *v = calloc(1, sizeof(*v));
    if (!v) { app_notice(app, T("メモリが足りません")); return NULL; }
    char err[256] = "", message[320];
    if (doc_open(&v->doc, path, err, sizeof(err))) {
        snprintf(message, sizeof(message), T("文書を開けません: %s"), err[0] ? err : T("形式が違います"));
        app_notice(app, message);
        free(v);
        return NULL;
    }
    if (doc_layout(&v->doc, &v->lay, FRAME_BODY_COLS)) {
        app_notice(app, T("メモリが足りません"));
        doc_free(&v->doc);
        free(v);
        return NULL;
    }
    v->lay_cols = FD_COLS;
    undo_init(&v->undo, undo_splice_free);
    v->screen = (Screen){v, doc_key, doc_draw, 0, NULL, doc_destroy};
    return &v->screen;
}
