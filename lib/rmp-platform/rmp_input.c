// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "rmp_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef KEY_LEFTALT
#define KEY_LEFTALT 56
#endif
#ifndef KEY_RIGHTSHIFT
#define KEY_RIGHTSHIFT 54
#endif
#define BITS (KEY_MAX + 1)
#define WORD (8 * sizeof(unsigned long))
#define WORDS ((BITS + WORD - 1) / WORD)

static struct {
    int fd, want_grab, grabbed, dropping, reported_missing;
    double last_scan;
    unsigned long device_down[WORDS];   /* as read from the device */
    unsigned long returned_down[WORDS]; /* as returned to the caller */
    rmp_key_event queue[RMP_INPUT_QUEUE];
    unsigned head, count;
    int extra;                          /* gpio-keys (RMP_INPUT_EXTRA_NAME) */
    char path[300];                     /* the node of fd */
} I = { -1, 0, 0, 0, 0, -1e9, {0}, {0}, {{0, 0}}, 0, 0, -1, "" };

/* The keys taken from the extra device. */
static int extra_key(int code) { return code == KEY_LEFTALT || code == KEY_RIGHTSHIFT; }

static int bit(const unsigned long *b, int code)
{
    return code >= 0 && code < BITS && (b[(unsigned)code / WORD] >> ((unsigned)code % WORD)) & 1u;
}
static void set_bit(unsigned long *b, int code, int on)
{
    if (code < 0 || code >= BITS) return;
    unsigned long m = 1ul << ((unsigned)code % WORD);
    if (on) b[(unsigned)code / WORD] |= m;
    else b[(unsigned)code / WORD] &= ~m;
}
static int any_down(const unsigned long *b)
{
    for (size_t i = 0; i < WORDS; i++) if (b[i]) return 1;
    return 0;
}

static void push(int code, int value)
{
    if (I.count == RMP_INPUT_QUEUE) {   /* only synthesized events get here */
        I.head = (I.head + 1) % RMP_INPUT_QUEUE;
        I.count--;
    }
    I.queue[(I.head + I.count) % RMP_INPUT_QUEUE] = (rmp_key_event){ code, value };
    I.count++;
}

/* Emit press/release for every key whose state differs from `now_down`. */
static void resync(const unsigned long *now_down)
{
    for (int code = 0; code < BITS; code++) {
        int was = bit(I.device_down, code), is = bit(now_down, code);
        if (was != is) push(code, is);
    }
    memcpy(I.device_down, now_down, sizeof I.device_down);
}

static void lose_device(const char *why)
{
    if (I.fd >= 0) {
        fprintf(stderr, "input: %s lost (%s)\n", RMP_INPUT_NAME, why);
        close(I.fd);
    }
    I.fd = -1;
    I.grabbed = 0;
    I.dropping = 0;
    unsigned long none[WORDS] = {0};
    resync(none);    /* release every held key */
}

static int try_open(const char *path, const char *want)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0 || strcmp(name, want) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void scan(double now)
{
    I.last_scan = now;
    DIR *dir = opendir(RMP_INPUT_DIR);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof path, "%s/%s", RMP_INPUT_DIR, e->d_name);
        /* Each node opened once per pass, never the one already in use. */
        if (I.fd >= 0 && !strcmp(path, I.path)) continue;
        int fd = -1;
        if (I.fd < 0) fd = try_open(path, RMP_INPUT_NAME);
        if (fd < 0) {
            if (I.extra < 0) I.extra = try_open(path, RMP_INPUT_EXTRA_NAME);
            continue;
        }
        snprintf(I.path, sizeof I.path, "%s", path);
        I.fd = fd;
        I.dropping = 0;
        I.reported_missing = 0;
        /* Keys already held (e.g. the Enter that started the app) are not
         * reported as presses; their releases arrive normally. Grabbing
         * waits until they are released (maybe_grab). */
        if (ioctl(fd, EVIOCGKEY(sizeof I.device_down), I.device_down) < 0)
            memset(I.device_down, 0, sizeof I.device_down);
        fprintf(stderr, "input: %s at %s\n", RMP_INPUT_NAME, path);
    }
    closedir(dir);
    if (I.fd < 0 && !I.reported_missing) {
        fprintf(stderr, "input: %s not found; looking again every %.0f s\n", RMP_INPUT_NAME, RMP_INPUT_RESCAN);
        I.reported_missing = 1;
    }
}

