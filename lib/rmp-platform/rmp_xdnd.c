// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* XDND for the X11 output. See rmp_xdnd.h.
 *
 * Target (another program's files dropped here): XdndEnter tells whether
 * text/uri-list is offered; the list is asked for at the first
 * XdndPosition (the files decide the action, e.g. a move within one file
 * system) and every XdndPosition is answered with XdndStatus from the
 * app's over callback; XdndDrop calls drop once the list is here, then
 * XdndFinished tells the source what was done. Modifiers are read from the
 * pointer (XdndPosition carries none).
 *
 * Source (files dragged out of here): the window owns XdndSelection and
 * grabs the pointer until the button comes up. The XdndAware window under
 * the pointer (looked for from the root down, XdndProxy honoured) gets
 * XdndEnter / XdndPosition / XdndLeave / XdndDrop; the list is given on
 * request (SelectionRequest). Over this window itself no message is sent:
 * the app's own callbacks decide, as for another program's drag.
 * Offered actions: copy and move; the suggestion follows the X convention
 * (Ctrl copy, Shift move, otherwise copy; the target may still choose move,
 * e.g. within one file system). The source never deletes anything: a target
 * that moves file URIs moves them itself. */
#define _POSIX_C_SOURCE 200809L   /* gethostname, strdup */
#include "rmp_xdnd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include "rmp_uri.h"

#define XDND_VERSION 5
#define STATUS_TIMEOUT 2.0    /* seconds without XdndStatus: refused */
#define FINISH_TIMEOUT 10.0   /* seconds without XdndFinished after a drop */
#define DATA_TIMEOUT 5.0      /* a drop waiting for the list */
#define DATA_MAX (4u << 20)   /* bytes of a uri list taken at most */

enum {
    A_AWARE, A_PROXY, A_ENTER, A_POSITION, A_STATUS, A_LEAVE, A_DROP, A_FINISHED, A_SELECTION, A_TYPELIST,
    A_COPY, A_MOVE, A_LINK, A_ASK, A_PRIVATE, A_ACTIONLIST, A_URILIST, A_TEXT, A_UTF8, A_TARGETS, A_INCR,
    A_PROPERTY, A_COUNT
};
static const char *const atom_names[A_COUNT] = {
    "XdndAware", "XdndProxy", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop",
    "XdndFinished", "XdndSelection", "XdndTypeList", "XdndActionCopy", "XdndActionMove", "XdndActionLink",
    "XdndActionAsk", "XdndActionPrivate", "XdndActionList", "text/uri-list", "text/plain;charset=utf-8",
    "UTF8_STRING", "TARGETS", "INCR", "RMP_XDND_DATA"
};

struct RmpXdnd {
    RmpX11 *x;
    xcb_connection_t *c;
    xcb_window_t win, root;
    xcb_atom_t a[A_COUNT];
    rmp_xdnd_over_fn over;
    rmp_xdnd_drop_fn drop;
    void *ctx;
    char hostname[256];
    /* Another program's drag over this window. */
    struct {
        xcb_window_t source;    /* 0: none */
        int version, uris, requested, have, dropped;
        unsigned allowed;       /* from XdndActionList (0: not given) */
        char **paths;
        size_t count;
        int wx, wy;             /* the last position, window pixels */
        unsigned position_allowed;
        double drop_at;
    } in;
    /* This window's drag. */
    struct {
        int active, released, dropped, waiting, pending, moved;
        char **paths;
        size_t count;
        char *list;             /* text/uri-list */
        size_t list_len;
        xcb_window_t target, send_to;   /* target == win: over this window */
        int version, accepted, action;
        int16_t rx, ry;         /* root coordinates */
        uint16_t state;         /* key and button mask */
        uint32_t time;
        double status_at, drop_at;
        int wx, wy;             /* over this window: window pixels */
        int result;             /* -1: none to report */
        xcb_cursor_t cursors[3];/* refused, copy, move */
        int cursor;             /* shown: -1 none yet */
        unsigned offered;       /* the XdndActionList now set */
    } out;
};

/* Defined by rmp_platform.c (rmp_platform.h); declared here to keep this
 * file's includes to rmp_x11.h. */
double rmp_now_seconds(void);

/* RMP_XDND_DEBUG=1: what the drag and drop sends and gets, on stderr. */
static int debug_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RMP_XDND_DEBUG") != NULL;
    return on;
}
#define DEBUG(...) do { if (debug_on()) { fprintf(stderr, "xdnd: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned mods_of(uint16_t state)
{
    return (state & XCB_MOD_MASK_SHIFT ? (unsigned)RMP_MOD_SHIFT : 0u) |
           (state & XCB_MOD_MASK_CONTROL ? (unsigned)RMP_MOD_CTRL : 0u) |
           (state & XCB_MOD_MASK_1 ? (unsigned)RMP_MOD_ALT : 0u);
}

