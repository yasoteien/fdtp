// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* XDND (version 5) for the X11 output, internal to the platform: drops of
 * files (text/uri-list) onto the window, and drags of files out of it.
 * rmp_platform.c wraps it as rmp_dnd_* (rmp_platform.h), mapping window
 * pixels to display coordinates. XCB only, like rmp_x11.c.
 * https://www.freedesktop.org/wiki/Specifications/XDND/ */
#ifndef RMP_XDND_H
#define RMP_XDND_H

#include <stddef.h>

#include "rmp_x11.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RmpXdnd RmpXdnd;
/* As rmp_dnd_over_fn / rmp_dnd_drop_fn, in window pixels. */
typedef int (*rmp_xdnd_over_fn)(void *ctx, int wx, int wy, const char *const *paths, size_t count,
                                unsigned mods, unsigned allowed);
typedef int (*rmp_xdnd_drop_fn)(void *ctx, int wx, int wy, const char *const *paths, size_t count, int action);

/* Hooks itself into the window's events (rmp_x11_set_event_hook). NULL on failure. */
RmpXdnd *rmp_xdnd_open(RmpX11 *x);
/* Unhooks, ends a drag (as refused) and frees. Before rmp_x11_close. */
void rmp_xdnd_close(RmpXdnd *d);
/* Drops are taken (XdndAware) while over and drop are set. */
void rmp_xdnd_set_target(RmpXdnd *d, rmp_xdnd_over_fn over, rmp_xdnd_drop_fn drop, void *ctx);
/* rmp_dnd_drag / rmp_dnd_take_result. */
int rmp_xdnd_drag(RmpXdnd *d, const char *const *paths, size_t count);
int rmp_xdnd_take_result(RmpXdnd *d);

#ifdef __cplusplus
}
#endif
#endif
