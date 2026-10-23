// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_RASTER_H
#define FDTP_RASTER_H
#include "cells.h"
#include "canvas.h"
/* Compatibility name; the backing buffer now covers the complete panel. */
typedef Canvas Raster;
/* Full-width canvas rows for GLES2 (no unpack-row-length extension needed). */
int raster_update(Canvas *r, Cells *c, FDRect out[FD_ROWS_MAX]);
/* For screens that draw pixels themselves (images, PDF, slides, the novel
 * editor): draw the floating boxes of cells, and their shadows, over what is
 * on the canvas, and the frame of the screen (a box without a shadow: only
 * its border cells and its lines, the picture inside is left). raster_update
 * already does this for the cell screens. */
void raster_boxes(Canvas *r, const Cells *c);
/* For those screens too: rows 0, FRAME_BOTTOM and FD_BAND of cells (and
 * FD_LINE when line is set) drawn through scratch (any Canvas, re-initialised
 * here) onto c, which must be at the current size. */
void raster_bands(Canvas *c, Canvas *scratch, Cells *cells, int line);
/* A line across the screen's frame (cells.h frame_open) at grid pixel row y
 * (y, y + 1), joining its left and right lines: for pixel screens whose rule
 * is not on a row's centre (the vertical novel editor's line numbers). */
void raster_frame_line(Canvas *r, const Cells *c, int y);
/* The picture area of those screens: the body of the frame (cells.h), in
 * canvas pixels (996 x 504 from x 14, y 24 at 85 x 25 on the Pomera). */
#define VIEW_TOP 24
#define VIEW_LEFT (CANVAS_GRID_X + 12)
#define VIEW_W (FRAME_BODY_COLS * 12)
#define VIEW_H (FRAME_BODY_ROWS * 24)
#endif
