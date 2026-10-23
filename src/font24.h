// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_FONT24_H
#define FDTP_FONT24_H
#include <stdint.h>
/* 16-dot source glyph (Shinonome), width 8 (one cell) or 16 (two cells).
 * The raster scales it 1.5x by area averaging into 12x24 cells. */
typedef struct { uint32_t code; unsigned char width; uint16_t rows[16]; } Font24Glyph;
/* Rows use bit (width-1-x) for the left-to-right pixel x. */
const Font24Glyph *font24_lookup(uint32_t code);
int font24_width(uint32_t code);
/* Decode one UTF-8 character, advancing *text; invalid input becomes '?'. */
uint32_t font24_decode(const char **text);
#endif
