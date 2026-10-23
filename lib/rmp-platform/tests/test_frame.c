// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Frame contract of rmp_platform.c + rmp_gfx.c with a recording EGL/GL mock
 * and a fake vsync display (platform_harness.c). Checks the order of the
 * calls the PoC proved on the device and the pacing gate:
 *  1. whole-display frame: age query -> eglSetDamageRegionKHR -> (layer
 *     FBO work) -> bind display -> glClear with scissor off -> glScissor ->
 *     draws -> eglSwapBuffers -> page flip; damage region bottom-left;
 *  2. interval 2: no frame while the flip is pending; rmp_wait returns as
 *     soon as it completes;
 *  3. partial frame (age 1): rects converted to bottom-left, no clear, the
 *     batch is flushed before every scissor change, draws stay in scissor;
 *     interval 2: the flip is held until the vblank before the target;
 *  4. no damage: nothing swapped, rmp_wait sleeps to the next slot;
 *  5. age 2 after a partial frame: previous damage included; on the slot;
 *  6. interval 1: frames while the flip is queued, flips every vblank;
 *  7. sweep of the real loop: intervals 1..4, build 0.5..15 ms, GPU work
 *     0..15 ms after the swap, with and without idle frames: never earlier
 *     than the interval, no MISSED when it fits, MISSED equals the flips
 *     later than their target, hold <= 1 vblank; stats;
 *  8. drmWaitVBlank unavailable: the hold falls back to the clock, still
 *     never early;
 *  9. grabbed Ctrl+C -> quit; swap failure -> -1;
 * 10. layers: invalidate keeping the target, resume a layer without clear;
 * 11. shutdown: restore CRTC before GL/EGL teardown, then close;
 * 12. screen switches (interval 2->1, 1->2, 2->2) with a whole-display frame,
 *     after steady frames, idle frames or the app being busy: never earlier
 *     than the interval, MISSED = flips after their deadline (a frame begun
 *     after its slot has two vblanks from the vblank it began in). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_gl.h"
