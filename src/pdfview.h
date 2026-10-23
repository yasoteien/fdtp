// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_PDFVIEW_H
#define FDTP_PDFVIEW_H
#include "screen.h"
/* path is borrowed during creation; an implemented screen keeps its own copy.
 * Returns an owned screen for app_push(), or NULL after reporting a notice.
 * On app_push() failure the caller must destroy the returned screen.
 * T1 always reports an unimplemented notice and returns NULL. */
Screen *pdfview_create(App *app, const char *path);
#endif
