// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* X11 output through XCB (internal to the platform): a window for EGL
 * (EGL_EXT_platform_xcb) and its keyboard. docs/PORT_X11.md.
 *
 * XCB, not Xlib: Mesa's libEGL already loads libxcb (also on the KMS path),
 * so the X11 output adds no library to the process.
 *
 * Keys arrive only while the window has the focus (nothing is grabbed).
 * X keycode - 8 = the Linux KEY_* code, so events are rmp_key_event as from
 * evdev: autorepeat (X sends release + press) becomes value 2, losing the
 * focus releases every held key, the window's close button queues F10
 * (quit with confirmation) and F11 or Alt+Enter toggles full screen (not
 * queued; the Pomera has no F11). The window asks the window manager to
 * keep it at 1024:600 (WM_NORMAL_HINTS), so a smaller window has no bars. */
#ifndef RMP_X11_H
#define RMP_X11_H

#include <stdint.h>

#include "rmp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RmpX11 RmpX11;

/* Connects to $DISPLAY and creates (does not map) a w x h window with the
 * root visual. quiet: no message when there is no X server (auto mode).
 * resizable: no aspect hint, any size from 480 x 300 (FDTP follows the
 * window). names: title, wm_instance, wm_class, icon and icon_words of the
 * options (NULL, or each NULL / 0: rmp-jukebox's name, no icon). NULL on
 * failure. */
RmpX11 *rmp_x11_open(int w, int h, int fullscreen, int quiet, int resizable, const rmp_platform_options *names);
void rmp_x11_close(RmpX11 *x);
/* For eglGetPlatformDisplayEXT(EGL_PLATFORM_XCB_EXT, connection, ...). */
void *rmp_x11_connection(RmpX11 *x);
int rmp_x11_screen(const RmpX11 *x);
uint32_t rmp_x11_visual(const RmpX11 *x);
/* For eglCreatePlatformWindowSurfaceEXT: a pointer to the xcb_window_t. */
void *rmp_x11_window(RmpX11 *x);
/* Maps the window (after the EGL surface exists). */
void rmp_x11_show(RmpX11 *x);
/* Cached requested / WM-reported state. Set uses EWMH ADD / REMOVE. */
int rmp_x11_is_fullscreen(const RmpX11 *x);
void rmp_x11_set_fullscreen(RmpX11 *x, int fullscreen);
int rmp_x11_fd(const RmpX11 *x);

/* Reads every waiting event without blocking. -1 when the connection broke. */
int rmp_x11_dispatch(RmpX11 *x);
/* Current window size. */
void rmp_x11_size(const RmpX11 *x, int *w, int *h);
/* 1 once after the size changed / the window was exposed. */
int rmp_x11_take_resized(RmpX11 *x);
int rmp_x11_take_exposed(RmpX11 *x);

int rmp_x11_keys_pending(const RmpX11 *x);
/* One queued key event: 1, or 0 when none. */
int rmp_x11_poll_key(RmpX11 *x, rmp_key_event *out);
int rmp_x11_key_down(const RmpX11 *x, int code);

#ifdef __cplusplus
}
#endif
#endif
