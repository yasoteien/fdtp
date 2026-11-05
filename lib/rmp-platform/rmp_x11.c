// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* X11 output through XCB. See rmp_x11.h. */
#include "rmp_x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#ifndef KEY_F10
#define KEY_F10 68
#endif
#ifndef KEY_F11
#define KEY_F11 87
#endif
#ifndef KEY_ENTER
#define KEY_ENTER 28
#define KEY_LEFTALT 56
#define KEY_RIGHTALT 100
#endif

#define KEYCODES 256     /* X keycodes 8..255 -> KEY_* 0..247 */
#define QUEUE 256

struct RmpX11 {
    xcb_connection_t *c;
    int close_key;                  /* the close button's key (rmp_platform_options); < 0: none, `closed` */
    int closed;                     /* close_key < 0: the close button was pressed */
    int mapped;                     /* MapNotify seen, UnmapNotify not since */
    int activate_pending;           /* rmp_x11_activate before the window was mapped */
    int screen_num;
    xcb_screen_t *screen;
    xcb_window_t window;
    xcb_atom_t wm_protocols, wm_delete, net_state, net_fullscreen;
    int w, h, resized, exposed;
    int fullscreen;                 /* last request, reconciled with WM state */
    int aspect_w, aspect_h;          /* the size the window is kept at (w:h) */
    int asked_w, asked_h;            /* the last size asked for (no loop) */
    unsigned char down[KEYCODES];
    unsigned char eaten[KEYCODES];   /* pressed as full screen: release not queued */
    rmp_key_event queue[QUEUE];
    int head, count;
    rmp_pointer_event pointer[QUEUE];
    int pointer_head, pointer_count;
    uint32_t last_time;              /* of the last key, button or motion event */
    rmp_x11_event_hook hook;         /* drag and drop (rmp_xdnd.c) */
    void *hook_ctx;
};

static xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void push(RmpX11 *x, int code, int value)
{
    if (x->count == QUEUE) return;   /* the app is not reading: drop */
    x->queue[(x->head + x->count++) % QUEUE] = (rmp_key_event){ code, value, 0 };
}

static unsigned mods_of(uint16_t state)
{
    return (state & XCB_MOD_MASK_SHIFT ? (unsigned)RMP_MOD_SHIFT : 0u) |
           (state & XCB_MOD_MASK_CONTROL ? (unsigned)RMP_MOD_CTRL : 0u) |
           (state & XCB_MOD_MASK_1 ? (unsigned)RMP_MOD_ALT : 0u);
}

static void push_pointer(RmpX11 *x, rmp_pointer_event e)
{
    if (e.type == RMP_POINTER_MOTION && x->pointer_count) {   /* keep the last of a run */
        rmp_pointer_event *last = &x->pointer[(x->pointer_head + x->pointer_count - 1) % QUEUE];
        if (last->type == RMP_POINTER_MOTION) {
            *last = e;
            return;
        }
    }
    if (x->pointer_count == QUEUE) return;   /* the app is not reading: drop */
    x->pointer[(x->pointer_head + x->pointer_count++) % QUEUE] = e;
}

int rmp_x11_is_fullscreen(const RmpX11 *x) { return x->fullscreen; }

void rmp_x11_set_fullscreen(RmpX11 *x, int fullscreen)
{
    fullscreen = fullscreen != 0;
    if (fullscreen == x->fullscreen) return;
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = x->window;
    ev.type = x->net_state;
    ev.data.data32[0] = (uint32_t)fullscreen;   /* _NET_WM_STATE_ADD / REMOVE */
    ev.data.data32[1] = x->net_fullscreen;
    ev.data.data32[3] = 1;   /* source: application */
    xcb_send_event(x->c, 0, x->screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&ev);
    xcb_flush(x->c);
    x->fullscreen = fullscreen;
}

/* Title bar and borders, which _NET_WORKAREA does not leave out. */
#define DECORATION_H 40

/* A window larger than the work area gets maximized by some window managers
 * (xfwm4 on the Pomera: 1024x600 under a 27 px panel), and a maximized
 * window ignores the aspect hint. Start at the largest w:h size that fits. */
