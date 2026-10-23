// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Revision 1 item 38: one cell's change as an undo step (sheetview.c keeps
 * the history, doc_undo.h). Before an edit the cell and every formula that
 * recalculation or a shared formula's master could change are copied
 * (their text and formula as strings, since the arena only grows and a
 * sheet may be read again from the file); after it, the copies that
 * differ become the step, with the shared-formula masters whose si the
 * edit cleared. Undoing puts the "before" copies back, redoing the
 * "after" ones. */
#include "sheet_model.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t row, col;
    int present;
    SheetCell cell;             /* text and formula: see the strings */
    char *text, *formula;       /* SK_STR/SK_ERR text, the formula; NULL none */
} CellSnap;
typedef struct { CellSnap before, after; } CellChange;
typedef struct { size_t index; uint32_t row, col, before, after; } SharedChange;
struct SheetCapture {
    CellSnap *cells;
    size_t n;
    uint32_t *si;               /* s->shared[i].si before */
    size_t nshared;
};
struct SheetStep {
    int sheet;
    uint32_t row, col;          /* the edited cell */
    CellChange *changes;
    size_t n;
    SharedChange *shared;
    size_t nshared;
};

static void snap_free(CellSnap *s)
{
    free(s->text);
    free(s->formula);
    s->text = s->formula = NULL;
}
static int snap_take(const Book *b, const Sheet *s, const SheetCell *c, uint32_t row, uint32_t col, CellSnap *out)
{
    memset(out, 0, sizeof(*out));
    out->row = row;
    out->col = col;
    if (!c) return 0;
    out->present = 1;
    out->cell = *c;
    if ((c->kind == SK_STR || c->kind == SK_ERR) && !(out->text = strdup(cell_string(b, s, c)))) return -1;
    if (c->formula != SHEET_NONE && !(out->formula = strdup(arena_str(&s->arena, c->formula)))) return -1;
    return 0;
}
static int same_string(const char *a, const char *b) { return a ? b && !strcmp(a, b) : !b; }
static int snap_same(const CellSnap *a, const CellSnap *b)
{
    if (a->present != b->present) return 0;
    if (!a->present) return 1;
    const SheetCell *x = &a->cell, *y = &b->cell;
    if (x->kind != y->kind || x->flags != y->flags || x->style != y->style || x->aux != y->aux) return 0;
    if (memcmp(&x->num, &y->num, sizeof(x->num))) return 0;
    if (x->kind == SK_SST && x->text != y->text) return 0;
    return same_string(a->text, b->text) && same_string(a->formula, b->formula);
}
/* Formulas that recalculation may give a new value, and the members of
 * shared formulas (their master's edit rewrites them). */
static int watched(const SheetCell *c)
{
    return (c->flags & (SF_FORMULA | SF_CALC)) == (SF_FORMULA | SF_CALC) || (c->flags & SF_SHARED);
}

void sheet_capture_free(SheetCapture *cap)
{
    if (!cap) return;
    for (size_t i = 0; i < cap->n; ++i) snap_free(&cap->cells[i]);
    free(cap->cells);
    free(cap->si);
    free(cap);
}
SheetCapture *sheet_capture(const Book *b, Sheet *s, uint32_t row, uint32_t col)
{
    SheetCapture *cap = calloc(1, sizeof(*cap));
    if (!cap) return NULL;
    size_t n = 1;
    for (size_t i = 0; i < s->count; ++i)
        if (watched(&s->cells[i]) && (s->cells[i].row != row || s->cells[i].col != col)) n++;
    cap->cells = calloc(n, sizeof(*cap->cells));
    cap->si = calloc(s->nshared ? s->nshared : 1, sizeof(*cap->si));
    if (!cap->cells || !cap->si) { sheet_capture_free(cap); return NULL; }
    if (snap_take(b, s, sheet_find(s, row, col), row, col, &cap->cells[cap->n++])) { sheet_capture_free(cap); return NULL; }
    for (size_t i = 0; i < s->count; ++i) {
        const SheetCell *c = &s->cells[i];
        if (!watched(c) || (c->row == row && c->col == col)) continue;
        if (snap_take(b, s, c, c->row, c->col, &cap->cells[cap->n++])) { sheet_capture_free(cap); return NULL; }
    }
    for (size_t i = 0; i < s->nshared; ++i) cap->si[i] = s->shared[i].si;
    cap->nshared = s->nshared;
    return cap;
}
void sheet_step_free(void *p)
{
    SheetStep *st = p;
    if (!st) return;
    for (size_t i = 0; i < st->n; ++i) { snap_free(&st->changes[i].before); snap_free(&st->changes[i].after); }
    free(st->changes);
    free(st->shared);
    free(st);
}
SheetStep *sheet_step_make(const Book *b, Sheet *s, int sheet, uint32_t row, uint32_t col, SheetCapture *cap, int *same)
{
    *same = 0;
    SheetStep *st = calloc(1, sizeof(*st));
    if (!st) { sheet_capture_free(cap); return NULL; }
    st->sheet = sheet;
    st->row = row;
    st->col = col;
    st->changes = calloc(cap->n ? cap->n : 1, sizeof(*st->changes));
    st->shared = calloc(cap->nshared ? cap->nshared : 1, sizeof(*st->shared));
    int failed = !st->changes || !st->shared;
    for (size_t i = 0; i < cap->n && !failed; ++i) {
        CellSnap now;
        CellSnap *was = &cap->cells[i];
        if (snap_take(b, s, sheet_find(s, was->row, was->col), was->row, was->col, &now)) { snap_free(&now); failed = 1; break; }
        if (snap_same(was, &now)) { snap_free(&now); continue; }
        st->changes[st->n].before = *was;
        st->changes[st->n].after = now;
        memset(was, 0, sizeof(*was));
        st->n++;
    }
    for (size_t i = 0; i < cap->nshared && i < s->nshared && !failed; ++i)
        if (cap->si[i] != s->shared[i].si)
            st->shared[st->nshared++] = (SharedChange){i, s->shared[i].row, s->shared[i].col, cap->si[i], s->shared[i].si};
    sheet_capture_free(cap);
    if (failed) { sheet_step_free(st); return NULL; }
    if (!st->n && !st->nshared) { sheet_step_free(st); *same = 1; return NULL; }
    return st;
}
void sheet_step_where(const SheetStep *st, int *sheet, uint32_t *row, uint32_t *col)
{
    *sheet = st->sheet;
    *row = st->row;
    *col = st->col;
}

