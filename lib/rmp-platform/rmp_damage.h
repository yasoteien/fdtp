// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Damage history for partial redraw (EGL_EXT_buffer_age semantics).
 * Pure C, no graphics dependencies (tested on the Mac).
 *
 * A back buffer of age n shows the frame presented n frames ago; it must
 * repaint the damage of the current (uncommitted) frame plus the previous
 * n-1 presented frames. Age 0 (unknown) or older than the history: all. */
#ifndef RMP_DAMAGE_H
#define RMP_DAMAGE_H

#include "rmp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RMP_DAMAGE_HISTORY 8   /* GBM keeps up to 4 buffers: ages 1..4 */
#define RMP_DAMAGE_RECTS 48    /* per frame; more are merged */

typedef struct { int x0, y0, x1, y1; } rmp_box;   /* half-open */

typedef struct {
    int w, h;
    unsigned long frames;                 /* committed (presented) frames */
    rmp_box cur[RMP_DAMAGE_RECTS];
    int cur_n, cur_full;
    rmp_box hist[RMP_DAMAGE_HISTORY][RMP_DAMAGE_RECTS];
    int hist_n[RMP_DAMAGE_HISTORY], hist_full[RMP_DAMAGE_HISTORY];
} rmp_damage;

void rmp_damage_init(rmp_damage *d, int w, int h);
void rmp_damage_add(rmp_damage *d, rmp_rect r);   /* clipped to w x h */
void rmp_damage_add_full(rmp_damage *d);
/* 1 if the current frame has any damage. */
int  rmp_damage_pending(const rmp_damage *d);
/* Region for a buffer of `age`. Returns the count (1..max), non-overlapping;
 * *full = 1 when it is the whole surface. max >= 1. Returns 0 only when
 * there is nothing to repaint (no current damage is NOT checked here:
 * a stale buffer can still need repainting). */
int  rmp_damage_region(const rmp_damage *d, int age, rmp_rect *out, int max, int *full);
/* The current frame was presented: move it into the history. */
void rmp_damage_commit(rmp_damage *d);

/* Helpers shared with screens. */
rmp_box rmp_box_from_rect(rmp_rect r);
rmp_rect rmp_rect_from_box(rmp_box b);
int  rmp_box_empty(rmp_box b);
rmp_box rmp_box_union(rmp_box a, rmp_box b);
rmp_box rmp_box_intersect(rmp_box a, rmp_box b);
int  rmp_rect_intersects(rmp_rect a, rmp_rect b);
rmp_rect rmp_rect_intersect(rmp_rect a, rmp_rect b);
long rmp_rect_area(rmp_rect r);

#ifdef __cplusplus
}
#endif
#endif
