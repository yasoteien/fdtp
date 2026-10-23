// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 6 slide viewer (docs/FDTP_VIEWERS.md): pptx.
 * The shown slide is drawn once into an RGB buffer of the fitted size
 * (slide_render.c); moving to another slide draws that one in its place,
 * so only one slide's pixels are ever held.
 * Revision 1 item 41: the text of the slides can be edited as in the docx
 * view. e (or RET) chooses a paragraph of the slide (the text boxes,
 * shapes and table cells; ↑↓ as the docx view's cursor, the paragraph
 * framed on the slide), RET edits it on row 23 with the docx input line
 * (doc_input.h), C-s saves (only the edited slides' parts are rewritten,
 * NAME.bak is made before the first save), C-z / C-y undo and redo one
 * paragraph's edit (UNDO_LEVELS steps, doc_undo.h). Formats, pictures and
 * places are kept; the paragraph's text takes its first run's format
 * (slide_edit.c).
 * Revision 1 item 44: F5 presents the current slide over the whole canvas;
 * navigation is read-only, Esc/F5 returns to the current slide in view mode.
 * Window control is supplied by the platform adapter through App callbacks. */
#include "slideview.h"
#include "slide_model.h"
#include "i18n.h"
#include "doc_input.h"
#include "doc_undo.h"
#include "app.h"
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARK 0xFF00FFu      /* the frame round the chosen paragraph (FD's magenta) */

typedef enum { MODE_VIEW, MODE_SELECT, MODE_EDIT, MODE_GOTO, MODE_QUIT } Mode;
/* One undo step: a splice of one slide's XML. */
typedef struct { int slide; UndoSplice *splice; } SlideStep;

/* VIEW_W VIEW_H VIEW_TOP: raster.h (1024 x 552 at y 24 on the Pomera). */
typedef struct {
    Screen screen;
    char *path;
    Canvas *bands;
    SlideDeck *deck;
    int count, index, shown;
    int slideshow, was_fullscreen;
    void (*set_fullscreen)(int);
    Mode mode;
    int w, h;
    unsigned char *rgb;
    /* The paragraphs of slide list_slide (its XML as it is now, parsed). */
    char *xml;
    size_t xml_len;
    SPart part;
    SPara **paras;
    int npara, list_slide, cur;
    DocInput input;
    Ime *ime;
    UndoHist undo;
    int saves;
} SlideView;

static void show(SlideView *v, App *a, int index)
{
    if (index < 0) index = 0;
    if (index >= v->count) index = v->count - 1;
    if (index == v->shown) return;
    v->index = v->shown = index;
    char err[256] = "", warn[256] = "";
    if (slide_render(v->deck, index, v->rgb, v->w, v->h, err, sizeof(err), warn, sizeof(warn))) app_notice(a, err);
    else if (warn[0]) app_notice(a, warn);
}
static void redraw(SlideView *v, App *a)
{
    v->shown = -1;
    show(v, a, v->index);
}
/* The slide fitted to the view; again after the screen changed size (an X
 * window resized). 1: a new size (drawn again), 0: the same, -1: no memory. */
static int fit(SlideView *v, App *a)
{
    long long cx, cy;
    slide_size(v->deck, &cx, &cy);
    int area_w = v->slideshow ? CANVAS_W : VIEW_W;
    int area_h = v->slideshow ? CANVAS_H : VIEW_H;
    double scale = fmin((double)area_w / cx, (double)area_h / cy);
    int w = (int)fmin(area_w, fmax(1, floor(cx * scale + .5)));
    int h = (int)fmin(area_h, fmax(1, floor(cy * scale + .5)));
    if (v->rgb && w == v->w && h == v->h) return 0;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return -1;
    free(v->rgb);
    v->rgb = rgb; v->w = w; v->h = h;
    if (v->shown >= 0) { int index = v->index; v->shown = -1; show(v, a, index); }
    return 1;
}

