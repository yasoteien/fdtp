// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SLIDEVIEW_H
#define FDTP_SLIDEVIEW_H
#include "screen.h"
/* path is borrowed during creation; an implemented screen keeps its own copy.
 * Returns an owned screen for app_push(), or NULL after reporting a notice.
 * On app_push() failure the caller must destroy the returned screen.
 * Stage 6: a pptx shown one slide at a time (slide_model.h); a file that
 * cannot be opened reports the reason on row 23 and returns NULL.
 * Revision 1 item 41: e/RET chooses a paragraph of the slide, RET edits it
 * on row 23, C-s saves, C-z / C-y undo and redo (slideview.c). */
Screen *slideview_create(App *app, const char *path);
#endif