static void fit_work_area(RmpX11 *x, int *w, int *h)
{
    xcb_atom_t work = atom(x->c, "_NET_WORKAREA");
    if (work == XCB_ATOM_NONE) return;
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(x->c, xcb_get_property(x->c, 0, x->screen->root, work, XCB_ATOM_CARDINAL, 0, 4), NULL);
    if (r && xcb_get_property_value_length(r) >= 16) {
        const uint32_t *v = xcb_get_property_value(r);
        int aw = (int)v[2], ah = (int)v[3] - DECORATION_H;
        if (aw > 0 && ah > 0 && (*w > aw || *h > ah)) {
            int fw = aw, fh = (int)((long)aw * x->aspect_h / x->aspect_w);
            if (fh > ah) {
                fh = ah;
                fw = (int)((long)ah * x->aspect_w / x->aspect_h);
            }
            *w = fw;
            *h = fh;
        }
    }
    free(r);
}

/* The second part of a window's WM_CLASS is class_name (0 when not, or unreadable). */
static int has_class(RmpX11 *x, xcb_window_t w, const char *class_name)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(x->c, xcb_get_property(x->c, 0, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 64), NULL);
    int yes = 0;
    if (r && r->format == 8) {
        const char *v = xcb_get_property_value(r);
        int n = xcb_get_property_value_length(r);
        const char *zero = memchr(v, 0, (size_t)n);
        if (zero) {
            const char *cls = zero + 1;
            size_t left = (size_t)(v + n - cls), len = strlen(class_name);
            yes = left >= len && !memcmp(cls, class_name, len) && (left == len || cls[len] == 0);
        }
    }
    free(r);
    return yes;
}

/* place_apart (rmp_platform_options): how many windows of the class the
 * window manager lists (_NET_CLIENT_LIST) decides where this one goes, so
 * that the others stay in view to drop on (as FDTM): the first in the
 * middle of the work area, then right, left, middle again 48 dots lower,
 * and so on. 1 with *px, *py set, or 0 (no list: the window manager's). */
static int place_apart(RmpX11 *x, const char *class_name, int w, int h, int *px, int *py)
{
    xcb_atom_t list = atom(x->c, "_NET_CLIENT_LIST"), work = atom(x->c, "_NET_WORKAREA");
    if (list == XCB_ATOM_NONE || work == XCB_ATOM_NONE) return 0;
    int others = 0;
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(x->c, xcb_get_property(x->c, 0, x->screen->root, list, XCB_ATOM_WINDOW, 0, 4096), NULL);
    if (r && r->format == 32) {
        const xcb_window_t *v = xcb_get_property_value(r);
        int n = xcb_get_property_value_length(r) / 4;
        for (int i = 0; i < n; i++) others += has_class(x, v[i], class_name);
    }
    if (!r) return 0;
    free(r);
    int ax = 0, ay = 0, aw = x->screen->width_in_pixels, ah = x->screen->height_in_pixels;
    r = xcb_get_property_reply(x->c, xcb_get_property(x->c, 0, x->screen->root, work, XCB_ATOM_CARDINAL, 0, 4), NULL);
    if (r && xcb_get_property_value_length(r) >= 16) {
        const uint32_t *v = xcb_get_property_value(r);
        ax = (int)v[0];
        ay = (int)v[1];
        aw = (int)v[2];
        ah = (int)v[3];
    }
    free(r);
    const int border = 4;   /* the frame's side borders, roughly */
    int right = ax + aw - w - 2 * border, middle = ax + (aw - w) / 2;
    if (right < ax) right = ax;
    int slot = others % 3, down = 48 * (others / 3);
    int y = ay + (ah - h - DECORATION_H) / 2;
    if (y < ay) y = ay;
    y += down;
    if (y + h + DECORATION_H > ay + ah) y = ay + ah - h - DECORATION_H;
    if (y < ay) y = ay;
    *px = slot == 1 ? right : slot == 2 ? ax : middle;
    *py = y;
    return 1;
}

/* The window's names and icon (rmp_platform_options): WM_NAME, WM_CLASS,
 * and when the app gave them _NET_WM_NAME and _NET_WM_ICON. */
