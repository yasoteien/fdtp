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
    x->queue[(x->head + x->count++) % QUEUE] = (rmp_key_event){ code, value };
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
    x->net_state = atom(c, "_NET_WM_STATE");
    x->net_fullscreen = atom(c, "_NET_WM_STATE_FULLSCREEN");
    x->aspect_w = w;
    x->aspect_h = h;
    fit_work_area(x, &w, &h);
    x->w = w;
    x->h = h;
    x->fullscreen = fullscreen != 0;
    x->window = xcb_generate_id(c);
    const uint32_t values[2] = { x->screen->black_pixel,
                                 XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_EXPOSURE |
                                     XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE |
                                     XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, x->window, x->screen->root, 0, 0, (uint16_t)w, (uint16_t)h, 0,
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
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, x->window, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32,
                        18, hints);
    if (fullscreen)
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, x->window, x->net_state, XCB_ATOM_ATOM, 32, 1,
                            &x->net_fullscreen);
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

int rmp_x11_dispatch(RmpX11 *x)
{
    xcb_generic_event_t *ev, *next = NULL;
    while ((ev = next ? next : xcb_poll_for_event(x->c)) != NULL) {
        next = NULL;
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
                push(x, KEY_F10, 1);   /* as F10: quit, with confirmation */
                push(x, KEY_F10, 0);
            }
            break;
        }
        default:
            break;
        }
        free(ev);
    }
    return xcb_connection_has_error(x->c) ? -1 : 0;
}

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