/* ---- the paragraphs (revision 1 item 41) ---- */
static void drop_paras(SlideView *v)
{
    free(v->paras);
    v->paras = NULL;
    v->npara = 0;
    spart_free(&v->part);
    free(v->xml);
    v->xml = NULL;
    v->xml_len = 0;
    v->list_slide = -1;
}
/* The paragraphs of the shown slide; the chosen one stays (clamped). */
static int load_paras(SlideView *v, App *a)
{
    drop_paras(v);
    char err[256] = "";
    if (slide_get_xml(v->deck, v->index, &v->xml, &v->xml_len, err, sizeof(err))) { app_notice(a, err); return -1; }
    if (slide_edit_list(v->xml, v->xml_len, &v->part, &v->paras, &v->npara)) {
        free(v->xml);
        v->xml = NULL;
        app_notice(a, T("このスライドの文字を読めません"));
        return -1;
    }
    v->list_slide = v->index;
    if (v->cur >= v->npara) v->cur = v->npara - 1;
    if (v->cur < 0) v->cur = 0;
    return 0;
}
static void choose(SlideView *v, App *a)
{
    v->cur = 0;
    if (load_paras(v, a)) return;
    if (!v->npara) { app_notice(a, T("このスライドに直せる文字はありません")); return; }
    v->mode = MODE_SELECT;
}
/* To another slide while choosing: its paragraphs, from the first. */
static void go(SlideView *v, App *a, int index)
{
    int was = v->index;
    show(v, a, index);
    if (v->mode != MODE_SELECT || v->index == was) return;
    v->cur = 0;
    if (load_paras(v, a) || !v->npara) v->mode = MODE_VIEW;
}

static void step_free(void *p)
{
    SlideStep *s = p;
    if (!s) return;
    undo_splice_free(s->splice);
    free(s);
}
static void edit_begin(SlideView *v, App *a)
{
    if (v->cur < 0 || v->cur >= v->npara) return;
    const SPara *p = v->paras[v->cur];
    if (p->src.refuse) { app_notice(a, T(p->src.refuse)); return; }
    char *text = slide_edit_text(p);
    if (!v->ime) v->ime = ime_open();
    if (!text || doc_input_open(&v->input, v->ime, T("段落"), text)) {
        doc_input_close(&v->input);
        free(text);
        app_notice(a, T("メモリが足りません"));
        return;
    }
    free(text);
    v->mode = MODE_EDIT;
}
static void edit_finish(SlideView *v, App *a, int accepted)
{
    char *text = accepted ? doc_input_text(&v->input) : NULL;
    doc_input_close(&v->input);
    v->mode = MODE_SELECT;
    if (!accepted) return;
    if (!text) { app_notice(a, T("メモリが足りません")); return; }
    const SPara *p = v->paras[v->cur];
    char *old = slide_edit_text(p);
    int unchanged = old && !strcmp(old, text);
    free(old);
    if (unchanged) { free(text); return; }
    char err[256] = "";
    size_t n = 0;
    char *xml = slide_edit_rewrite(v->xml, v->xml_len, p, text, &n, err, sizeof(err));
    free(text);
    if (!xml) { app_notice(a, err); return; }
    int same = 0;
    UndoSplice *s = undo_splice_make((const unsigned char *)v->xml, v->xml_len, (const unsigned char *)xml, n, &same);
    if (same) { free(xml); return; }
    SlideStep *st = s ? malloc(sizeof(*st)) : NULL;
    if (st) { st->slide = v->index; st->splice = s; }
    else undo_splice_free(s);
    slide_set_xml(v->deck, v->index, xml, n);
    UndoStep step = { st, v->cur, v->cur, v->saves };
    if (!st) { undo_lost(&v->undo); app_notice(a, T("メモリが足りないため、取り消しの記録を消しました")); }
    else if (undo_push(&v->undo, a->settings.undo_levels, &step)) app_notice(a, T("メモリが足りないため、取り消しの記録を消しました"));
    redraw(v, a);
    load_paras(v, a);
}
static void undo_redo(SlideView *v, App *a, int redo)
{
    UndoStep *u = redo ? undo_next_redo(&v->undo) : undo_next_undo(&v->undo);
    if (!u) { app_notice(a, redo ? T("やり直す物はありません") : T("取り消す物はありません")); return; }
    const SlideStep *st = u->data;
    char err[256] = "", message[320];
    char *xml = NULL;
    size_t n = 0, m = 0;
    unsigned char *next = NULL;
    if (!slide_get_xml(v->deck, st->slide, &xml, &n, err, sizeof(err)))
        next = undo_splice_apply(st->splice, !redo, (const unsigned char *)xml, n, &m);
    free(xml);
    if (!next) {
        undo_clear(&v->undo);
        snprintf(message, sizeof(message), "%s: %s", redo ? T("やり直せません") : T("取り消せません"), err[0] ? err : T("記録がスライドと合いません"));
        app_notice(a, message);
        return;
    }
    slide_set_xml(v->deck, st->slide, (char *)next, m);
    if (redo) undo_done_redo(&v->undo);
    else undo_done_undo(&v->undo);
    /* To that slide and paragraph, chosen. */
    v->index = st->slide;
    redraw(v, a);
    v->cur = (int)(redo ? u->after : u->before);
    if (!load_paras(v, a) && v->npara) v->mode = MODE_SELECT;
    snprintf(message, sizeof(message), T("%s（スライド %d の %d 段落目）"), redo ? T("やり直しました") : T("取り消しました"),
             st->slide + 1, v->cur + 1);
    app_notice(a, message);
}
static void save(SlideView *v, App *a)
{
    if (!undo_dirty(&v->undo)) { app_notice(a, T("変更はありません")); return; }
    char err[256] = "", message[320];
    int backed = 0;
    if (slide_save(v->deck, 1, &backed, err, sizeof(err))) {
        snprintf(message, sizeof(message), T("保存できません: %s"), err[0] ? err : T("書き込めません"));
        app_notice(a, message);
        return;
    }
    undo_saved(&v->undo);
    v->saves++;
    if (v->mode == MODE_SELECT) load_paras(v, a);
    app_notice(a, backed ? T("保存しました（元のファイルは .bak に残しました）") : T("保存しました"));
}