static xcb_atom_t action_atom(const RmpXdnd *d, int action)
{
    return action == RMP_DND_COPY ? d->a[A_COPY] : action == RMP_DND_MOVE ? d->a[A_MOVE] : XCB_ATOM_NONE;
}

static int action_of(const RmpXdnd *d, xcb_atom_t a)
{
    return a == d->a[A_COPY] ? RMP_DND_COPY : a == d->a[A_MOVE] ? RMP_DND_MOVE : RMP_DND_NONE;
}

static void send_message(RmpXdnd *d, xcb_window_t to, xcb_atom_t type, uint32_t l0, uint32_t l1, uint32_t l2,
                         uint32_t l3, uint32_t l4)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = to;
    ev.type = type;
    ev.data.data32[0] = l0;
    ev.data.data32[1] = l1;
    ev.data.data32[2] = l2;
    ev.data.data32[3] = l3;
    ev.data.data32[4] = l4;
    xcb_send_event(d->c, 0, to, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    xcb_flush(d->c);
    DEBUG("send %s to 0x%x: %x %x %x %x %x", type == d->a[A_ENTER] ? "Enter" : type == d->a[A_POSITION] ? "Position"
          : type == d->a[A_STATUS] ? "Status" : type == d->a[A_LEAVE] ? "Leave" : type == d->a[A_DROP] ? "Drop"
          : type == d->a[A_FINISHED] ? "Finished" : "?", (unsigned)to, l0, l1, l2, l3, l4);
}

/* A property of ATOM / WINDOW values: up to max of them into out; how many. */
static int read_ids(RmpXdnd *d, xcb_window_t w, xcb_atom_t property, xcb_atom_t type, uint32_t *out, int max)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(d->c, xcb_get_property(d->c, 0, w, property, type, 0, (uint32_t)max), NULL);
    int n = 0;
    if (r && r->type == type && r->format == 32) {
        n = xcb_get_property_value_length(r) / 4;
        if (n > max) n = max;
        memcpy(out, xcb_get_property_value(r), (size_t)n * 4);
    }
    free(r);
    return n;
}

static uint16_t pointer_state(RmpXdnd *d, int16_t *rx, int16_t *ry)
{
    xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(d->c, xcb_query_pointer(d->c, d->root), NULL);
    uint16_t state = 0;
    if (r) {
        state = r->mask;
        if (rx) *rx = r->root_x;
        if (ry) *ry = r->root_y;
    }
    free(r);
    return state;
}

/* Root coordinates to this window's pixels. */
static void to_window(RmpXdnd *d, int rx, int ry, int *wx, int *wy)
{
    xcb_translate_coordinates_reply_t *r = xcb_translate_coordinates_reply(
        d->c, xcb_translate_coordinates(d->c, d->root, d->win, (int16_t)rx, (int16_t)ry), NULL);
    *wx = r ? r->dst_x : rx;
    *wy = r ? r->dst_y : ry;
    free(r);
}

