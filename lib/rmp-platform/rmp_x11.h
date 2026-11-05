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
 * options (NULL, or each NULL / 0: rmp-jukebox's name, no icon), and
 * place_apart (the first window of its WM_CLASS in the middle of the work
 * area, the next at the right, the left, the middle lower down...),
 * window_w / window_h (another first size; w:h stays the aspect), positioned
 * x, y (the frame there), close_key (< 0: rmp_x11_take_close). NULL on
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
/* Withdraws it (unmap + the ICCCM synthetic UnmapNotify). */
void rmp_x11_hide(RmpX11 *x);
/* Maps it if needed, then raises it and asks for the focus
 * (_NET_ACTIVE_WINDOW once the window manager has mapped it). */
void rmp_x11_activate(RmpX11 *x);
/* The options' close_key < 0: the close button sets a flag instead of
 * queuing a key. Take: 1 once after a press. Pending: without taking. */
int rmp_x11_take_close(RmpX11 *x);
int rmp_x11_close_pending(const RmpX11 *x);
/* Connects to $DISPLAY just to read the screen's size and the work area
 * (x, y, w, h; the whole screen without _NET_WORKAREA). 0, or -1 (no X). */
int rmp_x11_probe(int *screen_w, int *screen_h, int work[4]);
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

/* Pointer events in window pixels (rmp_poll_pointer maps them to the
 * display). Motion is queued only while a button is held, and a run of
 * motion events is kept as the last one. */
int rmp_x11_pointer_pending(const RmpX11 *x);
int rmp_x11_poll_pointer(RmpX11 *x, rmp_pointer_event *out);

/* For drag and drop (rmp_xdnd.c). The hook sees every event first (an
 * xcb_generic_event_t *) and returns 1 when it took it (it is then not
 * handled here); after the events of each rmp_x11_dispatch it is called
 * once more with NULL (time-outs). NULL hook: none. */
typedef int (*rmp_x11_event_hook)(void *ctx, const void *event);
void rmp_x11_set_event_hook(RmpX11 *x, rmp_x11_event_hook hook, void *ctx);
uint32_t rmp_x11_window_id(const RmpX11 *x);
uint32_t rmp_x11_root(const RmpX11 *x);
/* The server time of the last key, button or motion event (0: none yet). */
uint32_t rmp_x11_last_time(const RmpX11 *x);

#ifdef __cplusplus
}
#endif
#endif