/* Restore even if the screen is destroyed while presenting. */
static void slideshow_end(SlideView *v)
{
    if (!v->slideshow) return;
    v->slideshow = 0;
    if (!v->was_fullscreen && v->set_fullscreen) v->set_fullscreen(0);
}
static void slideshow_begin(SlideView *v, App *a)
{
    v->mode = MODE_VIEW;
    v->was_fullscreen = a->is_fullscreen ? a->is_fullscreen() : 0;
    v->set_fullscreen = a->set_fullscreen;
    v->slideshow = 1;
    if (v->set_fullscreen) v->set_fullscreen(1);
}
/* ---- keys ---- */
static int common_key(SlideView *v, App *a, const Key *k)
{
    if (k->key != KCHAR || !(k->mods & MOD_CTRL) || (k->mods & MOD_ALT)) return 0;
    switch (k->ch) {
    case 's': case 'S': save(v, a); return 1;
    case 'z': case 'Z': undo_redo(v, a, 0); return 1;
    case 'y': case 'Y': undo_redo(v, a, 1); return 1;
    default: return 0;
    }
}
static int slide_key(Screen *s, App *a, const Key *k)
{
    SlideView *v = s->self;
    if (v->slideshow) {
        switch (k->key) {
        case KESC: case KF5: slideshow_end(v); break;
        case KRIGHT: case KDOWN: case KPGDN: case KRET: show(v, a, v->index + 1); break;
        case KLEFT: case KUP: case KPGUP: case KBS: show(v, a, v->index - 1); break;
        case KHOME: show(v, a, 0); break;
        case KEND: show(v, a, v->count - 1); break;
        case KCHAR:
            if (k->ch == ' ' && !(k->mods & (MOD_CTRL | MOD_ALT))) show(v, a, v->index + 1);
            break;
        default: break;
        }
        return 0;
    }
    if (k->key == KF5 && (v->mode == MODE_VIEW || v->mode == MODE_SELECT)) {
        slideshow_begin(v, a);
        return 0;
    }
    if (v->mode == MODE_EDIT) {
        int r = doc_input_key(&v->input, k);
        if (v->input.failed) { app_notice(a, T("メモリが足りません")); r = DOC_INPUT_CANCEL; }
        if (r != DOC_INPUT_MORE) edit_finish(v, a, r == DOC_INPUT_ACCEPT);
        return 0;
    }
    if (v->mode == MODE_GOTO || v->mode == MODE_QUIT) {
        WidgetResult r = widgets_key(&a->widgets, k);
        if (r != WIDGET_ACCEPT && r != WIDGET_CANCEL) return 0;
        Mode m = v->mode;
        v->mode = MODE_VIEW;
        if (r == WIDGET_CANCEL) return 0;
        if (m == MODE_QUIT) return 1;
        char *end;
        long n = strtol(a->widgets.text, &end, 10);
        while (*end == ' ') end++;
        if (end == a->widgets.text || *end || n < 1 || n > v->count) app_notice(a, T("スライド番号が違います"));
        else show(v, a, (int)n - 1);
        return 0;
    }
    if (common_key(v, a, k)) return 0;
    if (v->mode == MODE_SELECT) {
        switch (k->key) {
        case KESC: v->mode = MODE_VIEW; return 0;
        case KUP: if (v->cur > 0) v->cur--; return 0;
        case KDOWN: if (v->cur + 1 < v->npara) v->cur++; return 0;
        case KHOME: v->cur = 0; return 0;
        case KEND: v->cur = v->npara - 1; return 0;
        case KRET: edit_begin(v, a); return 0;
        case KPGDN: go(v, a, v->index + 1); return 0;
        case KPGUP: case KBS: go(v, a, v->index - 1); return 0;
        case KCHAR: break;
        default: return 0;
        }
        if (k->mods & (MOD_CTRL | MOD_ALT)) return 0;
        if (k->ch == 'e' || k->ch == 'E') edit_begin(v, a);
        else if (k->ch == 'q' || k->ch == 'Q') v->mode = MODE_VIEW;
        else if (k->ch == ' ') go(v, a, v->index + 1);
        return 0;
    }
    if (k->key == KESC || (k->key == KCHAR && k->ch == 'q' && !(k->mods & (MOD_CTRL | MOD_ALT)))) {
        if (undo_dirty(&v->undo)) { widgets_confirm(&a->widgets, T("保存せずに戻りますか？")); v->mode = MODE_QUIT; return 0; }
        return 1;
    }
    switch (k->key) {
    case KPGDN:
    case KDOWN:   /* revision 1 §3 14: ↑↓ are the previous and next slide */
        show(v, a, v->index + 1);
        return 0;
    case KPGUP:
    case KUP:
    case KBS:
        show(v, a, v->index - 1);
        return 0;
    case KHOME:
        show(v, a, 0);
        return 0;
    case KEND:
        show(v, a, v->count - 1);
        return 0;
    case KRET:
        choose(v, a);
        return 0;
    case KCHAR:
        break;
    default:
        return 0;
    }
    if (k->mods & (MOD_CTRL | MOD_ALT)) return 0;
    if (k->ch == ' ') show(v, a, v->index + 1);
    else if (k->ch == 'g') {
        widgets_input(&a->widgets, T("スライド番号"), "", NULL);
        v->mode = MODE_GOTO;
    } else if (k->ch == 'e' || k->ch == 'E') choose(v, a);
    return 0;
}

