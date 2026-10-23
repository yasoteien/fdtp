// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_FONT12_H
#define FDTP_FONT12_H
#include <stdint.h>
/* 12-dot Shinonome glyph for ruby text, drawn at 1x: width 6 (ASCII) or
 * 12 (JIS X 0208). Rows use bit (width-1-x) for the left-to-right pixel x. */
typedef struct { uint32_t code; unsigned char width; uint16_t rows[12]; } Font12Glyph;
/* NULL when the 12-dot fonts have no glyph; callers then fall back to the
 * 16-dot glyph scaled to 0.75 (novel_glyph.c). */
const Font12Glyph *font12_find(uint32_t code);
#endif
