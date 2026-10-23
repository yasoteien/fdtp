// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "novel_glyph.h"
#include "font24.h"
#include "font12.h"
#include "cells.h"
#include <stddef.h>

typedef struct { const uint16_t *rows; int w, h, form, shift; } Sampler;

static int bit(const Sampler *s, int x, int y)
{
    return x >= 0 && x < s->w && y >= 0 && y < s->h && ((s->rows[y] >> (s->w - 1 - x)) & 1);
}
/* Source bit in the transformed glyph space. */
static int sample(const Sampler *s, int tx, int ty)
{
    switch (s->form) {
    case NG_ROTATE: return bit(s, ty, s->h - 1 - tx);           /* 90 degrees clockwise */
    case NG_PUNCT: return bit(s, tx - s->shift, ty + s->shift); /* lower left -> upper right */
    case NG_SMALL: return bit(s, tx - s->shift, ty + s->shift);
    default: return bit(s, tx, ty);
    }
}
static void put(Canvas *c, int x, int y, uint32_t rgb, int n, int total)
{
    if (x < 0 || y < 0 || x >= CANVAS_W || y >= CANVAS_H || !n) return;
    unsigned char *p = CANVAS_PX(c, x, y);
    for (int i = 0; i < 3; ++i) {
        int f = (int)((rgb >> (16 - 8 * i)) & 255);
        p[i] = (unsigned char)((f * n + p[i] * (total - n)) / total);
    }
    p[3] = 255;
}
/* Area-averaged scaling by num/den with k*k samples per output pixel. */
static void render(Canvas *c, int x, int y, const Sampler *s, int num, int den, int k, uint32_t rgb)
{
    int tw = s->form == NG_ROTATE ? s->h : s->w, th = s->form == NG_ROTATE ? s->w : s->h;
    int ow = tw * num / den, oh = th * num / den;
    for (int oy = 0; oy < oh; ++oy)
        for (int ox = 0; ox < ow; ++ox) {
            int n = 0;
            for (int j = 0; j < k; ++j)
                for (int i = 0; i < k; ++i)
                    n += sample(s, ((ox * k + i) * den) / (num * k), ((oy * k + j) * den) / (num * k));
            put(c, x + ox, y + oy, rgb, n, k * k);
        }
}
int ng_vertical_form(uint32_t c)
{
    static const uint32_t rotate[] = {
        0x2010, 0x2014, 0x2015, 0x2025, 0x2026, 0x2190, 0x2192, 0x3008, 0x3009, 0x300a, 0x300b,
        0x300c, 0x300d, 0x300e, 0x300f, 0x3010, 0x3011, 0x3014, 0x3015, 0x3016, 0x3017,
        0x3018, 0x3019, 0x301a, 0x301b, 0x301c, 0x30fc, 0xff08, 0xff09, 0xff0d, 0xff1a,
        0xff1b, 0xff1c, 0xff1d, 0xff1e, 0xff3b, 0xff3d, 0xff3f, 0xff5b, 0xff5c, 0xff5d, 0xff5e,
        0xffe3,
    };
    static const uint32_t small[] = {
        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308e,
        0x3095, 0x3096, 0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30c3, 0x30e3, 0x30e5,
        0x30e7, 0x30ee, 0x30f5, 0x30f6,
    };
    if (c == 0x3001 || c == 0x3002 || c == 0xff0c || c == 0xff0e) return NG_PUNCT;
    for (size_t i = 0; i < sizeof(rotate) / sizeof(rotate[0]); ++i) if (rotate[i] == c) return NG_ROTATE;
    for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); ++i) if (small[i] == c) return NG_SMALL;
    return NG_UPRIGHT;
}
int ng_body_width(uint32_t code) { return font24_width(code) * 12; }
void ng_body(Canvas *c, int x, int y, uint32_t code, uint32_t rgb, int form)
{
    const Font24Glyph *g = font24_lookup(code);
    Sampler s = {g->rows, g->width, 16, form, form == NG_PUNCT ? 9 : 2};
    render(c, x, y, &s, 3, 2, 2, rgb);
}
int ng_ruby_width(uint32_t code)
{
    const Font12Glyph *g = font12_find(code);
    if (g) return g->width;
    return font24_width(code) * 6;
}
void ng_ruby(Canvas *c, int x, int y, uint32_t code, uint32_t rgb, int form)
{
    const Font12Glyph *g = font12_find(code);
    if (g) {
        Sampler s = {g->rows, g->width, 12, form, form == NG_PUNCT ? 7 : 1};
        render(c, x, y, &s, 1, 1, 1, rgb);
        return;
    }
    /* Not in the 12-dot fonts (e.g. half-width kana): the 16-dot glyph at 0.75. */
    const Font24Glyph *f = font24_lookup(code);
    Sampler s = {f->rows, f->width, 16, form, form == NG_PUNCT ? 9 : 2};
    render(c, x, y, &s, 3, 4, 3, rgb);
}
void ng_dot(Canvas *c, int x, int y, uint32_t rgb)
{
    /* Sesame dot (U+FE45), drawn because JIS X 0208 fonts lack it. */
    static const uint16_t rows[12] = {0x000, 0x000, 0x000, 0x0c0, 0x0e0, 0x070, 0x078, 0x038,
                                      0x018, 0x000, 0x000, 0x000};
    Sampler s = {rows, 12, 12, NG_UPRIGHT, 0};
    render(c, x, y, &s, 1, 1, 1, rgb);
}
void ng_fill(Canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < CANVAS_H; ++yy)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < CANVAS_W; ++xx) {
            unsigned char *p = CANVAS_PX(c, xx, yy);
            p[0] = (unsigned char)(rgb >> 16); p[1] = (unsigned char)(rgb >> 8); p[2] = (unsigned char)rgb; p[3] = 255;
        }
}
int ng_text(Canvas *c, int x, int y, const char *s, uint32_t fg, int max_width)
{
    int used = 0;
    while (*s) {
        const char *before = s;
        uint32_t code = font24_decode(&s);
        int w = ng_body_width(code);
        if (max_width >= 0 && used + w > max_width) { s = before; break; }
        ng_body(c, x + used, y, code, fg, NG_UPRIGHT);
        used += w;
    }
    return used;
}
int ng_text_width(const char *s)
{
    int used = 0;
    while (*s) used += ng_body_width(font24_decode(&s));
    return used;
}
