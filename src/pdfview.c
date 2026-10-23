// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Stage 3 PDF viewer (docs/FDTP_VIEWERS.md). libmupdf on Linux only
 * (mk/pdfview.mk defines FDTP_WITH_MUPDF); elsewhere a notice stub.
 * Revision 1 item 40: the writing mode (e) puts a character cursor on the
 * page's structured text; Shift+arrows select, and highlights, underlines,
 * strike-outs and notes are added as standard PDF annotations, saved into
 * the file itself (an incremental update). Item 39: Alt+p opens the PDF menu
 * (pdftools.h) on the file and its page. */
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
#include "pdfview.h"
#include "app.h"

#ifndef FDTP_WITH_MUPDF
Screen *pdfview_create(App *app, const char *path)
{
    (void)path;
    app_notice(app, T("未実装です"));
    return NULL;
}
#else
#include "doc_input.h"
#include "ime.h"
#include "pdftools.h"
#include "raster.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

/* VIEW_LEFT VIEW_TOP VIEW_W VIEW_H: raster.h (the body of the screen's frame). */
#define PDF_HITS 64
/* mupdf's resource cache (fonts, decoded images); the device has 994 MB. */
#define PDF_STORE (32u << 20)
#define PDF_SCALE_MIN 0.05f
#define PDF_SCALE_MAX 8.0f
/* The characters of one page the cursor can visit (a dense page has a few
 * thousand); more are not offered. */
#define PDF_TEXT_MAX 100000
#define PDF_PASSWORD_CAP 128
/* Side of a note's icon on the page, in points. */
#define PDF_NOTE_SIZE 20.f

enum { MODE_FIT, MODE_WIDTH, MODE_FREE };
/* What the keys go to: the view, or a box (widgets) or the note's line. */
enum { INPUT_NONE, INPUT_PAGE, INPUT_SEARCH, INPUT_PASSWORD, INPUT_DELETE, INPUT_QUIT, INPUT_TOOLS, INPUT_NOTE };

/* One character of the page's text (fz page space) and its line. */
typedef struct { fz_rect r; int c, line; } PdfChar;
typedef struct { int first, count, vertical; } PdfLine;

typedef struct {
    Screen screen;
    char *path, *dir;
    const char *base;             /* in path: the file's name */
    const char *names[1];         /* the selection given to pdftools (item 39) */
    Canvas *bands;
    fz_context *ctx;
    fz_document *doc;
    int count;
    /* Target state; render() brings the list and pixmap up to it. */
    int page, mode, pending, input;
    int land_bottom;              /* the next page load shows its bottom (↑) */
    int reload_page;              /* this page's list is drawn again where it is (annotations changed) */
    float scale, cx, cy;          /* zoom and viewport centre in page points */
    /* The one loaded page: its display list and the viewport's pixels. */
    int list_page;
    fz_display_list *list;
    fz_rect bounds;
    fz_pixmap *pix;
    int view_x, view_y;           /* device position of the viewport's corner */
    float view_scale;
    int view_w, view_h;           /* VIEW_W x VIEW_H when it was drawn (resize) */
    /* Search: the hits on hit_page, and the current one. */
    char query[WIDGET_CAP];
    int hit_page, hit, hit_count, reveal, search_at;
    int marks[PDF_HITS];
    fz_quad quads[PDF_HITS];
    /* The user password (item 40: kept to open the file again after a full
     * save); never in the widgets' text or history, which see only '*'. */
    int locked, owner;            /* owner: opened with the owner password */
    char password[PDF_PASSWORD_CAP];
    /* Item 40, the writing mode: the text of text_page (its fz_page kept for
     * the annotations), the cursor, the selection's other end (-1: none),
     * and the point cursor of a page without text. */
    int writing, text_page, cur, anchor, keep_cur;
    fz_page *text_fzpage;
    PdfChar *chars;
    PdfLine *lines;
    int nchars, nlines;
    fz_point point;
    int dirty, stale;             /* stale: saved, but could not be opened again */
    Ime *ime;
    DocInput note;
    pdf_annot *target;            /* the note being edited, or the annotation to delete (kept) */
    int tab;                      /* the annotation Tab went to last */
} PdfView;

static void quiet(void *user, const char *message) { (void)user; (void)message; }

static void drop_page(PdfView *v)
{
    fz_drop_pixmap(v->ctx, v->pix); v->pix = NULL;
    fz_drop_display_list(v->ctx, v->list); v->list = NULL;
    v->list_page = -1;
}

static void drop_target(PdfView *v)
{
    pdf_drop_annot(v->ctx, v->target); v->target = NULL;
}

/* Forgets the page's text (and its fz_page). */
static void drop_text(PdfView *v)
{
    drop_target(v);
    fz_drop_page(v->ctx, v->text_fzpage); v->text_fzpage = NULL;
    free(v->chars); free(v->lines); v->chars = NULL; v->lines = NULL;
    v->nchars = v->nlines = 0; v->text_page = -1; v->anchor = -1;
}

/* Loads page v->page as a display list; the fz_page itself is dropped. */
static int load_page(PdfView *v)
{
    fz_context *ctx = v->ctx; fz_page *page = NULL; int failed = 0;
    drop_page(v); v->bounds = fz_empty_rect;
    fz_var(page);
    fz_try(ctx) {
        page = fz_load_page(ctx, v->doc, v->page);
        v->bounds = fz_bound_page(ctx, page);
        v->list = fz_new_display_list_from_page(ctx, page);
    }
    fz_always(ctx) fz_drop_page(ctx, page);
    fz_catch(ctx) failed = 1;
    if (failed || fz_is_empty_rect(v->bounds)) {
        fz_drop_display_list(ctx, v->list); v->list = NULL; return -1;
    }
    v->list_page = v->page; return 0;
}

static float page_w(const PdfView *v) { return v->bounds.x1 - v->bounds.x0; }
static float page_h(const PdfView *v) { return v->bounds.y1 - v->bounds.y0; }

static void fit_scale(PdfView *v)
{
    if (v->mode == MODE_FIT) v->scale = fminf(VIEW_W / page_w(v), VIEW_H / page_h(v));
    else if (v->mode == MODE_WIDTH) v->scale = VIEW_W / page_w(v);
    v->scale = fminf(PDF_SCALE_MAX, fmaxf(PDF_SCALE_MIN, v->scale));
}

/* Keeps the viewport on the page; a page smaller than it is centred. */
static void clamp_view(PdfView *v)
{
    float hw = VIEW_W / 2.f / v->scale, hh = VIEW_H / 2.f / v->scale;
    const fz_rect *b = &v->bounds;
    if (page_w(v) <= 2 * hw) v->cx = (b->x0 + b->x1) / 2;
    else v->cx = fminf(b->x1 - hw, fmaxf(b->x0 + hw, v->cx));
    if (page_h(v) <= 2 * hh) v->cy = (b->y0 + b->y1) / 2;
    else v->cy = fminf(b->y1 - hh, fmaxf(b->y0 + hh, v->cy));
}

static void view_top(PdfView *v)
{
    v->cx = (v->bounds.x0 + v->bounds.x1) / 2;
    v->cy = v->bounds.y0 + VIEW_H / 2.f / v->scale;
}

static fz_rect hit_rect(const PdfView *v)
{
    fz_rect r = fz_empty_rect;
    for (int i = v->hit; i < v->hit_count; i++) {
        if (i > v->hit && v->marks[i]) break;
        r = fz_union_rect(r, fz_rect_from_quad(v->quads[i]));
    }
    return r;
}

/* The quad that starts the hit after quad v->hit, or -1. */
static int next_hit(const PdfView *v)
{
    for (int i = v->hit + 1; i < v->hit_count; i++) if (v->marks[i]) return i;
    return -1;
}

