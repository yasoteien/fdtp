// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Revision 1 items 38 and 41: the undo history (doc_undo.h). */
#include "doc_undo.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_LEVELS 100

void undo_init(UndoHist *h, void (*free_data)(void *))
{
    memset(h, 0, sizeof(*h));
    h->free_data = free_data;
}
static void drop(UndoHist *h, size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i)
        if (h->free_data && h->steps[i].data) h->free_data(h->steps[i].data);
}
void undo_free(UndoHist *h)
{
    drop(h, 0, h->n);
    free(h->steps);
    h->steps = NULL;
    h->n = h->cap = h->pos = 0;
}
static unsigned long state(const UndoHist *h) { return h->base + h->pos; }
void undo_clear(UndoHist *h)
{
    unsigned long now = state(h);
    drop(h, 0, h->n);
    h->n = h->pos = 0;
    h->base = now;
    if (h->saved != now) h->saved = UNDO_NOWHERE;
}
void undo_lost(UndoHist *h)
{
    undo_clear(h);
    h->base++;
    h->saved = UNDO_NOWHERE;
}
int undo_push(UndoHist *h, int levels, const UndoStep *step)
{
    if (levels <= 0) levels = DEFAULT_LEVELS;
    unsigned long now = state(h);
    /* The redo side goes; a saved state there can no longer be reached. */
    drop(h, h->pos, h->n);
    h->n = h->pos;
    if (h->saved != UNDO_NOWHERE && h->saved > now) h->saved = UNDO_NOWHERE;
    if (h->n == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 16;
        UndoStep *p = realloc(h->steps, cap * sizeof(*p));
        if (!p) {
            if (h->free_data && step->data) h->free_data(step->data);
            undo_lost(h);
            return -1;
        }
        h->steps = p;
        h->cap = cap;
    }
    h->steps[h->n++] = *step;
    h->pos = h->n;
    /* Beyond UNDO_LEVELS: the oldest steps go (their states with them). */
    while (h->n > (size_t)levels) {
        drop(h, 0, 1);
        memmove(h->steps, h->steps + 1, (h->n - 1) * sizeof(*h->steps));
        h->n--;
        h->pos--;
        h->base++;
    }
    if (h->saved != UNDO_NOWHERE && h->saved < h->base) h->saved = UNDO_NOWHERE;
    return 0;
}
UndoStep *undo_next_undo(UndoHist *h) { return h->pos ? &h->steps[h->pos - 1] : NULL; }
UndoStep *undo_next_redo(UndoHist *h) { return h->pos < h->n ? &h->steps[h->pos] : NULL; }
void undo_done_undo(UndoHist *h) { if (h->pos) h->pos--; }
void undo_done_redo(UndoHist *h) { if (h->pos < h->n) h->pos++; }
void undo_saved(UndoHist *h) { h->saved = state(h); }
int undo_dirty(const UndoHist *h) { return h->saved != state(h); }

/* ---- splices ---- */
static unsigned char *copy_bytes(const unsigned char *p, size_t n)
{
    unsigned char *q = malloc(n ? n : 1);
    if (q && n) memcpy(q, p, n);
    return q;
}
UndoSplice *undo_splice_make(const unsigned char *a, size_t an, const unsigned char *b, size_t bn, int *same)
{
    if (same) *same = 0;
    size_t head = 0, limit = an < bn ? an : bn;
    while (head < limit && a[head] == b[head]) ++head;
    if (head == an && an == bn) { if (same) *same = 1; return NULL; }
    size_t tail = 0;
    while (tail < limit - head && a[an - 1 - tail] == b[bn - 1 - tail]) ++tail;
    UndoSplice *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->at = head;
    s->old_len = an - head - tail;
    s->new_len = bn - head - tail;
    s->old_bytes = copy_bytes(a + head, s->old_len);
    s->new_bytes = copy_bytes(b + head, s->new_len);
    if (!s->old_bytes || !s->new_bytes) { undo_splice_free(s); return NULL; }
    return s;
}
unsigned char *undo_splice_apply(const UndoSplice *s, int backward, const unsigned char *cur, size_t n, size_t *out)
{
    const unsigned char *have = backward ? s->new_bytes : s->old_bytes;
    const unsigned char *put = backward ? s->old_bytes : s->new_bytes;
    size_t have_len = backward ? s->new_len : s->old_len, put_len = backward ? s->old_len : s->new_len;
    if (s->at > n || have_len > n - s->at || memcmp(cur + s->at, have, have_len)) return NULL;
    size_t len = n - have_len + put_len;
    unsigned char *r = malloc(len ? len : 1);
    if (!r) return NULL;
    memcpy(r, cur, s->at);
    memcpy(r + s->at, put, put_len);
    memcpy(r + s->at + put_len, cur + s->at + have_len, n - s->at - have_len);
    *out = len;
    return r;
}
void undo_splice_free(void *p)
{
    UndoSplice *s = p;
    if (!s) return;
    free(s->old_bytes);
    free(s->new_bytes);
    free(s);
}
