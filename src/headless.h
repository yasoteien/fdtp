// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_HEADLESS_H
#define FDTP_HEADLESS_H
#include "app.h"
/* Both executables accept DIRECTORY OUTPUT.ppm [COLUMNS], or:
 * [--headless] [--script FILE | --keys TEXT] [--out PREFIX] [--time EPOCH]
 * [--columns 1|2|3|5] [--size auto|COLSxROWS] [--display WxH] [DIRECTORY].
 * SNAP -> PREFIX-NNN.ppm/.cells (JSON).
 * Headless records external requests, never executes them. */
int headless_main(int argc, char **argv);
typedef struct {
    KeyScript parser;
    double ready_at;
    unsigned snapshots;
    int ended;
    const char *prefix;
    /* The screen (WIDE_NOTES.md): --size (-1: SCREENSIZE), and the output
     * --display / DISPLAY:WxH emulates (0 x 0: none, then 80 x 25 in auto). */
    int size_cols, size_rows, display_w, display_h;
} ScriptRunner;
/* Headless only: takes the size the runner and SCREENSIZE ask for. */
void headless_fit(ScriptRunner *r, App *app, Canvas *canvas, Cells *cells);
void headless_script_init(ScriptRunner *r,const char *text,const char *prefix,double now);
/* 0 waiting, 1 event delivered, 2 end, -1 syntax/I/O error. The caller owns
 * timing: Linux passes monotonic time; headless advances to ready_at. */
int headless_script_step(ScriptRunner *r,App *app,Canvas *canvas,Cells *cells,double now);
int headless_snapshot(App *app,Canvas *canvas,Cells *cells,const char *prefix,unsigned serial);
#endif