static void set_names(RmpX11 *x, const rmp_platform_options *o)
{
    static const char jukebox[] = "rmp-jukebox";
    const char *title = o && o->title ? o->title : jukebox;
    const char *instance = o && o->wm_instance ? o->wm_instance : o && o->wm_class ? o->wm_class : jukebox;
    const char *class_name = o && o->wm_class ? o->wm_class : instance;
    xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        (uint32_t)strlen(title), title);
    /* WM_CLASS: "instance\0class\0". */
    size_t ni = strlen(instance) + 1, nc = strlen(class_name) + 1;
    char *wm_class = malloc(ni + nc);
    if (wm_class) {
        memcpy(wm_class, instance, ni);
        memcpy(wm_class + ni, class_name, nc);
        xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            (uint32_t)(ni + nc), wm_class);
        free(wm_class);
    }
    if (o && o->title) {
        xcb_atom_t net_name = atom(x->c, "_NET_WM_NAME"), utf8 = atom(x->c, "UTF8_STRING");
        if (net_name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE)
            xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->window, net_name, utf8, 8,
                                (uint32_t)strlen(o->title), o->title);
    }
    if (o && o->icon && o->icon_words >= 2) {
        xcb_atom_t net_icon = atom(x->c, "_NET_WM_ICON");
        if (net_icon != XCB_ATOM_NONE)
            xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->window, net_icon, XCB_ATOM_CARDINAL, 32,
                                (uint32_t)o->icon_words, o->icon);
    }
}

RmpX11 *rmp_x11_open(int w, int h, int fullscreen, int quiet, int resizable, const rmp_platform_options *names)
{
    const char *display = getenv("DISPLAY");
    if (!display || !*display) {
        if (!quiet) fprintf(stderr, "x11: DISPLAY is not set\n");
        return NULL;
    }
    int screen_num = 0;
    xcb_connection_t *c = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(c)) {
        if (!quiet) fprintf(stderr, "x11: cannot connect to %s\n", display);
        xcb_disconnect(c);
        return NULL;
    }
    RmpX11 *x = calloc(1, sizeof *x);
    if (!x) {
        xcb_disconnect(c);
        return NULL;
    }
    x->c = c;
    x->screen_num = screen_num;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_num && it.rem; i++) xcb_screen_next(&it);
    x->screen = it.data;
    x->wm_protocols = atom(c, "WM_PROTOCOLS");
    x->wm_delete = atom(c, "WM_DELETE_WINDOW");
    x->close_key = names && names->close_key > 0 ? names->close_key : names && names->close_key < 0 ? -1 : KEY_F10;
    x->net_state = atom(c, "_NET_WM_STATE");
    x->net_fullscreen = atom(c, "_NET_WM_STATE_FULLSCREEN");
    x->aspect_w = w;
    x->aspect_h = h;
    if (names && names->window_w > 0 && names->window_h > 0) {   /* another first size, same w:h */
        w = names->window_w;
        h = names->window_h;
    }
    fit_work_area(x, &w, &h);
    x->w = w;
    x->h = h;
    x->fullscreen = fullscreen != 0;
    int px = 0, py = 0, placed = 0;
    if (names && names->positioned && !fullscreen) {
        px = names->x;
        py = names->y;
        placed = 1;
    } else if (names && names->place_apart && !fullscreen) {
        const char *cls = names->wm_class ? names->wm_class : names->wm_instance ? names->wm_instance : "rmp-jukebox";
        placed = place_apart(x, cls, w, h, &px, &py);
    }
    x->window = xcb_generate_id(c);
    const uint32_t values[2] = { x->screen->black_pixel,
                                 XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_EXPOSURE |
                                     XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                     XCB_EVENT_MASK_BUTTON_MOTION |
                                     XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE |
                                     XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, x->window, x->screen->root, (int16_t)px, (int16_t)py, (uint16_t)w, (uint16_t)h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, x->screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    set_names(x, names);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, x->window, x->wm_protocols, XCB_ATOM_ATOM, 32, 1, &x->wm_delete);
    /* WM_NORMAL_HINTS (ICCCM WM_SIZE_HINTS, 18 words): keep the window at
     * the display's w:h, at least half of it, so the window manager sizes the window itself
     * and the scaled display has no bars (full screen still may). */
    const uint32_t aw = (uint32_t)x->aspect_w, ah = (uint32_t)x->aspect_h;
    uint32_t hints[18] = { 16 | 128 /* PMinSize | PAspect */, 0, 0, 0, 0, aw / 2, ah / 2, 0, 0, 0, 0,
                           aw, ah, aw, ah, 0, 0, 0 };
    if (resizable) {   /* any size: only a least one (PMinSize) */
        hints[0] = 16;
        hints[5] = 480;
        hints[6] = 300;
        for (int i = 11; i < 15; i++) hints[i] = 0;
    }
    if (placed) {   /* USPosition | PPosition: xfwm4 keeps it */
        hints[0] |= 1 | 4;
        hints[1] = (uint32_t)px;
        hints[2] = (uint32_t)py;
    }
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, x->window, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32,
                        18, hints);
    if (fullscreen)
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, x->window, x->net_state, XCB_ATOM_ATOM, 32, 1,
                            &x->net_fullscreen);
    if (names && names->map_unfocused) {   /* EWMH: user time 0 = do not focus it on map */
        const uint32_t zero = 0;
        xcb_atom_t user_time = atom(c, "_NET_WM_USER_TIME");
        if (user_time != XCB_ATOM_NONE)
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, x->window, user_time, XCB_ATOM_CARDINAL, 32, 1, &zero);
    }
    xcb_flush(c);
    return x;
}

