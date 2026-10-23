// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 5: the docx as screen lines. Paragraphs wrap at the width; page and
 * section breaks are separator lines; tables are drawn with the grid's
 * ruled-line characters (the Shinonome box drawing glyphs are two cells
 * wide, so every border and column width is a multiple of two). */
#include "i18n.h"
#include "doc_model.h"
#include "cells.h"
#include "font24.h"
#include <stdlib.h>
#include <string.h>

#define TAB_STOP 4

static int add_line(DocLayout *l, size_t item, int kind, size_t off, size_t len)
{
    if (l->failed) return -1;
    if (l->n + 1 > l->cap) {
        size_t c = l->cap ? l->cap * 2 : 256;
        DocLine *p = realloc(l->lines, c * sizeof(*p));
        if (!p) { l->failed = 1; return -1; }
        l->lines = p;
        l->cap = c;
    }
    l->lines[l->n++] = (DocLine){item, kind, off, len};
    return 0;
}
static DocText *add_tline(DocLayout *l, size_t item)
{
    if (l->failed) return NULL;
    if (l->ntl + 1 > l->ctl) {
        size_t c = l->ctl ? l->ctl * 2 : 64;
        DocText *p = realloc(l->tlines, c * sizeof(*p));
        if (!p) { l->failed = 1; return NULL; }
        l->tlines = p;
        l->ctl = c;
    }
    DocText *t = &l->tlines[l->ntl];
    memset(t, 0, sizeof(*t));
    if (add_line(l, item, DOC_LINE_TABLE, l->ntl, 0)) return NULL;
    l->ntl++;
    return t;
}
void doc_layout_free(DocLayout *l)
{
    for (size_t i = 0; i < l->ntl; ++i) doc_text_free(&l->tlines[i]);
    free(l->tlines);
    free(l->lines);
    memset(l, 0, sizeof(*l));
}

static int glyph_width(uint32_t c, int col)
{
    if (c == '\t') return TAB_STOP - col % TAB_STOP;
    if (c < 0x20 || c == 0x7f) return 0;
    if (c == 0x2011) return 1;
    return font24_width(c);
}
/* Splits [s, e) of t at '\n' and at the width; calls back per line. */
typedef struct { size_t off, len; } Span;
typedef struct { Span *s; size_t n, cap; int failed; } Spans;
static void span_add(Spans *v, size_t off, size_t len)
{
    if (v->failed) return;
    if (v->n + 1 > v->cap) {
        size_t c = v->cap ? v->cap * 2 : 16;
        Span *p = realloc(v->s, c * sizeof(*p));
        if (!p) { v->failed = 1; return; }
        v->s = p;
        v->cap = c;
    }
    v->s[v->n++] = (Span){off, len};
}
static void wrap(const DocText *t, size_t s, size_t e, int width, Spans *out)
{
    size_t start = s;
    int col = 0;
    for (size_t k = s; k < e; ++k) {
        uint32_t c = t->code[k];
        if (c == '\n') {
            span_add(out, start, k - start);
            start = k + 1;
            col = 0;
            continue;
        }
        int w = glyph_width(c, col);
        if (col + w > width && k > start) {
            span_add(out, start, k - start);
            start = k;
            col = 0;
            w = glyph_width(c, col);
        }
        col += w;
    }
    span_add(out, start, e - start);
}
static int text_width(const DocText *t, size_t s, size_t e)
{
    int col = 0;
    for (size_t k = s; k < e; ++k) col += glyph_width(t->code[k], col);
    return col;
}

static void paragraph(DocLayout *l, const DocItem *it, size_t index, int width)
{
    if (it->sep_before) add_line(l, index, DOC_LINE_SEP, 0, 0);
    const DocText *t = &it->disp;
    size_t segments = 1;
    for (size_t k = 0; k < t->n; ++k) if (t->code[k] == '\f') segments++;
    size_t s = 0, seg = 0;
    for (;;) {
        size_t e = s;
        while (e < t->n && t->code[e] != '\f') ++e;
        if (e > s || segments == 1) {
            Spans v = {0};
            wrap(t, s, e, width, &v);
            if (v.failed) l->failed = 1;
            for (size_t i = 0; i < v.n; ++i) add_line(l, index, DOC_LINE_TEXT, v.s[i].off, v.s[i].len);
            free(v.s);
        }
        if (++seg == segments) break;
        add_line(l, index, DOC_LINE_SEP, 0, 0);
        s = e + 1;
    }
    if (it->sep_after) add_line(l, index, DOC_LINE_SEP, 0, 0);
}