static void set_aware(RmpXdnd *d, int on)
{
    if (on) {
        const uint32_t version = XDND_VERSION;
        xcb_change_property(d->c, XCB_PROP_MODE_REPLACE, d->win, d->a[A_AWARE], XCB_ATOM_ATOM, 32, 1, &version);
    } else {
        xcb_delete_property(d->c, d->win, d->a[A_AWARE]);
    }
    xcb_flush(d->c);
}

static int hook(void *ctx, const void *event);
static void offer_actions(RmpXdnd *d, unsigned mask);

RmpXdnd *rmp_xdnd_open(RmpX11 *x)
{
    RmpXdnd *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->x = x;
    d->c = rmp_x11_connection(x);
    d->win = rmp_x11_window_id(x);
    d->root = rmp_x11_root(x);
    xcb_intern_atom_cookie_t cookies[A_COUNT];
    for (int i = 0; i < A_COUNT; i++)
        cookies[i] = xcb_intern_atom(d->c, 0, (uint16_t)strlen(atom_names[i]), atom_names[i]);
    for (int i = 0; i < A_COUNT; i++) {
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(d->c, cookies[i], NULL);
        d->a[i] = r ? r->atom : XCB_ATOM_NONE;
        free(r);
    }
    if (gethostname(d->hostname, sizeof d->hostname - 1) < 0) d->hostname[0] = 0;
    d->out.result = -1;
    d->out.cursor = -1;
    /* What a drag from here offers (the targets read it at XdndEnter). */
    offer_actions(d, RMP_DND_COPY | RMP_DND_MOVE);
    xcb_flush(d->c);
    rmp_x11_set_event_hook(x, hook, d);
    return d;
}

/* ---- target ------------------------------------------------------------- */
static void reset_in(RmpXdnd *d)
{
    rmp_uri_free_paths(d->in.paths, d->in.count);
    memset(&d->in, 0, sizeof d->in);
}

static void request_list(RmpXdnd *d, uint32_t time)
{
    xcb_convert_selection(d->c, d->win, d->a[A_SELECTION], d->a[A_URILIST], d->a[A_PROPERTY], time);
    xcb_flush(d->c);
    d->in.requested = 1;
}

static unsigned allowed_in(const RmpXdnd *d, xcb_atom_t suggested)
{
    unsigned allowed = d->in.allowed;
    int s = action_of(d, suggested);
    if (s) allowed |= (unsigned)s;
    /* Ask, link or private only: a file can still be copied. */
    return allowed ? allowed : (unsigned)RMP_DND_COPY;
}

static int ask_over(RmpXdnd *d)
{
    if (!d->in.uris || !d->over) return RMP_DND_NONE;
    unsigned mods = mods_of(pointer_state(d, NULL, NULL));
    return d->over(d->ctx, d->in.wx, d->in.wy, d->in.have ? (const char *const *)d->in.paths : NULL,
                   d->in.have ? d->in.count : 0, mods, d->in.position_allowed);
}

static void finish_drop(RmpXdnd *d)
{
    int action = RMP_DND_NONE;
    if (d->in.have && d->in.count && d->drop) {
        action = ask_over(d);
        if (action != RMP_DND_NONE)
            action = d->drop(d->ctx, d->in.wx, d->in.wy, (const char *const *)d->in.paths, d->in.count, action);
    }
    send_message(d, d->in.source, d->a[A_FINISHED], d->win, action != RMP_DND_NONE ? 1u : 0u,
                 d->in.version >= 5 ? action_atom(d, action) : 0, 0, 0);
    reset_in(d);
}