static int render(PdfView *v, App *a)
{
    fz_context *ctx = v->ctx;
    if (v->list_page != v->page || v->reload_page == v->page) {
        /* Annotations changed (or the file was saved anew): the same page
         * again, where the view is. */
        int keep = v->reload_page == v->page && v->list_page == v->page;
        v->reload_page = -1;
        if (load_page(v)) { app_notice(a, T("ページを描けません")); v->land_bottom = 0; return -1; }
        fit_scale(v);
        if (!keep) {
            view_top(v);
            if (v->land_bottom) v->cy = v->bounds.y1 - VIEW_H / 2.f / v->scale;
        }
        v->land_bottom = 0;
    } else fit_scale(v);
    if (v->reveal && v->hit_page == v->page) {
        fz_rect r = hit_rect(v);
        v->cx = (r.x0 + r.x1) / 2; v->cy = (r.y0 + r.y1) / 2;
    }
    v->reveal = 0;
    clamp_view(v);
    fz_matrix ctm = fz_scale(v->scale, v->scale);
    fz_irect page = fz_round_rect(fz_transform_rect(v->bounds, ctm));
    int vx = (int)floorf(v->cx * v->scale - VIEW_W / 2.f), vy = (int)floorf(v->cy * v->scale - VIEW_H / 2.f);
    fz_irect view = { vx, vy, vx + VIEW_W, vy + VIEW_H };
    fz_irect box = fz_intersect_irect(page, view);
    fz_pixmap *pix = NULL; fz_device *dev = NULL; int failed = 0;
    fz_drop_pixmap(ctx, v->pix); v->pix = NULL;   /* only one page's pixels at a time */
    if (fz_is_empty_irect(box)) return 0;
    fz_var(pix); fz_var(dev);
    fz_try(ctx) {
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, NULL, 0);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_display_list(ctx, v->list, dev, ctm, fz_rect_from_irect(box), NULL);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) failed = 1;
    if (failed) { fz_drop_pixmap(ctx, pix); app_notice(a, T("ページを描けません")); return -1; }
    v->pix = pix; v->view_x = vx; v->view_y = vy; v->view_scale = v->scale;
    v->view_w = VIEW_W; v->view_h = VIEW_H;
    return 0;
}

static void request(PdfView *v) { v->pending = 1; }

static void go_page(PdfView *v, int page)
{
    v->land_bottom = 0;
    if (page < 0) page = 0;
    if (page >= v->count) page = v->count - 1;
    if (page != v->page) { v->page = page; request(v); }
}

/* Searches page n; the current page uses its display list. */
static int search_hits(PdfView *v)
{
    fz_context *ctx = v->ctx; volatile int count = 0;
    fz_try(ctx) {
        if (v->search_at == v->list_page && v->list)
            count = fz_search_display_list(ctx, v->list, v->query, v->marks, v->quads, PDF_HITS);
        else count = fz_search_page_number(ctx, v->doc, v->search_at, v->query, v->marks, v->quads, PDF_HITS);
    }
    fz_catch(ctx) count = 0;
    return count;
}

static int search_page(PdfView *v, int n)
{
    v->search_at = n;
    int count = search_hits(v);
    v->hit_count = count; v->hit_page = count ? n : -1;
    return count;
}

/* from_start: the first search, which begins on the current page. */
static void find(PdfView *v, App *a, int from_start)
{
    if (!v->query[0]) { app_notice(a, T("検索文字を入力してください")); return; }
    int next = from_start || v->hit_page != v->page ? -1 : next_hit(v);
    if (next >= 0) { v->hit = next; v->reveal = 1; request(v); return; }
    for (int k = from_start ? 0 : 1; k <= v->count; k++) {
        int n = (v->page + k) % v->count;
        if (search_page(v, n)) {
            v->hit = 0; v->reveal = 1;
            if (k == v->count || (k && n <= v->page)) app_notice(a, T("先頭から検索しました"));
            if (n != v->page) v->page = n;
            request(v); return;
        }
        if (k == v->count) break;
    }
    v->hit_page = -1; v->hit_count = 0;
    app_notice(a, T("見つかりません"));
}

static void zoom(PdfView *v, float factor)
{
    float s = fminf(PDF_SCALE_MAX, fmaxf(PDF_SCALE_MIN, v->scale * factor));
    if (s != v->scale) { v->scale = s; v->mode = MODE_FREE; request(v); }
}

/* After cx/cy moved: a new picture when the viewport's corner moved. */
static void view_moved(PdfView *v)
{
    clamp_view(v);
    if (v->list_page != v->page || !v->pix) return;
    if (v->view_x != (int)floorf(v->cx * v->scale - VIEW_W / 2.f) ||
        v->view_y != (int)floorf(v->cy * v->scale - VIEW_H / 2.f)) request(v);
}

static void pan(PdfView *v, int dx, int dy)
{
    v->cx += dx / v->scale; v->cy += dy / v->scale; view_moved(v);
}

/* Revision 1 §3 14: ↑↓ scroll a page taller than the view; at its top or
 * bottom edge (or when it fits) they go to the previous page, shown from its
 * bottom, or the next one, from its top. */
static void scroll_or_turn(PdfView *v, int down)
{
    if (v->list_page == v->page && !fz_is_empty_rect(v->bounds)) {
        float hh = VIEW_H / 2.f / v->scale, eps = 0.5f / v->scale;
        clamp_view(v);
        int room = page_h(v) > 2 * hh && (down ? v->cy < v->bounds.y1 - hh - eps : v->cy > v->bounds.y0 + hh + eps);
        if (room) { pan(v, 0, down ? 64 : -64); return; }
    }
    int target = v->page + (down ? 1 : -1);
    if (target < 0 || target >= v->count) return;
    go_page(v, target);
    v->land_bottom = !down;
}

/* ---- the writing mode (revision 1 item 40) ---- */

static pdf_document *pdf_of(PdfView *v) { return pdf_specifics(v->ctx, v->doc); }

/* Counts (chars NULL) or copies the text lines of the blocks, depth first
 * through structure blocks. mupdf 1.25 makes a line of each character of
 * vertical text: the lines of one column (a block's vertical lines one under
 * the other, overlapping across) are joined into one. */
static void walk_text(PdfView *v, fz_stext_block *b, int *nchars, int *nlines)
{
    for (; b; b = b->next) {
        if (b->type == FZ_STEXT_BLOCK_STRUCT) {
            if (b->u.s.down) walk_text(v, b->u.s.down->first_block, nchars, nlines);
            continue;
        }
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        int column = 0; fz_rect last = fz_empty_rect;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) {
            int first = *nchars, vertical = l->wmode == 1 || fabsf(l->dir.y) > fabsf(l->dir.x);
            float overlap = fminf(l->bbox.x1, last.x1) - fmaxf(l->bbox.x0, last.x0);
            int join = vertical && column && overlap > 0.5f * (l->bbox.x1 - l->bbox.x0) && l->bbox.y0 >= last.y0;
            int line = join ? *nlines - 1 : *nlines;
            for (fz_stext_char *ch = l->first_char; ch && *nchars < PDF_TEXT_MAX; ch = ch->next) {
                if (v->chars) v->chars[*nchars] = (PdfChar){ fz_rect_from_quad(ch->quad), ch->c, line };
                ++*nchars;
            }
            if (*nchars == first) continue;
            if (join) { if (v->lines) v->lines[line].count += *nchars - first; }
            else {
                if (v->lines) v->lines[line] = (PdfLine){ first, *nchars - first, vertical };
                ++*nlines;
            }
            column = vertical; last = l->bbox;
        }
    }
}

/* The first character in the view, for the cursor when the mode begins. */
static int first_visible(const PdfView *v)
{
    float hw = VIEW_W / 2.f / v->scale, hh = VIEW_H / 2.f / v->scale;
    for (int i = 0; i < v->nchars; i++) {
        const fz_rect *r = &v->chars[i].r;
        float x = (r->x0 + r->x1) / 2, y = (r->y0 + r->y1) / 2;
        if (x >= v->cx - hw && x <= v->cx + hw && y >= v->cy - hh && y <= v->cy + hh) return i;
    }
    return 0;
}

