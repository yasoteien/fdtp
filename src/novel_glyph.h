// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVEL_GLYPH_H
#define FDTP_NOVEL_GLYPH_H
/* Pixel glyphs for the novel editor. Body text is the filer's look: 16-dot
 * Shinonome scaled 1.5x by area averaging (12x24 or 24x24). Ruby text is
 * 12-dot Shinonome at 1x. Colors are 0xRRGGBB, blended over the canvas. */
#include <stdint.h>
#include "canvas.h"

/* Vertical-writing forms made from the horizontal glyphs. */
enum { NG_UPRIGHT, NG_ROTATE, NG_PUNCT, NG_SMALL };
/* The form a character takes in vertical writing: brackets, dashes, the
 * long vowel mark and the like rotate 90 degrees clockwise; 、。，． move to
 * the upper right; small kana move slightly up and right. */
int ng_vertical_form(uint32_t code);
int ng_body_width(uint32_t code);  /* 12 or 24 */
/* (x, y) is the top left. NG_ROTATE turns the glyph box too (a half-width
 * glyph becomes 24 wide and 12 tall). */
void ng_body(Canvas *c, int x, int y, uint32_t code, uint32_t rgb, int form);
int ng_ruby_width(uint32_t code);  /* 6 or 12 */
void ng_ruby(Canvas *c, int x, int y, uint32_t code, uint32_t rgb, int form);
void ng_dot(Canvas *c, int x, int y, uint32_t rgb); /* 12x12 sesame dot */
void ng_fill(Canvas *c, int x, int y, int w, int h, uint32_t rgb);
/* Body-font text; stops before max_width pixels (-1: no limit). Returns the width drawn. */
int ng_text(Canvas *c, int x, int y, const char *utf8, uint32_t fg, int max_width);
int ng_text_width(const char *utf8);
#endif
