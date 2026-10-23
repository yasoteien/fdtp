// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rmp_pacing.h"

#include <math.h>

void rmp_pacing_init(rmp_pacing *p, double period)
{
    p->interval = 2;
    p->period = period > 0 && isfinite(period) ? period : 1.0 / 60;
    p->have_flip = 0;
    p->last_flip = 0;
    p->pending = 0;
    p->not_before = 0;
}

void rmp_pacing_set_interval(rmp_pacing *p, int vblanks)
{
    int n = vblanks < 1 ? 1 : vblanks > 4 ? 4 : vblanks;
    /* An idle slot follows the old interval's rule (interval 1: a margin
     * after a vblank, off the 2-vblank grid; interval 2: up to two vblanks
     * ahead): drop it, the new interval's due/target apply at once. Only
     * ever later than those, so dropping it never shows a frame earlier. */
    if (n != p->interval) p->not_before = 0;
    p->interval = n;
}

double rmp_pacing_due(const rmp_pacing *p, double now)
{
    double due = now;
    if (p->interval >= 2) {
        /* Where the previous flip landed decides this frame's target. */
        if (p->pending) return INFINITY;
        /* Begin at the vblank two before the target (n = 2: right away). */
        if (p->have_flip)
            due = p->last_flip + (p->interval - 2) * p->period;
    }
    if (p->not_before > due) due = p->not_before;
    return due;
}

/* Interval >= 2: the vblank planned for a frame begun at `now`; *late set
 * when that slot had already begun (the frame is then shown at the next
 * vblank instead). */
static double slot_target(const rmp_pacing *p, double now, int *late)
{
    /* interval vblanks after the last flip; the idle slot if one was set;
     * and never a vblank at or before now (a late frame is shown at the
     * next vblank, not skipped to the next slot). */
    double t = p->last_flip + (p->interval + (p->pending ? 1 : 0)) * p->period;
    if (p->not_before > 0 && p->not_before + 2 * p->period > t) t = p->not_before + 2 * p->period;
    *late = t <= now;
    if (*late) t = p->last_flip + (floor((now - p->last_flip) / p->period + 1e-9) + 1) * p->period;
    return t;
}

double rmp_pacing_target(const rmp_pacing *p, double now)
{
    if (!p->have_flip) return now + p->period;
    if (p->interval == 1) {
        /* At least one vblank after the last flip, one more while a flip
         * is still queued, and never a vblank that has already begun. */
        double t = p->last_flip + (1 + (p->pending ? 1 : 0)) * p->period;
        if (t - p->period < now) {
            double k = ceil((now - (t - p->period)) / p->period);
            t += k * p->period;
        }
        return t;
    }
    int late;
    return slot_target(p, now, &late);
}

double rmp_pacing_deadline(const rmp_pacing *p, double now)
{
    double t = rmp_pacing_target(p, now);
    if (!p->have_flip || p->interval == 1) return t;
    /* Begun after its slot (the app was busy past it): shown at the next
     * vblank, but on time up to the vblank after that - the two vblanks a
     * frame begun on its slot has. Without this a light frame begun late
     * in a vblank counted as MISSED by chance (2026-10-21). */
    int late;
    (void)slot_target(p, now, &late);
    return late ? t + p->period : t;
}

void rmp_pacing_idle(rmp_pacing *p, double now)
{
    double n = p->interval;
    if (!p->have_flip) { p->not_before = now + n * p->period; return; }
    /* Slots display at last_flip + k*n*period. A slot's frame may begin at
     * the vblank two before it (interval >= 2; its flip is held), or right
     * after the vblank before it (interval 1, the PoC rule). Pick the first
     * begin time still ahead. */
    double first = n >= 2 ? p->last_flip + (n - 2) * p->period
                          : p->last_flip + RMP_PACING_MARGIN;
    double step = n * p->period;
    double k = now < first ? 0 : floor((now - first) / step) + 1;
    p->not_before = first + k * step;
}

void rmp_pacing_presented(rmp_pacing *p, int pending)
{
    p->pending = pending;
    p->not_before = 0;
}

void rmp_pacing_flip_done(rmp_pacing *p, double t)
{
    p->pending = 0;
    p->have_flip = 1;
    p->last_flip = t;
}