/* The text of the current page (the page's own content, not annotations). */
static int build_text(PdfView *v, App *a)
{
    fz_context *ctx = v->ctx; fz_page *page = NULL; fz_stext_page *st = NULL; fz_device *dev = NULL;
    volatile int failed = 0;
    drop_text(v);
    fz_var(page); fz_var(st); fz_var(dev);
    fz_try(ctx) {
        page = fz_load_page(ctx, v->doc, v->page);
        fz_stext_options opts; memset(&opts, 0, sizeof(opts));
        st = fz_new_stext_page(ctx, fz_bound_page(ctx, page));
        dev = fz_new_stext_device(ctx, st, &opts);
        fz_run_page_contents(ctx, page, dev, fz_identity, NULL);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) failed = 1;
    int nchars = 0, nlines = 0;
    if (!failed) {
        walk_text(v, st->first_block, &nchars, &nlines);
        v->chars = malloc(sizeof(PdfChar) * (size_t)(nchars ? nchars : 1));
        v->lines = malloc(sizeof(PdfLine) * (size_t)(nlines ? nlines : 1));
        if (!v->chars || !v->lines) failed = 2;
        else { nchars = nlines = 0; walk_text(v, st->first_block, &nchars, &nlines); }
    }
    fz_drop_stext_page(ctx, st);
    if (failed) {
        fz_drop_page(ctx, page); drop_text(v);
        app_notice(a, failed == 2 ? T("メモリが足りません") : T("ページの文字を読めません"));
        v->writing = 0; return -1;
    }
    v->text_fzpage = page; v->text_page = v->page; v->nchars = nchars; v->nlines = nlines; v->tab = -1;
    if (v->keep_cur >= 0 && v->keep_cur < nchars) v->cur = v->keep_cur;
    else v->cur = first_visible(v);
    v->keep_cur = -1; v->anchor = -1;
    v->point = (fz_point){ v->cx, v->cy };
    return 0;
}

/* Where the cursor is, in page space. */
static fz_rect cursor_rect(const PdfView *v)
{
    if (v->nchars) return v->chars[v->cur].r;
    return (fz_rect){ v->point.x - 4, v->point.y - 4, v->point.x + 4, v->point.y + 4 };
}

