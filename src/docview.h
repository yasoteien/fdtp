// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DOCVIEW_H
#define FDTP_DOCVIEW_H
#include "screen.h"
/* path is borrowed during creation; an implemented screen keeps its own copy.
 * Returns an owned screen for app_push(), or NULL after reporting a notice.
 * On app_push() failure the caller must destroy the returned screen.
 * Stage 5 (docs/FDTP_VIEWERS.md): views a .docx; RET/e edits the paragraph
 * under the cursor on row 23, o adds a paragraph, C-k deletes one, C-s
 * saves (only the main document part is rewritten; NAME.bak is made before
 * the first save), Esc/q goes back. The model is in doc_model.h.
 * Revision 1 item 38: C-z / C-y undo and redo one paragraph's change
 * (UNDO_LEVELS steps, doc_undo.h). */
Screen *docview_create(App *app, const char *path);
#endif
