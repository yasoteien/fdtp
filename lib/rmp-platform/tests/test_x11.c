// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* The X11 window's names and icon (rmp_x11.c, revision 1 item 23): by
 * default WM_NAME / WM_CLASS stay "rmp-jukebox" with no _NET_WM_NAME and no
 * _NET_WM_ICON; given in the options they are set as asked.
 * libxcb is not linked: the xcb functions rmp_x11.c calls are defined here
 * and record the properties. Needs only libxcb's headers (Makefile). */
#include "../rmp_x11.c"
#include <assert.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ---- Fake libxcb --------------------------------------------------------- */
static char fake_connection;   /* only its address is used */
static xcb_setup_t fake_setup;
static xcb_screen_t fake_screen;
#define MAX_ATOMS 32
#define ATOM_BASE 1000
static char atom_names[MAX_ATOMS][64];
static int atom_count;
#define MAX_PROPS 32
static struct {
    xcb_atom_t property, type;
    uint8_t format;
    uint32_t len;
    unsigned char *data;
} props[MAX_PROPS];
static int prop_count;
static unsigned sent_count;
static xcb_client_message_event_t sent;
static uint32_t selected_events;
static xcb_generic_event_t *pending_event;
static xcb_atom_t wm_states[4];
static int wm_state_count, wm_state_missing, wm_state_malformed;

xcb_connection_t *xcb_connect(const char *displayname, int *screenp)
{
    (void)displayname;
    if (screenp) *screenp = 0;
    return (xcb_connection_t *)&fake_connection;
}
int xcb_connection_has_error(xcb_connection_t *c) { (void)c; return 0; }
void xcb_disconnect(xcb_connection_t *c) { (void)c; }
const struct xcb_setup_t *xcb_get_setup(xcb_connection_t *c) { (void)c; return &fake_setup; }
xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t *R)
{
    (void)R;
    fake_screen.root = 1;
    fake_screen.width_in_pixels = 1024;
    fake_screen.height_in_pixels = 600;
    return (xcb_screen_iterator_t){ &fake_screen, 1, 0 };
}
void xcb_screen_next(xcb_screen_iterator_t *i) { i->rem = 0; }
uint32_t xcb_generate_id(xcb_connection_t *c) { (void)c; return 7; }
int xcb_flush(xcb_connection_t *c) { (void)c; return 1; }
int xcb_get_file_descriptor(xcb_connection_t *c) { (void)c; return -1; }
xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *c)
{
    (void)c;
    xcb_generic_event_t *event = pending_event;
    pending_event = NULL;
    return event;
}
xcb_generic_event_t *xcb_poll_for_queued_event(xcb_connection_t *c) { (void)c; return NULL; }

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *c, uint8_t only_if_exists, uint16_t name_len,
                                         const char *name)
{
    (void)c; (void)only_if_exists;
    for (int i = 0; i < atom_count; i++)
        if (strlen(atom_names[i]) == name_len && !memcmp(atom_names[i], name, name_len))
            return (xcb_intern_atom_cookie_t){ (unsigned)i };
    assert(atom_count < MAX_ATOMS && name_len < 64);
    memcpy(atom_names[atom_count], name, name_len);
    atom_names[atom_count][name_len] = 0;
    return (xcb_intern_atom_cookie_t){ (unsigned)atom_count++ };
}
xcb_intern_atom_reply_t *xcb_intern_atom_reply(xcb_connection_t *c, xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t **e)
{
    (void)c; (void)e;
    xcb_intern_atom_reply_t *r = calloc(1, sizeof *r);
    r->atom = ATOM_BASE + cookie.sequence;
    return r;
}
/* The atom of a name, or XCB_ATOM_NONE when it was never interned. */
static xcb_atom_t interned(const char *name)
{
    for (int i = 0; i < atom_count; i++)
        if (!strcmp(atom_names[i], name)) return ATOM_BASE + (xcb_atom_t)i;
    return XCB_ATOM_NONE;
}

