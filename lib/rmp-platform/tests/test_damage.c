// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* rmp_damage: buffer-age regions must cover every pixel damaged in the
 * frames the buffer missed, never overlap, stay on screen and respect max.
 * Boundaries: age 0 / 1 / history / history+1, first frame, empty and
 * off-screen rects, per-frame overflow (> RMP_DAMAGE_RECTS), max = 1,
 * full frames inside the history. Random: 20000 regions checked per pixel. */
#include "rmp_damage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1024
#define H 600
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static unsigned long seed = 99;
static int rnd(int n) { seed = seed * 6364136223846793005ul + 1442695040888963407ul; return (int)((seed >> 33) % (unsigned long)n); }

static unsigned char need[W * H], got[W * H];
static void mark(unsigned char *m, rmp_rect r)
{
    rmp_rect s = rmp_rect_intersect(r, (rmp_rect){ 0, 0, W, H });
    for (int y = s.y; y < s.y + s.h; y++) memset(m + y * W + s.x, 1, (size_t)s.w);
}

static void check(const rmp_damage *d, int age, int max, const rmp_rect *frames_rects[], const int *counts,
                  const int *fulls, int frames_back, const char *what)
{
    rmp_rect out[64];
    int full = 0, n = rmp_damage_region(d, age, out, max, &full);
    memset(need, 0, sizeof need);
    memset(got, 0, sizeof got);
    int need_full = 0;
    for (int k = 0; k < frames_back; k++) {
        if (fulls[k]) need_full = 1;
        for (int i = 0; i < counts[k]; i++) mark(need, frames_rects[k][i]);
    }
    if (n < 0 || n > (max < 1 ? 1 : max)) FAIL("%s: count %d > max %d", what, n, max);
    for (int i = 0; i < n; i++) {
        rmp_rect r = out[i];
        if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > W || r.y + r.h > H)
            FAIL("%s: rect %d (%d,%d %dx%d) empty or off screen", what, i, r.x, r.y, r.w, r.h);
        for (int j = i + 1; j < n; j++)
            if (rmp_rect_intersects(r, out[j])) FAIL("%s: rects %d and %d overlap", what, i, j);
        mark(got, r);
    }
    if (need_full && !full) FAIL("%s: a full frame in range must give a full region", what);
    if (full && (n != 1 || out[0].w != W || out[0].h != H)) FAIL("%s: full must be one screen rect", what);
    for (int i = 0; i < W * H; i++)
        if (need[i] && !got[i]) FAIL("%s: pixel (%d,%d) damaged but not repainted", what, i % W, i / W);
}

int main(void)
{
    rmp_damage d;
    rmp_rect out[64];
    int full;
    rmp_damage_init(&d, W, H);
    if (rmp_damage_pending(&d)) FAIL("fresh tracker has damage");
    if (rmp_damage_region(&d, 0, out, 8, &full) != 1 || !full) FAIL("age 0 must be full");
    if (rmp_damage_region(&d, 1, out, 8, &full) != 0 || full) FAIL("age 1 with no damage: nothing");
    if (rmp_damage_region(&d, 2, out, 8, &full) != 1 || !full) FAIL("age 2 before any frame must be full");
    /* Empty, negative and off-screen rects are ignored; partial ones clipped. */
    rmp_damage_add(&d, (rmp_rect){ 10, 10, 0, 5 });
    rmp_damage_add(&d, (rmp_rect){ 10, 10, -3, 5 });
    rmp_damage_add(&d, (rmp_rect){ W, 0, 10, 10 });
    rmp_damage_add(&d, (rmp_rect){ -20, -20, 10, 10 });
    if (rmp_damage_pending(&d)) FAIL("empty/off-screen rects must be ignored");
    rmp_damage_add(&d, (rmp_rect){ W - 5, H - 5, 50, 50 });
    if (rmp_damage_region(&d, 1, out, 8, &full) != 1 || out[0].x != W - 5 || out[0].w != 5 || out[0].h != 5)
        FAIL("partly off-screen rect must be clipped");
    rmp_damage_commit(&d);
    /* History boundaries. */
    for (int f = 0; f < RMP_DAMAGE_HISTORY + 2; f++) {
        rmp_damage_add(&d, (rmp_rect){ f * 10, 0, 5, 5 });
        rmp_damage_commit(&d);
    }
    rmp_damage_add(&d, (rmp_rect){ 500, 500, 4, 4 });
    if (rmp_damage_region(&d, RMP_DAMAGE_HISTORY, out, 64, &full) < 1 || full) FAIL("age == history must be partial");
    if (rmp_damage_region(&d, RMP_DAMAGE_HISTORY + 1, out, 64, &full) != 1 || !full) FAIL("age > history must be full");
    if (rmp_damage_region(&d, -1, out, 64, &full) != 1 || !full) FAIL("negative age must be full");
    /* max = 1: one bounding rectangle. */
    if (rmp_damage_region(&d, 3, out, 1, &full) != 1) FAIL("max 1");
    rmp_damage_commit(&d);
    puts("PASS boundaries: age 0/-1/history+1 full, age 2 before 2 frames full, age==history partial, "
         "empty/negative/off-screen ignored, partly off-screen clipped, max 1");

    /* Random: frames of random rects (sometimes > RMP_DAMAGE_RECTS, sometimes full). */
    static rmp_rect hist[RMP_DAMAGE_HISTORY + 1][200];
    int counts[RMP_DAMAGE_HISTORY + 1], fulls[RMP_DAMAGE_HISTORY + 1];
    rmp_damage_init(&d, W, H);
    int committed = 0, checks = 0;
    for (int f = 0; f < 2500; f++) {
        /* Shift history: index 0 = current frame. */
        memmove(hist[1], hist[0], sizeof hist[0] * RMP_DAMAGE_HISTORY);
        memmove(counts + 1, counts, sizeof counts[0] * RMP_DAMAGE_HISTORY);
        memmove(fulls + 1, fulls, sizeof fulls[0] * RMP_DAMAGE_HISTORY);
        int n = rnd(10) == 0 ? 60 + rnd(140) : rnd(20);
        counts[0] = n;
        fulls[0] = rnd(60) == 0;
        for (int i = 0; i < n; i++) {
            rmp_rect r = { rnd(W + 40) - 20, rnd(H + 40) - 20, rnd(rnd(4) ? 60 : 600), rnd(rnd(4) ? 60 : 400) };
            hist[0][i] = r;
            rmp_damage_add(&d, r);
        }
        if (fulls[0]) rmp_damage_add_full(&d);
        for (int t = 0; t < 8; t++) {
            int age = 1 + rnd(RMP_DAMAGE_HISTORY);
            int max = rnd(3) ? 32 : 1 + rnd(4);
            if (age - 1 > committed) continue;   /* full: checked above */
            const rmp_rect *rects[RMP_DAMAGE_HISTORY + 1];
            for (int k = 0; k < age; k++) rects[k] = hist[k];
            char what[64];
            snprintf(what, sizeof what, "frame %d age %d max %d", f, age, max);
            check(&d, age, max, rects, counts, fulls, age, what);
            checks++;
        }
        rmp_damage_commit(&d);
        committed++;
    }
    printf("PASS random: %d regions over 2500 frames (0..199 rects per frame, overflow > %d merged, "
           "1/60 full frames, max 1..32) cover every damaged pixel, never overlap, stay on screen\n",
           checks, RMP_DAMAGE_RECTS);
    return 0;
}