/* Puts one copy back. force: the file was saved since the copy was made, so
 * it no longer holds what the copy's flags say: the cell is written again
 * (SF_EDITED) and keeps the present SF_ORIG_FORMULA (whether the file has a
 * formula there, for calcChain). */
static int restore(Sheet *s, const CellSnap *t, int force)
{
    SheetCell *c = sheet_find(s, t->row, t->col);
    uint16_t orig = c ? (uint16_t)(c->flags & SF_ORIG_FORMULA) : 0;
    if (!t->present) {
        if (!c) return 0;
        if (!force) {
            size_t i = (size_t)(c - s->cells);
            memmove(c, c + 1, (s->count - i - 1) * sizeof(*c));
            s->count--;
            return 0;
        }
        /* The saved file has the cell: an empty one replaces it. */
        uint32_t row = c->row, col = c->col;
        memset(c, 0, sizeof(*c));
        c->row = row; c->col = col;
        c->kind = SK_EMPTY;
        c->text = c->formula = SHEET_NONE;
        c->flags = (uint16_t)(SF_EDITED | orig);
        return 0;
    }
    if (!c && !(c = sheet_insert(s, t->row, t->col))) return -1;
    *c = t->cell;
    c->row = t->row; c->col = t->col;
    c->gen = 0; c->eval = 0; c->affected = 0;
    if (c->kind != SK_SST) c->text = SHEET_NONE;
    c->formula = SHEET_NONE;
    int failed = 0;
    if (t->text && (c->text = arena_add(&s->arena, t->text, strlen(t->text))) == SHEET_NONE) failed = 1;
    if (t->formula && (c->formula = arena_add(&s->arena, t->formula, strlen(t->formula))) == SHEET_NONE) failed = 1;
    if (failed) { c->kind = SK_EMPTY; c->flags = (uint16_t)(SF_EDITED | orig); return -1; }
    if (force) c->flags = (uint16_t)((c->flags & ~(SF_ORIG_FORMULA | SF_RECALC | SF_REWRITE)) | SF_EDITED | orig);
    return 0;
}
int sheet_step_apply(Book *b, Sheet *s, const SheetStep *st, int backward, int force)
{
    int failed = 0;
    for (size_t i = 0; i < st->n; ++i)
        if (restore(s, backward ? &st->changes[i].before : &st->changes[i].after, force)) failed = 1;
    for (size_t i = 0; i < st->nshared; ++i) {
        const SharedChange *x = &st->shared[i];
        if (x->index < s->nshared && s->shared[x->index].row == x->row && s->shared[x->index].col == x->col)
            s->shared[x->index].si = backward ? x->before : x->after;
    }
    /* xlsx: the sheet is written when a cell says so (a sheet back as the
     * file has it can be let go and read again); csv is written whole. */
    if (b->type == BOOK_XLSX) {
        s->dirty = 0;
        for (size_t i = 0; i < s->count && !s->dirty; ++i)
            if (s->cells[i].flags & (SF_EDITED | SF_RECALC | SF_REWRITE)) s->dirty = 1;
    } else s->dirty = 1;
    return failed ? -1 : 0;
}
