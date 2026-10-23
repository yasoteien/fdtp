// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SYSINFO_H
#define FDTP_SYSINFO_H
#include "app.h"
/* Revision 1 item 31: the system information (Alt+i). The free space is of
 * directory (the filer's). NULL with a notice when there is no memory. */
Screen *sysinfo_create(App *app, const char *directory);
#endif
