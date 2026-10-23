// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "novel_layout.h"
#include "novel_glyph.h"
#include "novel_doc.h"
#include "font24.h"
#include "cells.h"
#include <stdlib.h>
#include <string.h>

void nl_init(NLayout *l) { memset(l, 0, sizeof(*l)); }
void nl_free(NLayout *l)
{
    free(l->cp); free(l->off); free(l->span); free(l->u); free(l->r); free(l->row);
    memset(l, 0, sizeof(*l));
}
static int grow(void **p, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap) return 0;
    size_t next = *cap ? *cap : 64;
    while (next < need) next *= 2;
    void *q = realloc(*p, next * size);
    if (!q) return -1;
    *p = q; *cap = next;
    return 0;
}
static NUnit *add_unit(NLayout *l)
{
    if (grow((void **)&l->u, &l->cu, l->nu + 1, sizeof(NUnit))) return NULL;
    NUnit *u = &l->u[l->nu++];
    memset(u, 0, sizeof(*u));
    u->group = -1;
    u->color = WHITE;
    return u;
}
static int add_row(NLayout *l, size_t unit0, size_t ruby0)
{
    if (grow((void **)&l->row, &l->crow, l->nrow + 1, sizeof(NRow))) return -1;
    l->row[l->nrow++] = (NRow){unit0, 0, ruby0, 0, 0, 0};
    return 0;
}
static int warn_color(int warn) { return warn == NR_RED ? RED : warn == NR_MAGENTA ? MAGENTA : WHITE; }