static void on_enter(RmpXdnd *d, const xcb_client_message_event_t *m)
{
    reset_in(d);
    if (!d->over) return;
    d->in.source = m->data.data32[0];
    d->in.version = (int)(m->data.data32[1] >> 24);
    uint32_t types[64];
    int n;
    if (m->data.data32[1] & 1) {
        n = read_ids(d, d->in.source, d->a[A_TYPELIST], XCB_ATOM_ATOM, types, 64);
    } else {
        n = 0;
        for (int i = 2; i < 5; i++)
            if (m->data.data32[i]) types[n++] = m->data.data32[i];
    }
    for (int i = 0; i < n; i++)
        if (types[i] == d->a[A_URILIST]) d->in.uris = 1;
    uint32_t actions[16];
    n = read_ids(d, d->in.source, d->a[A_ACTIONLIST], XCB_ATOM_ATOM, actions, 16);
    for (int i = 0; i < n; i++) d->in.allowed |= (unsigned)action_of(d, actions[i]);
}

static void on_position(RmpXdnd *d, const xcb_client_message_event_t *m)
{
    xcb_window_t source = m->data.data32[0];
    if (!d->in.source || source != d->in.source) return;
    int rx = (int)(m->data.data32[2] >> 16), ry = (int)(m->data.data32[2] & 0xffff);
    to_window(d, rx, ry, &d->in.wx, &d->in.wy);
    d->in.position_allowed = allowed_in(d, m->data.data32[4]);
    if (d->in.uris && !d->in.requested) request_list(d, d->in.version >= 1 ? m->data.data32[3] : XCB_CURRENT_TIME);
    int action = ask_over(d);
    /* Bit 1: send a position for every move (no rectangle to stay quiet in). */
    send_message(d, source, d->a[A_STATUS], d->win, (action != RMP_DND_NONE ? 1u : 0u) | 2u, 0, 0,
                 action_atom(d, action));
}

static void on_drop(RmpXdnd *d, const xcb_client_message_event_t *m)
{
    if (!d->in.source || m->data.data32[0] != d->in.source) return;
    d->in.dropped = 1;
    d->in.drop_at = rmp_now_seconds();
    if (d->in.have || !d->in.uris) finish_drop(d);
    else if (!d->in.requested) request_list(d, m->data.data32[2]);
}

static void on_selection_notify(RmpXdnd *d, const xcb_selection_notify_event_t *e)
{
    if (e->requestor != d->win || e->selection != d->a[A_SELECTION] || !d->in.source) return;
    if (e->property != XCB_ATOM_NONE) {
        xcb_get_property_reply_t *r = xcb_get_property_reply(
            d->c, xcb_get_property(d->c, 1, d->win, e->property, XCB_GET_PROPERTY_TYPE_ANY, 0, DATA_MAX / 4), NULL);
        if (r && r->type != d->a[A_INCR] && r->format == 8 && !r->bytes_after) {
            rmp_uri_free_paths(d->in.paths, d->in.count);
            d->in.count = rmp_uri_list_to_paths(xcb_get_property_value(r), (size_t)xcb_get_property_value_length(r),
                                                d->hostname, &d->in.paths);
        }
        free(r);
    }
    d->in.have = 1;
    DEBUG("list: %zu paths%s%s", d->in.count, d->in.count ? ", first " : "", d->in.count ? d->in.paths[0] : "");
    if (d->in.dropped) finish_drop(d);
}

/* ---- source ------------------------------------------------------------- */
static void show_cursor(RmpXdnd *d, int which)
{
    if (which == d->out.cursor) return;
    if (!d->out.cursors[which]) {
        static const uint16_t glyph[3] = { 0 /* X_cursor */, 90 /* plus */, 52 /* fleur */ };
        xcb_font_t font = xcb_generate_id(d->c);
        xcb_open_font(d->c, font, 6, "cursor");
        d->out.cursors[which] = xcb_generate_id(d->c);
        xcb_create_glyph_cursor(d->c, d->out.cursors[which], font, font, glyph[which], (uint16_t)(glyph[which] + 1),
                                0, 0, 0, 0xffff, 0xffff, 0xffff);
        xcb_close_font(d->c, font);
    }
    d->out.cursor = which;
    xcb_change_active_pointer_grab(d->c, d->out.cursors[which], XCB_CURRENT_TIME,
                                   XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                       XCB_EVENT_MASK_POINTER_MOTION);
    xcb_flush(d->c);
}