/* Grab only while no key is held, so the console never misses a release. */
static void maybe_grab(void)
{
    if (I.fd < 0 || !I.want_grab || I.grabbed || any_down(I.device_down)) return;
    if (ioctl(I.fd, EVIOCGRAB, (void *)1) == 0) I.grabbed = 1;
    else {
        fprintf(stderr, "input: EVIOCGRAB failed: %s (keys also reach the console)\n", strerror(errno));
        I.want_grab = 0;
    }
}

void rmp_input_open(int grab, double now)
{
    rmp_input_close();
    I.want_grab = grab;
    I.reported_missing = 0;
    scan(now);
    maybe_grab();
}

void rmp_input_close(void)
{
    if (I.fd >= 0) {
        if (I.grabbed) ioctl(I.fd, EVIOCGRAB, (void *)0);
        close(I.fd);
    }
    if (I.extra >= 0) close(I.extra);
    I.extra = -1;
    I.fd = -1;
    I.grabbed = 0;
    I.count = 0;
    I.head = 0;
    memset(I.device_down, 0, sizeof I.device_down);
    memset(I.returned_down, 0, sizeof I.returned_down);
}

int rmp_input_fd(void) { return I.fd; }
int rmp_input_extra_fd(void) { return I.extra; }
int rmp_input_connected(void) { return I.fd >= 0; }

static void read_device(void)
{
    struct input_event ev[64];
    while (I.fd >= 0 && I.count + 64 <= RMP_INPUT_QUEUE) {
        ssize_t n = read(I.fd, ev, sizeof ev);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            lose_device(strerror(errno));
            return;
        }
        if (n == 0) return;
        size_t count = (size_t)n / sizeof ev[0];
        for (size_t i = 0; i < count; i++) {
            const struct input_event *e = &ev[i];
            if (I.dropping) {
                if (e->type == EV_SYN && e->code == SYN_REPORT) {
                    I.dropping = 0;
                    unsigned long held[WORDS] = {0};
                    if (ioctl(I.fd, EVIOCGKEY(sizeof held), held) < 0) memset(held, 0, sizeof held);
                    resync(held);
                }
                continue;
            }
            if (e->type == EV_SYN && e->code == SYN_DROPPED) { I.dropping = 1; continue; }
            if (e->type != EV_KEY || e->code > KEY_MAX || e->value < 0 || e->value > 2) continue;
            set_bit(I.device_down, e->code, e->value != 0);
            push(e->code, e->value);
        }
        if ((size_t)n < sizeof ev) return;
    }
}

/* Alt and right Shift from gpio-keys; on a read error the device is
 * dropped and its held keys released. */
static void read_extra(void)
{
    struct input_event ev[16];
    while (I.extra >= 0 && I.count + 16 <= RMP_INPUT_QUEUE) {
        ssize_t n = read(I.extra, ev, sizeof ev);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close(I.extra);
            I.extra = -1;
            for (int code = 0; code < BITS; code++)
                if (extra_key(code) && bit(I.device_down, code)) { set_bit(I.device_down, code, 0); push(code, 0); }
            return;
        }
        if (n == 0) return;
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            const struct input_event *e = &ev[i];
            if (e->type != EV_KEY || !extra_key(e->code) || e->value < 0 || e->value > 2) continue;
            set_bit(I.device_down, e->code, e->value != 0);
            push(e->code, e->value);
        }
        if ((size_t)n < sizeof ev) return;
    }
}

int rmp_input_poll(rmp_key_event *out, int max, double now)
{
    if (I.fd < 0 && now - I.last_scan >= RMP_INPUT_RESCAN) scan(now);   /* gpio-keys: found along with it */
    read_device();
    read_extra();
    maybe_grab();
    int n = 0;
    while (n < max && I.count) {
        rmp_key_event e = I.queue[I.head];
        I.head = (I.head + 1) % RMP_INPUT_QUEUE;
        I.count--;
        set_bit(I.returned_down, e.code, e.value != 0);
        out[n++] = e;
    }
    return n;
}

int rmp_input_pending(void) { return I.count > 0; }

int rmp_input_key_down(int code) { return bit(I.returned_down, code); }
