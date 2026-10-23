// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rmp_damage.h"

#include <string.h>

int rmp_box_empty(rmp_box b) { return b.x0 >= b.x1 || b.y0 >= b.y1; }

rmp_box rmp_box_union(rmp_box a, rmp_box b)
{
    if (rmp_box_empty(a)) return b;
    if (rmp_box_empty(b)) return a;
    rmp_box r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                  a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}

rmp_box rmp_box_intersect(rmp_box a, rmp_box b)
{
    rmp_box r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
                  a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    if (rmp_box_empty(r)) { rmp_box e = {0, 0, 0, 0}; return e; }
    return r;
}

rmp_box rmp_box_from_rect(rmp_rect r)
{
    rmp_box b = { r.x, r.y, r.x + r.w, r.y + r.h };
    if (r.w <= 0 || r.h <= 0) { rmp_box e = {0, 0, 0, 0}; return e; }
    return b;
}

rmp_rect rmp_rect_from_box(rmp_box b)
{
    rmp_rect r = { b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0 };
    if (rmp_box_empty(b)) { rmp_rect e = {0, 0, 0, 0}; return e; }
    return r;
}

static int box_overlap(rmp_box a, rmp_box b)
{
    return !rmp_box_empty(a) && !rmp_box_empty(b) &&
           a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

int rmp_rect_intersects(rmp_rect a, rmp_rect b)
{
    return box_overlap(rmp_box_from_rect(a), rmp_box_from_rect(b));
}

rmp_rect rmp_rect_intersect(rmp_rect a, rmp_rect b)
{
    return rmp_rect_from_box(rmp_box_intersect(rmp_box_from_rect(a), rmp_box_from_rect(b)));
}

long rmp_rect_area(rmp_rect r) { return r.w > 0 && r.h > 0 ? (long)r.w * (long)r.h : 0; }

static long box_area(rmp_box b)
{
    return rmp_box_empty(b) ? 0 : (long)(b.x1 - b.x0) * (long)(b.y1 - b.y0);
}

void rmp_damage_init(rmp_damage *d, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->w = w > 0 ? w : 0;
    d->h = h > 0 ? h : 0;
}

/* Index of the box whose union with b grows the total area the least. */
static int cheapest(const rmp_box *boxes, int n, rmp_box b)
{
    int best = 0;
    long best_cost = -1;
    for (int i = 0; i < n; ++i) {
        long cost = box_area(rmp_box_union(boxes[i], b)) - box_area(boxes[i]) - box_area(b);
        if (best_cost < 0 || cost < best_cost) { best = i; best_cost = cost; }
    }
    return best;
}

void rmp_damage_add(rmp_damage *d, rmp_rect r)
{
    rmp_box screen = { 0, 0, d->w, d->h };
    rmp_box b = rmp_box_intersect(rmp_box_from_rect(r), screen);
    if (rmp_box_empty(b) || d->cur_full) return;
    if (d->cur_n < RMP_DAMAGE_RECTS) {
        d->cur[d->cur_n++] = b;
    } else {
        int i = cheapest(d->cur, d->cur_n, b);
        d->cur[i] = rmp_box_union(d->cur[i], b);
    }
}

void rmp_damage_add_full(rmp_damage *d) { d->cur_full = 1; d->cur_n = 0; }

int rmp_damage_pending(const rmp_damage *d) { return d->cur_full || d->cur_n > 0; }

/* Merges overlapping boxes until none overlap. */
static int merge_overlaps(rmp_box *b, int n)
{
    for (int merged = 1; merged;) {
        merged = 0;
        for (int i = 0; i < n && !merged; ++i)
            for (int j = i + 1; j < n; ++j)
                if (box_overlap(b[i], b[j])) {
                    b[i] = rmp_box_union(b[i], b[j]);
                    b[j] = b[--n];
                    merged = 1;
                    break;
                }
    }
    return n;
}

int rmp_damage_region(const rmp_damage *d, int age, rmp_rect *out, int max, int *full)
{
    static rmp_box boxes[RMP_DAMAGE_RECTS * (RMP_DAMAGE_HISTORY + 1)];
    int n = 0;
    *full = age <= 0 || age > RMP_DAMAGE_HISTORY ||
            (unsigned long)(age - 1) > d->frames || d->cur_full || max < 1;
    for (int k = 0; k < age - 1 && !*full; ++k) {
        unsigned slot = (unsigned)((d->frames - 1 - (unsigned long)k) % RMP_DAMAGE_HISTORY);
        if (d->hist_full[slot]) { *full = 1; break; }
        for (int i = 0; i < d->hist_n[slot]; ++i) boxes[n++] = d->hist[slot][i];
    }
    if (*full) {
        if (max < 1) return 0;
        out[0] = (rmp_rect){ 0, 0, d->w, d->h };
        return 1;
    }
    for (int i = 0; i < d->cur_n; ++i) boxes[n++] = d->cur[i];
    n = merge_overlaps(boxes, n);
    while (n > max) {
        /* Merge the pair whose union adds the least area, then re-merge. */
        int bi = 0, bj = 1;
        long best = -1;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                long cost = box_area(rmp_box_union(boxes[i], boxes[j])) -
                            box_area(boxes[i]) - box_area(boxes[j]);
                if (best < 0 || cost < best) { best = cost; bi = i; bj = j; }
            }
        boxes[bi] = rmp_box_union(boxes[bi], boxes[bj]);
        boxes[bj] = boxes[--n];
        n = merge_overlaps(boxes, n);
    }
    for (int i = 0; i < n; ++i) out[i] = rmp_rect_from_box(boxes[i]);
    if (n == 1 && boxes[0].x0 == 0 && boxes[0].y0 == 0 && boxes[0].x1 == d->w && boxes[0].y1 == d->h)
        *full = 1;
    return n;
}

void rmp_damage_commit(rmp_damage *d)
{
    unsigned slot = (unsigned)(d->frames % RMP_DAMAGE_HISTORY);
    d->hist_full[slot] = d->cur_full;
    d->hist_n[slot] = d->cur_full ? 0 : d->cur_n;
    memcpy(d->hist[slot], d->cur, sizeof(d->cur[0]) * (size_t)d->hist_n[slot]);
    d->frames++;
    d->cur_n = 0;
    d->cur_full = 0;
}