#include "platform_harness.h"
#include "rmp_gfx.h"
#include "rmp_pacing.h"

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define CHECK(c) do { if (!(c)) FAIL("line %d: %s", __LINE__, #c); } while (0)

/* Every draw after `from` must happen inside the scissor that was set for
 * one of the rects (GL coordinates), on the display framebuffer. */
static void draws_inside(int from, int to, const rmp_rect *r, int n)
{
    for (int i = from; i < to; i++) {
        const mock_event *e = &mock_log[i];
        if (e->type != EV_DRAW || e->fb != 0) continue;
        CHECK(e->scissor_on);
        int ok = 0;
        for (int k = 0; k < n; k++)
            if (e->sx == r[k].x && e->sy == 600 - (r[k].y + r[k].h) && e->sw == r[k].w && e->sh == r[k].h) ok = 1;
        if (!ok) FAIL("draw %d with scissor (%d,%d %dx%d) not a repaint rect", i, e->sx, e->sy, e->sw, e->sh);
    }
}

#define P_ H_PERIOD
#define WAKE 0.001
#define SLACK 0.0015

/* Settle a queued flip (it lands; the platform sees it). */
static void settle(void)
{
    if (!h_pending) return;
    if (h_pending_land > h_now) h_now = h_pending_land;
    (void)rmp_frame_ready();
}

typedef struct { int presented, late, late_cold, missed; double max_hold, min_gap, max_gap; } Run;

/* The documented loop with `build` seconds of drawing; every idle_every-th
 * frame has no damage. Checks every flip against the interval and against
 * the frame's rmp_frame_time(), and the platform's MISSED against them. */
static Run run_loop(int interval, double build, double gpu, int idle_every, int frames, int fits, int cold_fits)
{
    static double targets[512];
    static int cold[512];
    rmp_rect rects[RMP_REPAINT_MAX];
    Run r = { 0, 0, 0, 0, 0, 1e9, 0 };
    rmp_set_frame_interval(interval);
    h_gpu = gpu;
    settle();
    mock_gl_reset_log();
    unsigned long missed0 = h_missed_total();
    int flip0 = h_flips, waits0 = h_vblank_waits;
    for (int frame = 1; r.presented < frames; frame++) {
        int guard = 0;
        while (rmp_wait(0.5) != 1) CHECK(++guard < 100);
        double ft = rmp_frame_time();
        int idle = idle_every && frame % idle_every == 0;
        if (!idle) rmp_frame_damage(&(rmp_rect){ frame % 1000, 100, 4, 4 });
        int was_pending = h_pending;
        double hold0 = h_wait_ms;
        int n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
        if (idle) { CHECK(n == 0); CHECK(rmp_frame_end() == 0); continue; }
        CHECK(n >= 1);
        h_now += build;
        rmp_frame_clip(&rects[0]);
        CHECK(rmp_frame_end() == 0);
        double hold = (h_wait_ms - hold0) / 1000;
        if (hold > r.max_hold) r.max_hold = hold;
        targets[r.presented] = ft;
        cold[r.presented++] = !was_pending;
        mock_gl_reset_log();
    }
    settle();
    CHECK(h_flips - flip0 == r.presented);
    CHECK(h_wait_with_pending == 0);
    CHECK(h_vblank_waits - waits0 == (interval >= 2 ? r.presented : 0));
    for (int i = 0; i < r.presented; i++) {
        double land = h_flip_times[(flip0 + i) % 4096], prev = h_flip_times[(flip0 + i - 1) % 4096], gap = land - prev;
        if (gap < r.min_gap) r.min_gap = gap;
        if (gap > r.max_gap) r.max_gap = gap;
        if (gap < interval * P_ - 1e-6)
            FAIL("interval %d build %.1fms gpu %.0fms: flip %d only %.2f ms after the previous", interval,
                 build * 1000, gpu * 1000, i, gap * 1000);
        double want = targets[i] > prev + interval * P_ ? targets[i] : prev + interval * P_;
        if (land > want + 1e-6) { if (cold[i]) r.late_cold++; else r.late++; }
    }
    r.missed = (int)(h_missed_total() - missed0);
    if (r.missed != r.late + r.late_cold)
        FAIL("interval %d build %.1fms gpu %.0fms idle %d: MISSED %d but %d flips were later than their target",
             interval, build * 1000, gpu * 1000, idle_every, r.missed, r.late + r.late_cold);
    if (fits && r.late) FAIL("interval %d build %.1fms gpu %.0fms idle %d: %d late", interval, build * 1000, gpu * 1000, idle_every, r.late);
    if (cold_fits && r.late_cold) FAIL("interval %d build %.1fms gpu %.0fms idle %d: %d late (nothing queued)", interval, build * 1000, gpu * 1000, idle_every, r.late_cold);
    if (fits && cold_fits && !idle_every && fabs(r.max_gap - interval * P_) > 1e-6)
        FAIL("interval %d build %.1fms gpu %.0fms: gap %.2f ms", interval, build * 1000, gpu * 1000, r.max_gap * 1000);
    if (fits && cold_fits && idle_every && interval >= 2)   /* idle slots keep the interval grid */
        for (int i = 0; i < r.presented; i++) {
            double k = (h_flip_times[(flip0 + i) % 4096] - h_flip_times[(flip0 + i - 1) % 4096]) / (interval * P_);
            if (fabs(k - lround(k)) > 1e-6) FAIL("interval %d idle: gap of %.3f intervals", interval, k);
        }
    if (r.max_hold > P_ + WAKE + 1e-6) FAIL("hold %.2f ms is longer than one vblank", r.max_hold * 1000);
    return r;
}

/* ---- 12. screen switches: the interval changes and the frame redraws the
 * whole display (as app_main.cpp: the key is handled after rmp_wait returns,
 * activate sets the interval, rmp_frame_invalidate_all, then the frame). */
typedef struct {
    int frames;             /* presented */
    int missed, late;       /* platform MISSED / flips later than max(target, prev + interval) */
    int switch_late;        /* the switch frame's flip, vblanks after its target (0 = on time) */
    int switch_gap;         /* its flip, vblanks after the previous flip */
    int switch_wait;        /* vblanks from the key to the switch frame's begin */
} SwitchRun;

typedef struct {
    int from, to;
    double idle;            /* seconds of no damage before the key (idle frames) */
    double stall;           /* seconds the app is busy right before the key (no platform calls) */
    double work;            /* between rmp_wait returning and rmp_frame_begin (activate, update) */
    double build, gpu;      /* the switch frame */
    double build_after, gpu_after;
    double key_after;       /* > 0: after the idle frames, a real key this long after the slot began
                               (wakes rmp_wait mid-slot) */
} SwitchCase;

static SwitchRun switch_run(const SwitchCase *c)
{
    enum { N = 64 };
    double target[N], begun[N], last_at_begin[N];
    int ivl[N], idled[N], sw = -1;
    rmp_rect rects[RMP_REPAINT_MAX];
    SwitchRun r = { 0, 0, 0, -1, -1, -1 };
    rmp_set_frame_interval(c->from);
    settle();
    unsigned long missed0 = h_missed_total();
    int flip0 = h_flips;
    double key_at = 0;
    /* 20 frames before, the switch frame, 20 after. */
    for (int i = 0; r.frames < 41; i++) {
        CHECK(i < 10000);
        int is_switch = r.frames == 20 && sw < 0;
        if (is_switch && c->idle > 0) {   /* no damage for a while: idle frames */
            double until = h_now + c->idle;
            while (h_now < until) {
                while (rmp_wait(0.05) != 1) {}
                CHECK(rmp_frame_begin(rects, RMP_REPAINT_MAX) == 0);
                CHECK(rmp_frame_end() == 0);
            }
        }
        if (is_switch && c->stall > 0) h_now += c->stall;
        if (is_switch && c->key_after > 0) {
            h_now += c->key_after;
            h_key(30, 1);
            CHECK(rmp_wait(0.05) == 0);   /* woken by the key */
            rmp_key_event ev[4];
            CHECK(rmp_poll_keys(ev, 4) == 1);
        } else
            (void)rmp_wait(0.05);
        if (is_switch) {   /* the key: activate the other screen */
            key_at = h_now;
            rmp_set_frame_interval(c->to);
            rmp_frame_invalidate_all();
            h_now += c->work;
            sw = r.frames;
        }
        if (!rmp_frame_ready()) continue;
        if (r.frames != sw) rmp_frame_damage(&(rmp_rect){ i % 900, 100, 40, 40 });
        int iv = rmp_frame_interval();
        double ft = rmp_frame_time();    /* = the target rmp_frame_begin plans (same clock) */
        double b0 = h_now;
        CHECK(!h_pending || iv == 1);
        last_at_begin[r.frames] = h_flip_times[(h_flips - 1) % 4096];
        /* Idle frames planned a slot (not_before) - unless the interval then
         * changed, which drops it: the slot is interval after the last flip. */
        idled[r.frames] = r.frames == sw && c->idle > 0 && c->from == c->to;
        int n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
        CHECK(n >= 1);
        if (r.frames == sw) {
            CHECK(rmp_frame_is_full());
            r.switch_wait = (int)lround(floor((b0 - key_at) / P_ + 1e-6));
        }
        int before = r.frames < 20;
        h_gpu = r.frames == sw ? c->gpu : before ? 0.005 : c->gpu_after;
        h_now += r.frames == sw ? c->build : before ? 0.005 : c->build_after;
        rmp_frame_clip(&rects[0]);
        CHECK(rmp_frame_end() == 0);
        target[r.frames] = ft;
        begun[r.frames] = b0;
        ivl[r.frames++] = iv;
    }
    settle();
    CHECK(h_flips - flip0 == r.frames);
    CHECK(flip0 > 0);   /* flip 0 is judged against the flip before this run */
    for (int i = 0; i < r.frames; i++) {
        double land = h_flip_times[(flip0 + i) % 4096], prev = h_flip_times[(flip0 + i - 1) % 4096];
        if (land - prev < ivl[i] * P_ - 1e-6)
            FAIL("switch %d->%d: flip %d only %.2f ms after the previous (interval %d)", c->from, c->to, i,
                 (land - prev) * 1000, ivl[i]);
        if (land < begun[i]) FAIL("flip before its frame began");
        /* On time up to the later of its target and interval after the
         * previous flip; at interval >= 2, a frame begun after its slot
         * (interval after the last flip; no idle frames in between) has two
         * vblanks from the vblank it began in. */
        double want = target[i] > prev + ivl[i] * P_ ? target[i] : prev + ivl[i] * P_;
        double slot = last_at_begin[i] + ivl[i] * P_;
        if (ivl[i] >= 2 && !idled[i] && begun[i] >= slot - 1e-9) {
            double vb = last_at_begin[i] + floor((begun[i] - last_at_begin[i]) / P_ + 1e-9) * P_;
            if (vb + 2 * P_ > want) want = vb + 2 * P_;
        }
        if (land > want + 1e-6) r.late++;
        if (i == sw) {
            r.switch_late = (int)lround((land - want) / P_);
            r.switch_gap = (int)lround((land - prev) / P_);
        }
    }
    r.missed = (int)(h_missed_total() - missed0);
    h_gpu = 0;
    return r;
}

static void paint_rect(rmp_rect r)
{
    rmp_frame_clip(&r);
    rmp_paint_box(rmp_gfx_painter(), (float)r.x + 1, (float)r.y + 1, 4, 4, 1, rmp_hex(0xFF0000, 1));
    rmp_paint_text(rmp_gfx_painter(), (float)r.x + 2.3f, (float)r.y + 2, "AB", 10, rmp_hex(0xFFFFFF, 1), 0);
}

int main(void)
{
    rmp_platform_options o;
    rmp_platform_default_options(&o);
    CHECK(o.grab_keyboard && o.manage_terminal && o.install_signal_handlers && !o.log_stats && !o.card);
    CHECK(!o.title && !o.wm_instance && !o.wm_class && !o.icon && !o.icon_words);   /* rmp-jukebox's name, no icon */
    o.manage_terminal = 0;
    o.install_signal_handlers = 0;
    CHECK(rmp_platform_init(&o) == 0);
    CHECK(rmp_display_width() == 1024 && rmp_display_height() == 600);
    CHECK(mock_programs_linked == 6);
    int tex = mock_find(EV_TEXIMAGE, 0);
    CHECK(tex >= 0 && mock_log[tex].a == 1024 && mock_log[tex].b == 1024);
    tex = mock_find(EV_TEXIMAGE, tex + 1);
    CHECK(tex >= 0 && mock_log[tex].a == 768 && mock_log[tex].b == 768);
    CHECK(rmp_frame_interval() == 2);

    /* 1. Whole display (first frame), with a layer drawn first. */
    rmp_layer *layer = rmp_layer_create(1024, 576);
    CHECK(layer);
    mock_gl_reset_log();
    mock_age = 0;
    CHECK(rmp_wait(0.1) == 1 && rmp_frame_ready());
    rmp_rect rects[RMP_REPAINT_MAX];
    int n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
    CHECK(n == 1 && rects[0].x == 0 && rects[0].y == 0 && rects[0].w == 1024 && rects[0].h == 600 && rmp_frame_is_full());
    rmp_layer_begin(layer, 0x080C10);
    rmp_paint_box(rmp_gfx_static_painter(), 10, 10, 50, 20, 4, rmp_hex(0x123456, 1));
    rmp_layer_end();
    rmp_frame_clip(&rects[0]);
    rmp_layer_paste(layer, (rmp_rect){ 0, 0, 1024, 576 }, 0, 0);
    paint_rect((rmp_rect){ 100, 100, 50, 50 });
    rmp_frame_clip(&rects[0]);
    CHECK(rmp_frame_end() == 0);
    int age = mock_find(EV_AGE, 0), dmg = mock_find(EV_DAMAGE, 0), lfb = mock_find(EV_BINDFB, 0);
    int lclear = mock_find(EV_CLEAR, 0), dfb = mock_find(EV_BINDFB, lfb + 1), dclear = mock_find(EV_CLEAR, lclear + 1);
    int sc = mock_find(EV_SCISSOR, 0), draw = mock_find(EV_DRAW, 0), swap = mock_find(EV_SWAP, 0);
    int present = mock_find(EV_EXTERNAL, 0);
    CHECK(age >= 0 && dmg > age && lfb > dmg && mock_log[lfb].a != 0);          /* layer FBO */
    CHECK(lclear > lfb && mock_log[lclear].fb != 0 && !mock_log[lclear].scissor_on);
    CHECK(draw > lclear && mock_log[draw].fb != 0 && !mock_log[draw].scissor_on); /* layer content */
    CHECK(dfb > draw && mock_log[dfb].a == 0);
    CHECK(dclear > dfb && mock_log[dclear].fb == 0 && !mock_log[dclear].scissor_on);
    CHECK(mock_count(EV_CLEAR, dclear + 1, mock_log_n) == 0);                   /* cleared once */
    CHECK(sc > dclear && mock_log[sc].a == 0 && mock_log[sc].b == 0 && mock_log[sc].c == 1024 && mock_log[sc].d == 600);
    CHECK(swap > sc && present > swap && mock_find(EV_DRAW, sc) < swap);
    CHECK(mock_damage_n == 1 && mock_damage_rects[0] == 0 && mock_damage_rects[1] == 0 &&
          mock_damage_rects[2] == 1024 && mock_damage_rects[3] == 600);
    CHECK(h_modesets == 1 && !h_pending);
    puts("PASS 1: whole frame: age -> damage -> layer FBO (cleared) -> display clear (scissor off) -> scissor -> draws -> swap -> flip");

    /* 2. A second frame (flip) then the interval-2 gate. */
    rmp_frame_damage(&(rmp_rect){ 0, 0, 1, 1 });
    CHECK(rmp_wait(0.1) == 1);
    n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
    mock_age = 0;
    CHECK(n >= 1);
    rmp_frame_clip(&rects[0]);
    CHECK(rmp_frame_end() == 0 && h_pending);
    double queued_land = h_pending_land;
    rmp_frame_damage(&(rmp_rect){ 5, 5, 10, 10 });
    CHECK(!rmp_frame_ready());
    mock_gl_reset_log();
    CHECK(rmp_frame_begin(rects, RMP_REPAINT_MAX) == 0 && rmp_frame_end() == 0);
    CHECK(mock_find(EV_SWAP, 0) < 0 && mock_find(EV_AGE, 0) < 0);
    CHECK(rmp_wait(1.0) == 1);
    CHECK(h_flips >= 1 && fabs(h_flip_times[h_flips - 1] - queued_land) < 1e-9);
    CHECK(fabs(h_now - queued_land) < 1e-9);   /* begins as soon as the flip completed */
    CHECK(fabs(rmp_frame_time() - (queued_land + 2 * H_PERIOD)) < 1e-9);
    puts("PASS 2: interval 2: begin returns 0 while the flip is pending; rmp_wait returns when it completes; frame time = flip + 2 vblanks");

    /* 3. Partial frame, age 1: the pending (5,5,10,10) plus a second rect. */
    rmp_frame_damage(&(rmp_rect){ 500, 300, 50, 40 });
    mock_age = 1;
    mock_gl_reset_log();
    n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
    CHECK(n == 2 && !rmp_frame_is_full());
    CHECK(mock_damage_n == 2);
    for (int i = 0; i < 2; i++) {
        rmp_rect r = rects[i];
        int found = 0;
        for (int k = 0; k < 2; k++)
            if (mock_damage_rects[k * 4] == r.x && mock_damage_rects[k * 4 + 1] == 600 - (r.y + r.h) &&
                mock_damage_rects[k * 4 + 2] == r.w && mock_damage_rects[k * 4 + 3] == r.h) found = 1;
        CHECK(found);
    }
    for (int i = 0; i < n; i++) paint_rect(rects[i]);
    CHECK(rmp_frame_end() == 0);
    CHECK(mock_find(EV_CLEAR, 0) < 0);
    int s1 = mock_find(EV_SCISSOR, 0), s2 = mock_find(EV_SCISSOR, s1 + 1);
    CHECK(s1 >= 0 && s2 > s1 && mock_count(EV_DRAW, s1, s2) >= 1);   /* flushed before the 2nd scissor */
    draws_inside(0, mock_log_n, rects, n);
    /* Held until the vblank between, then queued: lands 2 vblanks after. */
    CHECK(h_vblank_waits == 1 && fabs(h_now - (queued_land + H_PERIOD)) < 1e-9);
    CHECK(h_pending && fabs(h_pending_land - (queued_land + 2 * H_PERIOD)) < 1e-9);
    CHECK(mock_find(EV_SWAP, 0) >= 0 && mock_find(EV_EXTERNAL, mock_find(EV_SWAP, 0)) > 0);
    puts("PASS 3: partial frame: bottom-left damage rects, no clear, batch flushed before each scissor, draws inside; "
         "swap at once, flip held to the middle vblank, lands flip + 2 vblanks");

    /* 4. No damage: nothing swapped; the wait goes to the next slot. */
    CHECK(rmp_wait(1.0) == 1);
    mock_gl_reset_log();
    double t0 = h_now;
    CHECK(rmp_frame_begin(rects, RMP_REPAINT_MAX) == 0 && rmp_frame_end() == 0);
    CHECK(mock_find(EV_SWAP, 0) < 0 && mock_find(EV_AGE, 0) < 0);
    CHECK(!rmp_frame_ready());
    CHECK(rmp_wait(1.0) == 1 && h_now >= t0 + 2 * H_PERIOD - 1e-9 && h_now < t0 + 2 * H_PERIOD + 0.0011);
    puts("PASS 4: idle frame: no swap, no EGL call; rmp_wait sleeps to the next 2-vblank slot");

    /* 5. Age 2 after the partial frame: previous damage is repainted too. */
    rmp_frame_damage(&(rmp_rect){ 900, 10, 20, 20 });
    mock_age = 2;
    n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
    CHECK(n == 3);
    rmp_frame_clip(&rects[0]);
    CHECK(rmp_frame_end() == 0);
    CHECK(fabs(h_pending_land - (t0 + 4 * H_PERIOD)) < 1e-9);   /* the slot after the skipped one */
    puts("PASS 5: age 2 includes the previous frame's two rects plus the new one; shown on the next slot");

    /* 6. Interval 1: begin while the flip is queued; flips every vblank. */
    rmp_set_frame_interval(1);
    int first = h_flips, waits_before = h_vblank_waits;
    mock_age = 3;
    for (int f = 0; f < 120; f++) {
        rmp_frame_damage(&(rmp_rect){ f, 0, 2, 2 });
        CHECK(rmp_wait(0.1) == 1);
        n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
        CHECK(n >= 1);
        h_now += 0.010;   /* 10 ms of drawing */
        rmp_frame_clip(&rects[0]);
        CHECK(rmp_frame_end() == 0);
        if (f > 2) CHECK(h_pending);
    }
    for (int i = first + 2; i < h_flips; i++)
        CHECK(fabs(h_flip_times[i] - h_flip_times[i - 1] - H_PERIOD) < 1e-6);
    rmp_frame_stats st = rmp_get_frame_stats();
    CHECK(st.interval == 1 && st.missed == 0 && fabs(st.fps - 60.0) < 1.5);
    CHECK(h_vblank_waits == waits_before && st.hold_ms == 0);   /* interval 1: no hold */
    printf("PASS 6: interval 1: %d flips one vblank apart while building 10 ms frames; stats fps %.2f missed %u build %.2f ms\n",
           h_flips - first, st.fps, st.missed, st.build_ms);

    /* 7. Sweep of the real loop. Fits: build + GPU in two vblanks (less the
     * wake-up slack); interval 1 also each in one vblank, 3 ms less after
     * idle frames, and frames begun with nothing queued in one vblank less
     * the margin (the unchanged PoC rule). */
    {
        const double builds[] = { 0.0005, 0.005, 0.010, H_PERIOD - RMP_PACING_MARGIN - 0.0005, 0.015 };
        const double gpus[] = { 0, 0.005, 0.010, 0.015 };
        int cases = 0, fitting = 0, late_total = 0;
        for (int iv = 1; iv <= 4; iv++)
            for (int b = 0; b < 5; b++)
                for (int g = 0; g < 4; g++)
                    for (int idle = 0; idle < 2; idle++) {
                        double bw = builds[b], gw = gpus[g];
                        int fits = iv >= 2 ? bw + gw <= 2 * P_ - SLACK
                                 : bw <= P_ - SLACK && gw <= P_ - SLACK &&
                                   bw + gw <= 2 * P_ - SLACK - (idle ? RMP_PACING_MARGIN : 0);
                        int cold_fits = iv >= 2 ? fits : bw + gw <= P_ - RMP_PACING_MARGIN - WAKE;
                        Run r = run_loop(iv, bw, gw, idle ? 3 : 0, 60, fits, cold_fits);
                        cases++;
                        fitting += fits && cold_fits;
                        late_total += r.missed;
                        if (iv == 2 && !idle && (b == 1 || b == 4))
                            printf("  interval 2 build %5.2fms gpu %4.1fms: gap %.2f..%.2f ms, MISSED %d, hold <= %.2f ms\n",
                                   bw * 1000, gw * 1000, r.min_gap * 1000, r.max_gap * 1000, r.missed, r.max_hold * 1000);
                    }
        h_gpu = 0;
        printf("PASS 7: %d sweeps x 60 frames (%d fit): never earlier than the interval; MISSED 0 when it fits; "
               "MISSED = flips later than their target (%d in the rest); hold <= 1 vblank\n", cases, fitting, late_total);
        /* Stats at interval 2: 5 ms build, the rest of the vblank held. */
        rmp_set_frame_interval(2);
        settle();
        double until = h_now + 2.5;
        while (h_now < until) {
            rmp_frame_damage(&(rmp_rect){ 0, 0, 2, 2 });
            while (rmp_wait(0.5) != 1) {}
            CHECK(rmp_frame_begin(rects, RMP_REPAINT_MAX) >= 1);
            h_now += 0.005;
            rmp_frame_clip(&rects[0]);
            CHECK(rmp_frame_end() == 0);
        }
        st = rmp_get_frame_stats();
        CHECK(st.interval == 2 && st.missed == 0 && fabs(st.fps - 30.0) < 1.0);
        CHECK(fabs(st.hold_ms - (H_PERIOD * 1000 - 5)) < 0.05 && st.swap_ms < 0.05 && fabs(st.build_ms - 5) < 0.05);
        printf("PASS 7 stats: interval 2, build 5 ms: fps %.2f missed %u build %.2f swap %.2f hold %.2f ms\n",
               st.fps, st.missed, st.build_ms, st.swap_ms, st.hold_ms);
    }

    /* 8. No drmWaitVBlank: hold by the clock (vblank before + margin). */
    h_vblank_fails = 1;
    {
        Run r = run_loop(2, 0.0005, 0, 0, 30, 1, 1);   /* fast frames: early unless held */
        CHECK(r.missed == 0 && fabs(r.min_gap - 2 * P_) < 1e-6 && r.max_hold == 0);
        r = run_loop(3, 0.005, 0.010, 3, 30, 1, 1);
        CHECK(r.missed == 0);
    }
    h_vblank_fails = 0;
    h_gpu = 0;
    puts("PASS 8: drmWaitVBlank failing: the flip is held by the clock (vblank before the target + margin), never early, no MISSED");

    /* 12. Screen switches (the device run 20261021-024041: MISSED 1 right
     * after LIST<->PARTS and after main->scope). Each case: 20 frames at
     * `from`, the key, one whole-display frame at `to`, 20 frames after.
     * Checked: never earlier than the interval across the change; MISSED
     * equals the flips that really landed after their target; a switch
     * frame that fits its budget is not counted; one that does not is
     * counted once (the frames after it are not). */
    {
        static const struct { const char *name; SwitchCase c; int missed; } cases[] = {
            /* fits: no MISSED from the interval change itself */
            { "main->scope 2->1, light",                { 2, 1, 0, 0, 0, .008, .008, .0076, .008, 0 }, 0 },
            { "main->scope 2->1, after 1 s idle",       { 2, 1, 1, 0, 0, .008, .008, .0076, .008, 0 }, 0 },
            { "main->scope 2->1, idle, key 5 ms into a slot", { 2, 1, 1, 0, 0, .008, .008, .0076, .008, .005 }, 0 },
            { "scope->main 1->2, idle, key 5 ms into a slot", { 1, 2, 1, 0, 0, .008, .010, .005, .005, .005 }, 0 },
            { "scope->main 1->2, light",                { 1, 2, 0, 0, 0, .008, .010, .005, .005, 0 }, 0 },
            { "LIST<->PARTS 2->2, full redraw",         { 2, 2, 0, 0, 0, .008, .015, .005, .005, 0 }, 0 },
            { "PARTS 2->2 after 1 s idle, full",        { 2, 2, 1, 0, 0, .008, .015, .005, .005, 0 }, 0 },
            /* does not fit: really late, counted once */
            { "main->scope 2->1, first frame 57 ms",    { 2, 1, 0, 0, 0, .057, .015, .0076, .008, 0 }, 1 },
            { "LIST 2->2, full redraw 74 ms",           { 2, 2, 0, 0, 0, .074, .005, .005, .005, 0 }, 1 },
            { "PARTS after 1 s idle, GPU 28 ms",        { 2, 2, 1, 0, 0, .005, .028, .005, .005, 0 }, 1 },
            { "PARTS after 1 s idle, 12 ms before begin + 20 ms GPU", { 2, 2, 1, 0, .012, .006, .020, .005, .005, 0 }, 1 },
            /* begun after its slot (the app was busy > 2 vblanks since the last
             * flip, no idle frames): shown at the next vblank, judged with the
             * two vblanks every interval >= 2 frame has from the vblank it began in */
            { "after a 1 s stall, light (10 ms), early in a vblank", { 2, 2, 0, 1.0004, 0, .005, .005, .005, .005, 0 }, 0 },
            { "after a 1 s stall, light (10 ms), mid-vblank",        { 2, 2, 0, 1.012, 0, .005, .005, .005, .005, 0 }, 0 },
            { "after a 1 s stall, light (10 ms), late in a vblank",  { 2, 2, 0, 1.016, 0, .005, .005, .005, .005, 0 }, 0 },
            { "after a 1 s stall, full redraw 12 ms + GPU 6 ms",     { 2, 2, 0, 1.008, 0, .012, .006, .005, .005, 0 }, 0 },
            { "after a 1 s stall, full redraw 40 ms",                { 2, 2, 0, 1.008, 0, .040, .005, .005, .005, 0 }, 1 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            SwitchRun r = switch_run(&cases[i].c);
            if (r.missed != r.late)
                FAIL("12 %s: MISSED %d but %d flips landed after their target", cases[i].name, r.missed, r.late);
            /* The key wakes the loop mid-slot: the switch frame begins at once
             * (2 -> 1: not at the old interval-2 slot). */
            if (cases[i].c.key_after > 0 && cases[i].c.to == 1 && r.switch_wait != 0)
                FAIL("12 %s: the switch frame began %d vblanks after the key", cases[i].name, r.switch_wait);
            if (r.missed != cases[i].missed)
                FAIL("12 %s: MISSED %d, expected %d (switch frame %d vblanks late, gap %d)", cases[i].name,
                     r.missed, cases[i].missed, r.switch_late, r.switch_gap);
            printf("  %-52s MISSED %d  switch frame: begun %d vblanks after the key, %d after the previous flip, %+d vs its deadline\n",
                   cases[i].name, r.missed, r.switch_wait, r.switch_gap, r.switch_late);
        }
        puts("PASS 12: interval 2->1, 1->2 and 2->2 switches with a whole-display frame: never earlier than the interval; "
             "MISSED only when the frame really lands after its target, once");
    }

    /* 9. Grabbed Ctrl+C, swap failure. */
    rmp_key_event ev[8];
    rmp_set_frame_interval(2);   /* queue a flip: interval 2 is not ready while it is pending */
    rmp_frame_damage(&(rmp_rect){ 0, 0, 3, 3 });
    while (!rmp_wait(0.1)) {}
    CHECK(rmp_frame_begin(rects, RMP_REPAINT_MAX) >= 1);
    rmp_frame_clip(&rects[0]);
    CHECK(rmp_frame_end() == 0);
    CHECK(h_pending && !rmp_frame_ready());
    h_key(29, 1); h_key(46, 1);
    double before = h_now;
    CHECK(rmp_wait(1.0) == 0 && h_now == before);   /* keys pending: return at once */
    CHECK(rmp_poll_keys(ev, 8) == 2 && rmp_quit_requested() && rmp_key_down(29));
    CHECK(rmp_wait(1.0) == 0 && h_now == before);   /* quit requested: never sleeps */
    h_clear_quit();
    h_key(29, 0); h_key(46, 0);
    CHECK(rmp_poll_keys(ev, 8) == 2 && !rmp_quit_requested());
    h_key(29, 1); h_key(46, 1); h_key(29, 0);       /* one batch: Ctrl released after C */
    CHECK(rmp_poll_keys(ev, 8) == 3 && rmp_quit_requested() && !rmp_key_down(29));
    h_clear_quit();
    h_key(46, 1);                                   /* C alone */
    CHECK(rmp_poll_keys(ev, 8) == 1 && !rmp_quit_requested());
    rmp_frame_damage(&(rmp_rect){ 0, 0, 3, 3 });
    while (!rmp_wait(0.1)) {}
    n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
    CHECK(n >= 1);
    mock_swap_fails = 1;
    CHECK(rmp_frame_end() == -1);
    mock_swap_fails = 0;
    puts("PASS 9: grabbed Ctrl+C sets quit; swap failure returns -1");

    /* 10. Layers: raw GL (the app's text) inside a layer, then back to it. */
    {
        rmp_layer *L = rmp_layer_create(64, 32);
        CHECK(L);
        mock_gl_reset_log();
        rmp_layer_begin(L, 0x101010);
        int bind = mock_find(EV_BINDFB, 0);
        CHECK(bind >= 0 && mock_log[bind].a != 0);
        GLuint fbo = (GLuint)mock_log[bind].a;
        rmp_paint_box(rmp_gfx_static_painter(), 2, 2, 20, 10, 3, rmp_hex(0x204060, 1));
        rmp_gfx_flush();
        glUseProgram(999);                       /* raw GL, as the app's text pass */
        glBindTexture(GL_TEXTURE_2D, 77);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        rmp_gfx_invalidate_state_keep_target();
        rmp_paint_box(rmp_gfx_static_painter(), 30, 2, 20, 10, 3, rmp_hex(0x406020, 1));
        rmp_gfx_flush();
        int draws = mock_count(EV_DRAW, 0, mock_log_n), last = -1;
        CHECK(draws == 3);
        for (int i = 0; i < mock_log_n; i++) {
            if (mock_log[i].type == EV_BINDFB) CHECK(mock_log[i].a == (int)fbo);   /* never the display */
            if (mock_log[i].type == EV_DRAW) { CHECK(mock_log[i].fb == fbo && !mock_log[i].scissor_on); last = i; }
        }
        CHECK(mock_log[last].program != 999);            /* gfx re-selected its program */
        CHECK(mock_count(EV_CLEAR, 0, mock_log_n) == 1); /* only rmp_layer_begin cleared */
        rmp_layer_end();
        CHECK(mock_log[mock_log_n - 1].type == EV_BINDFB && mock_log[mock_log_n - 1].a == 0);
        /* Resume: same FBO, no clear, scissor off; layer_end returns to the display. */
        glEnable(GL_SCISSOR_TEST);
        mock_gl_reset_log();
        rmp_layer_resume(L);
        rmp_paint_box(rmp_gfx_static_painter(), 2, 20, 20, 10, 3, rmp_hex(0x602040, 1));
        rmp_layer_end();
        int rb = mock_find(EV_BINDFB, 0), rd = mock_find(EV_DRAW, 0), re = mock_find(EV_BINDFB, rb + 1);
        CHECK(rb >= 0 && mock_log[rb].a == (int)fbo && rd > rb && mock_log[rd].fb == fbo && !mock_log[rd].scissor_on);
        CHECK(re > rd && mock_log[re].a == 0 && mock_find(EV_CLEAR, 0) < 0);
        /* The plain invalidate, by contrast, goes back to the display. */
        mock_gl_reset_log();
        rmp_layer_resume(L);
        rmp_gfx_invalidate_state();
        rmp_paint_box(rmp_gfx_painter(), 2, 2, 4, 4, 1, rmp_hex(0xFFFFFF, 1));
        rmp_gfx_flush();
        rd = mock_find(EV_DRAW, 0);
        CHECK(rd >= 0 && mock_log[rd].fb == 0);
        rmp_layer_resume(NULL);                          /* ignored */
        rmp_layer_destroy(L);
        puts("PASS 10: rmp_gfx_invalidate_state_keep_target stays in the layer (no rebind to the display, no clear, "
             "program re-selected); rmp_layer_resume draws into it again without clear, scissor off");
    }

    /* 13. A display the app sizes itself (FDTP on a wide screen). The mock
     * surface is 1024 x 600. */
    {
        int ow, oh;
        rmp_output_size(&ow, &oh);
        CHECK(ow == 1024 && oh == 600);
        CHECK(rmp_set_display_size(0, 600) < 0 && rmp_set_display_size(5000, 600) < 0);
        mock_gl_reset_log();
        CHECK(rmp_set_display_size(1280, 720) == 0);
        CHECK(rmp_display_width() == 1280 && rmp_display_height() == 720);
        int t = mock_find(EV_TEXIMAGE, 0), f = mock_find(EV_MAGFILTER, 0);
        CHECK(t >= 0 && mock_log[t].a == 1280 && mock_log[t].b == 720);   /* scaled down 0.8 */
        CHECK(f >= 0 && mock_log[f].a == GL_LINEAR);
        CHECK(rmp_wait(0.1) >= 0);
        while (!rmp_frame_ready()) CHECK(rmp_wait(0.1) >= 0);
        int k = rmp_frame_begin(rects, RMP_REPAINT_MAX);
        CHECK(k == 1 && rects[0].w == 1280 && rects[0].h == 720 && rmp_frame_is_full());
        paint_rect((rmp_rect){ 0, 0, 1280, 720 });
        CHECK(rmp_frame_end() == 0);
        mock_gl_reset_log();
        CHECK(rmp_set_display_size(512, 300) == 0);                         /* exactly 2x */
        t = mock_find(EV_TEXIMAGE, 0); f = mock_find(EV_MAGFILTER, 0);
        CHECK(t >= 0 && mock_log[t].a == 512 && mock_log[t].b == 300 && f >= 0 && mock_log[f].a == GL_NEAREST);
        mock_gl_reset_log();
        CHECK(rmp_set_display_size(1024, 600) == 0);                        /* the surface: 1:1 */
        CHECK(rmp_display_width() == 1024 && mock_find(EV_TEXIMAGE, 0) < 0);
        while (!rmp_frame_ready()) CHECK(rmp_wait(0.1) >= 0);
        k = rmp_frame_begin(rects, RMP_REPAINT_MAX);
        CHECK(k == 1 && rects[0].w == 1024 && rects[0].h == 600);
        CHECK(rmp_frame_end() == 0);
        puts("PASS 13: rmp_set_display_size: scaled texture of the new size (linear at 0.8, nearest at 2x), "
             "1:1 again at the surface's size, the next frame repaints all");
    }

    /* 11. Shutdown order. */
    rmp_layer_destroy(layer);
    mock_gl_reset_log();
    rmp_platform_shutdown();
    int restore = -1, close = -1;
    for (int i = 0; i < mock_log_n; i++)
        if (mock_log[i].type == EV_EXTERNAL) {
            if (mock_log[i].a == X_RESTORE) restore = i;
            if (mock_log[i].a == X_CLOSE) close = i;
        }
    int del = mock_find(EV_DELPROGRAM, 0), term = mock_find(EV_TERMINATE, 0);
    CHECK(restore >= 0 && del > restore && term > del && close > term && h_closed);
    rmp_platform_shutdown();   /* idempotent */
    puts("PASS 11: shutdown restores the CRTC first, then GL objects, EGL, KMS; second call is a no-op");
    return 0;
}