void rmp_x11_close(RmpX11 *x)
{
    if (!x) return;
    if (x->window) xcb_destroy_window(x->c, x->window);
    xcb_disconnect(x->c);
    free(x);
}

void *rmp_x11_connection(RmpX11 *x) { return x->c; }
int rmp_x11_screen(const RmpX11 *x) { return x->screen_num; }
uint32_t rmp_x11_visual(const RmpX11 *x) { return x->screen->root_visual; }
void *rmp_x11_window(RmpX11 *x) { return &x->window; }
int rmp_x11_fd(const RmpX11 *x) { return xcb_get_file_descriptor(x->c); }

void rmp_x11_show(RmpX11 *x)
{
    xcb_map_window(x->c, x->window);
    xcb_flush(x->c);
}

static void key_press(RmpX11 *x, int code)
{
    if (code < 0 || code >= KEYCODES) return;
    int repeat = x->down[code];
    x->down[code] = 1;
    /* Full screen: F11, or Alt+Enter (the Pomera has no F11). Neither
     * reaches the app, so Alt+Enter never acts as Enter. */
    if (!repeat && (code == KEY_F11 || (code == KEY_ENTER && (x->down[KEY_LEFTALT] || x->down[KEY_RIGHTALT])))) {
        x->eaten[code] = 1;
        rmp_x11_set_fullscreen(x, !x->fullscreen);
    }
    if (x->eaten[code]) return;
    push(x, code, repeat ? 2 : 1);
}

static void key_release(RmpX11 *x, int code)
{
    if (code < 0 || code >= KEYCODES || !x->down[code]) return;
    x->down[code] = 0;
    if (x->eaten[code]) x->eaten[code] = 0;
    else push(x, code, 0);
}

/* Window managers may ignore the WM_NORMAL_HINTS aspect (xfwm4 on the
 * Pomera placed a 1024x549 window under its panel): ask for the largest
 * 1024:600 size inside the one given, so no bars show. Not when the window
 * covers the screen (full screen: bars are expected there), and only once
 * per size so a window manager that refuses cannot make a loop. */
static void keep_aspect(RmpX11 *x)
{
    if (x->w == x->screen->width_in_pixels && x->h == x->screen->height_in_pixels) return;
    int w = x->w, h = (int)((long)x->w * x->aspect_h / x->aspect_w);
    if (h > x->h) {
        h = x->h;
        w = (int)((long)x->h * x->aspect_w / x->aspect_h);
    }
    if ((w == x->w && h == x->h) || (w == x->asked_w && h == x->asked_h)) return;
    x->asked_w = w;
    x->asked_h = h;
    const uint32_t size[2] = { (uint32_t)w, (uint32_t)h };
    xcb_configure_window(x->c, x->window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    xcb_flush(x->c);
}

/* WM actions (including its own full screen shortcut) can change the
 * property independently of our requests. An absent property means off;
 * keep the cached state on an X error or malformed property. */
static void read_fullscreen(RmpX11 *x)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(x->c,
        xcb_get_property(x->c, 0, x->window, x->net_state, XCB_ATOM_ATOM, 0, UINT32_MAX), NULL);
    if (!r) return;
    if (r->type == XCB_ATOM_NONE) x->fullscreen = 0;
    else if (r->type == XCB_ATOM_ATOM && r->format == 32 && !r->bytes_after) {
        const xcb_atom_t *states = xcb_get_property_value(r);
        int count = xcb_get_property_value_length(r) / (int)sizeof *states;
        x->fullscreen = 0;
        for (int i = 0; i < count; i++)
            if (states[i] == x->net_fullscreen) x->fullscreen = 1;
    }
    free(r);
}

/* _NET_ACTIVE_WINDOW (source 2, as a pager: xfwm4's focus stealing
 * prevention lets it through) and a raise for window managers without EWMH. */
