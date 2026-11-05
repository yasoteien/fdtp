// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* A hand test for XDND (rmp_xdnd.c) on a real X server: a small window that
 * drags the files named on the command line (any directories) when button 1
 * is pressed in it and the pointer moves, and prints what the target took.
 *   make build/xdnd_source   (Linux, libxcb1-dev)
 *   DISPLAY=:0 build/xdnd_source /abs/path ... &  (then xdotool: press in
 *   the window, move to the target, release)
 * With --accept it also prints what is dropped on it. No EGL: the window is
 * left black. */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../rmp_xdnd.h"

double rmp_now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int over(void *ctx, int x, int y, const char *const *paths, size_t count, unsigned mods, unsigned allowed)
{
    (void)ctx; (void)paths; (void)mods;
    printf("over %d,%d: %zu paths, allowed %u\n", x, y, count, allowed);
    fflush(stdout);
    return allowed & RMP_DND_COPY ? RMP_DND_COPY : RMP_DND_MOVE;
}

static int drop(void *ctx, int x, int y, const char *const *paths, size_t count, int action)
{
    (void)ctx;
    printf("drop %d,%d action %d:", x, y, action);
    for (size_t i = 0; i < count; i++) printf(" [%s]", paths[i]);
    printf("\n");
    fflush(stdout);
    return action;
}

int main(int argc, char **argv)
{
    int accept = argc > 1 && !strcmp(argv[1], "--accept");
    const char *const *paths = (const char *const *)argv + 1 + accept;
    size_t count = (size_t)(argc - 1 - accept);
    rmp_platform_options o;
    memset(&o, 0, sizeof o);
    o.title = "xdnd-source";
    o.wm_class = "XdndSource";
    RmpX11 *x = rmp_x11_open(200, 150, 0, 0, 1, &o);
    if (!x) return 1;
    RmpXdnd *d = rmp_xdnd_open(x);
    if (accept) rmp_xdnd_set_target(d, over, drop, NULL);
    rmp_x11_show(x);
    int pressed = 0, dragging = 0;
    for (;;) {
        struct pollfd p = { rmp_x11_fd(x), POLLIN, 0 };
        poll(&p, 1, 100);
        if (rmp_x11_dispatch(x) < 0) break;
        rmp_pointer_event e;
        while (rmp_x11_poll_pointer(x, &e)) {
            if (e.type == RMP_POINTER_PRESS && e.button == 1) pressed = 1;
            else if (e.type == RMP_POINTER_RELEASE) pressed = 0;
            else if (e.type == RMP_POINTER_MOTION && pressed && !dragging && count) {
                dragging = rmp_xdnd_drag(d, paths, count) == 0;
                printf("drag of %zu: %s\n", count, dragging ? "started" : "refused");
                fflush(stdout);
            }
        }
        int r = rmp_xdnd_take_result(d);
        if (r >= 0) {
            printf("result %d\n", r);
            fflush(stdout);
            dragging = pressed = 0;
        }
        rmp_key_event k;
        while (rmp_x11_poll_key(x, &k))
            if (k.code == 68) goto done;   /* F10 or the close button */
    }
done:
    rmp_xdnd_close(d);
    rmp_x11_close(x);
    return 0;
}