xcb_void_cookie_t xcb_create_window(xcb_connection_t *c, uint8_t depth, xcb_window_t wid, xcb_window_t parent,
                                    int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t border_width,
                                    uint16_t _class, xcb_visualid_t visual, uint32_t value_mask,
                                    const void *value_list)
{
    (void)c; (void)depth; (void)wid; (void)parent; (void)x; (void)y; (void)width; (void)height;
    (void)border_width; (void)_class; (void)visual; (void)value_mask;
    selected_events = ((const uint32_t *)value_list)[1];
    return (xcb_void_cookie_t){ 0 };
}
xcb_void_cookie_t xcb_change_property(xcb_connection_t *c, uint8_t mode, xcb_window_t window, xcb_atom_t property,
                                      xcb_atom_t type, uint8_t format, uint32_t data_len, const void *data)
{
    (void)c;
    CHECK(mode == XCB_PROP_MODE_REPLACE && window == 7);
    int i = 0;
    while (i < prop_count && props[i].property != property) i++;
    if (i == prop_count) {
        assert(prop_count < MAX_PROPS);
        prop_count++;
    } else {
        free(props[i].data);
    }
    size_t bytes = (size_t)data_len * (format / 8);
    props[i].property = property;
    props[i].type = type;
    props[i].format = format;
    props[i].len = data_len;
    props[i].data = malloc(bytes ? bytes : 1);
    memcpy(props[i].data, data, bytes);
    return (xcb_void_cookie_t){ 0 };
}
xcb_void_cookie_t xcb_send_event(xcb_connection_t *c, uint8_t propagate, xcb_window_t destination,
                                 uint32_t event_mask, const char *event)
{
    (void)c;
    CHECK(!propagate && destination == fake_screen.root);
    CHECK(event_mask == (XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY));
    memcpy(&sent, event, sizeof sent);
    sent_count++;
    return (xcb_void_cookie_t){ 0 };
}
xcb_get_property_cookie_t xcb_get_property(xcb_connection_t *c, uint8_t _delete, xcb_window_t window,
                                           xcb_atom_t property, xcb_atom_t type, uint32_t long_offset,
                                           uint32_t long_length)
{
    (void)c; (void)_delete; (void)window; (void)type; (void)long_offset; (void)long_length;
    return (xcb_get_property_cookie_t){ property };
}
/* No _NET_WORKAREA: the window keeps the size asked for. */
xcb_get_property_reply_t *xcb_get_property_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie,
                                                 xcb_generic_error_t **e)
{
    (void)c; (void)e;
    if (cookie.sequence != interned("_NET_WM_STATE")) return NULL;
    xcb_get_property_reply_t *r = calloc(1, sizeof *r + sizeof wm_states);
    r->type = wm_state_missing ? XCB_ATOM_NONE : XCB_ATOM_ATOM;
    r->format = wm_state_malformed ? 8 : 32;
    r->value_len = (uint32_t)wm_state_count;
    memcpy(r + 1, wm_states, (size_t)wm_state_count * sizeof wm_states[0]);
    return r;
}
void *xcb_get_property_value(const xcb_get_property_reply_t *R) { return (void *)(R + 1); }
int xcb_get_property_value_length(const xcb_get_property_reply_t *R) { return (int)R->value_len * (R->format / 8); }
xcb_void_cookie_t xcb_destroy_window(xcb_connection_t *c, xcb_window_t window)
{
    (void)c; (void)window;
    return (xcb_void_cookie_t){ 0 };
}
xcb_void_cookie_t xcb_map_window(xcb_connection_t *c, xcb_window_t window)
{
    (void)c; (void)window;
    return (xcb_void_cookie_t){ 0 };
}
xcb_void_cookie_t xcb_configure_window(xcb_connection_t *c, xcb_window_t window, uint16_t value_mask,
                                       const void *value_list)
{
    (void)c; (void)window; (void)value_mask; (void)value_list;
    return (xcb_void_cookie_t){ 0 };
}

