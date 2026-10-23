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
#endif
