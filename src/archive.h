// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_ARCHIVE_H
#define FDTP_ARCHIVE_H
#include "filer.h"
/* Begin archive browsing, extraction, recursive extraction, or creation.
 * selection and its strings are borrowed; copy any data retained after return.
 * names/current are relative to selection->directory. Prompts and confirmation
 * belong to these operations. A browser screen, once created, belongs to App.
 * Returns 0 when accepted, -1 on failure/cancellation (after a notice).
 *
 * archive_list: browse the cursor's archive.
 * archive_unpack: ask the destination on row 23 (initially the current
 *   directory) and extract each archive among names, completely.
 * archive_unpack_tree: the same, choosing the destination in a tree.
 * archive_pack: the 「書庫の作成」 box (revision 1 §13): archive name with
 *   the name history (first the cursor's name, or the directory's when
 *   names are marked), format LHA/ZIP/TAR/TAR.GZ/TAR.BZ2/TAR.XZ (↑↓; the
 *   extension follows, and a typed extension chooses the format), level
 *   1-9 (←→), LHa names Shift_JIS or UTF-8, and for an existing archive,
 *   which is added to after a question, 「無条件で上書き」 or 「日付が新しい
 *   時だけ」. TAB moves between them, RET makes the archive, Esc cancels.
 *   LHa is written by lha_write.c in steps with progress in the box (Esc
 *   stops it); zip and tar run their commands. The choices are kept for
 *   the next p of the run.
 *
 * Formats: tar family (`tar -tvf`), zip (own central-directory reader,
 * `unzip`/`zip`), LHa (own header reader, `lhasa` extracts, lha_write.c
 * creates), 7z: refused.
 * Commands are found on ~/.fdtp/bin, then $PATH. Members whose names are
 * absolute, contain "..", or lead through a stored symbolic link are never
 * extracted (the request is refused with a notice). */
int archive_list(App *app, const FileSelection *selection);
int archive_unpack(App *app, const FileSelection *selection);
int archive_unpack_tree(App *app, const FileSelection *selection);
int archive_pack(App *app, const FileSelection *selection);
/* Browse the archive at path (borrowed): a screen like the filer's list.
 * RET/v opens a member: it is extracted to a new directory under the cache
 * ($FDTP_CACHE_DIR, else ~/.cache/fdtp; removed when the browser closes)
 * and opened with launcher_open(). BS goes up, and leaves at the top.
 * u/U extract the marked entries (all when nothing is marked).
 * Returns 0 when the browser was pushed (the listing is read on the next
 * tick; a failure then pops it with a notice), -1 after a notice. */
int archive_browse(App *app, const char *path);
#endif