static void send_activate(RmpX11 *x)
{
    xcb_atom_t active = atom(x->c, "_NET_ACTIVE_WINDOW");
    if (active != XCB_ATOM_NONE) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = x->window;
        ev.type = active;
        ev.data.data32[0] = 2;
        ev.data.data32[1] = x->last_time;   /* 0 (CurrentTime) before any input */
        xcb_send_event(x->c, 0, x->screen->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&ev);
    }
    const uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(x->c, x->window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    xcb_flush(x->c);
}

void rmp_x11_activate(RmpX11 *x)
{
    if (x->mapped) {
        send_activate(x);
        return;
    }
    /* Not managed yet: the window manager would ignore the request. */
    x->activate_pending = 1;
    rmp_x11_show(x);
}

void rmp_x11_hide(RmpX11 *x)
{
    x->activate_pending = 0;
    xcb_unmap_window(x->c, x->window);
    /* ICCCM 4.1.4: withdrawn also when the window manager has not mapped it yet. */
    xcb_unmap_notify_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_UNMAP_NOTIFY;
    ev.event = x->screen->root;
    ev.window = x->window;
    xcb_send_event(x->c, 0, x->screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&ev);
    xcb_flush(x->c);
}

int rmp_x11_take_close(RmpX11 *x)
{
    int c = x->closed;
    x->closed = 0;
    return c;
}

int rmp_x11_close_pending(const RmpX11 *x) { return x->closed; }

int rmp_x11_probe(int *screen_w, int *screen_h, int work[4])
{
    const char *display = getenv("DISPLAY");
    if (!display || !*display) return -1;
    int screen_num = 0;
    xcb_connection_t *c = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(c)) {
        xcb_disconnect(c);
        return -1;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_num && it.rem; i++) xcb_screen_next(&it);
    xcb_screen_t *screen = it.data;
    *screen_w = screen->width_in_pixels;
    *screen_h = screen->height_in_pixels;
    work[0] = work[1] = 0;
    work[2] = *screen_w;
    work[3] = *screen_h;
    xcb_atom_t area = atom(c, "_NET_WORKAREA");
    if (area != XCB_ATOM_NONE) {
        xcb_get_property_reply_t *r =
            xcb_get_property_reply(c, xcb_get_property(c, 0, screen->root, area, XCB_ATOM_CARDINAL, 0, 4), NULL);
        if (r && xcb_get_property_value_length(r) >= 16) {
            const uint32_t *v = xcb_get_property_value(r);
            if (v[2] > 0 && v[3] > 0)
                for (int i = 0; i < 4; i++) work[i] = (int)v[i];
        }
        free(r);
    }
    xcb_disconnect(c);
    return 0;
}