static void end_drag(RmpXdnd *d, int result)
{
    if (!d->out.released) {
        xcb_ungrab_pointer(d->c, XCB_CURRENT_TIME);
        xcb_flush(d->c);
    }
    d->out.active = 0;
    d->out.result = result;
    DEBUG("drag ended: %d", result);
}

/* The XdndAware window at root (rx, ry), its version and where messages go
 * (XdndProxy). 0 when none. */
static xcb_window_t find_target(RmpXdnd *d, int rx, int ry, xcb_window_t *send_to, int *version)
{
    xcb_window_t w = d->root;
    for (int depth = 0; depth < 16; depth++) {
        xcb_translate_coordinates_reply_t *r = xcb_translate_coordinates_reply(
            d->c, xcb_translate_coordinates(d->c, d->root, w, (int16_t)rx, (int16_t)ry), NULL);
        xcb_window_t child = r ? r->child : XCB_WINDOW_NONE;
        free(r);
        if (child == XCB_WINDOW_NONE) break;
        w = child;
        uint32_t v;
        if (read_ids(d, w, d->a[A_AWARE], XCB_ATOM_ATOM, &v, 1) == 1 && v >= 3) {
            uint32_t proxy;
            *send_to = read_ids(d, w, d->a[A_PROXY], XCB_ATOM_WINDOW, &proxy, 1) == 1 && proxy ? proxy : w;
            *version = v < XDND_VERSION ? (int)v : XDND_VERSION;
            return w;
        }
    }
    return XCB_WINDOW_NONE;
}

/* XdndActionList: what the target may choose (GTK's way: with Ctrl or
 * Shift held only that action, so that a file manager that prefers a move
 * within one file system copies on Ctrl). */
static void offer_actions(RmpXdnd *d, unsigned mask)
{
    if (mask == d->out.offered) return;
    uint32_t actions[2];
    int n = 0;
    if (mask & RMP_DND_COPY) actions[n++] = d->a[A_COPY];
    if (mask & RMP_DND_MOVE) actions[n++] = d->a[A_MOVE];
    xcb_change_property(d->c, XCB_PROP_MODE_REPLACE, d->win, d->a[A_ACTIONLIST], XCB_ATOM_ATOM, 32, (uint32_t)n, actions);
    d->out.offered = mask;
}

static void send_position(RmpXdnd *d)
{
    unsigned mods = mods_of(d->out.state);
    int suggested = mods & RMP_MOD_CTRL ? RMP_DND_COPY : mods & RMP_MOD_SHIFT ? RMP_DND_MOVE : RMP_DND_COPY;
    offer_actions(d, mods & (RMP_MOD_CTRL | RMP_MOD_SHIFT) ? (unsigned)suggested : (unsigned)(RMP_DND_COPY | RMP_DND_MOVE));
    send_message(d, d->out.send_to, d->a[A_POSITION], d->win, 0,
                 ((uint32_t)(uint16_t)d->out.rx << 16) | (uint16_t)d->out.ry, d->out.time,
                 action_atom(d, suggested));
    d->out.waiting = 1;
    d->out.pending = 0;
    d->out.status_at = rmp_now_seconds();
}

static void leave_target(RmpXdnd *d)
{
    if (d->out.target && d->out.target != d->win)
        send_message(d, d->out.send_to, d->a[A_LEAVE], d->win, 0, 0, 0, 0);
    d->out.target = d->out.send_to = XCB_WINDOW_NONE;
    d->out.accepted = 0;
    d->out.action = RMP_DND_NONE;
    d->out.waiting = d->out.pending = 0;
}

