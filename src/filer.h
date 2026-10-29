// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_FILER_H
#define FDTP_FILER_H
#include "screen.h"
#include "dirlist.h"
/* Selected names/paths are borrowed until the next filer mutation.
 * Owned copies must be made by asynchronous operations. */
typedef struct {
    const char *directory;
    const char *current;
    const char *const *names;
    size_t count;
} FileSelection;
Screen *filer_create(const char *directory);
int filer_selection(Screen *screen, FileSelection *out);
/* Nonzero if any file is marked in the active pane. */
int filer_marked(Screen *screen);
/* Borrowed adjacent pane directory, or NULL when unsplit. */
const char *filer_destination(Screen *screen);
int filer_change_directory(Screen *screen, App *app, const char *path);
void filer_reload(Screen *screen, App *app);
void filer_command(Screen *screen, App *app, const char *id);
/* Stage 1c: takes app->settings (columns, sort, dot files, links, types)
 * into every pane. Does nothing for a screen that is not a filer. */
void filer_apply_settings(Screen *screen, App *app);
/* The mouse (the macOS window): col,row are cells. Nothing happens, and 0
 * is returned, unless the filer is on top with no box open and an entry is
 * there. FILER_POINT moves the cursor (and the active pane) there,
 * (a line-23 notice is no box; a click clears it as a key does),
 * FILER_SELECT also clears the marks (a click), FILER_TOGGLE marks or
 * unmarks it (Cmd+click), FILER_RANGE marks from the last click to it
 * (Shift+click). Directories are never marked, as with Space. */
enum { FILER_POINT, FILER_SELECT, FILER_TOGGLE, FILER_RANGE };
int filer_mouse(Screen *screen, App *app, int col, int row, int action);
/* 1 when the entry at the cell is marked. */
int filer_mouse_marked(Screen *screen, App *app, int col, int row);
/* What a drag from the cell carries: every marked name when that entry is
 * marked, else that entry alone. Borrowed as filer_selection. -1: nothing. */
int filer_drag_selection(Screen *screen, App *app, int col, int row, FileSelection *out);
/* Where a drop at the cell goes (malloc'd): the directory there, else the
 * pane's own directory. NULL when the filer cannot take a drop now. */
char *filer_drop_directory(Screen *screen, App *app, int col, int row);
#endif
