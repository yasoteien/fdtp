// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rmp_paint.h"

#include <math.h>
#include <string.h>

/* Five columns, low bit at the top (poc/pomera-scope src/cpu_render.c). */
static const unsigned char glyphs[][5] = {
    {62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
    {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
    {54,73,73,73,54},{6,73,73,41,30},
    {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
    {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
    {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
    {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
    {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
    {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
    {7,8,112,8,7},{97,81,73,69,67},
    {0,96,96,0,0},{0,54,54,0,0},{99,19,8,100,99},{8,8,8,8,8}
};

rmp_color rmp_hex(unsigned hex, float a)
{
    rmp_color c = { (float)((hex >> 16) & 255) / 255, (float)((hex >> 8) & 255) / 255,
                    (float)(hex & 255) / 255, a };
    return c;
}

int rmp_glyph_bitmap(unsigned char ch, unsigned char out[5])
{
    static const char symbols[] = ".:%-";
    unsigned char extra[5] = {0};
    const unsigned char *g = NULL;
    int n = -1;
    const char *q;
    if (ch >= '0' && ch <= '9') n = ch - '0';
    else if (ch >= 'A' && ch <= 'Z') n = 10 + ch - 'A';
    else if (ch && (q = strchr(symbols, ch)) != NULL) n = 36 + (int)(q - symbols);
    if (n >= 0) g = glyphs[n];
    else {
        if (ch == '+') { extra[0] = 8; extra[1] = 8; extra[2] = 62; extra[3] = 8; extra[4] = 8; }
        else if (ch == '/') { extra[0] = 64; extra[1] = 48; extra[2] = 8; extra[3] = 6; extra[4] = 1; }
        else if (ch == '[') { extra[1] = 127; extra[2] = 65; extra[3] = 65; }
        else if (ch == ']') { extra[1] = 65; extra[2] = 65; extra[3] = 127; }
        else if (ch == '|') extra[2] = 127;
        else return 0;
        g = extra;
    }
    memcpy(out, g, 5);
    return 1;
}

void rmp_paint_shape(const rmp_painter *p, float x, float y, float w, float h,
                     float radius, rmp_color top, rmp_color bottom)
{
    if (w <= 0 || h <= 0) return;
    rmp_shape s = { x, y, w, h, fminf(radius, fminf(w, h) * .5f), top, bottom };
    p->shape(p->ctx, &s);
}

void rmp_paint_box(const rmp_painter *p, float x, float y, float w, float h,
                   float radius, rmp_color c)
{
    rmp_paint_shape(p, x, y, w, h, radius, c, c);
}

void rmp_paint_text(const rmp_painter *p, float x, float y, const char *s,
                    float size, rmp_color c, float tracking)
{
    float sy = size / 7, sx = sy * 1.06f;
    for (; *s; s++, x += 6 * sx + tracking) {
        unsigned char bitmap[5];
        if (!rmp_glyph_bitmap((unsigned char)*s, bitmap)) continue;
        if (p->glyph && (size == 9 || size == 10)) {
            p->glyph(p->ctx, x, y, (unsigned char)*s, size, c);
            continue;
        }
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (bitmap[col] & (1u << row))
                    rmp_paint_box(p, x + (float)col * sx, y + (float)row * sy, sx, sy, 0, c);
    }
}

float rmp_text_width(const char *s, float size, float tracking)
{
    size_t n = strlen(s);
    return n ? (float)n * (size / 7 * 6 * 1.06f + tracking) - tracking : 0;
}
