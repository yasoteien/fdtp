// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_CELLS_H
#define FDTP_CELLS_H
#include <stdint.h>
#include <string.h>
/* The character grid: 12x24 cells, 80 x 25 on the Pomera, more on a wide
 * screen (WIDE_NOTES.md, set by screen_apply in canvas.c). Row FD_ROWS-1
 * is the band (row 24 at 80 x 25), FD_ROWS-2 the message line (row 23).
 * The storage is for the largest grid; only FD_COLS x FD_ROWS is used. */
#define FD_COLS_MAX 320
#define FD_ROWS_MAX 90
extern int fd_cols, fd_rows;
#define FD_COLS fd_cols
#define FD_ROWS fd_rows
#define FD_W (fd_cols * 12)
#define FD_H (fd_rows * 24)
#define FD_BAND (fd_rows - 1)
#define FD_LINE (fd_rows - 2)
/* Extra columns and rows over 80 x 25 (0 on the Pomera). */
#define FD_XCOLS (fd_cols - 80)
#define FD_XROWS (fd_rows - 25)
/* The FD frame's row 1 dividers (Page | Total | Used | Free), in the middle
 * of these columns: 13 36 57 at 80 columns; on a wider grid Total and Used
 * get a third each of the extra columns, Free the rest. */
static inline int fd_divider(int i) { return i == 0 ? 13 : i == 1 ? 36 + FD_XCOLS / 3 : 57 + 2 * FD_XCOLS / 3; }
enum { BG, CYAN, WHITE, YELLOW, MAGENTA, BLACK, BLUE, RED, GREEN };
extern const uint32_t fd_palette[9];
typedef struct { uint32_t code; unsigned char fg, bg, part; } Cell;
/* A floating box drawn over the cells (revision 1 §7): raster draws its frame
 * in dots through the centres of the border cells (CYAN, two dots, corner
 * radius six, as the top FD frame) and, with CELL_BOX_SHADOW, a one-cell
 * shadow to the right and below (black ground, grey characters). rule, when
 * not 0, is an absolute row inside the box with a horizontal line through
 * its centre joining the frame. A later box covers an earlier one. */
#define CELLS_BOXES 6
enum { CELL_BOX_FRAME = 1, CELL_BOX_SHADOW = 2 };
/* top_from..top_to and bottom_from..bottom_to (columns, end excluded; 0 0:
 * none) are gaps in the top and bottom lines of the frame where a title or a
 * hint is written over the frame row (revision 1 item 25). */
typedef struct { short col, row, w, h, rule, top_from, top_to, bottom_from, bottom_to; unsigned char kind; } CellBox;
/* Extra lines inside a box, drawn with its frame (same CYAN two-dot line):
 * horizontal through the centres of row at, cells from..to (inclusive), or
 * vertical through the centres of column at, rows from..to. A line that
 * ends on the box's frame cell joins the frame. box: the box it belongs to
 * (hidden where a later box covers it). */
#define CELLS_LINES 64
typedef struct { short at, from, to; unsigned char vertical; signed char box; } CellLine;
#define CELLS_SEPARATOR_WORDS ((FD_ROWS_MAX + 31) / 32)
/* A field added here must be added to cells_copy and clear_fields (cells.c). */
typedef struct {
    Cell data[FD_ROWS_MAX][FD_COLS_MAX];
    unsigned char dirty[FD_ROWS_MAX][FD_COLS_MAX];
    int title_start, title_end, date_start, date_end;
    int lines_dirty;
    int frame; /* top FD frame; disabled by full-screen viewers */
    /* bit r: a horizontal line centred in row r (cells_separator) */
    uint32_t separators[CELLS_SEPARATOR_WORDS];
    int box_count;
    CellBox boxes[CELLS_BOXES];
    int line_count;
    CellLine lines[CELLS_LINES];
} Cells;
/* Sets the grid size: at least 80 x 25, at most FD_COLS_MAX x FD_ROWS_MAX.
 * Cells drawn before keep their contents; screens lay out anew. */
void fd_set_size(int cols, int rows);
void cells_init(Cells *c);
/* *dst = *src for the grid in use (instead of copying all the storage). */
void cells_copy(Cells *dst, const Cells *src);
void cells_clear(Cells *c);
void cells_put(Cells *c, int col, int row, uint32_t code, int fg, int bg);
int cells_text(Cells *c, int col, int row, const char *s, int fg, int bg, int limit);
int cells_width(const char *s);
/* Paths on the screen. $HOME and below are shown as ~ (the user name stays
 * off the screen); copied into buf when replaced, else path itself. */
