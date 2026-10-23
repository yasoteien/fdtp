// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_CANVAS_H
#define FDTP_CANVAS_H
#include <stddef.h>
#include <stdint.h>
/* The picture sent to the display: canvas_w x canvas_h RGBA, the character
 * grid (FD_W x FD_H, cells.h) at (canvas_grid_x, 0), the rest the ground's
 * navy. The Pomera: 1024 x 600 with the 960 x 600 grid at x 32. A wide
 * screen (WIDE_NOTES.md): the screen's size, the grid centred across and
 * at the top (at most 23 dots of navy under it). */
#define CANVAS_W_MAX 3840
#define CANVAS_H_MAX 2160
extern int canvas_w, canvas_h, canvas_grid_x;
#define CANVAS_W canvas_w
#define CANVAS_H canvas_h
#define CANVAS_GRID_X canvas_grid_x
typedef struct { int x, y, w, h; } FDRect;
/* The storage is for the largest canvas (address space only: the pages
 * past w x h are never touched); rows are w pixels apart. */
typedef struct Canvas {
    int w, h, full_dirty;
    unsigned char rgba[(size_t)CANVAS_W_MAX * CANVAS_H_MAX * 4];
} Canvas;
#define CANVAS_PX(c, x, y) (&(c)->rgba[((size_t)(y) * (size_t)(c)->w + (size_t)(x)) * 4])
#define CANVAS_ROW(c, y) CANVAS_PX(c, 0, y)
/* At the current size (canvas_w x canvas_h), cleared to the navy ground. */
void canvas_init(Canvas *c);
/* canvas_init when c is not at the current size; 1 when it was. */
int canvas_fit(Canvas *c);
void canvas_clear(Canvas *c, uint32_t rgb);
int canvas_write_ppm(const Canvas *c, const char *path);

/* ---- the screen size (WIDE_NOTES.md) ------------------------------------ */
typedef struct { int cols, rows, canvas_w, canvas_h; } ScreenSize;
/* SCREENSIZE / --size: "auto" (0 x 0) or "COLSxROWS" (80x25 up to
 * FD_COLS_MAX x FD_ROWS_MAX; smaller or larger is clamped). -1: not either. */
int screen_parse(const char *text, int *cols, int *rows);
/* cols x rows asked (0 x 0: auto) on an output of out_w x out_h pixels
 * (0 x 0: not known). Auto: 80 x 25 on 1024 x 600 and smaller; larger, the
 * screen divided by a whole-number scale (1, 2 at 2160 rows ...) into 12 x 24
 * cells. 80 x 25 is always the Pomera's 1024 x 600 canvas. */
ScreenSize screen_choose(int cols, int rows, int out_w, int out_h);
/* Makes s the current size: fd_cols/fd_rows and canvas_w/h/grid_x. */
void screen_apply(ScreenSize s);
#endif