int rmp_x11_dispatch(RmpX11 *x)
{
    xcb_generic_event_t *ev, *next = NULL;
    while ((ev = next ? next : xcb_poll_for_event(x->c)) != NULL) {
        next = NULL;
        switch (ev->response_type & 0x7f) {   /* the server time, for selections */
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE:
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
        case XCB_MOTION_NOTIFY:
            x->last_time = ((const xcb_key_press_event_t *)ev)->time;
            break;
        default:
            break;
        }
        if (x->hook && x->hook(x->hook_ctx, ev)) {
            free(ev);
            continue;
        }
        switch (ev->response_type & 0x7f) {
        case XCB_KEY_PRESS:
            key_press(x, ((xcb_key_press_event_t *)ev)->detail - 8);
            break;
        case XCB_KEY_RELEASE: {
            /* Autorepeat: a release followed by a press of the same key at
             * the same time. Keep the key down; the press becomes value 2. */
            const xcb_key_release_event_t *r = (const xcb_key_release_event_t *)ev;
            next = xcb_poll_for_queued_event(x->c);
            if (next && (next->response_type & 0x7f) == XCB_KEY_PRESS) {
                const xcb_key_press_event_t *p = (const xcb_key_press_event_t *)next;
                if (p->detail == r->detail && p->time == r->time) break;
            }
            key_release(x, r->detail - 8);
            break;
        }
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE: {
            const xcb_button_press_event_t *b = (const xcb_button_press_event_t *)ev;
            const int press = (ev->response_type & 0x7f) == XCB_BUTTON_PRESS;
            rmp_pointer_event e = { 0, b->event_x, b->event_y, b->detail, 0, mods_of(b->state), 0, 0 };
            if (b->detail == 4 || b->detail == 5) {   /* the wheel: one step per press */
                if (!press) break;
                e.type = RMP_POINTER_WHEEL;
                e.wheel = b->detail == 4 ? -1 : +1;
                e.button = 0;
            } else if (b->detail >= 1 && b->detail <= 3) {
                e.type = press ? RMP_POINTER_PRESS : RMP_POINTER_RELEASE;
            } else {
                break;   /* sideways wheel, extra buttons */
            }
            push_pointer(x, e);
            break;
        }
        case XCB_MOTION_NOTIFY: {
            const xcb_motion_notify_event_t *m = (const xcb_motion_notify_event_t *)ev;
            push_pointer(x, (rmp_pointer_event){ RMP_POINTER_MOTION, m->event_x, m->event_y, 0, 0, mods_of(m->state), 0, 0 });
            break;
        }
        case XCB_FOCUS_OUT:
            /* Keys released elsewhere never reach this window. */
            for (int code = 0; code < KEYCODES; code++) key_release(x, code);
            break;
        case XCB_CONFIGURE_NOTIFY: {
            const xcb_configure_notify_event_t *e = (const xcb_configure_notify_event_t *)ev;
            if (e->window == x->window && (e->width != x->w || e->height != x->h)) {
                x->w = e->width;
                x->h = e->height;
                x->resized = 1;
                keep_aspect(x);
            }
            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            const xcb_property_notify_event_t *e = (const xcb_property_notify_event_t *)ev;
            if (e->window == x->window && e->atom == x->net_state) read_fullscreen(x);
            break;
        }
        case XCB_EXPOSE:
            if (((xcb_expose_event_t *)ev)->count == 0) x->exposed = 1;
            break;
        case XCB_CLIENT_MESSAGE: {
            const xcb_client_message_event_t *m = (const xcb_client_message_event_t *)ev;
            if (m->type == x->wm_protocols && m->data.data32[0] == x->wm_delete) {
                if (x->close_key < 0) {
                    x->closed = 1;
                } else {
                    push(x, x->close_key, 1);   /* F10 by default: quit, with confirmation */
                    push(x, x->close_key, 0);
                }
            }
            break;
        }
        case XCB_MAP_NOTIFY:
            if (((const xcb_map_notify_event_t *)ev)->window == x->window) {
                x->mapped = 1;
                if (x->activate_pending) {
                    x->activate_pending = 0;
                    send_activate(x);
                }
            }
            break;
        case XCB_UNMAP_NOTIFY:
            if (((const xcb_unmap_notify_event_t *)ev)->window == x->window) x->mapped = 0;
            break;
        default:
            break;
        }
        free(ev);
    }
    if (x->hook) x->hook(x->hook_ctx, NULL);
    return xcb_connection_has_error(x->c) ? -1 : 0;
}

void rmp_x11_set_event_hook(RmpX11 *x, rmp_x11_event_hook hook, void *ctx)
{
    x->hook = hook;
    x->hook_ctx = ctx;
}
uint32_t rmp_x11_window_id(const RmpX11 *x) { return x->window; }
uint32_t rmp_x11_root(const RmpX11 *x) { return x->screen->root; }
uint32_t rmp_x11_last_time(const RmpX11 *x) { return x->last_time; }

void rmp_x11_size(const RmpX11 *x, int *w, int *h)
{
    *w = x->w;
    *h = x->h;
}

int rmp_x11_take_resized(RmpX11 *x)
{
    int r = x->resized;
    x->resized = 0;
    return r;
}

int rmp_x11_take_exposed(RmpX11 *x)
{
    int r = x->exposed;
    x->exposed = 0;
    return r;
}

int rmp_x11_keys_pending(const RmpX11 *x) { return x->count > 0; }

int rmp_x11_poll_key(RmpX11 *x, rmp_key_event *out)
{
    if (!x->count) return 0;
    *out = x->queue[x->head];
    x->head = (x->head + 1) % QUEUE;
    x->count--;
    return 1;
}

int rmp_x11_key_down(const RmpX11 *x, int code) { return code >= 0 && code < KEYCODES && x->down[code]; }

int rmp_x11_pointer_pending(const RmpX11 *x) { return x->pointer_count > 0; }

int rmp_x11_poll_pointer(RmpX11 *x, rmp_pointer_event *out)
{
    if (!x->pointer_count) return 0;
    *out = x->pointer[x->pointer_head];
    x->pointer_head = (x->pointer_head + 1) % QUEUE;
    x->pointer_count--;
    return 1;
}