static fz_point cursor_point(const PdfView *v)
{
    fz_rect r = cursor_rect(v);
    return (fz_point){ (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2 };
}

/* Scrolls the view (by the least) to show the cursor. */
static void follow(PdfView *v)
{
    if (v->list_page != v->page) return;
    fz_rect r = cursor_rect(v);
    float hw = VIEW_W / 2.f / v->scale, hh = VIEW_H / 2.f / v->scale, m = 8 / v->scale;
    if (r.x1 - r.x0 + 2 * m > 2 * hw) v->cx = (r.x0 + r.x1) / 2;
    else if (r.x0 - m < v->cx - hw) v->cx = r.x0 - m + hw;
    else if (r.x1 + m > v->cx + hw) v->cx = r.x1 + m - hw;
    if (r.y1 - r.y0 + 2 * m > 2 * hh) v->cy = (r.y0 + r.y1) / 2;
    else if (r.y0 - m < v->cy - hh) v->cy = r.y0 - m + hh;
    else if (r.y1 + m > v->cy + hh) v->cy = r.y1 + m - hh;
    view_moved(v);
}

/* The character of line n nearest to the cursor along the cursor's line. */
static int nearest_in_line(const PdfView *v, int n)
{
    const PdfLine *l = &v->lines[n];
    int across = v->lines[v->chars[v->cur].line].vertical;
    fz_point p = cursor_point(v); int best = l->first; float bd = INFINITY;
    for (int i = l->first; i < l->first + l->count; i++) {
        const fz_rect *r = &v->chars[i].r;
        float d = across ? fabsf((r->y0 + r->y1) / 2 - p.y) : fabsf((r->x0 + r->x1) / 2 - p.x);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Arrows move the cursor by a character or a line (a vertical line: ↑↓
 * along it, ← to the next line); Home End to the ends of the line. Shift
 * keeps the other end of the selection where it was. */
static void move_cursor(PdfView *v, int key, int shift)
{
    if (!v->nchars) {
        float step = 24 / v->scale;
        if (key == KLEFT) v->point.x -= step;
        else if (key == KRIGHT) v->point.x += step;
        else if (key == KUP) v->point.y -= step;
        else if (key == KDOWN) v->point.y += step;
        v->point.x = fminf(v->bounds.x1, fmaxf(v->bounds.x0, v->point.x));
        v->point.y = fminf(v->bounds.y1, fmaxf(v->bounds.y0, v->point.y));
        follow(v); return;
    }
    if (shift) { if (v->anchor < 0) v->anchor = v->cur; }
    else v->anchor = -1;
    int line = v->chars[v->cur].line;
    const PdfLine *l = &v->lines[line];
    int step = 0, lines = 0;
    switch (key) {
    case KLEFT: if (l->vertical) lines = 1; else step = -1; break;
    case KRIGHT: if (l->vertical) lines = -1; else step = 1; break;
    case KUP: if (l->vertical) step = -1; else lines = -1; break;
    case KDOWN: if (l->vertical) step = 1; else lines = 1; break;
    case KHOME: v->cur = l->first; break;
    case KEND: v->cur = l->first + l->count - 1; break;
    default: break;
    }
    if (step) v->cur = v->cur + step < 0 ? 0 : v->cur + step >= v->nchars ? v->nchars - 1 : v->cur + step;
    if (lines && line + lines >= 0 && line + lines < v->nlines) v->cur = nearest_in_line(v, line + lines);
    follow(v);
}

static int selection(const PdfView *v, int *lo, int *hi)
{
    if (v->anchor < 0 || !v->nchars) return 0;
    *lo = v->anchor < v->cur ? v->anchor : v->cur;
    *hi = v->anchor < v->cur ? v->cur : v->anchor;
    return 1;
}

static const char *annot_name(enum pdf_annot_type t)
{
    switch (t) {
    case PDF_ANNOT_TEXT: return T("付箋");
    case PDF_ANNOT_HIGHLIGHT: return T("ハイライト");
    case PDF_ANNOT_UNDERLINE: return T("下線");
    case PDF_ANNOT_STRIKE_OUT: return T("取り消し線");
    case PDF_ANNOT_SQUIGGLY: return T("波線");
    case PDF_ANNOT_FREE_TEXT: return T("テキスト");
    default: return T("注釈");
    }
}

/* The types the mode makes, and so may delete. */
static int ours(enum pdf_annot_type t)
{
    return t == PDF_ANNOT_TEXT || t == PDF_ANNOT_HIGHLIGHT || t == PDF_ANNOT_UNDERLINE ||
           t == PDF_ANNOT_STRIKE_OUT || t == PDF_ANNOT_SQUIGGLY;
}

/* Annotations the cursor can be on: not links, form fields or popups. */
static int shown_annot(fz_context *ctx, pdf_annot *an)
{
    enum pdf_annot_type t = pdf_annot_type(ctx, an);
    return t != PDF_ANNOT_LINK && t != PDF_ANNOT_WIDGET && t != PDF_ANNOT_POPUP;
}

/* 1 when the character box r is on the annotation: its centre inside one of
 * the quads (a highlight, an underline ...), or the box meeting the
 * annotation's rectangle (a note's icon). */
static int annot_has(fz_context *ctx, pdf_annot *an, fz_rect r)
{
    volatile int in = 0;
    fz_point p = { (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2 };
    fz_try(ctx) {
        if (pdf_annot_has_quad_points(ctx, an) && pdf_annot_quad_point_count(ctx, an) > 0) {
            int n = pdf_annot_quad_point_count(ctx, an);
            for (int i = 0; i < n && !in; i++) in = fz_is_point_inside_rect(p, fz_rect_from_quad(pdf_annot_quad_point(ctx, an, i)));
        } else in = !fz_is_empty_rect(fz_intersect_rect(r, pdf_bound_annot(ctx, an)));
    }
    fz_catch(ctx) in = 0;
    return in;
}

/* The topmost annotation under the cursor (borrowed), or NULL. */
static pdf_annot *annot_at_cursor(PdfView *v)
{
    if (v->text_page != v->page || !v->text_fzpage) return NULL;
    fz_context *ctx = v->ctx; pdf_page *pp = pdf_page_from_fz_page(ctx, v->text_fzpage);
    if (!pp) return NULL;
    fz_rect r = cursor_rect(v); pdf_annot *found = NULL;
    for (pdf_annot *an = pdf_first_annot(ctx, pp); an; an = pdf_next_annot(ctx, an))
        if (shown_annot(ctx, an) && annot_has(ctx, an, r)) found = an;
    return found;
}

static const char *contents_of(fz_context *ctx, pdf_annot *an)
{
    const char *volatile s = "";
    fz_try(ctx) s = pdf_annot_contents(ctx, an);
    fz_catch(ctx) s = "";
    return s ? s : "";
}

/* 1 when annotations may be written; otherwise a notice. */
static int can_write(PdfView *v, App *a)
{
    if (!pdf_of(v)) { app_notice(a, T("PDF ではないので書き込めません")); return 0; }
    /* mupdf's permission is the file's /P alone; the owner may do anything. */
    if (!v->owner && !fz_has_permission(v->ctx, v->doc, FZ_PERMISSION_ANNOTATE)) {
        app_notice(a, T("この PDF は注釈の書き込みが許されていません")); return 0;
    }
    if (v->stale) { app_notice(a, T("開き直せなかったので、書き込めません")); return 0; }
    if (v->text_page != v->page || !v->text_fzpage) return 0;
    return 1;
}

/* The page was changed: its picture is made again, where it is. */
static void changed(PdfView *v)
{
    v->dirty = 1; v->reload_page = v->page; request(v);
}

static void stamp(fz_context *ctx, pdf_annot *an)
{
    int64_t now = (int64_t)time(NULL);
    pdf_set_annot_creation_date(ctx, an, now);
    pdf_set_annot_modification_date(ctx, an, now);
}

/* MuPDF's appearance streams for underlines, strike-outs and notes have no
 * /Resources (optional in the PDF spec), and macOS PDFKit (Preview) then
 * draws them wrongly: the underline white, the note black (tried with the
 * saved file). An empty /Resources makes it draw them as MuPDF does. */
static void appearance_resources(fz_context *ctx, pdf_annot *an)
{
    pdf_obj *n = pdf_dict_getp(ctx, pdf_annot_obj(ctx, an), "AP/N");
    if (n && pdf_is_stream(ctx, n) && !pdf_dict_get(ctx, n, PDF_NAME(Resources))) pdf_dict_put_dict(ctx, n, PDF_NAME(Resources), 0);
}

/* Highlight, underline or strike-out over the selection: one quad per line
 * (the union of the selected characters' boxes on it). */
static void add_markup(PdfView *v, App *a, enum pdf_annot_type type)
{
    int lo, hi;
    if (!can_write(v, a)) return;
    if (!selection(v, &lo, &hi)) { app_notice(a, T("範囲を選んでください（Shift+矢印）")); return; }
    int n = v->chars[hi].line - v->chars[lo].line + 1;
    fz_quad *q = malloc(sizeof(fz_quad) * (size_t)n);
    if (!q) { app_notice(a, T("メモリが足りません")); return; }
    int count = 0;
    for (int i = lo; i <= hi; ) {
        int line = v->chars[i].line; fz_rect r = fz_empty_rect;
        for (; i <= hi && v->chars[i].line == line; i++) r = fz_union_rect(r, v->chars[i].r);
        if (fz_is_empty_rect(r)) continue;
        /* A vertical line reads downwards: its quad is turned so that the
         * underline (the ll-lr edge) is on the right, as 傍線. */
        if (v->lines[line].vertical)
            q[count++] = (fz_quad){ { r.x0, r.y0 }, { r.x0, r.y1 }, { r.x1, r.y0 }, { r.x1, r.y1 } };
        else q[count++] = fz_quad_from_rect(r);
    }
    if (!count) { free(q); app_notice(a, T("選んだ字に大きさがありません")); return; }
    static const float yellow[3] = { 1, 1, 0 }, blue[3] = { 0, 0.33f, 1 }, red[3] = { 0.9f, 0, 0 };
    const float *colour = type == PDF_ANNOT_HIGHLIGHT ? yellow : type == PDF_ANNOT_UNDERLINE ? blue : red;
    fz_context *ctx = v->ctx; pdf_annot *an = NULL; volatile int failed = 0;
    fz_var(an);
    fz_try(ctx) {
        an = pdf_create_annot(ctx, pdf_page_from_fz_page(ctx, v->text_fzpage), type);
        pdf_set_annot_color(ctx, an, 3, colour);
        pdf_set_annot_quad_points(ctx, an, count, q);
        stamp(ctx, an);
        pdf_update_annot(ctx, an); appearance_resources(ctx, an);
    }
    fz_always(ctx) pdf_drop_annot(ctx, an);
    fz_catch(ctx) failed = 1;
    free(q);
    if (failed) { app_notice(a, T("注釈を付けられません")); return; }
    v->anchor = -1; changed(v);
}

/* t: a new note at the cursor, or the cursor's note to rewrite. */
static void begin_note(PdfView *v, App *a)
{
    if (!can_write(v, a)) return;
    pdf_annot *on = annot_at_cursor(v);
    drop_target(v);
    if (on && pdf_annot_type(v->ctx, on) == PDF_ANNOT_TEXT) v->target = pdf_keep_annot(v->ctx, on);
    if (!v->ime) v->ime = ime_open();
    if (doc_input_open(&v->note, v->ime, v->target ? T("付箋を直す") : T("付箋"), v->target ? contents_of(v->ctx, v->target) : "")) {
        doc_input_close(&v->note); drop_target(v); app_notice(a, T("メモリが足りません")); return;
    }
    v->input = INPUT_NOTE;
}

static void finish_note(PdfView *v, App *a, int accepted)
{
    char *text = accepted ? doc_input_text(&v->note) : NULL;
    doc_input_close(&v->note); v->input = INPUT_NONE;
    if (accepted && !text) { app_notice(a, T("メモリが足りません")); drop_target(v); return; }
    if (!accepted || (!*text && !v->target)) { free(text); drop_target(v); return; }
    fz_context *ctx = v->ctx; pdf_annot *an = NULL; volatile int failed = 0;
    fz_var(an);
    fz_try(ctx) {
        if (v->target) an = pdf_keep_annot(ctx, v->target);
        else {
            static const float colour[3] = { 1, 0.85f, 0 };
            fz_rect r = cursor_rect(v);
            an = pdf_create_annot(ctx, pdf_page_from_fz_page(ctx, v->text_fzpage), PDF_ANNOT_TEXT);
            pdf_set_annot_rect(ctx, an, (fz_rect){ r.x0, r.y0, r.x0 + PDF_NOTE_SIZE, r.y0 + PDF_NOTE_SIZE });
            pdf_set_annot_color(ctx, an, 3, colour);
            pdf_set_annot_creation_date(ctx, an, (int64_t)time(NULL));
        }
        pdf_set_annot_contents(ctx, an, text);
        pdf_set_annot_modification_date(ctx, an, (int64_t)time(NULL));
        pdf_update_annot(ctx, an); appearance_resources(ctx, an);
    }
    fz_always(ctx) pdf_drop_annot(ctx, an);
    fz_catch(ctx) failed = 1;
    free(text); drop_target(v);
    if (failed) { app_notice(a, T("付箋を貼れません")); return; }
    changed(v);
}

/* d: asks before deleting the annotation under the cursor. */
static void ask_delete(PdfView *v, App *a)
{
    if (!can_write(v, a)) return;
    pdf_annot *on = annot_at_cursor(v);
    if (!on) { app_notice(a, T("カーソルの所に注釈はありません")); return; }
    enum pdf_annot_type t = pdf_annot_type(v->ctx, on);
    if (!ours(t)) { app_notice(a, T("この種類の注釈は消せません")); return; }
    drop_target(v); v->target = pdf_keep_annot(v->ctx, on);
    char message[128];
    snprintf(message, sizeof(message), T("この%sを消しますか？"), annot_name(t));
    widgets_confirm(&a->widgets, message); v->input = INPUT_DELETE;
}

static void delete_target(PdfView *v, App *a)
{
    fz_context *ctx = v->ctx; volatile int failed = 0;
    fz_try(ctx) pdf_delete_annot(ctx, pdf_page_from_fz_page(ctx, v->text_fzpage), v->target);
    fz_catch(ctx) failed = 1;
    drop_target(v);
    if (failed) { app_notice(a, T("注釈を消せません")); return; }
    changed(v);
}

/* RET: the whole text of the cursor's annotation in a box. */
static void read_note(PdfView *v, App *a)
{
    pdf_annot *on = annot_at_cursor(v);
    if (!on) { app_notice(a, T("カーソルの所に注釈はありません")); return; }
    const char *s = contents_of(v->ctx, on);
    if (!*s) { app_notice(a, T("この注釈に文はありません")); return; }
    /* One row per line of the text (an info box row holds 511 bytes),
     * named by its number. */
    static char rows[WIDGET_INFO_LINES][512], numbers[WIDGET_INFO_LINES][8];
    const char *names[WIDGET_INFO_LINES], *values[WIDGET_INFO_LINES];
    int n = 0;
    while (*s && n < WIDGET_INFO_LINES) {
        size_t len = strcspn(s, "\r\n");
        if (len > sizeof(rows[0]) - 1) {
            len = sizeof(rows[0]) - 1;
            while (len && ((unsigned char)s[len] & 0xc0) == 0x80) len--;   /* not inside a character */
        }
        memcpy(rows[n], s, len); rows[n][len] = 0;
        snprintf(numbers[n], sizeof(numbers[n]), "%d", n + 1);
        names[n] = numbers[n]; values[n] = rows[n]; n++;
        s += len;
        if (*s == '\r' && s[1] == '\n') s++;
        if (*s == '\r' || *s == '\n') s++;
    }
    widgets_info(&a->widgets, annot_name(pdf_annot_type(v->ctx, on)), names, values, n);
}

/* Tab: the cursor to the next annotation of the page. */
static void next_annot(PdfView *v, App *a)
{
    if (v->text_page != v->page || !v->text_fzpage) return;
    fz_context *ctx = v->ctx; pdf_page *pp = pdf_page_from_fz_page(ctx, v->text_fzpage);
    int n = 0;
    for (pdf_annot *an = pp ? pdf_first_annot(ctx, pp) : NULL; an; an = pdf_next_annot(ctx, an)) n += shown_annot(ctx, an);
    if (!n) { app_notice(a, T("このページに注釈はありません")); return; }
    v->tab = (v->tab + 1) % n;
    int k = 0; pdf_annot *an = pdf_first_annot(ctx, pp);
    for (; an; an = pdf_next_annot(ctx, an)) if (shown_annot(ctx, an) && k++ == v->tab) break;
    fz_rect r = pdf_bound_annot(ctx, an);
    fz_point p = { (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2 };
    if (pdf_annot_has_quad_points(ctx, an) && pdf_annot_quad_point_count(ctx, an) > 0) {
        fz_rect q = fz_rect_from_quad(pdf_annot_quad_point(ctx, an, 0));
        p = (fz_point){ q.x0 + 1, (q.y0 + q.y1) / 2 };
    }
    v->anchor = -1;
    if (!v->nchars) { v->point = p; follow(v); return; }
    /* The first character inside it, else the nearest one. */
    int best = -1; float bd = INFINITY;
    for (int i = 0; i < v->nchars; i++) {
        fz_rect c = v->chars[i].r; fz_point m = { (c.x0 + c.x1) / 2, (c.y0 + c.y1) / 2 };
        if (annot_has(ctx, an, c)) { best = i; break; }
        float d = hypotf(m.x - p.x, m.y - p.y);
        if (d < bd) { bd = d; best = i; }
    }
    v->cur = best; follow(v);
}

/* Opens the document again after a save. When that fails the old one stays
 * for viewing (it still reads the file it opened); save() then marks it
 * stale, so it is never written again. */
static int reopen(PdfView *v)
{
    fz_context *ctx = v->ctx; fz_document *volatile doc = NULL; volatile int count = 0;
    volatile int failed = 0;
    v->keep_cur = v->writing ? v->cur : -1;
    drop_text(v); drop_page(v);
    fz_try(ctx) {
        doc = fz_open_document(ctx, v->path);
        if (fz_needs_password(ctx, doc)) {
            int ok = fz_authenticate_password(ctx, doc, v->password);
            if (!ok || (v->owner && !(ok & 4))) fz_throw(ctx, FZ_ERROR_GENERIC, "password");
        }
        count = fz_count_pages(ctx, doc);
        if (count < 1) fz_throw(ctx, FZ_ERROR_GENERIC, "no pages");
    }
    fz_catch(ctx) failed = 1;
    if (failed) fz_drop_document(ctx, doc);
    else { fz_drop_document(ctx, v->doc); v->doc = doc; v->count = count; }
    if (v->page >= v->count) v->page = v->count - 1;
    v->hit_page = -1; v->hit_count = 0;
    /* The picture comes back where it was (render keeps the view). */
    v->list_page = v->page; v->reload_page = v->page; request(v);
    return failed ? -1 : 0;
}

/* Ctrl+s. Incremental: the changes are appended to the file itself (the
 * bytes before stay as they were). A file mupdf repaired cannot take that:
 * it is written whole beside the original (same directory) and renamed over
 * it. Either way the file is opened again: mupdf's document does not follow
 * its own save (a second incremental save was written over the first, at the
 * old end of the file, with a wrong /Prev). Returns 0 when saved. */
static int save(PdfView *v, App *a)
{
    fz_context *ctx = v->ctx; pdf_document *pdf = pdf_of(v);
    if (!v->dirty) { app_notice(a, T("変更はありません")); return 0; }
    if (!pdf) { app_notice(a, T("PDF ではないので書き込めません")); return -1; }
    if (v->stale) { app_notice(a, T("保存できません: 開き直せなかったファイルです")); return -1; }
    struct stat st;
    if (stat(v->path, &st) || access(v->path, W_OK)) { app_notice(a, T("保存できません: 書き込めません")); return -1; }
    pdf_write_options opts = pdf_default_write_options;
    volatile int failed = 0; int whole = !pdf_can_be_saved_incrementally(ctx, pdf);
    if (!whole) {
        opts.do_incremental = 1;
        fz_try(ctx) pdf_save_document(ctx, pdf, v->path, &opts);
        fz_catch(ctx) failed = 1;
        /* Nothing half-written is left: the file goes back to its length. */
        if (failed) {
            if (truncate(v->path, st.st_size)) { /* the notice below says it failed */ }
            app_notice(a, T("保存できません: 書き込みに失敗しました")); return -1;
        }
    } else {
        char real[PATH_MAX], tmp[PATH_MAX + 32];
        if (!fz_realpath(v->path, real) || snprintf(tmp, sizeof(tmp), "%s.fdtp-%ld", real, (long)getpid()) >= (int)sizeof(tmp)) {
            app_notice(a, T("保存できません: 名前が長すぎます")); return -1;
        }
        struct stat old;
        if (!lstat(tmp, &old)) { app_notice(a, T("保存できません: 作業用の名前が使われています")); return -1; }
        fz_try(ctx) pdf_save_document(ctx, pdf, tmp, &opts);
        fz_catch(ctx) failed = 1;
        if (failed || chmod(tmp, st.st_mode & 07777) || rename(tmp, real)) {
            unlink(tmp); app_notice(a, T("保存できません: 書き込みに失敗しました")); return -1;
        }
    }
    v->dirty = 0;
    if (reopen(v)) {
        /* The old document no longer matches the file: no more saves. */
        v->stale = 1; app_notice(a, T("保存しましたが、開き直せません")); return 0;
    }
    app_notice(a, whole ? T("保存しました（全体を書き直しました）") : T("保存しました"));
    return 0;
}

/* Alt+p: the PDF menu on this file, from this page. */
static void open_tools(PdfView *v, App *a)
{
    FileSelection sel = { v->dir, v->base, v->names, 1 };
    pdftools_open(a, &sel, PDFTOOLS_MENU, v->page);
}

static void ask_save(PdfView *v, App *a, int input, const char *label, const char *yes, const char *no)
{
    static const char *options[3];
    options[0] = yes; options[1] = no; options[2] = T("キャンセル");
    widgets_choice(&a->widgets, label, options, 3);
    v->input = input;
}

static void enter_writing(PdfView *v, App *a)
{
    if (!pdf_of(v)) { app_notice(a, T("PDF ではないので書き込めません")); return; }
    v->writing = 1; v->keep_cur = -1;
    if (v->text_page != v->page) request(v);
}

static void leave_writing(PdfView *v)
{
    v->writing = 0; v->anchor = -1;
}

/* The password box: each character is kept here and shown as '*'; the box's
 * text never holds the password, and nothing goes into the history. */
static int password_key(PdfView *v, App *a, const Key *k)
{
    Widgets *w = &a->widgets;
    size_t n = strlen(v->password);
    if (k->key == KESC) { w->kind = WIDGET_NONE; v->input = INPUT_NONE; return 1; }
    if (k->key == KBS) {
        if (n) {
            do n--; while (n && ((unsigned char)v->password[n] & 0xc0) == 0x80);
            v->password[n] = 0;
            size_t stars = strlen(w->text); if (stars) w->text[stars - 1] = 0;
        }
    } else if (k->key == KRET) {
        int ok = fz_authenticate_password(v->ctx, v->doc, v->password);
        if (!ok) {
            memset(v->password, 0, sizeof(v->password));
            widgets_input(w, T("パスワードが違います。もう一度"), "", NULL); return 0;
        }
        w->kind = WIDGET_NONE; v->input = INPUT_NONE;
        v->owner = (ok & 4) != 0;   /* bit 2: the owner password */
        volatile int failed = 0;
        fz_try(v->ctx) v->count = fz_count_pages(v->ctx, v->doc);
        fz_catch(v->ctx) failed = 1;
        if (failed || v->count < 1) { app_notice(a, T("PDF を開けません")); return 1; }
        v->locked = 0; request(v); return 0;
    } else if (k->key == KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) && k->ch >= 0x20 && k->ch != 0x7f &&
               k->ch != IME_CH_ZENKAKU_HANKAKU) {
        char b[4]; int len;
        uint32_t c = k->ch;
        if (c < 0x80) { b[0] = (char)c; len = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); len = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); len = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); len = 4; }
        size_t stars = strlen(w->text);
        if (n + (size_t)len < sizeof(v->password) && stars + 1 < sizeof(w->text)) {
            memcpy(v->password + n, b, (size_t)len); v->password[n + (size_t)len] = 0;
            w->text[stars] = '*'; w->text[stars + 1] = 0;
        }
    }
    w->cursor = strlen(w->text);
    return 0;
}

static const WidgetHelpItem pdf_help[] = {
    { NULL, N_("見る") },
    { "SPC PgDn", N_("次のページ") }, { "BS PgUp", N_("前のページ") }, { "Home End", N_("最初・最後のページ") },
    { "↑↓", N_("上下に動かす（端で前・次のページ）") }, { "←→", N_("左右に動かす") }, { "g", N_("ページ番号で移る") },
    { "w f", N_("幅に合わせる・全体を出す") }, { "+ -", N_("拡大・縮小") }, { "/ n", N_("検索・次を検索") },
    { "Alt+p", N_("PDF のメニュー") }, { "q Esc", N_("戻る（変更があれば保存を聞く）") },
    { NULL, N_("書き込み（e で入る・出る）") },
    { N_("矢印"), N_("文字のカーソルを動かす") }, { N_("Shift+矢印"), N_("範囲を選ぶ（行をまたげる）") },
    { "Home End", N_("行の頭・行の終わり") }, { "h", N_("ハイライト") }, { "u", N_("下線") }, { "s", N_("取り消し線") },
    { "t", N_("付箋を貼る・直す（日本語はAlt+Space）") }, { "d Del", N_("カーソルの所の注釈を消す") },
    { "RET", N_("付箋の文を読む") }, { "Tab", N_("次の注釈へ") }, { "Esc", N_("選んだ範囲をやめる・書き込みを出る") },
    { "Ctrl+s", N_("保存（元のファイルに追記）") },
};

static void show_pdf_help(App *a)
{
    static WidgetHelpItem translated[sizeof(pdf_help) / sizeof(pdf_help[0])];
    int n = (int)(sizeof(pdf_help) / sizeof(pdf_help[0]));
    for (int i = 0; i < n; ++i) {
        translated[i] = pdf_help[i];
        if (translated[i].key) translated[i].key = T(translated[i].key);
        if (translated[i].text) translated[i].text = T(translated[i].text);
    }
    widgets_help(&a->widgets, T("PDF のキー"), translated, n);
}
static void pdf_frame_keys(Cells *cells, int col, const FrameKey *keys, int n)
{
    FrameKey translated[10];
    for (int i = 0; i < n; ++i)
        translated[i] = (FrameKey){ T(keys[i].key), T(keys[i].name) };
    frame_keys(cells, col, translated, n);
}
static int pdf_key(Screen *s, App *a, const Key *k)
{
    PdfView *v = s->self;
    int ctrl = (k->mods & MOD_CTRL) != 0, alt = (k->mods & MOD_ALT) != 0;
    if (v->input == INPUT_PASSWORD) return password_key(v, a, k);
    if (v->input == INPUT_NOTE) {
        int r = doc_input_key(&v->note, k);
        if (v->note.failed) { app_notice(a, T("メモリが足りません")); r = DOC_INPUT_CANCEL; }
        if (r != DOC_INPUT_MORE) finish_note(v, a, r == DOC_INPUT_ACCEPT);
        return 0;
    }
    if (v->input) {
        int input = v->input;
        WidgetResult r = widgets_key(&a->widgets, k);
        if (r != WIDGET_ACCEPT && r != WIDGET_CANCEL) return 0;
        v->input = INPUT_NONE;
        if (input == INPUT_DELETE) {
            if (r == WIDGET_ACCEPT) delete_target(v, a); else drop_target(v);
            return 0;
        }
        if (r == WIDGET_CANCEL) return 0;
        if (input == INPUT_PAGE) {
            char *end; long n = strtol(a->widgets.text, &end, 10);
            while (*end == ' ') end++;
            if (end == a->widgets.text || *end || n < 1 || n > v->count) app_notice(a, T("ページ番号が違います"));
            else go_page(v, (int)n - 1);
        } else if (input == INPUT_SEARCH) {
            snprintf(v->query, sizeof(v->query), "%s", a->widgets.text);
            v->hit_page = -1; v->hit_count = 0; find(v, a, 1);
        } else if (input == INPUT_QUIT) {
            if (a->widgets.selected == 1) return 1;
            if (a->widgets.selected == 0) return save(v, a) ? 0 : 1;
        } else if (input == INPUT_TOOLS) {
            if (a->widgets.selected == 0 && save(v, a)) return 0;
            if (a->widgets.selected <= 1) open_tools(v, a);
        }
        return 0;
    }
    if (k->key == KCHAR && ctrl && !alt && (k->ch == 's' || k->ch == 'S')) { save(v, a); return 0; }
    if (k->key == KCHAR && alt && !ctrl && (k->ch == 'p' || k->ch == 'P')) {
        if (v->dirty) ask_save(v, a, INPUT_TOOLS, T("書き込みを保存してから PDF のメニューを開きますか？"), T("保存して開く(S)"), T("保存せずに開く(N)"));
        else open_tools(v, a);
        return 0;
    }
    if (ctrl || alt) return 0;
    int quit = k->key == KESC || (k->key == KCHAR && k->ch == 'q');
    if (v->writing) {
        int ready = v->text_page == v->page && v->list_page == v->page;
        int shift = (k->mods & MOD_SHIFT) != 0;
        /* A key before the tick (after a save, or a page key): the page and
         * its text now, so that no key is lost. */
        int needs_text = k->key == KLEFT || k->key == KRIGHT || k->key == KUP || k->key == KDOWN || k->key == KHOME ||
                         k->key == KEND || k->key == KDEL || k->key == KRET || k->key == KTAB ||
                         (k->key == KCHAR && k->ch < 128 && strchr("hustd", (int)k->ch) && k->ch);
        if (!ready && needs_text) {
            if (v->pending) { v->pending = 0; render(v, a); }
            if (v->text_page != v->page && v->list_page == v->page && !build_text(v, a)) follow(v);
            ready = v->writing && v->text_page == v->page && v->list_page == v->page;
        }
        if (k->key == KESC && v->anchor >= 0) { v->anchor = -1; return 0; }
        if (quit || (k->key == KCHAR && k->ch == 'e')) { leave_writing(v); return 0; }
        switch (k->key) {
        case KLEFT: case KRIGHT: case KUP: case KDOWN: case KHOME: case KEND:
            if (ready) move_cursor(v, k->key, shift);
            return 0;
        case KDEL: if (ready) ask_delete(v, a); return 0;
        case KRET: if (ready) read_note(v, a); return 0;
        case KTAB: if (ready) next_annot(v, a); return 0;
        default: break;
        }
        if (k->key == KCHAR) switch (k->ch) {
        case 'h': if (ready) add_markup(v, a, PDF_ANNOT_HIGHLIGHT); return 0;
        case 'u': if (ready) add_markup(v, a, PDF_ANNOT_UNDERLINE); return 0;
        case 's': if (ready) add_markup(v, a, PDF_ANNOT_STRIKE_OUT); return 0;
        case 't': if (ready) begin_note(v, a); return 0;
        case 'd': if (ready) ask_delete(v, a); return 0;
        case '?': show_pdf_help(a); return 0;
        default: break;
        }
    } else if (quit) {
        if (!v->dirty) return 1;
        ask_save(v, a, INPUT_QUIT, T("書き込みを保存しますか？"), T("保存して戻る(S)"), T("保存せずに戻る(N)"));
        return 0;
    }
    switch (k->key) {
    case KPGDN: go_page(v, v->page + 1); return 0;
    case KPGUP: case KBS: go_page(v, v->page - 1); return 0;
    case KHOME: go_page(v, 0); return 0;
    case KEND: go_page(v, v->count - 1); return 0;
    case KLEFT: pan(v, -64, 0); return 0;
    case KRIGHT: pan(v, 64, 0); return 0;
    case KUP: scroll_or_turn(v, 0); return 0;
    case KDOWN: scroll_or_turn(v, 1); return 0;
    case KCHAR: break;
    default: return 0;
    }
    switch (k->ch) {
    case ' ': go_page(v, v->page + 1); break;
    case 'w': if (v->mode != MODE_WIDTH) { v->mode = MODE_WIDTH; fit_scale(v); view_top(v); request(v); } break;
    case 'f': if (v->mode != MODE_FIT) { v->mode = MODE_FIT; request(v); } break;
    case '+': case '=': zoom(v, 1.25f); break;
    case '-': zoom(v, 1 / 1.25f); break;
    case 'g': widgets_input(&a->widgets, T("ページ番号"), "", NULL); v->input = INPUT_PAGE; break;
    case '/': widgets_input(&a->widgets, T("検索"), v->query, NULL); v->input = INPUT_SEARCH; break;
    case 'n': find(v, a, 0); break;
    case 'e': enter_writing(v, a); break;
    case 'h': case '?': show_pdf_help(a); break;
    }
    return 0;
}

/* A two-dot frame of colour around x0..x1, y0..y1 (canvas pixels). */
static void frame(Canvas *c, int x0, int y0, int x1, int y1, const unsigned char rgb[4])
{
    for (int t = 0; t < 2; t++) {
        int ax = x0 - 2 + t, ay = y0 - 2 + t, bx = x1 + 1 - t, by = y1 + 1 - t;
        for (int x = ax; x <= bx; x++) for (int e = 0; e < 2; e++) {
            int y = e ? by : ay;
            if (x >= 0 && x < VIEW_W && y >= VIEW_TOP && y < VIEW_TOP + VIEW_H) memcpy(CANVAS_PX(c, VIEW_LEFT + x, y), rgb, 4);
        }
        for (int y = ay; y <= by; y++) for (int e = 0; e < 2; e++) {
            int x = e ? bx : ax;
            if (x >= 0 && x < VIEW_W && y >= VIEW_TOP && y < VIEW_TOP + VIEW_H) memcpy(CANVAS_PX(c, VIEW_LEFT + x, y), rgb, 4);
        }
    }
}

/* Page space to the canvas (x: from VIEW_LEFT, y: absolute) as drawn. */
static void to_view(const PdfView *v, fz_rect r, int *x0, int *y0, int *x1, int *y1)
{
    float sc = v->view_scale;
    *x0 = (int)floorf(r.x0 * sc) - v->view_x; *y0 = VIEW_TOP + (int)floorf(r.y0 * sc) - v->view_y;
    *x1 = (int)ceilf(r.x1 * sc) - v->view_x; *y1 = VIEW_TOP + (int)ceilf(r.y1 * sc) - v->view_y;
}

/* The selection: a light blue over the characters' boxes. */
static void tint(Canvas *c, int x0, int y0, int x1, int y1)
{
    for (int y = y0 < VIEW_TOP ? VIEW_TOP : y0; y < y1 && y < VIEW_TOP + VIEW_H; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < VIEW_W; x++) {
            unsigned char *p = CANVAS_PX(c, VIEW_LEFT + x, y);
            p[0] = (unsigned char)((p[0] * 160 + 40 * 95) / 255);
            p[1] = (unsigned char)((p[1] * 160 + 140 * 95) / 255);
            p[2] = (unsigned char)((p[2] * 160 + 255 * 95) / 255);
        }
}

static void draw_writing(PdfView *v, Canvas *c)
{
    static const unsigned char cursor[4] = { 0xe6, 0x00, 0x78, 0xff };
    int x0, y0, x1, y1, lo, hi;
    if (selection(v, &lo, &hi))
        for (int i = lo; i <= hi; i++) { to_view(v, v->chars[i].r, &x0, &y0, &x1, &y1); tint(c, x0, y0, x1, y1); }
    to_view(v, cursor_rect(v), &x0, &y0, &x1, &y1);
    frame(c, x0, y0, x1, y1, cursor);
}

/* Row 23 in the writing mode: the annotation under the cursor and its text,
 * else the selection or where the cursor is. */
static void writing_line(PdfView *v, Cells *cells)
{
    char line[512];
    pdf_annot *on = annot_at_cursor(v);
    if (on) {
        const char *s = contents_of(v->ctx, on);
        size_t n = (size_t)snprintf(line, sizeof(line), "%s%s", annot_name(pdf_annot_type(v->ctx, on)), *s ? ": " : "");
        for (; *s && n < sizeof(line) - 1; s++) line[n++] = *s == '\n' || *s == '\r' || *s == '\t' ? ' ' : *s;
        /* Cut short: not inside a character. */
        if (((unsigned char)*s & 0xc0) == 0x80) {
            while (n && ((unsigned char)line[n - 1] & 0xc0) == 0x80) n--;
            if (n) n--;   /* its lead byte */
        }
        line[n] = 0;
        cells_text(cells, 2, FD_LINE, line, WHITE, BG, FD_COLS - 2);
        return;
    }
    int lo, hi;
    if (!v->nchars) snprintf(line, sizeof(line), "%s", T("書き込み: このページに文字はありません（付箋は貼れます）"));
    else if (selection(v, &lo, &hi)) snprintf(line, sizeof(line), T("書き込み: %d 字を選んでいます"), hi - lo + 1);
    else snprintf(line, sizeof(line), T("書き込み: 字 %d/%d"), v->cur + 1, v->nchars);
    cells_text(cells, 2, FD_LINE, line, CYAN, BG, FD_COLS - 2);
}

/* The input method's candidate list is drawn above row 23, over the page:
 * its cells are copied from the cells' picture (scratch). */
static void copy_candidates(Canvas *c, const Canvas *scratch, const Cells *cells)
{
    for (int row = 1; row < FD_LINE; row++)
        for (int col = 0; col < FD_COLS; col++) {
            if (cells->data[row][col].bg == BG) continue;
            for (int y = row * 24; y < row * 24 + 24; y++)
                memcpy(CANVAS_PX(c, CANVAS_GRID_X + col * 12, y), CANVAS_PX(scratch, CANVAS_GRID_X + col * 12, y), 12 * 4);
        }
}

static void pdf_draw(Screen *s, App *a, Canvas *c, Cells *cells)
{
    PdfView *v = s->self; fz_context *ctx = v->ctx;
    static const unsigned char yellow[4] = { 0xff, 0xff, 0x00, 0xff };
    canvas_clear(c, 0x000055);
    /* The screen changed size (an X window resized): drawn again at the tick. */
    if (v->pix && (v->view_w != VIEW_W || v->view_h != VIEW_H)) request(v);
    if (v->pix) {
        int px = fz_pixmap_x(ctx, v->pix), py = fz_pixmap_y(ctx, v->pix);
        int w = fz_pixmap_width(ctx, v->pix), h = fz_pixmap_height(ctx, v->pix);
        int stride = fz_pixmap_stride(ctx, v->pix); const unsigned char *sm = fz_pixmap_samples(ctx, v->pix);
        int ox = px - v->view_x, oy = VIEW_TOP + py - v->view_y;
        for (int y = 0; y < h; y++) {
            int cy = oy + y; if (cy < VIEW_TOP || cy >= VIEW_TOP + VIEW_H) continue;
            const unsigned char *row = sm + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                int cx = ox + x; if (cx < 0 || cx >= VIEW_W) continue;
                unsigned char *p = CANVAS_PX(c, VIEW_LEFT + cx, cy);
                memcpy(p, row + x * 3, 3); p[3] = 0xff;
            }
        }
        if (v->hit_page == v->list_page && v->hit_page == v->page && v->hit_count && !v->writing) {
            fz_rect r = hit_rect(v); int x0, y0, x1, y1;
            to_view(v, r, &x0, &y0, &x1, &y1);
            frame(c, x0, y0, x1, y1, yellow);
        }
        if (v->writing && v->text_page == v->page && v->list_page == v->page) draw_writing(v, c);
    }
    /* Revision 1 item 29: the page in the screen's frame (cells.h), the name
     * in its top line, the page, the zoom and the mode in its bottom line. */
    cells_init(cells); frame_open(cells);
    const char *mode = v->mode == MODE_FIT ? T("全体") : v->mode == MODE_WIDTH ? T("幅") : "";
    char where[96];
    if (v->locked) snprintf(where, sizeof(where), "%s", T("パスワード"));
    else snprintf(where, sizeof(where), T("%s%s%.0f%%  ページ %d/%d"), mode, *mode ? " " : "", v->scale * 100, v->page + 1, v->count);
    int room = FRAME_STATUS_END - cells_width(where) - 2 - FRAME_TITLE_COL - (v->dirty ? 9 : 0);
    int limit = FRAME_STATUS_END - FRAME_TITLE_COL - 9;
    frame_title(cells, FRAME_TITLE_COL, v->base, room < limit ? (room > 8 ? room : 8) : limit, v->dirty ? T("変更あり") : NULL, MAGENTA);
    frame_status(cells, FRAME_STATUS_END, where, 40);
    int line = 0;
    if (v->locked) {
        static const FrameKey keys[] = { { "RET", N_("決定") }, { "Esc", N_("戻る") } };
        pdf_frame_keys(cells, 2, keys, 2);
    } else if (v->input == INPUT_NOTE) {
        static const FrameKey keys[] = { { "RET", N_("貼る") }, { "Esc", N_("やめる") }, { "Alt+Space", N_("日本語") } };
        pdf_frame_keys(cells, 2, keys, 3);
        doc_input_draw(&v->note, cells); line = 1;
        if (v->ime) {
            char label[32];
            snprintf(label, sizeof(label), "[%s]", ime_label(v->ime));
            cells_text(cells, 52 * FD_COLS / 80, FD_BAND, label, BG, CYAN, 8);
        }
    } else if (v->writing) {
        static const FrameKey keys[] = { { N_("S+矢印"), N_("選択") }, { "h", N_("ハイライト") }, { "u", N_("下線") }, { "s", N_("取消線") },
                                         { "t", N_("付箋") }, { "d", N_("消す") }, { "C-s", N_("保存") }, { "e", N_("戻る") } };
        pdf_frame_keys(cells, 2, keys, 8);
        if (v->text_page == v->page) { writing_line(v, cells); line = 1; }
    } else {
        static const FrameKey keys[] = { { "w", N_("幅") }, { "f", N_("全体") }, { "SPC", N_("次") }, { "BS", N_("前") }, { "g", N_("ページ") },
                                         { "+-", N_("拡縮") }, { "/", N_("検索") }, { "n", N_("次") }, { "e", N_("書込") }, { "q", N_("戻る") } };
        pdf_frame_keys(cells, 2, keys, 10);
    }
    if (v->pending) cells_text(cells, FD_COLS - 7, FD_BAND, T("描画中"), BG, YELLOW, 7);
    widgets_draw(&a->widgets, cells);
    raster_bands(c, v->bands, cells, line || widgets_row23(&a->widgets));
    if (v->input == INPUT_NOTE) copy_candidates(c, v->bands, cells);
    raster_boxes(c, cells);
    c->full_dirty = 1;
}