/* Natural advance and glyph for horizontal writing / full-width vertical. */
static void shape(NLayout *l, NUnit *u, uint32_t c)
{
    if (c == '\t') { u->code = ' '; u->adv = 48; return; }
    if (c < 32 || c == 127) { u->code = '?'; u->color = CYAN; c = '?'; }
    else u->code = c;
    int w = font24_width(c);
    if (w == 1) u->flags |= NU_HALF;
    u->adv = w * 12;
    if (l->vertical && w == 2) { u->adv = 24; u->form = (unsigned char)ng_vertical_form(c); }
}
static int alnum(uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
/* Vertical: runs of one or two half-width letters/digits stand upright in one
 * 24 px square (tate-chu-yoko); longer runs and other half-width ASCII are
 * turned 90 degrees, 12 px each; half-width kana stand upright, centred. */
static void vertical_runs(NLayout *l)
{
    size_t i = 0;
    while (i < l->nu) {
        NUnit *u = &l->u[i];
        if ((u->flags & (NU_HALF | NU_HIDDEN)) != NU_HALF) { ++i; continue; }
        if (!alnum(u->code)) {
            if (u->code < 128) { u->adv = 12; u->form = NG_ROTATE; }
            else { u->adv = 24; u->cross = 6; }
            ++i;
            continue;
        }
        size_t j = i, count = 0, members[3];
        int group = u->group;
        while (j < l->nu && l->u[j].group == group &&
               ((l->u[j].flags & NU_HIDDEN) || ((l->u[j].flags & NU_HALF) && alnum(l->u[j].code)))) {
            if (!(l->u[j].flags & NU_HIDDEN)) { if (count < 3) members[count] = j; ++count; }
            ++j;
        }
        /* u itself is a visible half-width alnum, so count >= 1; guard anyway
         * (and always move on, or the loop would not end). */
        if (count == 0) { i = j > i ? j : i + 1; continue; }
        if (count <= 2) {
            l->u[members[0]].adv = 24;
            l->u[members[0]].cross = count == 1 ? 6 : 0;
            if (count == 2) { l->u[members[1]].adv = 0; l->u[members[1]].cross = 12; l->u[members[1]].flags |= NU_TCY2; }
        } else {
            for (size_t k = i; k < j; ++k)
                if (!(l->u[k].flags & NU_HIDDEN)) { l->u[k].adv = 12; l->u[k].form = NG_ROTATE; }
        }
        i = j;
    }
}
static size_t index_of(const NLayout *l, size_t off)
{
    size_t lo = 0, hi = l->ncp;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (l->off[mid] < off) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static int fail(NLayout *l)
{
    l->nu = l->nr = l->nrow = 0;
    l->failed = 1;
    if (!add_row(l, 0, 0)) l->row[0].start = 0;
    return -1;
}
int nl_run(NLayout *l, const char *text, size_t len, int vertical, size_t cursor,
           size_t pre_start, size_t pre_end, size_t hl_start, size_t hl_end)
{
    l->vertical = vertical; l->failed = 0; l->len = len;
    l->nu = l->nr = l->nrow = l->ncp = 0; l->expanded = -1;
    if (len + 1 > l->ccp) {
        size_t cap = l->ccp ? l->ccp : 256;
        while (cap < len + 1) cap *= 2;
        uint32_t *cp = realloc(l->cp, cap * sizeof(*cp));
        if (cp) l->cp = cp;
        size_t *off = realloc(l->off, cap * sizeof(*off));
        if (off) l->off = off;
        if (!cp || !off) return fail(l);
        l->ccp = cap;
    }
    for (size_t at = 0; at < len;) {
        size_t next;
        l->off[l->ncp] = at;
        l->cp[l->ncp++] = novel_decode(text, len, at, &next);
        at = next;
    }
    l->off[l->ncp] = len;
    size_t spans = novel_ruby_parse(l->cp, l->ncp, &l->span, &l->cspan);
    if (spans == (size_t)-1) return fail(l);
    l->nspan = spans;
    if (cursor != (size_t)-1) {
        size_t ci = index_of(l, cursor);
        for (size_t s = 0; s < spans; ++s)
            if (l->span[s].start <= ci && ci < l->span[s].end) { l->expanded = (int)s; break; }
    }
    /* Units in text order. */
    size_t s = 0;
    for (size_t i = 0; i < l->ncp;) {
        while (s < spans && l->span[s].end <= i) ++s;
        const NRuby *sp = s < spans && l->span[s].start <= i ? &l->span[s] : NULL;
        if (sp && (int)s != l->expanded && i == sp->start) {
            for (size_t k = sp->start; k < sp->end; ++k) {
                NUnit *u = add_unit(l);
                if (!u) return fail(l);
                u->off = l->off[k]; u->next = l->off[k + 1];
                shape(l, u, l->cp[k]);
                if (sp->kind == NR_ESCAPE) {
                    if (k == sp->start) { u->flags |= NU_HIDDEN; u->adv = 0; }
                    continue;
                }
                u->group = (int)s;
                if (k < sp->base || k >= sp->base_end) { u->flags |= NU_HIDDEN; u->adv = 0; u->flags &= (unsigned char)~NU_HALF; }
                else {
                    u->color = (unsigned char)warn_color(sp->warn);
                    if (sp->kind == NR_EMPHASIS) u->flags |= NU_EMPHASIS;
                }
            }
            i = sp->end;
            continue;
        }
        NUnit *u = add_unit(l);
        if (!u) return fail(l);
        u->off = l->off[i]; u->next = l->off[i + 1];
        shape(l, u, l->cp[i]);
        if (sp && (int)s == l->expanded && sp->kind != NR_ESCAPE) {
            if (i >= sp->base && i < sp->base_end) u->color = (unsigned char)warn_color(sp->warn);
            else if (i < sp->ruby || i >= sp->ruby_end) u->color = CYAN;
        } else if (sp && (int)s == l->expanded && i == sp->start) u->color = CYAN;
        ++i;
    }
    for (size_t k = 0; k < l->nu; ++k) {
        NUnit *u = &l->u[k];
        if (u->off >= pre_start && u->off < pre_end) u->flags |= NU_PREEDIT;
        if (u->off >= hl_start && u->off < hl_end) u->flags |= NU_PREEDIT_HL;
    }
    if (vertical) vertical_runs(l);
    /* Fold into rows; a ruby group moves to the next row as a whole. */
    int cap = l->width > 0 ? l->width : vertical ? NL_ROW_V : NL_ROW_H, pos = 0;
    if (add_row(l, 0, 0)) return fail(l);
    size_t k = 0;
    while (k < l->nu) {
        size_t end = k + 1;
        int group = l->u[k].group, width = l->u[k].adv;
        if (group >= 0) {
            width = 0;
            while (end < l->nu && l->u[end].group == group) ++end;
            int base = 0, ruby = 0;
            for (size_t q = k; q < end; ++q) base += l->u[q].adv;
            const NRuby *sp = &l->span[group];
            size_t nr = sp->ruby_end - sp->ruby;
            for (size_t q = sp->ruby; q < sp->ruby_end; ++q)
                ruby += vertical ? 12 : ng_ruby_width(l->cp[q]);
            width = base;
            if ((ruby + 11) / 12 * 12 > width) width = (ruby + 11) / 12 * 12;
            if (width > cap) width = cap;
            if (pos && pos + width > cap) {
                if (add_row(l, k, l->nr)) return fail(l);
                pos = 0;
            }
            int at = pos + (width - base) / 2, seen_base = 0;
            for (size_t q = k; q < end; ++q) {
                NUnit *u = &l->u[q];
                if (u->flags & NU_HIDDEN) { u->pos = seen_base ? pos + width : pos; continue; }
                seen_base = 1;
                u->pos = u->flags & NU_TCY2 ? at - 24 : at;
                at += u->adv;
            }
            int gap = width - ruby, acc = 0;
            for (size_t q = 0; q < nr; ++q) {
                if (grow((void **)&l->r, &l->cr, l->nr + 1, sizeof(NRubyGlyph))) return fail(l);
                uint32_t c = l->cp[sp->ruby + q];
                int w = vertical ? 12 : ng_ruby_width(c);
                NRubyGlyph *g = &l->r[l->nr++];
                g->code = c;
                g->pos = pos + acc + (int)((long)gap * (long)(2 * q + 1) / (long)(2 * nr));
                if (vertical && ng_ruby_width(c) == 6) g->pos += 3;
                g->half = ng_ruby_width(c) == 6;
                g->color = WHITE;
                g->form = (unsigned char)(vertical ? ng_vertical_form(c) : NG_UPRIGHT);
                acc += w;
            }
            pos += width;
        } else {
            while (end < l->nu && l->u[end].group < 0 && l->u[end].adv == 0) ++end;
            for (size_t q = k + 1; q < end; ++q) width += l->u[q].adv;
            if (pos && pos + width > cap) {
                if (add_row(l, k, l->nr)) return fail(l);
                pos = 0;
            }
            int last = pos;
            for (size_t q = k; q < end; ++q) {
                NUnit *u = &l->u[q];
                u->pos = u->flags & NU_TCY2 ? last : pos;
                if (u->adv) last = pos;
                pos += u->adv;
            }
        }
        NRow *row = &l->row[l->nrow - 1];
        row->units = end - row->unit0;
        row->rubies = l->nr - row->ruby0;
        row->length = pos;
        k = end;
    }
    for (size_t r = 0; r < l->nrow; ++r) {
        NRow *row = &l->row[r];
        row->start = row->units ? l->u[row->unit0].off : (r ? l->len : 0);
    }
    return 0;
}
static size_t row_of_unit(const NLayout *l, size_t k)
{
    size_t lo = 0, hi = l->nrow;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (l->row[mid].unit0 <= k) lo = mid; else hi = mid;
    }
    return lo;
}
void nl_locate(const NLayout *l, size_t off, size_t *row, int *pos)
{
    if (off >= l->len || !l->nu) {
        *row = l->nrow ? l->nrow - 1 : 0;
        *pos = l->nrow ? l->row[*row].length : 0;
        return;
    }
    for (size_t k = 0; k < l->nu; ++k) {
        const NUnit *u = &l->u[k];
        if (u->off < off || (u->flags & NU_HIDDEN)) continue;
        *row = row_of_unit(l, k);
        *pos = u->pos + (u->flags & NU_TCY2 ? 12 : 0);
        return;
    }
    *row = l->nrow - 1;
    *pos = l->row[*row].length;
}
static int visible(const NUnit *u) { return !(u->flags & NU_HIDDEN); }
size_t nl_hit(const NLayout *l, size_t row, int pos)
{
    if (row >= l->nrow) row = l->nrow - 1;
    const NRow *r = &l->row[row];
    for (size_t k = r->unit0; k < r->unit0 + r->units; ++k) {
        const NUnit *u = &l->u[k];
        if (!visible(u) || (u->flags & NU_TCY2)) continue;
        /* A tate-chu-yoko pair shares one square: its halves pick each one. */
        int pair = k + 1 < l->nu && (l->u[k + 1].flags & NU_TCY2);
        if (pos < u->pos + (pair ? 6 : u->adv / 2)) return u->off;
        if (pair && pos < u->pos + 18) return l->u[k + 1].off;
    }
    return nl_row_end(l, row);
}
size_t nl_row_start(const NLayout *l, size_t row)
{
    if (row >= l->nrow) return l->len;
    const NRow *r = &l->row[row];
    for (size_t k = r->unit0; k < r->unit0 + r->units; ++k)
        if (visible(&l->u[k])) return l->u[k].off;
    return r->start;
}
size_t nl_row_end(const NLayout *l, size_t row)
{
    if (row + 1 >= l->nrow) return l->len;
    const NRow *r = &l->row[row];
    for (size_t k = r->unit0 + r->units; k > r->unit0; --k)
        if (visible(&l->u[k - 1])) return l->u[k - 1].off;
    return r->start;
}
