// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SPLITJOIN_H
#define FDTP_SPLITJOIN_H
#include "app.h"
#include "fileops.h"
/* Revision 1 item 37: Alt+n splits the cursor's file into NAME.001, .002 ...
 * (SPLIT_SIZE, default 10 MB); Alt+h joins the marked files in an order
 * chosen in its box. A PDF goes to pdftools (pages, not bytes). Return 0
 * once the box is up, -1 after a notice. */
int splitjoin_split(App *app, const FileSelection *selection);
int splitjoin_join(App *app, const FileSelection *selection);
#endif
