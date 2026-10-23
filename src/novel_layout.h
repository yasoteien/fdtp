// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVEL_LAYOUT_H
#define FDTP_NOVEL_LAYOUT_H
/* Folding one line (paragraph) of the novel into display rows, with ruby.
 * Horizontal rows are 960 px (40 full-width characters) and vertical columns
 * 552 px (23) unless NLayout.width says otherwise (the editor narrows them
 * for its rules and line numbers). Positions run along the row: x for horizontal writing,
 * y for vertical. A ruby group (base plus ruby) is never split: it is as
 * wide as the longer of base and ruby (rounded to 12 px), the base centred
 * in it and the ruby spread evenly over it, so an overhanging ruby opens
 * space around its base instead of overlapping its neighbours.
 * Only lines near the visible part are laid out (noveledit.c). */
#include <stddef.h>
#include <stdint.h>
#include "novel_ruby.h"

#define NL_ROW_H 960
#define NL_ROW_V 552
enum { NU_HIDDEN = 1, NU_PREEDIT = 2, NU_PREEDIT_HL = 4, NU_EMPHASIS = 8, NU_HALF = 16, NU_TCY2 = 32 };
typedef struct {
    uint32_t code;      /* glyph: '?' for control characters, ' ' for a tab */
    size_t off, next;   /* bytes of this character in the line */
    int pos, adv;       /* along the row, px from its start */
    int cross;          /* across offset in vertical tate-chu-yoko, px */
    int group;          /* index of the collapsed ruby span, or -1 */
    unsigned char form; /* NG_* from novel_glyph.h */
    unsigned char color;/* fd_palette index */
    unsigned char flags;
} NUnit;
typedef struct { uint32_t code; int pos; unsigned char color, form, half; } NRubyGlyph;
typedef struct { size_t unit0, units, ruby0, rubies, start; int length; } NRow;
typedef struct {
    int vertical, failed;
    int width;          /* row length in px set by the caller; 0: NL_ROW_H or NL_ROW_V */
    size_t len, ncp, ccp, nspan, cspan;
    uint32_t *cp; size_t *off;
    NRuby *span;
    int expanded;       /* span shown as notation because the caret is in it */
    NUnit *u; size_t nu, cu;
    NRubyGlyph *r; size_t nr, cr;
    NRow *row; size_t nrow, crow;
} NLayout;

void nl_init(NLayout *l);
void nl_free(NLayout *l);
/* Lay out text[0..len). cursor is the caret's byte offset, or (size_t)-1
 * when the caret is in another line: a ruby span containing the caret is
 * shown as written. [pre_start, pre_end) is an input-method preedit already
 * inserted into text, [hl_start, hl_end) its segment being converted.
 * Returns 0, or -1 when out of memory (the layout is then empty). */
int nl_run(NLayout *l, const char *text, size_t len, int vertical, size_t cursor,
           size_t pre_start, size_t pre_end, size_t hl_start, size_t hl_end);
/* Row and position of a caret placed before byte off (len: the line end). */
void nl_locate(const NLayout *l, size_t off, size_t *row, int *pos);
/* Caret offset nearest to position pos of row. */
size_t nl_hit(const NLayout *l, size_t row, int pos);
/* Caret offsets at the start and at the end of a row. */
size_t nl_row_start(const NLayout *l, size_t row);
size_t nl_row_end(const NLayout *l, size_t row);
#endif