/* ---- drawing ---- */
static void mark_px(Canvas *c, int x, int y)
{
    unsigned char *p = CANVAS_PX(c, x, y);
    p[0] = (MARK >> 16) & 255; p[1] = (MARK >> 8) & 255; p[2] = MARK & 255; p[3] = 255;
}
/* A 2-dot frame round the chosen paragraph's place, inside the slide. */
static void mark_paragraph(SlideView *v, Canvas *c, int left, int top)
{
    if (v->list_slide != v->index || v->cur < 0 || v->cur >= v->npara) return;
    int n = 0;
    const SParaBox *b = slide_boxes(v->deck, &n);
    for (int i = 0; i < n; ++i) {
        if (b[i].gid != v->paras[v->cur]->gid) continue;
        int x0 = b[i].x0 - 3, y0 = b[i].y0 - 2, x1 = b[i].x1 + 2, y1 = b[i].y1 + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > v->w - 1) x1 = v->w - 1;
        if (y1 > v->h - 1) y1 = v->h - 1;
        for (int x = x0; x <= x1; ++x)
            for (int t = 0; t < 2; ++t) {
                if (y0 + t <= y1) mark_px(c, left + x, top + y0 + t);
                if (y1 - t >= y0) mark_px(c, left + x, top + y1 - t);
            }
        for (int y = y0; y <= y1; ++y)
            for (int t = 0; t < 2; ++t) {
                if (x0 + t <= x1) mark_px(c, left + x0 + t, top + y);
                if (x1 - t >= x0) mark_px(c, left + x1 - t, top + y);
            }
        return;
    }
}
/* Row 23 while choosing: which paragraph, and its text. */
static void paragraph_line(SlideView *v, Cells *cells)
{
    if (v->cur < 0 || v->cur >= v->npara) return;
    const SPara *p = v->paras[v->cur];
    char info[96];
    snprintf(info, sizeof(info), T("段落 %d/%d%s"), v->cur + 1, v->npara, p->src.refuse ? T("  （直せない段落）") : "");
    int x = 2 + cells_text(cells, 2, FD_LINE, info, CYAN, BG, FD_COLS - 2);
    char *text = slide_edit_text(p);
    if (!text) return;
    for (char *q = text; *q; ++q) if (*q == '\n' || *q == '\t') *q = ' ';
    if (x + 2 < FD_COLS) cells_text(cells, x + 2, FD_LINE, text, WHITE, BG, FD_COLS - x - 2);
    free(text);
}
/* Translate before frame_keys measures and draws the band. */
/* C-z undoes an edit. The shared catalog has 取消 as Cancel (the directory
 * picker's Esc), so English names the key here; Japanese keeps 取消. */
