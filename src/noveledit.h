// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVELEDIT_H
#define FDTP_NOVELEDIT_H
#include <stddef.h>
#include "screen.h"
#include "widgets.h"
/* Novel editor (docs/FDTP_NOVEL.md): ruby notation shown with ruby,
 * horizontal (15 rows x 40) and vertical (27 columns x 22) writing, Japanese
 * input through ime.h. A pixel screen. A rule under the title row; C-l
 * shows line numbers (horizontal 36 wide, numbers on the left; vertical,
 * numbers above the columns), each with a rule next to the text.
 * path is borrowed during creation; the screen keeps its own copy. A missing
 * file opens as a new, empty document (created by the first save; a missing
 * directory is an error box). Ctrl+Shift+s saves under another name (asked
 * in a box with the name history; an existing file asks before it is
 * overwritten) and goes on editing that file. F1 lists the keys.
 * F2 (revision 1 4): a choice of 出版社の書式に整形 / 原稿形式へ / Web 形式へ
 * (novel_format.h; each one undo group, the list of changes in an
 * information box and appended to $FDTP_FORMAT_LOG, default
 * ~/.fdtp/format.log) or Word 縦書きで書き出す (novel_docx.h; template
 * $FDTP_WORD_TEMPLATE, default ~/.fdtp/template/本文フォーマット.docx, else the
 * built-in one, written beside the file as NAME.docx). Each runs only after a y/n box; the Word
 * box says it cannot be undone. The rules and NOVEL_WORD_RUBY come from
 * App.settings (~/.fdtprc).
 * Returns an owned screen for app_push(), or NULL after reporting a notice.
 * On app_push() failure the caller must destroy the returned screen.
 * Files: the vertical/horizontal and line-number choices per file are kept in
 * $FDTP_NOVEL_STATE (default ~/.fdtp/novel-state); 30 s after a change a
 * copy goes to $FDTP_NOVEL_AUTOSAVE (default ~/.fdtp/autosave/), removed when
 * the editor is closed normally or the file is saved. */
Screen *noveledit_create(App *app, const char *path);
/* The F1 help box's rows (widgets_help; revision 1 item 25). */
const WidgetHelpItem *noveledit_help(int *count);
/* Test hook: JSON describing the editor state and the glyphs drawn by the
 * last draw (boxes in canvas pixels). Returns the length written (always
 * NUL-terminated within cap). */
size_t noveledit_debug(Screen *screen, char *out, size_t cap);
#endif