/* ---- tables ---- */
enum { UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8 };
static uint32_t box(int dirs)
{
    switch (dirs) {
    case LEFT | RIGHT: return 0x2500;
    case UP | DOWN: return 0x2502;
    case DOWN | RIGHT: return 0x250c;
    case DOWN | LEFT: return 0x2510;
    case UP | RIGHT: return 0x2514;
    case UP | LEFT: return 0x2518;
    case UP | DOWN | RIGHT: return 0x251c;
    case UP | DOWN | LEFT: return 0x2524;
    case DOWN | LEFT | RIGHT: return 0x252c;
    case UP | LEFT | RIGHT: return 0x2534;
    case UP | DOWN | LEFT | RIGHT: return 0x253c;
    case UP: case DOWN: return 0x2502;
    default: return 0x2500;
    }
}
/* One screen line built on a grid of columns, then turned into glyphs. */
typedef struct { uint32_t code[FD_COLS_MAX * 2]; unsigned char color[FD_COLS_MAX * 2]; int width; } Grid;
static void grid_init(Grid *g, int width)
{
    g->width = width;
    for (int i = 0; i < width; ++i) { g->code[i] = ' '; g->color[i] = WHITE; }
}
static void grid_put(Grid *g, int x, uint32_t c, int color)
{
    int w = font24_width(c);
    if (x < 0 || x + w > g->width) return;
    g->code[x] = c;
    g->color[x] = (unsigned char)color;
    if (w == 2) g->code[x + 1] = 0;
}
static void grid_emit(DocLayout *l, size_t item, const Grid *g)
{
    DocText *t = add_tline(l, item);
    if (!t) return;
    int last = g->width;
    while (last > 0 && g->code[last - 1] == ' ') --last;
    for (int i = 0; i < last; ++i) if (g->code[i]) doc_text_add(t, g->code[i], g->color[i]);
    if (t->failed) l->failed = 1;
}
static void grid_text(Grid *g, int x, int room, const DocText *t, size_t s, size_t e)
{
    int col = 0;
    for (size_t k = s; k < e; ++k) {
        uint32_t c = t->code[k];
        int w = glyph_width(c, col);
        if (col + w > room) break;
        if (c == 0x2011) grid_put(g, x + col, '-', t->color[k]);
        else if (c >= 0x20 && c != 0x7f) grid_put(g, x + col, c, t->color[k]);
        col += w;
    }
}
/* Too many columns for the grid: each row's cells on lines of their own. */
static void table_plain(DocLayout *l, const DocTable *tb, size_t index, int width)
{
    for (size_t r = 0; r < tb->n; ++r) {
        DocText row = {0};
        for (size_t i = 0; i < tb->rows[r].n; ++i) {
            const DocText *c = &tb->rows[r].cells[i].text;
            if (i) doc_text_utf8(&row, " | ", 3, CYAN);
            for (size_t k = 0; k < c->n; ++k) doc_text_add(&row, c->code[k] == '\n' ? ' ' : c->code[k], c->color[k]);
        }
        Spans v = {0};
        wrap(&row, 0, row.n, width, &v);
        for (size_t i = 0; i < v.n; ++i) {
            Grid g;
            grid_init(&g, width);
            grid_text(&g, 0, width, &row, v.s[i].off, v.s[i].off + v.s[i].len);
            grid_emit(l, index, &g);
        }
        if (v.failed || row.failed) l->failed = 1;
        free(v.s);
        doc_text_free(&row);
        Grid g;
        grid_init(&g, width);
        for (int x = 0; x + 2 <= width; x += 2) grid_put(&g, x, 0x2500, CYAN);
        grid_emit(l, index, &g);
    }
}
static void table(DocLayout *l, const DocItem *it, size_t index, int width)
{
    const DocTable *tb = it->table;
    int ncols = 0;
    for (size_t r = 0; r < tb->n; ++r) {
        int n = 0;
        for (size_t i = 0; i < tb->rows[r].n; ++i) n += tb->rows[r].cells[i].span;
        if (n > ncols) ncols = n;
    }
    if (!ncols) {
        Grid g;
        grid_init(&g, width);
        const char *s = T("[表]");
        int x = 0;
        while (*s) { uint32_t c = font24_decode(&s); grid_put(&g, x, c, CYAN); x += font24_width(c); }
        grid_emit(l, index, &g);
        return;
    }
    int avail = width - 2 * (ncols + 1);
    if (avail < 2 * ncols) { table_plain(l, tb, index, width); return; }
    int *w = calloc((size_t)ncols, sizeof(int)), *bx = calloc((size_t)ncols + 1, sizeof(int));
    unsigned char *above = calloc((size_t)ncols + 1, 1), *below = calloc((size_t)ncols + 1, 1);
    if (!w || !bx || !above || !below) { free(w); free(bx); free(above); free(below); l->failed = 1; return; }
    /* Natural widths of the columns from their single cells. */
    for (size_t r = 0; r < tb->n; ++r) {
        int col = 0;
        for (size_t i = 0; i < tb->rows[r].n && col < ncols; ++i) {
            const DocCell *c = &tb->rows[r].cells[i];
            if (c->span == 1) {
                size_t s = 0;
                for (size_t k = 0; k <= c->text.n; ++k) {
                    if (k == c->text.n || c->text.code[k] == '\n') {
                        int tw = text_width(&c->text, s, k);
                        if (tw > w[col]) w[col] = tw;
                        s = k + 1;
                    }
                }
            }
            col += c->span;
        }
    }
    long total = 0;
    for (int i = 0; i < ncols; ++i) {
        if (w[i] < 2) w[i] = 2;
        w[i] += w[i] & 1;
        total += w[i];
    }
    if (total > avail) {
        long sum = 0;
        for (int i = 0; i < ncols; ++i) {
            int v = (int)((long)avail * w[i] / total);
            v &= ~1;
            w[i] = v < 2 ? 2 : v;
            sum += w[i];
        }
        while (sum > avail) {
            int big = 0;
            for (int i = 1; i < ncols; ++i) if (w[i] > w[big]) big = i;
            if (w[big] <= 2) break;
            w[big] -= 2;
            sum -= 2;
        }
    }
    bx[0] = 0;
    for (int i = 0; i < ncols; ++i) bx[i + 1] = bx[i] + w[i] + 2;
    int line_width = bx[ncols] + 2;
    /* Rows, with the border above each and one below the last. */
    for (size_t r = 0; r <= tb->n; ++r) {
        memset(below, 0, (size_t)ncols + 1);
        if (r < tb->n) {
            int col = 0;
            below[0] = 1;
            for (size_t i = 0; i < tb->rows[r].n && col < ncols; ++i) {
                col += tb->rows[r].cells[i].span;
                if (col > ncols) col = ncols;
                below[col] = 1;
            }
            for (int k = col + 1; k <= ncols; ++k) below[k] = 1;
        }
        Grid g;
        grid_init(&g, line_width);
        for (int k = 0; k <= ncols; ++k) {
            int dirs = (r > 0 && above[k] ? UP : 0) | (r < tb->n && below[k] ? DOWN : 0) | (k > 0 ? LEFT : 0) | (k < ncols ? RIGHT : 0);
            grid_put(&g, bx[k], box(dirs), CYAN);
            if (k < ncols) for (int x = bx[k] + 2; x < bx[k + 1]; x += 2) grid_put(&g, x, 0x2500, CYAN);
        }
        grid_emit(l, index, &g);
        if (r == tb->n) break;
        /* The cells' text, wrapped in their widths. */
        const DocRow *row = &tb->rows[r];
        size_t ncells = row->n;
        Spans *wrapped = calloc(ncells ? ncells : 1, sizeof(Spans));
        int *start = calloc(ncells ? ncells : 1, sizeof(int)), *room = calloc(ncells ? ncells : 1, sizeof(int));
        if (!wrapped || !start || !room) { free(wrapped); free(start); free(room); l->failed = 1; break; }
        size_t height = 1;
        int col = 0;
        for (size_t i = 0; i < ncells; ++i) {
            const DocCell *c = &row->cells[i];
            int span = c->span;
            if (col >= ncols) { room[i] = 0; continue; }
            if (col + span > ncols) span = ncols - col;
            start[i] = bx[col] + 2;
            room[i] = bx[col + span] - bx[col] - 2;
            col += span;
            if (!c->vcont) wrap(&c->text, 0, c->text.n, room[i], &wrapped[i]);
            if (wrapped[i].failed) l->failed = 1;
            if (wrapped[i].n > height) height = wrapped[i].n;
        }
        for (size_t y = 0; y < height; ++y) {
            grid_init(&g, line_width);
            for (int k = 0; k <= ncols; ++k) if (below[k]) grid_put(&g, bx[k], 0x2502, CYAN);
            for (size_t i = 0; i < ncells; ++i)
                if (room[i] > 0 && y < wrapped[i].n)
                    grid_text(&g, start[i], room[i], &row->cells[i].text, wrapped[i].s[y].off, wrapped[i].s[y].off + wrapped[i].s[y].len);
            grid_emit(l, index, &g);
        }
        for (size_t i = 0; i < ncells; ++i) free(wrapped[i].s);
        free(wrapped); free(start); free(room);
        memcpy(above, below, (size_t)ncols + 1);
    }
    free(w); free(bx); free(above); free(below);
}

int doc_layout(const DocFile *d, DocLayout *out, int width)
{
    memset(out, 0, sizeof(*out));
    if (width > FD_COLS_MAX * 2) width = FD_COLS_MAX * 2;
    for (size_t i = 0; i < d->nitems; ++i) {
        const DocItem *it = &d->items[i];
        if (it->deleted) continue;
        if (it->kind == DOC_TABLE) table(out, it, i, width);
        else paragraph(out, it, i, width);
        if (out->failed) break;
    }
    if (out->failed) { doc_layout_free(out); return -1; }
    return 0;
}