static const char *undo_label(void)
{
    return i18n_lang() == I18N_EN ? "Undo" : N_("取消");
}

static void slide_frame_keys(Cells *cells, int col, const FrameKey *keys, int n)
{
    FrameKey translated[8];
    for (int i = 0; i < n; ++i) {
        translated[i] = keys[i];
        translated[i].name = T(keys[i].name);
    }
    frame_keys(cells, col, translated, n);
}
static void slide_draw(Screen *s, App *a, Canvas *c, Cells *cells)
{
    SlideView *v = s->self;
    canvas_clear(c, v->slideshow ? 0 : 0x000055);
    if (fit(v, a) < 0) {
        cells_init(cells);
        app_notice(a, T("メモリが足りません"));
        c->full_dirty = 1;
        return;
    }
    int left = v->slideshow ? (c->w - v->w) / 2 : VIEW_LEFT + (VIEW_W - v->w) / 2;
    int top = v->slideshow ? (c->h - v->h) / 2 : VIEW_TOP + (VIEW_H - v->h) / 2;
    for (int y = 0; y < v->h; ++y) {
        unsigned char *row = CANVAS_PX(c, left, top + y);
        const unsigned char *src = v->rgb + (size_t)y * v->w * 3;
        for (int x = 0; x < v->w; ++x) {
            row[x * 4] = src[x * 3];
            row[x * 4 + 1] = src[x * 3 + 1];
            row[x * 4 + 2] = src[x * 3 + 2];
            row[x * 4 + 3] = 255;
        }
    }
    if (v->slideshow) {
        cells_init(cells);
        c->full_dirty = 1;
        return;
    }
    int choosing = v->mode == MODE_SELECT || v->mode == MODE_EDIT;
    if (choosing) mark_paragraph(v, c, left, top);
    /* Revision 1 item 29: the slide in the screen's frame (cells.h), the
     * name in its top line, the slide number in its bottom line. */
    cells_init(cells);
    frame_open(cells);
    const char *base = strrchr(v->path, '/');
    base = base ? base + 1 : v->path;
    char where[48];
    snprintf(where, sizeof(where), T("スライド %d/%d"), v->index + 1, v->count);
    frame_title(cells, FRAME_TITLE_COL, base, FRAME_STATUS_END - FRAME_TITLE_COL - 24,
                undo_dirty(&v->undo) ? T("変更あり") : NULL, MAGENTA);
    frame_status(cells, FRAME_STATUS_END, where, 30);
    if (v->mode == MODE_EDIT) {
        doc_input_draw(&v->input, cells);
        static const FrameKey keys[] = { { "RET", N_("決定") }, { "Esc", N_("やめる") } };
        slide_frame_keys(cells, 2, keys, 2);
        if (v->ime) {
            char label[32];
            snprintf(label, sizeof(label), "[%s]", ime_label(v->ime));
            cells_text(cells, 52 * FD_COLS / 80, FD_BAND, label, BG, CYAN, 8);
        }
    } else if (v->mode == MODE_SELECT) {
        paragraph_line(v, cells);
        const FrameKey keys[] = { { "↑↓", N_("段落") }, { "RET", N_("直す") }, { "C-s", N_("保存") }, { "C-z", undo_label() },
                                  { "C-y", N_("やり直し") }, { "PgDn", N_("次") }, { "Esc", N_("戻る") } };
        slide_frame_keys(cells, 2, keys, 7);
    } else {
        const FrameKey keys[] = { { "SPC", N_("次") }, { "BS", N_("前") }, { "g", N_("番号") }, { "e", N_("直す") },
                                  { "C-s", N_("保存") }, { "C-z", undo_label() }, { "C-y", N_("やり直し") }, { "q", N_("戻る") } };
        slide_frame_keys(cells, 2, keys, 8);
    }
    widgets_draw(&a->widgets, cells);
    raster_bands(c, v->bands, cells, widgets_row23(&a->widgets) || choosing);
    /* The input method's candidates, above row 23 (doc_input_draw). */
    ImeCandidates cand = { 0 };
    if (v->mode == MODE_EDIT && v->ime) ime_candidates(v->ime, &cand);
    if (cand.active && cand.shown > 0) {
        int rows = cand.shown > FD_ROWS - 3 ? FD_ROWS - 3 : cand.shown;
        size_t bytes = (size_t)c->w * 4 * 24;
        for (int y = FD_LINE - rows; y < FD_LINE; ++y) memcpy(CANVAS_ROW(c, y * 24), CANVAS_ROW(v->bands, y * 24), bytes);
    }
    raster_boxes(c, cells);
    c->full_dirty = 1;
}
static void slide_destroy(Screen *s)
{
    SlideView *v = s->self;
    slideshow_end(v);
    drop_paras(v);
    doc_input_close(&v->input);
    if (v->ime) ime_close(v->ime);
    undo_free(&v->undo);
    slide_close(v->deck);
    free(v->rgb);
    free(v->path);
    free(v->bands);
    free(v);
}
Screen *slideview_create(App *a, const char *path)
{
    SlideView *v = calloc(1, sizeof(*v));
    if (!v || !path || !(v->path = strdup(path)) || !(v->bands = malloc(sizeof(Canvas)))) {
        app_notice(a, T("メモリが足りません"));
        if (v) { free(v->path); free(v); }
        return NULL;
    }
    v->list_slide = -1;
    undo_init(&v->undo, step_free);
    v->screen = (Screen){ v, slide_key, slide_draw, 1, NULL, slide_destroy };
    char err[256] = "";
    v->deck = slide_open(path, err, sizeof(err));
    if (!v->deck) {
        app_notice(a, err[0] ? err : T("pptx を開けません"));
        slide_destroy(&v->screen);
        return NULL;
    }
    v->count = slide_count(v->deck);
    v->shown = -1;
    if (fit(v, a) < 0) {
        app_notice(a, T("メモリが足りません"));
        slide_destroy(&v->screen);
        return NULL;
    }
    show(v, a, 0);
    return &v->screen;
}