/* ---- Helpers ------------------------------------------------------------- */
static void reset(void)
{
    for (int i = 0; i < prop_count; i++) free(props[i].data);
    prop_count = 0;
    atom_count = 0;
    sent_count = 0;
    selected_events = 0;
    wm_state_count = wm_state_missing = wm_state_malformed = 0;
    free(pending_event);
    pending_event = NULL;
}
/* The property, or -1 when it was not set. */
static int prop(xcb_atom_t property)
{
    if (property == XCB_ATOM_NONE) return -1;
    for (int i = 0; i < prop_count; i++)
        if (props[i].property == property) return i;
    return -1;
}
static int string_prop(xcb_atom_t property, xcb_atom_t type, const char *bytes, size_t len)
{
    int i = prop(property);
    return i >= 0 && props[i].type == type && props[i].format == 8 && props[i].len == len &&
           !memcmp(props[i].data, bytes, len);
}
static RmpX11 *open_window(const rmp_platform_options *names)
{
    setenv("DISPLAY", ":0", 1);
    RmpX11 *x = rmp_x11_open(1024, 600, 0, 1, 1, names);
    CHECK(x);
    return x;
}
/* rmp-jukebox's names and nothing else. */
static void check_jukebox(void)
{
    CHECK(string_prop(XCB_ATOM_WM_NAME, XCB_ATOM_STRING, "rmp-jukebox", 11));
    CHECK(string_prop(XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, "rmp-jukebox\0rmp-jukebox", 24));
    CHECK(prop(interned("_NET_WM_NAME")) < 0);
    CHECK(prop(interned("_NET_WM_ICON")) < 0);
    CHECK(interned("_NET_WM_ICON") == XCB_ATOM_NONE);
}

/* ---- Tests --------------------------------------------------------------- */
static void test_default_null(void)
{
    reset();
    RmpX11 *x = open_window(NULL);
    check_jukebox();
    rmp_x11_close(x);
}
static void test_default_zeroed(void)
{
    reset();
    rmp_platform_options o;
    memset(&o, 0, sizeof o);   /* what rmp_platform_default_options sets for these */
    RmpX11 *x = open_window(&o);
    check_jukebox();
    rmp_x11_close(x);
}
static void test_fdtp(void)
{
    reset();
    /* Two sizes: 2x2 then 1x1. */
    static const uint32_t icon[] = { 2, 2, 0xFF000055u, 0xFF00FFFFu, 0xFFFFFF00u, 0x00000000u,
                                     1, 1, 0xFF000055u };
    rmp_platform_options o;
    memset(&o, 0, sizeof o);
    o.title = "FDTP";
    o.wm_instance = "fdtp";
    o.wm_class = "FDTP";
    o.icon = icon;
    o.icon_words = sizeof icon / sizeof icon[0];
    RmpX11 *x = open_window(&o);
    CHECK(string_prop(XCB_ATOM_WM_NAME, XCB_ATOM_STRING, "FDTP", 4));
    CHECK(string_prop(XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, "fdtp\0FDTP", 10));
    CHECK(interned("UTF8_STRING") != XCB_ATOM_NONE);
    CHECK(string_prop(interned("_NET_WM_NAME"), interned("UTF8_STRING"), "FDTP", 4));
    int i = prop(interned("_NET_WM_ICON"));
    CHECK(i >= 0 && props[i].type == XCB_ATOM_CARDINAL && props[i].format == 32 && props[i].len == 9);
    CHECK(!memcmp(props[i].data, icon, sizeof icon));
    /* The rest of the window is as before. */
    CHECK(prop(XCB_ATOM_WM_NORMAL_HINTS) >= 0);
    CHECK(prop(interned("WM_PROTOCOLS")) >= 0);
    rmp_x11_close(x);
}
static void test_class_only(void)
{
    reset();
    rmp_platform_options o;
    memset(&o, 0, sizeof o);
    o.wm_class = "Viewer";
    RmpX11 *x = open_window(&o);
    CHECK(string_prop(XCB_ATOM_WM_NAME, XCB_ATOM_STRING, "rmp-jukebox", 11));
    CHECK(string_prop(XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, "Viewer\0Viewer", 14));
    CHECK(prop(interned("_NET_WM_NAME")) < 0);
    rmp_x11_close(x);
}