static void drag_moved(RmpXdnd *d)
{
    d->out.moved = 0;
    xcb_window_t send_to = XCB_WINDOW_NONE;
    int version = 0;
    xcb_window_t target = find_target(d, d->out.rx, d->out.ry, &send_to, &version);
    if (target != d->out.target) {
        DEBUG("over 0x%x (send to 0x%x, version %d) at %d,%d", (unsigned)target, (unsigned)send_to, version,
              d->out.rx, d->out.ry);
        leave_target(d);
        d->out.target = target;
        d->out.send_to = send_to;
        d->out.version = version;
        if (target && target != d->win)
            send_message(d, send_to, d->a[A_ENTER], d->win, (uint32_t)version << 24, d->a[A_URILIST], d->a[A_TEXT], 0);
    }
    if (!target) {
        show_cursor(d, 0);
        return;
    }
    if (target == d->win) {
        to_window(d, d->out.rx, d->out.ry, &d->out.wx, &d->out.wy);
        int action = d->over ? d->over(d->ctx, d->out.wx, d->out.wy, (const char *const *)d->out.paths, d->out.count,
                                       mods_of(d->out.state), RMP_DND_COPY | RMP_DND_MOVE)
                             : RMP_DND_NONE;
        d->out.accepted = action != RMP_DND_NONE;
        d->out.action = action;
        show_cursor(d, action);
        return;
    }
    if (d->out.waiting) d->out.pending = 1;   /* one position at a time */
    else send_position(d);
}

static void drop_now(RmpXdnd *d)
{
    if (d->out.target == d->win) {
        int action = d->out.accepted && d->drop ? d->drop(d->ctx, d->out.wx, d->out.wy,
                                                          (const char *const *)d->out.paths, d->out.count, d->out.action)
                                                : RMP_DND_NONE;
        d->out.target = XCB_WINDOW_NONE;
        end_drag(d, action);
    } else if (d->out.target && d->out.accepted) {
        send_message(d, d->out.send_to, d->a[A_DROP], d->win, 0, d->out.time, 0, 0);
        d->out.dropped = 1;
        d->out.drop_at = rmp_now_seconds();
    } else {
        leave_target(d);
        end_drag(d, RMP_DND_NONE);
    }
}

static void drag_released(RmpXdnd *d)
{
    if (d->out.moved) drag_moved(d);
    d->out.released = 1;
    xcb_ungrab_pointer(d->c, XCB_CURRENT_TIME);
    xcb_flush(d->c);
    if (!d->out.waiting) drop_now(d);   /* else at XdndStatus, or its time-out */
}

static void on_status(RmpXdnd *d, const xcb_client_message_event_t *m)
{
    if (!d->out.active || !d->out.target || d->out.target == d->win ||
        (m->data.data32[0] != d->out.target && m->data.data32[0] != d->out.send_to))
        return;
    d->out.waiting = 0;
    d->out.accepted = (m->data.data32[1] & 1) != 0;
    d->out.action = d->out.accepted ? action_of(d, m->data.data32[4]) : RMP_DND_NONE;
    if (d->out.accepted && d->out.action == RMP_DND_NONE) d->out.action = RMP_DND_COPY;   /* link, ask, private */
    if (!d->out.released) show_cursor(d, d->out.accepted ? d->out.action : 0);
    if (d->out.released && !d->out.dropped) drop_now(d);
    else if (d->out.pending) send_position(d);
}

static void on_finished(RmpXdnd *d, const xcb_client_message_event_t *m)
{
    if (!d->out.active || !d->out.dropped ||
        (m->data.data32[0] != d->out.target && m->data.data32[0] != d->out.send_to))
        return;
    int accepted = d->out.version >= 5 ? (m->data.data32[1] & 1) != 0 : 1;
    int action = d->out.version >= 5 ? action_of(d, m->data.data32[2]) : d->out.action;
    if (accepted && action == RMP_DND_NONE) action = d->out.action;
    d->out.target = XCB_WINDOW_NONE;
    end_drag(d, accepted ? action : RMP_DND_NONE);
}

