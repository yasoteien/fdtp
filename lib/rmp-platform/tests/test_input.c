// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* rmp_input.c against a mock /dev/input (directory, devices, reads, ioctls).
 * Cases: found by exact name among decoys; missing at start and found by a
 * later rescan (no rescan before 1 s); keys held at open are not reported
 * as presses and delay the grab until released; press/repeat/release in
 * order, non-key events ignored; max splits the queue; SYN_DROPPED resync
 * from EVIOCGKEY; unplug (ENODEV) releases held keys, rescans after 1 s;
 * grab failure; ungrab on close. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <assert.h>
#include <linux/input.h>

DIR *mock_opendir(const char *path);
struct dirent *mock_readdir(DIR *d);
int mock_closedir(DIR *d);
int mock_open(const char *path, int flags, ...);
int mock_close(int fd);
ssize_t mock_read(int fd, void *buf, size_t n);
int mock_ioctl(int fd, unsigned long req, ...);
#define opendir mock_opendir
#define readdir mock_readdir
#define closedir mock_closedir
#define open mock_open
#define close mock_close
#define read mock_read
#define ioctl mock_ioctl
#include "../rmp_input.c"

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define CHECK(c) do { if (!(c)) FAIL("line %d: %s", __LINE__, #c); } while (0)

typedef struct {
    const char *file, *name;
    int present, open_fd, grabbed, fail_grab, unplugged;
    unsigned long held[WORDS];
    struct input_event queue[1024];
    int head, count;
} Dev;
static Dev devs[5] = {
    { "event0", "tc3589x-keypad", 1, -1, 0, 0, 0, {0}, {{{0, 0}, 0, 0, 0}}, 0, 0 },
    { "event1", "keyd virtual pointer", 1, -1, 0, 0, 0, {0}, {{{0, 0}, 0, 0, 0}}, 0, 0 },
    { "event2", "keyd virtual keyboard (old)", 1, -1, 0, 0, 0, {0}, {{{0, 0}, 0, 0, 0}}, 0, 0 },
    { "event3", "keyd virtual keyboard", 0, -1, 0, 0, 0, {0}, {{{0, 0}, 0, 0, 0}}, 0, 0 },
    { "mice", "mouse", 1, -1, 0, 0, 0, {0}, {{{0, 0}, 0, 0, 0}}, 0, 0 },
};
static int opendirs, next_fd = 10;
static struct dirent entry;
static int dir_pos;

DIR *mock_opendir(const char *path) { CHECK(!strcmp(path, "/dev/input")); opendirs++; dir_pos = 0; return (DIR *)&dir_pos; }
struct dirent *mock_readdir(DIR *d)
{
    (void)d;
    while (dir_pos < 5 && !devs[dir_pos].present) dir_pos++;
    if (dir_pos >= 5) return NULL;
    snprintf(entry.d_name, sizeof entry.d_name, "%s", devs[dir_pos].file);
    dir_pos++;
    return &entry;
}
int mock_closedir(DIR *d) { (void)d; return 0; }
static Dev *by_fd(int fd) { for (int i = 0; i < 5; i++) if (devs[i].open_fd == fd) return &devs[i]; return NULL; }
int mock_open(const char *path, int flags, ...)
{
    CHECK(flags & O_NONBLOCK);
    for (int i = 0; i < 5; i++)
        if (devs[i].present && !strcmp(path + strlen("/dev/input/"), devs[i].file)) {
            devs[i].open_fd = next_fd++;
            return devs[i].open_fd;
        }
    errno = ENOENT;
    return -1;
}
int mock_close(int fd) { Dev *d = by_fd(fd); CHECK(d); d->open_fd = -1; d->grabbed = 0; return 0; }
ssize_t mock_read(int fd, void *buf, size_t n)
{
    Dev *d = by_fd(fd);
    CHECK(d);
    if (d->unplugged) { errno = ENODEV; return -1; }
    if (!d->count) { errno = EAGAIN; return -1; }
    size_t max = n / sizeof(struct input_event), k = 0;
    struct input_event *out = buf;
    while (k < max && d->count) { out[k++] = d->queue[d->head]; d->head = (d->head + 1) % 1024; d->count--; }
    return (ssize_t)(k * sizeof(struct input_event));
}
int mock_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    Dev *d = by_fd(fd);
    CHECK(d);
    if ((req & 0xF0000ul) == 0x10000ul) { snprintf(arg, req & 0xFFFF, "%s", d->name); return 0; }
    if ((req & 0xF0000ul) == 0x20000ul) { CHECK((req & 0xFFFF) == sizeof d->held); memcpy(arg, d->held, sizeof d->held); return 0; }
    if (req == EVIOCGRAB) {
        if (d->fail_grab) { errno = EBUSY; return -1; }
        d->grabbed = arg != NULL;
        return 0;
    }
    FAIL("unexpected ioctl %lx", req);
}
static void emit(Dev *d, int type, int code, int value)
{
    CHECK(d->count < 1024);
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (uint16_t)type; e.code = (uint16_t)code; e.value = value;
    d->queue[(d->head + d->count) % 1024] = e;
    d->count++;
    if (type == EV_KEY) set_bit(d->held, code, value != 0);
}
static void key(Dev *d, int code, int value) { emit(d, EV_KEY, code, value); emit(d, EV_SYN, SYN_REPORT, 0); }

int main(void)
{
    Dev *kb = &devs[3];
    rmp_key_event ev[64];
    /* Missing at start: no error, no fd; no rescan within 1 s. */
    rmp_input_open(1, 100.0);
    CHECK(!rmp_input_connected() && rmp_input_fd() == -1 && opendirs == 1);
    CHECK(rmp_input_poll(ev, 64, 100.5) == 0 && opendirs == 1);
    /* Appears with Enter still held (the key that started the app). */
    kb->present = 1;
    set_bit(kb->held, KEY_ENTER, 1);
    CHECK(rmp_input_poll(ev, 64, 101.0) == 0 && opendirs == 2 && rmp_input_connected());
    CHECK(devs[2].open_fd == -1 && devs[0].open_fd == -1);   /* decoys closed */
    CHECK(!kb->grabbed && !rmp_input_key_down(KEY_ENTER));    /* no press reported, no grab yet */
    key(kb, KEY_ENTER, 0);
    CHECK(rmp_input_poll(ev, 64, 101.1) == 1 && ev[0].code == KEY_ENTER && ev[0].value == 0);
    CHECK(kb->grabbed);                                       /* grabbed once nothing is held */
    /* Order, repeat, ignored non-key events, max split. */
    emit(kb, EV_MSC, 4, 7);
    key(kb, KEY_A, 1); key(kb, KEY_A, 2); key(kb, KEY_A, 2); key(kb, KEY_A, 0); key(kb, KEY_Q, 1);
    CHECK(rmp_input_poll(ev, 3, 101.2) == 3 && ev[0].value == 1 && ev[1].value == 2 && ev[2].value == 2);
    CHECK(rmp_input_pending() && rmp_input_key_down(KEY_A));
    CHECK(rmp_input_poll(ev, 64, 101.3) == 2 && ev[0].code == KEY_A && ev[0].value == 0 && ev[1].code == KEY_Q);
    CHECK(!rmp_input_pending() && !rmp_input_key_down(KEY_A) && rmp_input_key_down(KEY_Q));
    /* SYN_DROPPED: discard until SYN_REPORT, then resync from EVIOCGKEY. */
    emit(kb, EV_SYN, SYN_DROPPED, 0);
    emit(kb, EV_KEY, KEY_C, 1);              /* lost in the overflow */
    set_bit(kb->held, KEY_Q, 0);             /* Q released during the overflow */
    set_bit(kb->held, KEY_LEFTCTRL, 1);      /* Ctrl pressed during the overflow */
    emit(kb, EV_SYN, SYN_REPORT, 0);
    int n = rmp_input_poll(ev, 64, 101.4), sawq = 0, sawctrl = 0, sawc = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].code == KEY_Q && ev[i].value == 0) sawq = 1;
        if (ev[i].code == KEY_LEFTCTRL && ev[i].value == 1) sawctrl = 1;
        if (ev[i].code == KEY_C && ev[i].value == 1) sawc = 1;   /* held per EVIOCGKEY */
    }
    CHECK(sawq && sawctrl && sawc && n == 3);
    /* Unplug: releases for held keys (C, Ctrl), fd closed, rescan after 1 s. */
    kb->unplugged = 1;
    n = rmp_input_poll(ev, 64, 102.0);
    CHECK(n == 2 && ev[0].value == 0 && ev[1].value == 0 && !rmp_input_connected());
    CHECK(!rmp_input_key_down(KEY_C) && !rmp_input_key_down(KEY_LEFTCTRL));
    kb->present = 0; kb->unplugged = 0;
    memset(kb->held, 0, sizeof kb->held);
    int before = opendirs;
    rmp_input_poll(ev, 64, 102.5);           /* last search was at 101.0: search now */
    CHECK(opendirs == before + 1 && !rmp_input_connected());
    rmp_input_poll(ev, 64, 103.4);
    CHECK(opendirs == before + 1);           /* not within 1 s of that search */
    rmp_input_poll(ev, 64, 103.6);
    CHECK(opendirs == before + 2 && !rmp_input_connected());
    kb->present = 1;
    rmp_input_poll(ev, 64, 104.7);
    CHECK(rmp_input_connected() && kb->grabbed);
    /* Close ungrabs and closes. */
    rmp_input_close();
    CHECK(kb->open_fd == -1 && !kb->grabbed);
    /* Grab failure: keep reading without the grab. */
    kb->fail_grab = 1;
    rmp_input_open(1, 200.0);
    CHECK(rmp_input_connected() && !kb->grabbed && !I.want_grab);
    key(kb, KEY_F9, 1);
    CHECK(rmp_input_poll(ev, 64, 200.1) == 1 && ev[0].code == KEY_F9);
    rmp_input_close();
    /* No grab requested. */
    kb->fail_grab = 0;
    rmp_input_open(0, 300.0);
    CHECK(rmp_input_connected() && !kb->grabbed);
    rmp_input_close();
    puts("PASS input: exact-name match among decoys, missing at start then found by the 1 s rescan, "
         "held keys not reported and grab deferred until released, order/repeat/max split, "
         "SYN_DROPPED resync, unplug releases held keys and rescans, grab failure, no-grab mode");
    return 0;
}
