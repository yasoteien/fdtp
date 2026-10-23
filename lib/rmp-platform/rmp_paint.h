// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Backend-independent 2D painting interface, shared by every screen.
 * Implemented by rmp_gfx (GLES2, on the device) and rmp_paint_ref (CPU
 * reference, Mac tests and previews). Pure C.
 *
 * Coordinates: pixels, top-left origin, floats. Colours: straight alpha.
 * No blur or glow anywhere (POC_SPEC); edges get a 1px antialias ramp. */
#ifndef RMP_PAINT_H
#define RMP_PAINT_H

#include "rmp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float r, g, b, a; } rmp_color;

/* Rounded rectangle, vertical gradient top -> bottom. radius <= min(w,h)/2. */
typedef struct {
    float x, y, w, h, radius;
    rmp_color top, bottom;
} rmp_shape;

#define RMP_POLY_MAX 8

typedef struct rmp_painter {
    void *ctx;
    void (*shape)(void *ctx, const rmp_shape *s);
    /* Convex polygon, either winding, <= RMP_POLY_MAX points. Edges offset
     * outward by `expand`; coverage = clamp(0.5 + inside distance, 0, 1).
     * NULL: polygons are skipped. */
    void (*poly)(void *ctx, const float *xy, int n, float expand, rmp_color c);
    /* Optional glyph of the 5x7 font at size 9 or 10 (atlas). NULL: text is
     * painted dot by dot with shape(). Both give the same coverage. */
    void (*glyph)(void *ctx, float x, float y, unsigned char ch, float size, rmp_color c);
    /* Optional opaque 1px-aligned rectangles in one colour (waveform
     * columns). NULL: painted with shape(). */
    void (*fill_rects)(void *ctx, const rmp_rect *r, int n, rmp_color c);
} rmp_painter;

rmp_color rmp_hex(unsigned rgb, float alpha);

/* 5x7 bitmap font: 0-9, A-Z, . : % - + / [ ] |, space. Five columns, low bit
 * at the top. Returns 0 for characters it does not have. */
int   rmp_glyph_bitmap(unsigned char ch, unsigned char out[5]);
/* Text of the 5x7 font scaled to `size` (cap height), advancing
 * 6*size/7*1.06 + tracking per character. */
void  rmp_paint_text(const rmp_painter *p, float x, float y, const char *s,
                     float size, rmp_color c, float tracking);
float rmp_text_width(const char *s, float size, float tracking);
void  rmp_paint_shape(const rmp_painter *p, float x, float y, float w, float h,
                      float radius, rmp_color top, rmp_color bottom);
void  rmp_paint_box(const rmp_painter *p, float x, float y, float w, float h,
                    float radius, rmp_color c);

#ifdef __cplusplus
}
#endif
#endif
