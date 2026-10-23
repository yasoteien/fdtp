// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* CPU reference painter (pure C): the GL fragment model evaluated at pixel
 * centres into a native-endian XRGB8888 buffer. For Mac tests and previews;
 * not used on the device. Matches the PoC test reference
 * (poc/pomera-scope/tests/render_preview.c): rounded-rect SDF with a 1px
 * smoothstep edge, straight-alpha source-over, polygons by edge distance.
 * glyph and fill_rects stay NULL (text dot by dot, rects as shapes). */
#ifndef RMP_PAINT_REF_H
#define RMP_PAINT_REF_H

#include <stdint.h>
#include "rmp_paint.h"
#include "rmp_damage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *pixels;
    unsigned stride;          /* pixels per row */
    rmp_box clip;             /* scissor: only these pixels change */
    rmp_painter painter;
} rmp_paint_ref;

void rmp_paint_ref_init(rmp_paint_ref *r, uint32_t *pixels, unsigned stride, int w, int h);
void rmp_paint_ref_fill(uint32_t *pixels, unsigned stride, rmp_box area, uint32_t xrgb);
void rmp_paint_ref_poly_pixels(uint32_t *pixels, unsigned stride, rmp_box clip,
                               const float *xy, int n, float expand, rmp_color c);

#ifdef __cplusplus
}
#endif
#endif