static void on_selection_request(RmpXdnd *d, const xcb_selection_request_event_t *e)
{
    xcb_selection_notify_event_t n;
    memset(&n, 0, sizeof n);
    n.response_type = XCB_SELECTION_NOTIFY;
    n.time = e->time;
    n.requestor = e->requestor;
    n.selection = e->selection;
    n.target = e->target;
    n.property = XCB_ATOM_NONE;
    xcb_atom_t property = e->property != XCB_ATOM_NONE ? e->property : e->target;   /* obsolete clients */
    if (e->selection == d->a[A_SELECTION] && d->out.list) {
        if (e->target == d->a[A_TARGETS]) {
            const uint32_t targets[4] = { d->a[A_TARGETS], d->a[A_URILIST], d->a[A_TEXT], d->a[A_UTF8] };
            xcb_change_property(d->c, XCB_PROP_MODE_REPLACE, e->requestor, property, XCB_ATOM_ATOM, 32, 4, targets);
            n.property = property;
        } else if (e->target == d->a[A_URILIST] || e->target == d->a[A_TEXT] || e->target == d->a[A_UTF8]) {
            xcb_change_property(d->c, XCB_PROP_MODE_REPLACE, e->requestor, property, e->target, 8,
                                (uint32_t)d->out.list_len, d->out.list);
            n.property = property;
        }
    }
    xcb_send_event(d->c, 0, e->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&n);
    xcb_flush(d->c);
}

static void free_out(RmpXdnd *d)
{
    rmp_uri_free_paths(d->out.paths, d->out.count);
    free(d->out.list);
    d->out.paths = NULL;
    d->out.count = 0;
    d->out.list = NULL;
    d->out.list_len = 0;
}

