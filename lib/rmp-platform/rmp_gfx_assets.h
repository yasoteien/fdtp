// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Startup-only coverage atlases for rmp_gfx (pure C, tested on the Mac).
 * Derived from poc/pomera-scope src/gl_assets.h. No glow masks.
 *
 * Font: the PoC had 128 subpixel phases, one 192x192 texture each, so a
 *   batch broke at almost every glyph. Here 16 phases (1/16 px; the glyph x
 *   moves by <= 1/32 px) share ONE 768x768 alpha texture (4x4 phase tiles).
 *   Sampling is NEAREST at texel centres; the fp16 (Mali-400 mediump) UV
 *   error at 768 texels is <= 0.19 texel, below the 0.5 that would pick a
 *   neighbour. Each phase tile equals the PoC texture of phase 8*p/128.
 * Mask: the PoC had 49 separate 128x128 rounded-corner tiles (radius
 *   0..1.5 px in 1/32 steps). Here they share ONE 1024x1024 luminance
 *   texture, 7x7 cells of 132 texels: each tile has a 2-texel gutter that
 *   repeats its edge, so the fp16 UV error (<= 0.25 texel at 1024) under
 *   LINEAR filtering never blends in a neighbouring tile. */
#ifndef RMP_GFX_ASSETS_H
#define RMP_GFX_ASSETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RMP_FONT_PHASES 16
#define RMP_FONT_CELL_W 12
#define RMP_FONT_CELL_H 16
#define RMP_FONT_TILE_W (16 * RMP_FONT_CELL_W)   /* 192: 16 glyphs per row */
#define RMP_FONT_TILE_H (12 * RMP_FONT_CELL_H)   /* 192: 6 rows size 10 + 6 rows size 9 */
#define RMP_FONT_TILES_X 4
#define RMP_FONT_ATLAS_W (RMP_FONT_TILES_X * RMP_FONT_TILE_W)                       /* 768 */
#define RMP_FONT_ATLAS_H ((RMP_FONT_PHASES / RMP_FONT_TILES_X) * RMP_FONT_TILE_H)  /* 768 */

#define RMP_MASK_TILE 128
#define RMP_MASK_TILES 49        /* corner radius 0..1.5 px in 1/32 steps */
#define RMP_MASK_GUTTER 2
#define RMP_MASK_CELL (RMP_MASK_TILE + 2 * RMP_MASK_GUTTER)   /* 132 */
#define RMP_MASK_CELLS_X 7
#define RMP_MASK_ATLAS 1024

/* One phase tile (192x192 alpha) written at p with the given row stride.
 * phase/phases is the subpixel offset of the glyph origin. */
void rmp_font_phase_pixels(uint8_t *p, unsigned stride, unsigned phase, unsigned phases);
/* The whole RMP_FONT_ATLAS_W x RMP_FONT_ATLAS_H atlas. */
void rmp_font_atlas_pixels(uint8_t *p);
/* Origin (texels) of a glyph cell: index 0..95 = size 10, 96..191 = size 9. */
void rmp_font_cell_origin(unsigned phase, unsigned index, unsigned *x, unsigned *y);

/* One rounded-corner coverage tile (128x128) of radius index/32 px. */
void rmp_mask_tile_pixels(uint8_t *p, unsigned stride, unsigned index);
/* The whole 1024x1024 mask atlas with gutters. */
void rmp_mask_atlas_pixels(uint8_t *p);
/* Texel origin of tile `index` (inside its gutter). */
void rmp_mask_tile_origin(unsigned index, unsigned *x, unsigned *y);

#ifdef __cplusplus
}
#endif
#endif
