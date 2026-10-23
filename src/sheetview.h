// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SHEETVIEW_H
#define FDTP_SHEETVIEW_H
#include "screen.h"
/* path is borrowed during creation; an implemented screen keeps its own copy.
 * Returns an owned screen for app_push(), or NULL after reporting a notice.
 * On app_push() failure the caller must destroy the returned screen.
 * Opens .xlsx (zip), .tsv (TAB) and anything else as csv; a file whose
 * content starts with a zip signature is treated as xlsx whatever its name.
 * Stage 4 (docs/FDTP_VIEWERS.md): view, edit on row 23, C-s saves. */
Screen *sheetview_create(App *app, const char *path);
/* Setting (default 1): copy the file to NAME.bak before the first save of
 * each opened table. config.c sets it from ~/.fdtprc. */
void sheetview_set_backup(int keep);
#endif
