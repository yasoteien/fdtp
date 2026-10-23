// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_ORDER_BOX_H
#define FDTP_ORDER_BOX_H
#include "app.h"
#include <stddef.h>
/* Revision 1 items 37 and 39: the box that puts files in order before they
 * are joined (Alt+h) or merged into one PDF (the PDF menu), as 卓駆★'s
 * 「連結するファイルと連結の順番」: a floating box (revision 1 §7) with the
 * names in a list, ↑↓ to choose one, 上へ・下へ to move it, RET to accept,
 * Esc to give up. It copies names; done is called once, with the names in
 * the chosen order (valid during the call only) and accepted 1, or with
 * NULL, 0, 0 when given up, after the box has gone. Returns 0, or -1 after
 * a notice (no memory). */
typedef void (*OrderBoxDone)(App *app, void *context, const char *const *names, size_t count, int accepted);
int order_box_open(App *app, const char *title, const char *const *names, size_t count,
                   OrderBoxDone done, void *context);
#endif