/* Rendering is deferred to the tick after the key, so the frame drawn in
 * between shows 描画中. Keys that arrive before the tick only move the
 * target: a burst of page keys costs one render, not one each. The writing
 * mode's text is read at the same tick. */
static void pdf_tick(Screen *s, App *a, double now)
{
    (void)now; PdfView *v = s->self;
    if (!v->pending || v->locked) return;
    v->pending = 0; render(v, a); a->changed = 1;
    if (v->writing && v->text_page != v->page && v->list_page == v->page) {
        if (!build_text(v, a)) follow(v);
        if (v->pending) { v->pending = 0; render(v, a); }
    }
}

static void pdf_destroy(Screen *s)
{
    PdfView *v = s->self;
    doc_input_close(&v->note);
    if (v->ime) ime_close(v->ime);
    if (v->ctx) { drop_text(v); drop_page(v); fz_drop_document(v->ctx, v->doc); fz_drop_context(v->ctx); }
    memset(v->password, 0, sizeof(v->password));
    free(v->path); free(v->dir); free(v->bands); free(v);
}

Screen *pdfview_create(App *a, const char *path)
{
    PdfView *v = calloc(1, sizeof(*v));
    /* Item 39: the directory and the name, for the PDF menu's selection. */
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (!v || !path || !(v->path = strdup(path)) || !(v->bands = malloc(sizeof(Canvas))) ||
        !(v->dir = slash ? strndup(path, slash == path ? 1 : (size_t)(slash - path)) : strdup(".")) ||
        !(v->ctx = fz_new_context(NULL, NULL, PDF_STORE))) {
        app_notice(a, T("メモリが足りません"));
        if (v) { free(v->path); free(v->dir); free(v->bands); free(v); }
        return NULL;
    }
    v->base = slash ? v->path + (slash - path) + 1 : v->path;
    v->names[0] = v->base;
    v->screen = (Screen){ v, pdf_key, pdf_draw, 1, pdf_tick, pdf_destroy };
    fz_context *ctx = v->ctx; const char *volatile error = NULL;
    fz_set_warning_callback(ctx, quiet, NULL); fz_set_error_callback(ctx, quiet, NULL);
    v->list_page = v->hit_page = v->text_page = v->reload_page = -1; v->anchor = v->keep_cur = -1;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        v->doc = fz_open_document(ctx, path);
        if (fz_needs_password(ctx, v->doc)) v->locked = 1;
        else if ((v->count = fz_count_pages(ctx, v->doc)) < 1) error = T("PDF を開けません");
    }
    fz_catch(ctx) error = T("PDF を開けません");
    if (error) { app_notice(a, error); pdf_destroy(&v->screen); return NULL; }
    v->mode = MODE_FIT; v->scale = 1;
    /* Item 40: a PDF with a user password asks for it (shown as '*'). */
    if (v->locked) { widgets_input(&a->widgets, T("パスワード"), "", NULL); v->input = INPUT_PASSWORD; }
    else request(v);
    return &v->screen;
}
#endif