int rmp_xdnd_drag(RmpXdnd *d, const char *const *paths, size_t count)
{
    if (!d || d->out.active || !count) return -1;
    int16_t rx = 0, ry = 0;
    uint16_t state = pointer_state(d, &rx, &ry);
    if (!(state & XCB_BUTTON_MASK_1)) {   /* the button is up already */
        DEBUG("drag: button 1 is up (state %x)", state);
        return -1;
    }
    free_out(d);
    d->out.list = rmp_uri_list_from_paths(paths, count, &d->out.list_len);
    d->out.paths = calloc(count, sizeof *d->out.paths);
    if (!d->out.list || !d->out.paths) {
        free_out(d);
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        d->out.paths[i] = strdup(paths[i]);
        if (!d->out.paths[i]) {
            d->out.count = i;
            free_out(d);
            return -1;
        }
    }
    d->out.count = count;
    uint32_t time = rmp_x11_last_time(d->x);
    xcb_set_selection_owner(d->c, d->win, d->a[A_SELECTION], time ? time : XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *g = xcb_grab_pointer_reply(
        d->c,
        xcb_grab_pointer(d->c, 0, d->win,
                         XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                         time ? time : XCB_CURRENT_TIME),
        NULL);
    int grabbed = g && g->status == XCB_GRAB_STATUS_SUCCESS;
    DEBUG("drag %zu files: grab %d (time %u)", count, g ? g->status : -1, (unsigned)time);
    free(g);
    if (!grabbed) {
        free_out(d);
        return -1;
    }
    d->out.active = 1;
    d->out.released = d->out.dropped = d->out.waiting = d->out.pending = 0;
    d->out.target = d->out.send_to = XCB_WINDOW_NONE;
    d->out.accepted = 0;
    d->out.action = RMP_DND_NONE;
    d->out.result = -1;
    d->out.cursor = -1;
    d->out.rx = rx;
    d->out.ry = ry;
    d->out.state = state;
    d->out.time = time;
    drag_moved(d);
    return 0;
}

int rmp_xdnd_take_result(RmpXdnd *d)
{
    if (!d || d->out.active) return -1;
    int r = d->out.result;
    d->out.result = -1;
    return r;
}

/* ---- events ------------------------------------------------------------- */
static void tick(RmpXdnd *d)
{
    double now = rmp_now_seconds();
    if (d->out.active) {
        if (d->out.moved && !d->out.released) drag_moved(d);
        if (d->out.waiting && now - d->out.status_at > STATUS_TIMEOUT) {
            d->out.waiting = 0;
            d->out.accepted = 0;
            d->out.action = RMP_DND_NONE;
            if (d->out.released) drop_now(d);
            else if (d->out.pending) send_position(d);
        }
        if (d->out.active && d->out.dropped && now - d->out.drop_at > FINISH_TIMEOUT) {
            d->out.target = XCB_WINDOW_NONE;
            end_drag(d, d->out.action);   /* it was accepted: the target may well have done it */
        }
    }
    if (d->in.source && d->in.dropped && !d->in.have && now - d->in.drop_at > DATA_TIMEOUT) finish_drop(d);
}

static int hook(void *ctx, const void *event)
{
    RmpXdnd *d = ctx;
    if (!event) {
        tick(d);
        return 0;
    }
    const xcb_generic_event_t *ev = event;
    switch (ev->response_type & 0x7f) {
    case XCB_CLIENT_MESSAGE: {
        const xcb_client_message_event_t *m = event;
        if (m->format != 32) return 0;
        DEBUG("got message %u: %x %x %x %x %x", (unsigned)m->type, m->data.data32[0], m->data.data32[1],
              m->data.data32[2], m->data.data32[3], m->data.data32[4]);
        if (m->type == d->a[A_ENTER]) on_enter(d, m);
        else if (m->type == d->a[A_POSITION]) on_position(d, m);
        else if (m->type == d->a[A_LEAVE]) {
            if (m->data.data32[0] == d->in.source) reset_in(d);
        } else if (m->type == d->a[A_DROP]) on_drop(d, m);
        else if (m->type == d->a[A_STATUS]) on_status(d, m);
        else if (m->type == d->a[A_FINISHED]) on_finished(d, m);
        else return 0;
        return 1;
    }
    case XCB_SELECTION_NOTIFY:
        on_selection_notify(d, event);
        return 1;
    case XCB_SELECTION_REQUEST:
        on_selection_request(d, event);
        return 1;
    case XCB_MOTION_NOTIFY:
        if (!d->out.active || d->out.released) return 0;
        {
            const xcb_motion_notify_event_t *e = event;
            d->out.rx = e->root_x;
            d->out.ry = e->root_y;
            d->out.state = e->state;
            d->out.time = e->time;
            d->out.moved = 1;   /* looked at once per dispatch (tick) */
        }
        return 1;
    case XCB_BUTTON_PRESS:
        return d->out.active && !d->out.released;
    case XCB_BUTTON_RELEASE:
        if (!d->out.active || d->out.released) return 0;
        {
            const xcb_button_release_event_t *e = event;
            if (e->detail != 1) return 1;
            d->out.rx = e->root_x;
            d->out.ry = e->root_y;
            d->out.state = e->state;
            d->out.time = e->time;
            d->out.moved = 1;
            drag_released(d);
        }
        return 1;
    case XCB_KEY_PRESS:
        if (d->out.active && !d->out.released && ((const xcb_key_press_event_t *)event)->detail == 9) {   /* Esc */
            leave_target(d);
            end_drag(d, RMP_DND_NONE);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

void rmp_xdnd_set_target(RmpXdnd *d, rmp_xdnd_over_fn over, rmp_xdnd_drop_fn drop, void *ctx)
{
    if (!d) return;
    d->over = over;
    d->drop = drop;
    d->ctx = ctx;
    set_aware(d, over && drop);
}

void rmp_xdnd_close(RmpXdnd *d)
{
    if (!d) return;
    if (d->out.active) {
        leave_target(d);
        end_drag(d, RMP_DND_NONE);
    }
    for (int i = 0; i < 3; i++)
        if (d->out.cursors[i]) xcb_free_cursor(d->c, d->out.cursors[i]);
    rmp_x11_set_event_hook(d->x, NULL, NULL);
    reset_in(d);
    free_out(d);
    free(d);
}
