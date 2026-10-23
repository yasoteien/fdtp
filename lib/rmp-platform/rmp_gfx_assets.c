// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rmp_gfx_assets.h"

#include <math.h>
#include <string.h>

#include "rmp_paint.h"

static float asset_clamp(float x) { return fminf(1, fmaxf(0, x)); }
static float asset_distance(float qx, float qy, float r)
{
    return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
}
static float asset_fill(float distance)
{
    float u = asset_clamp((distance + .6f) / 1.2f);
    return 1 - u * u * (3 - 2 * u);
}

void rmp_font_phase_pixels(uint8_t *p, unsigned stride, unsigned phase, unsigned phases)
{
    for (unsigned y = 0; y < RMP_FONT_TILE_H; y++) memset(p + (size_t)y * stride, 0, RMP_FONT_TILE_W);
    for (unsigned index = 0; index < 192; index++) {
        unsigned char bitmap[5];
        if (!rmp_glyph_bitmap((unsigned char)(32 + index % 96), bitmap)) continue;
        float size = index < 96 ? 10 : 9, sy = size / 7, sx = sy * 1.06f;
        for (unsigned y = 0; y < RMP_FONT_CELL_H; y++)
            for (unsigned x = 0; x < RMP_FONT_CELL_W; x++) {
                float px = (float)x + .5f - 1 - (float)phase / (float)phases, py = (float)y + .5f - 1, a = 0;
                /* Preserve the original dot-by-dot source-over at shared edges. */
                for (unsigned col = 0; col < 5; col++)
                    for (unsigned row = 0; row < 7; row++)
                        if (bitmap[col] & (1u << row)) {
                            float qx = fabsf(px - ((float)col + .5f) * sx) - sx * .5f;
                            float qy = fabsf(py - ((float)row + .5f) * sy) - sy * .5f;
                            float coverage = asset_fill(asset_distance(qx, qy, 0));
                            a += coverage * (1 - a);
                        }
                p[(size_t)(index / 16 * RMP_FONT_CELL_H + y) * stride + index % 16 * RMP_FONT_CELL_W + x] =
                    (uint8_t)lroundf(255 * a);
            }
    }
}

void rmp_font_cell_origin(unsigned phase, unsigned index, unsigned *x, unsigned *y)
{
    *x = phase % RMP_FONT_TILES_X * RMP_FONT_TILE_W + index % 16 * RMP_FONT_CELL_W;
    *y = phase / RMP_FONT_TILES_X * RMP_FONT_TILE_H + index / 16 * RMP_FONT_CELL_H;
}

void rmp_font_atlas_pixels(uint8_t *p)
{
    for (unsigned phase = 0; phase < RMP_FONT_PHASES; phase++) {
        unsigned x, y;
        rmp_font_cell_origin(phase, 0, &x, &y);
        rmp_font_phase_pixels(p + (size_t)y * RMP_FONT_ATLAS_W + x, RMP_FONT_ATLAS_W, phase, RMP_FONT_PHASES);
    }
}

void rmp_mask_tile_pixels(uint8_t *p, unsigned stride, unsigned index)
{
    float radius = (float)index / 32;
    for (unsigned y = 0; y < RMP_MASK_TILE; y++)
        for (unsigned x = 0; x < RMP_MASK_TILE; x++) {
            float fx = -.75f + 3 * (float)x / (RMP_MASK_TILE - 1), fy = -.75f + 3 * (float)y / (RMP_MASK_TILE - 1);
            p[(size_t)y * stride + x] = (uint8_t)lroundf(255 * asset_fill(asset_distance(fx, fy, radius)));
        }
}

void rmp_mask_tile_origin(unsigned index, unsigned *x, unsigned *y)
{
    *x = index % RMP_MASK_CELLS_X * RMP_MASK_CELL + RMP_MASK_GUTTER;
    *y = index / RMP_MASK_CELLS_X * RMP_MASK_CELL + RMP_MASK_GUTTER;
}

void rmp_mask_atlas_pixels(uint8_t *p)
{
    memset(p, 0, (size_t)RMP_MASK_ATLAS * RMP_MASK_ATLAS);
    for (unsigned index = 0; index < RMP_MASK_TILES; index++) {
        unsigned ox, oy;
        rmp_mask_tile_origin(index, &ox, &oy);
        uint8_t *tile = p + (size_t)oy * RMP_MASK_ATLAS + ox;
        rmp_mask_tile_pixels(tile, RMP_MASK_ATLAS, index);
        /* Gutter: clamp-to-edge copy of the tile's border texels. */
        for (int y = -RMP_MASK_GUTTER; y < RMP_MASK_TILE + RMP_MASK_GUTTER; y++)
            for (int x = -RMP_MASK_GUTTER; x < RMP_MASK_TILE + RMP_MASK_GUTTER; x++) {
                if (x >= 0 && x < RMP_MASK_TILE && y >= 0 && y < RMP_MASK_TILE) continue;
                int sx = x < 0 ? 0 : x >= RMP_MASK_TILE ? RMP_MASK_TILE - 1 : x;
                int sy = y < 0 ? 0 : y >= RMP_MASK_TILE ? RMP_MASK_TILE - 1 : y;
                p[(size_t)((int)oy + y) * RMP_MASK_ATLAS + (size_t)((int)ox + x)] =
                    tile[(size_t)sy * RMP_MASK_ATLAS + (size_t)sx];
            }
    }
}