const char *cells_home_path(const char *path, char *buf, size_t size);
/* The end of s that fits in max columns (its start left out). */
const char *cells_tail(const char *s, int max);
/* path at col (cells_home_path), and when wider than limit, "…" and its end:
 * the name of the directory itself stays in sight. Returns the width used. */
int cells_path(Cells *c, int col, int row, const char *path, int fg, int bg, int limit);
void cells_invalidate(Cells *c);
/* Add a box (clipped to the screen, at least 2x2) and blank its cells.
 * Returns the box index, or -1 when there is no room or it is too small. */
int cells_box(Cells *c, int col, int row, int w, int h, int kind);
/* 1: (col,row) is under box i's shadow and not covered by a later box. */
int cells_shadowed(const Cells *c, int col, int row);
/* Index of the topmost box containing the cell, or -1. */
int cells_box_at(const Cells *c, int col, int row);
int cells_same_boxes(const Cells *a, const Cells *b);
/* Adds a line (CellLine) to the last box added; -1 when there is no box or
 * no room. */
int cells_box_line(Cells *c, int vertical, int at, int from, int to);
/* Horizontal lines through rows (Cells.separators). */
static inline void cells_separator(Cells *c, int row)
{ if (row >= 0 && row < FD_ROWS_MAX) c->separators[row / 32] |= 1u << (row % 32); }
static inline void cells_separator_off(Cells *c, int row)
{ if (row >= 0 && row < FD_ROWS_MAX) c->separators[row / 32] &= ~(1u << (row % 32)); }
static inline int cells_has_separator(const Cells *c, int row)
{ return row >= 0 && row < FD_ROWS_MAX && (c->separators[row / 32] >> (row % 32) & 1u); }
static inline void cells_no_separators(Cells *c) { memset(c->separators, 0, sizeof(c->separators)); }
static inline int cells_same_separators(const Cells *a, const Cells *b)
{ return !memcmp(a->separators, b->separators, sizeof(a->separators)); }
/* One item of a band on the last row, given as on the 80-column screen (left
 * column x80, width w80): on a wider one both grow in proportion and the
 * label is centred. CYAN ground, BG text, the hot character (a byte index
 * into the ASCII label, or -1) on YELLOW. */
void cells_band_item(Cells *c, int x80, int w80, const char *label, int hot);
/* The separators as one decimal number, bit r for row r (the snapshots'
 * "separators"; as before when only rows under 32 have one). */
void cells_separator_text(const Cells *c, char *out, size_t size);

/* ---- The frame of every full screen (revision 1 item 29; docs/FDTP_03 §7) ----
 * Box 0 of the screen: FD's frame (CYAN, two dots, round corners, no shadow)
 * over every column and rows 0 .. FRAME_BOTTOM. The body is inside it
 * (FRAME_BODY_COLS x FRAME_BODY_ROWS cells from column 1, row 1: 78 x 21 at
 * 80 x 25). Under it, outside: the message line (FD_LINE) and the band
 * (FD_BAND). The title is in a gap of the top line (YELLOW, from column 3 or
 * after a vertical line), the status (where the screen is, its mode) in a gap
 * of the bottom line at the right (CYAN). Lines inside join the frame. */
#define FRAME_BOTTOM (fd_rows - 3)
#define FRAME_RIGHT (fd_cols - 1)
#define FRAME_BODY_ROWS (fd_rows - 4)
#define FRAME_BODY_COLS (fd_cols - 2)
#define FRAME_TITLE_COL 3
/* The end (exclusive) of the status text at the right of the bottom line. */
#define FRAME_STATUS_END (fd_cols - 3)
/* Opens the frame: no top FD frame, no separators, box 0 (call it first,
 * before any other box). Returns the box index (0) or -1. */
int frame_open(Cells *c);
/* The title (YELLOW) from column col in a gap of the top line, at most limit
 * cells, then extra (when not NULL, after one blank, in extra_fg). Returns the
 * column after the gap's text. */
int frame_title(Cells *c, int col, const char *title, int limit, const char *extra, int extra_fg);
/* The status (CYAN) right-aligned to end (exclusive) in a gap of the bottom
 * line, cut on the left to at most limit cells. Returns its first column. */
int frame_status(Cells *c, int end, const char *text, int limit);
/* A line across the frame through row (├───┤), or down column col from row
 * from to row to (0 and FRAME_BOTTOM join the top and bottom lines: ┬ ┴). */
int frame_hline(Cells *c, int row);
int frame_vline(Cells *c, int col, int from, int to);
/* The band of a screen whose keys are not FD's function keys: from column
 * col, each key on YELLOW and its name on CYAN (as the FD band), a blank (two
 * when they fit) between. */
typedef struct { const char *key, *name; } FrameKey;
void frame_keys(Cells *c, int col, const FrameKey *keys, int n);
#endif
