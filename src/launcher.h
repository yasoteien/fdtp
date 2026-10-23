// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_LAUNCHER_H
#define FDTP_LAUNCHER_H
#include "screen.h"
#include <stddef.h>
typedef enum {
    LAUNCHER_TEXT, LAUNCHER_IMAGE, LAUNCHER_PDF, LAUNCHER_SHEET,
    LAUNCHER_DOC, LAUNCHER_SLIDE, LAUNCHER_ARCHIVE, LAUNCHER_EXTERNAL,
    LAUNCHER_NOVEL, LAUNCHER_MIDI
} LauncherKind;
/* Borrowed path. The longest registered extension that ends the name wins
 * (case-insensitive); unregistered names are text. */
LauncherKind launcher_kind(const char *path);
/* Add or replace the handler of an extension (".tar.gz", "tar.gz" or
 * "*.tar.gz"). command is an internal viewer name (textview imageview
 * pdfview sheetview docview slideview noveledit archive jukebox) or a shell command
 * run by /bin/sh -c in the file's directory, where %C is the file's path
 * (quoted; appended when the command has no %C). Strings are copied.
 * Returns 0 on success, -1 after a notice (app may be NULL: no notice). */
int launcher_register(App *app, const char *extension, const char *command);
/* Back to the built-in table (all registrations dropped). */
void launcher_reset(void);
/* The table, for listing/customising: entry i's extension (".pdf") and its
 * command (internal name or shell command). Borrowed until the next change.
 * Returns -1 when i is out of range. */
size_t launcher_count(void);
int launcher_entry(size_t i, const char **extension, const char **command);
/* Open by extension and give any created screen to App. path is borrowed.
 * An unregistered name whose contents look binary (launcher_binary) asks
 * first (revision 1 §1 2): テキスト表示(T) バイナリ表示(B) キャンセル.
 * .mid .rcp .r36 .g36 ("jukebox") run rmp-jukebox (launcher_jukebox).
 * Returns 0 if a screen/external request was accepted, -1 on failure after
 * reporting a notice. */
int launcher_open(App *app, const char *path);
/* 1 when the first 8 KB look binary, 0 for text, -1 when unreadable.
 * Without NUL: binary when more than one byte in 16 is a control character
 * (TAB LF CR FF VT BS ESC ^Z do not count). With NULs: text only when it
 * reads as UTF-16 without a byte order mark (even length, at most one pair
 * in 32 implausible, at least one in 64 ASCII). A byte order mark and an
 * empty file are text. */
int launcher_binary(const char *path);
/* Revision 1 §1 9: `rmp-jukebox PATH` as an external command that takes the
 * screen (no terminal, flags 0); an error box when rmp-jukebox is not on
 * $PATH. Returns 0 when the request was made, -1 otherwise. */
int launcher_jukebox(App *app, const char *path);
#endif
