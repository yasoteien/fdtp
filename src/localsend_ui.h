// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_LOCALSEND_UI_H
#define FDTP_LOCALSEND_UI_H
#include "app.h"
#include "filer.h"
/* Copies the selection; the helper runs only after choosing send/receive. */
int localsend_ui_open(App *app, const FileSelection *selection);
#endif
