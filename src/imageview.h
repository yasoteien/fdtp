// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_IMAGEVIEW_H
#define FDTP_IMAGEVIEW_H
#include "screen.h"
/* path is borrowed during creation; an implemented screen keeps its own copy.
 * Returns an owned screen for app_push(), or NULL after reporting a notice.
 * On app_push() failure the caller must destroy the returned screen.
 * Pixel screen: fit/+/-/0/1, arrows, SPC/BS, i, q/Esc.
 * RGB loading for other pixel viewers is declared in img_decode.h. */
Screen *imageview_create(App *app, const char *path);
#endif
