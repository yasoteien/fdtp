// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* rmp_pacing on a simulated vsync display: vblanks at k*period; a flip
 * queued at q lands on the first vblank strictly after q, and not before
 * the GPU has finished the frame (the kernel's implicit fence). The loop
 * mirrors rmp_wait / rmp_frame_begin / rmp_frame_end:
 *  - wait until due (time waits wake up to 1 ms late, like poll's ceil) or
 *    for the pending flip;
 *  - build for b seconds, swap; the GPU then renders for g seconds (one
 *    frame at a time);
 *  - interval >= 2: hold the flip until the vblank before the target
 *    (rmp_pacing_target); interval 1: wait for the previous flip (PoC);
 *  - queue the flip.
 * Checks, for intervals 1..4, build 0.5..20 ms, GPU 0..20 ms, with and
 * without idle (no-damage) frames:
 *  - consecutive flips are never closer than interval vblanks;
 *  - no flip is later than its target when build + GPU fit in two vblanks
 *    (less 1.5 ms of wake-up slack) - for interval 1 also each of them in
 *    one vblank, 3 ms less after idle frames, and frames begun with no flip
 *    queued (first / after idle) get one vblank less the margin, as before;
 *  - without idle frames and when it fits, flips are exactly interval apart;
 *  - the hold before the flip is at most one vblank (+ wake-up slack);
 *  - idle frames do not spin and keep the interval grid.
 * Plus clamping of the interval (0, -5, 5, 100) and target times. */
#include "rmp_pacing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
static const double P = 1344.0 * 635.0 / 51200000.0;   /* 16.67 ms, the mock LVDS mode */
#define WAKE 0.001     /* poll timeouts are whole milliseconds (ceil) */
#define SLACK 0.0015

static double land(double q) { return (floor(q / P + 1e-9) + 1) * P; }
static double wake(double now, double due) { return now + ceil((due - now) * 1000) / 1000;   /* as rmp_wait */ }

typedef struct {
    double min_gap, max_gap, max_hold;
    int flips, frames, idles, loops;
    int late_hot, late_cold;     /* later than target: begun while a flip was queued
                                    (interval 1) / begun with nothing queued */
} Result;

static Result run(int interval, double build, double gpu, int idle_every, double seconds)
{
    rmp_pacing p;
    rmp_pacing_init(&p, P);
    rmp_pacing_set_interval(&p, interval);
    double t = 0.003, pending_at = -1, last = -1, gpu_free = 0;
    double pending_target = 0;
    int pending_cold = 0;
    Result r = { 1e9, 0, 0, 0, 0, 0, 0, 0, 0 };
    int frame = 0;
    while (t < seconds) {
        if (++r.loops > 100000) FAIL("interval %d: loop spins", interval);
        if (pending_at >= 0 && pending_at <= t) {   /* flip event handled */
            if (last >= 0) {
                double gap = pending_at - last, want = pending_target;
                if (gap < r.min_gap) r.min_gap = gap;
                if (gap > r.max_gap) r.max_gap = gap;
                if (last + interval * P > want) want = last + interval * P;
                if (pending_at > want + 1e-6) { if (pending_cold) r.late_cold++; else r.late_hot++; }
            }
            rmp_pacing_flip_done(&p, pending_at);
            last = pending_at;
            pending_at = -1;
            r.flips++;
        }
        double due = rmp_pacing_due(&p, t);
        if (due > t) {                               /* rmp_wait sleeps */
            double next = isinf(due) ? pending_at : wake(t, due);
            if (pending_at >= 0 && pending_at < next) next = pending_at;
            if (!(next > t)) FAIL("interval %d: wait does not advance (t %.6f due %.6f)", interval, t, due);
            t = next;
            continue;
        }
        double target = rmp_pacing_target(&p, t);
        if (!(target > t)) FAIL("target not in the future");
        if (p.have_flip) {
            double k = (target - p.last_flip) / P;
            if (fabs(k - lround(k)) > 1e-6) FAIL("target off the vblank grid");
            if (target < p.last_flip + (interval + p.pending) * P - 1e-6) FAIL("target earlier than the interval");
        }
        frame++;
        if (idle_every && frame % idle_every == 0) {  /* no damage: skipped */
            rmp_pacing_idle(&p, t);
            if (!(p.not_before > t)) FAIL("idle must schedule a later slot");
            r.idles++;
            continue;
        }
        int cold = pending_at < 0;
        t += build;                                  /* draw, then eglSwapBuffers */
        double start = t > gpu_free ? t : gpu_free;
        gpu_free = start + gpu;
        if (interval >= 2 && p.have_flip && t < target - P) {   /* hold until vblank target-1 */
            if (target - P - t > r.max_hold) r.max_hold = target - P - t;
            t = target - P;
        }
        if (pending_at >= 0) {                       /* interval 1: wait for previous flip */
            if (pending_at > t) t = pending_at;
            double gap = pending_at - last, want = pending_target;
            if (last >= 0) {
                if (gap < r.min_gap) r.min_gap = gap;
                if (gap > r.max_gap) r.max_gap = gap;
                if (last + interval * P > want) want = last + interval * P;
                if (pending_at > want + 1e-6) { if (pending_cold) r.late_cold++; else r.late_hot++; }
            }
            rmp_pacing_flip_done(&p, pending_at);
            last = pending_at;
            pending_at = -1;
            r.flips++;
        }
        pending_at = land(t > gpu_free ? t : gpu_free);
        pending_target = target;
        pending_cold = cold;
        rmp_pacing_presented(&p, 1);
        r.frames++;
    }
    return r;
}

int main(void)
{
    rmp_pacing p;
    rmp_pacing_init(&p, 0);
    if (fabs(p.period - 1.0 / 60) > 1e-12) FAIL("bad period must fall back to 60 Hz");
    const int in[] = { 0, -5, 1, 2, 3, 4, 5, 100 }, want[] = { 1, 1, 1, 2, 3, 4, 4, 4 };
    for (int i = 0; i < 8; i++) {
        rmp_pacing_set_interval(&p, in[i]);
        if (p.interval != want[i]) FAIL("interval %d clamps to %d, got %d", in[i], want[i], p.interval);
    }
    puts("PASS clamp: 0,-5 -> 1; 5,100 -> 4; period 0 -> 1/60 s");
    const double builds[] = { 0.0005, 0.005, 0.010, P - RMP_PACING_MARGIN - 0.0005, 0.015, 0.020 };
    const double gpus[] = { 0, 0.005, 0.010, 0.015, 0.020 };
    int cases = 0, fitting = 0;
    for (int interval = 1; interval <= 4; interval++)
        for (int b = 0; b < 6; b++)
            for (int g = 0; g < 5; g++)
                for (int idle = 0; idle < 2; idle++) {
                    double build = builds[b], gw = gpus[g];
                    Result r = run(interval, build, gw, idle ? 3 : 0, 5.0);
                    double want_gap = interval * P;
                    cases++;
                    if (r.min_gap < want_gap - 1e-6)
                        FAIL("interval %d build %.1fms gpu %.0fms: flips %.2fms apart (< %.2fms)",
                             interval, build * 1000, gw * 1000, r.min_gap * 1000, want_gap * 1000);
                    if (r.max_hold > P + WAKE + 1e-9)
                        FAIL("interval %d: hold %.2fms longer than one vblank", interval, r.max_hold * 1000);
                    /* Interval 1 (unchanged PoC rule): after an idle frame the next one
                     * begins RMP_PACING_MARGIN after a vblank, 3 ms less to spend. */
                    int fits = interval >= 2 ? build + gw <= 2 * P - SLACK
                             : build <= P - SLACK && gw <= P - SLACK &&
                               build + gw <= 2 * P - SLACK - (idle ? RMP_PACING_MARGIN : 0);
                    int cold_fits = interval >= 2 ? fits : build + gw <= P - RMP_PACING_MARGIN - WAKE;
                    if (fits && r.late_hot)
                        FAIL("interval %d build %.1fms gpu %.0fms idle %d: %d flips later than their target",
                             interval, build * 1000, gw * 1000, idle, r.late_hot);
                    if (cold_fits && r.late_cold)
                        FAIL("interval %d build %.1fms gpu %.0fms idle %d: %d frames begun with nothing queued are late",
                             interval, build * 1000, gw * 1000, idle, r.late_cold);
                    if (fits && !idle && !r.late_cold && fabs(r.max_gap - want_gap) > 1e-6)
                        FAIL("interval %d build %.1fms gpu %.0fms: gaps up to %.2fms, expected exactly %.2fms",
                             interval, build * 1000, gw * 1000, r.max_gap * 1000, want_gap * 1000);
                    if (fits && cold_fits && idle && fmod(r.max_gap / P + 1e-6, interval) > 2e-6)
                        FAIL("interval %d with idle frames: gap %.2fms is off the %d-vblank grid", interval, r.max_gap * 1000, interval);
                    fitting += fits;
                    printf("interval %d build %5.2fms gpu %4.1fms idle %d: %4d flips, gap %6.2f..%6.2f ms, late %3d queued-behind + %3d begun-idle, hold <= %5.2f ms%s\n",
                           interval, build * 1000, gw * 1000, idle, r.flips, r.min_gap * 1000, r.max_gap * 1000,
                           r.late_hot, r.late_cold, r.max_hold * 1000,
                           fits ? "" : "  [does not fit: later, never earlier]");
                }
    printf("PASS sweep: %d cases (%d fit in two vblanks): never earlier than the interval; no flip later than its target when it fits; hold <= 1 vblank\n",
           cases, fitting);
    /* Target and due times. */
    rmp_pacing_init(&p, P);
    rmp_pacing_set_interval(&p, 2);
    if (fabs(rmp_pacing_target(&p, 1.0) - (1.0 + P)) > 1e-9) FAIL("target without flips");
    rmp_pacing_flip_done(&p, 10 * P);
    if (fabs(rmp_pacing_due(&p, 10 * P + .001) - 10 * P) > 1e-9) FAIL("interval 2 begins right after the flip");
    if (fabs(rmp_pacing_target(&p, 10 * P + .004) - 12 * P) > 1e-9) FAIL("target interval 2");
    if (fabs(rmp_pacing_target(&p, 11 * P + .004) - 12 * P) > 1e-9) FAIL("target interval 2, begun after the middle vblank");
    if (fabs(rmp_pacing_target(&p, 12 * P + .004) - 13 * P) > 1e-9) FAIL("late frame: the next vblank, not the next slot");
    /* Deadline (MISSED): the target, except for a frame begun after its slot. */
    if (fabs(rmp_pacing_deadline(&p, 10 * P + .004) - 12 * P) > 1e-9) FAIL("deadline = target on the slot");
    if (fabs(rmp_pacing_deadline(&p, 11 * P + .004) - 12 * P) > 1e-9) FAIL("deadline = target, begun after the middle vblank");
    if (fabs(rmp_pacing_deadline(&p, 12 * P + .004) - 14 * P) > 1e-9) FAIL("late frame: two vblanks from the one it began in");
    if (fabs(rmp_pacing_deadline(&p, 70 * P + .015) - 72 * P) > 1e-9) FAIL("late frame after a stall, late in the vblank");
    rmp_pacing_set_interval(&p, 4);
    if (fabs(rmp_pacing_due(&p, 10 * P) - 12 * P) > 1e-9) FAIL("interval 4 begins two vblanks before the target");
    if (fabs(rmp_pacing_target(&p, 12 * P + .001) - 14 * P) > 1e-9) FAIL("target interval 4");
    rmp_pacing_idle(&p, 12 * P + .001);   /* slot 14P skipped: next slot 18P, begin at 16P */
    if (fabs(p.not_before - 16 * P) > 1e-9 || fabs(rmp_pacing_target(&p, 16 * P + .001) - 18 * P) > 1e-9)
        FAIL("interval 4 idle slot");
    if (fabs(rmp_pacing_deadline(&p, 16 * P + .001) - 18 * P) > 1e-9) FAIL("interval 4 idle slot: deadline = target");
    if (fabs(rmp_pacing_deadline(&p, 18 * P + .001) - 20 * P) > 1e-9) FAIL("interval 4, begun after the idle slot");
    rmp_pacing_set_interval(&p, 2);
    rmp_pacing_presented(&p, 1);
    if (!isinf(rmp_pacing_due(&p, 0))) FAIL("interval 2 must wait for a pending flip");
    rmp_pacing_set_interval(&p, 1);
    if (fabs(rmp_pacing_target(&p, 10 * P + .004) - 12 * P) > 1e-9) FAIL("interval 1 target while pending");
    if (fabs(rmp_pacing_deadline(&p, 10 * P + .004) - 12 * P) > 1e-9) FAIL("interval 1: deadline = target");
    /* Changing the interval drops an idle slot planned with the old one. */
    rmp_pacing_init(&p, P);
    rmp_pacing_set_interval(&p, 2);
    rmp_pacing_flip_done(&p, 10 * P);
    rmp_pacing_idle(&p, 10 * P + .005);              /* interval 2 slot: begin at 12P */
    rmp_pacing_set_interval(&p, 2);                  /* same interval: kept */
    if (fabs(p.not_before - 12 * P) > 1e-9) FAIL("same interval keeps the idle slot");
    rmp_pacing_set_interval(&p, 1);                  /* 2 -> 1 (F9): begin now, not at 12P */
    if (p.not_before != 0 || rmp_pacing_due(&p, 10 * P + .006) > 10 * P + .006) FAIL("2 -> 1 waits for the old slot");
    if (fabs(rmp_pacing_target(&p, 10 * P + .006) - 12 * P) > 1e-9) FAIL("2 -> 1 target");
    rmp_pacing_init(&p, P);
    rmp_pacing_set_interval(&p, 1);
    rmp_pacing_flip_done(&p, 10 * P);
    rmp_pacing_idle(&p, 10 * P + .004);              /* interval 1 slot: 11P + margin */
    rmp_pacing_set_interval(&p, 2);                  /* 1 -> 2: target on the vblank grid */
    {
        double t = rmp_pacing_target(&p, 11 * P + .004), k = (t - 10 * P) / P;
        if (fabs(k - lround(k)) > 1e-6 || fabs(t - 12 * P) > 1e-9)
            FAIL("1 -> 2 after an interval 1 idle frame: target %.3f vblanks after the flip", k);
        if (rmp_pacing_due(&p, 11 * P + .004) > 10 * P + 1e-9) FAIL("1 -> 2 due");
    }
    if (rmp_pacing_due(&p, 10 * P + .004) > 10 * P + .004) FAIL("interval 1 begins while pending");
    puts("PASS: due/target: interval 2 begins at the flip, interval 4 two vblanks before the target, "
         "late frames take the next vblank (on time up to the one after), idle slots stay on the grid, interval 1 unchanged, an interval change drops the old idle slot");
    return 0;
}
