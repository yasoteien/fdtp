// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_FILEOPS_H
#define FDTP_FILEOPS_H
#include "filer.h"
/* Begin an interactive operation, including its input/confirmation when needed.
 * selection and its strings are borrowed for this call. Any implementation
 * retaining them must copy them before returning. names are relative to
 * directory; current is the cursor name (also relative), and count may be zero.
 * Directory operations use current/directory, not the marked-file names.
 * Returns 0 once the request is accepted, -1 on failure/cancellation. */
/* Begin copy/move to an already selected directory. */
int fileops_transfer_to(App *, const FileSelection *, const char *, int move);
int fileops_copy(App *app, const FileSelection *selection);
int fileops_move(App *app, const FileSelection *selection);
int fileops_delete(App *app, const FileSelection *selection);
int fileops_rename(App *app, const FileSelection *selection);
int fileops_mkdir(App *app, const FileSelection *selection);
int fileops_rmdir(App *app, const FileSelection *selection);
int fileops_attributes(App *app, const FileSelection *selection);
/* Shift+F2 / F12: asks for a path, then the file system's information box. */
int fileops_info(App *app, const FileSelection *selection);
int fileops_execute(App *app, const FileSelection *selection);
int fileops_shell(App *app, const FileSelection *selection);
int fileops_tree(App *app, const FileSelection *selection);
int fileops_find_directory(App *app, const FileSelection *selection);
int fileops_write_directory(App *app, const FileSelection *selection);
int fileops_copy_tree(App *app, const FileSelection *selection);
int fileops_move_tree(App *app, const FileSelection *selection);
int fileops_attributes_directory(App *app, const FileSelection *selection);
/* Revision 1 §1 8 (i): the box of the cursor's file (current), widgets_info. */
int fileops_file_info(App *app, const FileSelection *selection);
/* Revision 1 §2: F8 in a destination or path input opens the directory tree
 * (widgets_tree_hook). The entrances (fdtp, headless) call it after app_init. */
void fileops_tree_install(App *app);
#endif