static void notify_fullscreen(RmpX11 *x)
{
    xcb_property_notify_event_t event = { 0 };
    event.response_type = XCB_PROPERTY_NOTIFY;
    event.window = x->window;
    event.atom = x->net_state;
    pending_event = calloc(1, sizeof(xcb_generic_event_t));
    memcpy(pending_event, &event, sizeof event);
    CHECK(rmp_x11_dispatch(x) == 0);
}

static void test_fullscreen(void)
{
    reset();
    RmpX11 *x = open_window(NULL);
    CHECK(selected_events & XCB_EVENT_MASK_PROPERTY_CHANGE);
    CHECK(!rmp_x11_is_fullscreen(x));
    rmp_x11_set_fullscreen(x, 7);
    CHECK(rmp_x11_is_fullscreen(x) == 1 && sent_count == 1);
    CHECK(sent.type == x->net_state && sent.window == x->window && sent.format == 32);
    CHECK(sent.data.data32[0] == 1 && sent.data.data32[1] == x->net_fullscreen && sent.data.data32[3] == 1);
    rmp_x11_set_fullscreen(x, 1);
    CHECK(sent_count == 1);
    rmp_x11_set_fullscreen(x, 0);
    CHECK(!rmp_x11_is_fullscreen(x) && sent_count == 2 && sent.data.data32[0] == 0);
    key_press(x, KEY_F11);
    CHECK(rmp_x11_is_fullscreen(x) && sent_count == 3 && !x->count);
    key_press(x, KEY_F11);
    CHECK(sent_count == 3);
    key_release(x, KEY_F11);
    CHECK(!x->count);
    key_press(x, KEY_LEFTALT);
    key_press(x, KEY_ENTER);
    CHECK(!rmp_x11_is_fullscreen(x) && sent_count == 4);
    key_release(x, KEY_ENTER);
    key_release(x, KEY_LEFTALT);
    CHECK(x->count == 2); /* only Alt's press and release reach app */
    wm_states[0] = x->wm_delete;
    wm_states[1] = x->net_fullscreen;
    wm_state_count = 2;
    notify_fullscreen(x);
    CHECK(rmp_x11_is_fullscreen(x) && sent_count == 4);
    wm_state_count = 1;
    notify_fullscreen(x);
    CHECK(!rmp_x11_is_fullscreen(x));
    rmp_x11_set_fullscreen(x, 1);
    wm_state_missing = 1;
    notify_fullscreen(x);
    CHECK(!rmp_x11_is_fullscreen(x));
    rmp_x11_set_fullscreen(x, 1);
    wm_state_missing = 0;
    wm_state_malformed = 1;
    notify_fullscreen(x);
    CHECK(rmp_x11_is_fullscreen(x));
    rmp_x11_close(x);
    reset();
    x = rmp_x11_open(1024, 600, 1, 1, 1, NULL);
    CHECK(x && rmp_x11_is_fullscreen(x) == 1);
    CHECK(prop(interned("_NET_WM_STATE")) >= 0 && !sent_count);
    rmp_x11_close(x);
}

int main(void)
{
    test_default_null();
    test_default_zeroed();
    test_fdtp();
    test_class_only();
    test_fullscreen();
    reset();
    printf("PASS test_x11: default rmp-jukebox name and no icon; title, WM_CLASS, _NET_WM_NAME, _NET_WM_ICON when given; fullscreen ADD/REMOVE, shortcuts, WM property updates\n");
    return 0;
}
