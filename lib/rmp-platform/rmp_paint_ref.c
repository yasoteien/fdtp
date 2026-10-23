// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rmp_paint_ref.h"

#include <math.h>
#include <stddef.h>

static float clamp01(float x) { return fminf(1, fmaxf(0, x)); }

static void ref_shape(void *ctx, const rmp_shape *s)
{
    rmp_paint_ref *r = ctx;
    float pad = 1;
    int x0 = (int)floorf(s->x - pad), y0 = (int)floorf(s->y - pad);
    int x1 = (int)ceilf(s->x + s->w + pad), y1 = (int)ceilf(s->y + s->h + pad);
    if (x0 < r->clip.x0) x0 = r->clip.x0;
    if (y0 < r->clip.y0) y0 = r->clip.y0;
    if (x1 > r->clip.x1) x1 = r->clip.x1;
    if (y1 > r->clip.y1) y1 = r->clip.y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = (float)x + .5f - s->x, py = (float)y + .5f - s->y;
            float qx = fabsf(px - s->w * .5f) - (s->w * .5f - s->radius);
            float qy = fabsf(py - s->h * .5f) - (s->h * .5f - s->radius);
            float distance = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - s->radius;
            float u = clamp01((distance + .6f) / 1.2f), coverage = 1 - u * u * (3 - 2 * u);
            float t = clamp01(py / s->h);
            float a = (s->top.a + (s->bottom.a - s->top.a) * t) * coverage;
            uint32_t *dst = &r->pixels[(size_t)y * r->stride + (unsigned)x], old = *dst, v = 0xff000000u;
            const float top[3] = { s->top.r, s->top.g, s->top.b };
            const float bottom[3] = { s->bottom.r, s->bottom.g, s->bottom.b };
            for (int k = 0; k < 3; k++) {
                int shift = 16 - 8 * k;
                float c = top[k] + (bottom[k] - top[k]) * t;
                v |= (uint32_t)lroundf(c * 255 * a + (float)((old >> shift) & 255) * (1 - a)) << shift;
            }
            *dst = v;
        }
}

void rmp_paint_ref_poly_pixels(uint32_t *pixels, unsigned stride, rmp_box clip,
                               const float *xy, int n, float expand, rmp_color c)
{
    if (n < 3 || n > RMP_POLY_MAX) return;
    float area = 0, minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
        minx = fminf(minx, xy[i * 2]); maxx = fmaxf(maxx, xy[i * 2]);
        miny = fminf(miny, xy[i * 2 + 1]); maxy = fmaxf(maxy, xy[i * 2 + 1]);
    }
    float sign = area > 0 ? 1.f : -1.f, nx[RMP_POLY_MAX], ny[RMP_POLY_MAX], off[RMP_POLY_MAX];
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ex = xy[j * 2] - xy[i * 2], ey = xy[j * 2 + 1] - xy[i * 2 + 1], l = hypotf(ex, ey);
        if (l <= 0) return;
        /* Inward unit normal; distance(p) = n.p - off. */
        nx[i] = -ey / l * sign; ny[i] = ex / l * sign;
        off[i] = nx[i] * xy[i * 2] + ny[i] * xy[i * 2 + 1];
    }
    float pad = expand + 1;
    int x0 = (int)floorf(minx - pad), y0 = (int)floorf(miny - pad);
    int x1 = (int)ceilf(maxx + pad), y1 = (int)ceilf(maxy + pad);
    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = (float)x + .5f, py = (float)y + .5f, d = 1e9f;
            for (int i = 0; i < n; i++) d = fminf(d, nx[i] * px + ny[i] * py - off[i]);
            float a = fminf(1, fmaxf(0, .5f + d + expand)) * c.a;
            if (a <= 0) continue;
            uint32_t *dst = &pixels[(size_t)y * stride + (size_t)x], old = *dst;
            unsigned rr = (unsigned)lroundf(c.r * 255 * a + (float)((old >> 16) & 255) * (1 - a));
            unsigned gg = (unsigned)lroundf(c.g * 255 * a + (float)((old >> 8) & 255) * (1 - a));
            unsigned bb = (unsigned)lroundf(c.b * 255 * a + (float)(old & 255) * (1 - a));
            *dst = 0xff000000u | (rr << 16) | (gg << 8) | bb;
        }
}

static void ref_poly(void *ctx, const float *xy, int n, float expand, rmp_color c)
{
    rmp_paint_ref *r = ctx;
    rmp_paint_ref_poly_pixels(r->pixels, r->stride, r->clip, xy, n, expand, c);
}

void rmp_paint_ref_init(rmp_paint_ref *r, uint32_t *pixels, unsigned stride, int w, int h)
{
    r->pixels = pixels;
    r->stride = stride;
    r->clip = (rmp_box){ 0, 0, w, h };
    r->painter.ctx = r;
    r->painter.shape = ref_shape;
    r->painter.poly = ref_poly;
    r->painter.glyph = NULL;
    r->painter.fill_rects = NULL;
}

void rmp_paint_ref_fill(uint32_t *pixels, unsigned stride, rmp_box a, uint32_t v)
{
    for (int y = a.y0; y < a.y1; y++)
        for (int x = a.x0; x < a.x1; x++) pixels[(size_t)y * stride + (size_t)x] = v;
}
