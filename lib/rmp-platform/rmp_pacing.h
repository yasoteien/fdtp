// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Frame pacing: when may the next frame begin, and which vblank should it be
 * shown on, so that its vsync-aligned flip lands `interval` vblanks after
 * the previous one (never earlier). Pure C, tested on the Mac. Times are
 * CLOCK_MONOTONIC seconds; vblank k after the last flip is at
 * last_flip + k * period.
 *
 * interval 1: exactly the PoC loop (build while the previous flip is queued;
 *   rmp_frame_end waits for it). Not gated by time.
 * interval n >= 2: a frame is shown on target D = last_flip + n*period (or
 *   the idle slot, or a later vblank if it begins late). It may begin at
 *   D - 2*period (for n = 2: as soon as the previous flip completed), and
 *   the platform HOLDS its page flip until vblank D - period has passed
 *   (rmp_platform.c: drmWaitVBlank on that sequence), so it cannot latch
 *   earlier than D. Building, swapping and the GPU work after the swap get
 *   about two vblanks (33 ms), like interval 1, while the hold in
 *   rmp_frame_end is at most about one vblank.
 *   (Until 2026-10-21 a frame began only (n-1) vblanks + 3 ms after the
 *   flip and was queued at once: build + GPU had to fit in ~13.6 ms.) */
#ifndef RMP_PACING_H
#define RMP_PACING_H

#ifdef __cplusplus
extern "C" {
#endif

/* Seconds after a vblank before a time-based decision may assume it has
 * passed: covers the vertical blank (~0.9 ms on the 1024x600 LVDS mode) and
 * timestamp jitter. Used by interval 1 after an idle frame and by the
 * time-based hold when drmWaitVBlank is unavailable. */
#define RMP_PACING_MARGIN 0.003

typedef struct {
    int interval;        /* 1..4 */
    double period;       /* seconds per vblank */
    int have_flip;       /* last_flip is valid */
    double last_flip;    /* vblank time of the latest completed flip */
    int pending;         /* a flip is queued */
    double not_before;   /* after an idle (skipped) frame; 0 = none */
} rmp_pacing;

void   rmp_pacing_init(rmp_pacing *p, double period);
/* Clamps 1..4. A change drops the idle slot (not_before): it was planned
 * with the old interval (2026-10-21). */
void   rmp_pacing_set_interval(rmp_pacing *p, int vblanks);
/* Earliest time a frame may begin (<= now: ready). +INFINITY while a flip
 * that must complete first is pending (interval >= 2). */
double rmp_pacing_due(const rmp_pacing *p, double now);
/* Display time (a vblank on the last_flip grid) of a frame begun at `now`.
 * interval >= 2: the platform holds the flip until target - period. */
double rmp_pacing_target(const rmp_pacing *p, double now);
/* The latest vblank at which a frame begun at `now` is on time (MISSED
 * otherwise): the target, except at interval >= 2 for a frame begun after
 * its slot had passed (target = the next vblank): one vblank later, so it
 * has the two vblanks from the one it began in that a frame begun on its
 * slot has. Display (the hold) still uses the target. */
double rmp_pacing_deadline(const rmp_pacing *p, double now);
/* A frame was skipped for lack of damage: wait for the next slot. */
void   rmp_pacing_idle(rmp_pacing *p, double now);
/* A frame was queued for display (pending=1 unless it was a modeset). */
void   rmp_pacing_presented(rmp_pacing *p, int pending);
/* A flip completed at vblank time t. */
void   rmp_pacing_flip_done(rmp_pacing *p, double t);

#ifdef __cplusplus
}
#endif
#endif
