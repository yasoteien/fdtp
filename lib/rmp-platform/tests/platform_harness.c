// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* See platform_harness.h. rmp_platform.c is included with its clock and
 * poll() redirected to the virtual clock and the fake display. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "platform_harness.h"
#include "mock_gl.h"

int mock_clock_gettime(clockid_t id, struct timespec *ts);
int mock_poll(struct pollfd *fds, nfds_t n, int timeout);
#define clock_gettime mock_clock_gettime
#define poll mock_poll
#include "../rmp_platform.c"
#undef poll
#undef clock_gettime

double h_now = 1000.0;
int h_flips;
double h_flip_times[4096];
int h_presents, h_modesets, h_present_fails, h_no_free_buffer, h_restored, h_closed, h_pending;
double h_pending_land;
double h_gpu, h_wait_ms;
static double gpu_free;
int h_vblank_waits, h_vblank_fails, h_wait_with_pending;
unsigned long h_missed_total(void) { return P.missed_total; }
unsigned h_sequence(double t) { return (unsigned)lround(t / H_PERIOD); }
int h_keyboard = 1;
void h_clear_quit(void) { quit_flag = 0; }
static int modeset_done;

static double now_for_log(void) { return h_now; }
int mock_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    mock_time = now_for_log;
    /* Nearest nanosecond: truncating would read a flip that just landed as
     * still a fraction of a nanosecond ahead. */
    long long ns = llround(h_now * 1e9);
    ts->tv_sec = (time_t)(ns / 1000000000);
    ts->tv_nsec = (long)(ns % 1000000000);
    return 0;
}

static double land(double t) { return (floor(t / H_PERIOD + 1e-9) + 1) * H_PERIOD; }
static void complete(void)
{
    h_flip_times[h_flips++ % 4096] = h_pending_land;
    h_pending = 0;
}

/* ---- fake rmp_kms ---- */
struct RmpKms { int unused; };
static struct RmpKms the_kms;
RmpKms *rmp_kms_open(const char *card) { (void)card; modeset_done = 0; h_pending = 0; return &the_kms; }
int rmp_kms_fd(const RmpKms *k) { (void)k; return 3; }
int rmp_kms_width(const RmpKms *k) { (void)k; return 1024; }
int rmp_kms_height(const RmpKms *k) { (void)k; return 600; }
struct gbm_device *rmp_kms_gbm_device(RmpKms *k) { (void)k; return (struct gbm_device *)&the_kms; }
struct gbm_surface *rmp_kms_gbm_surface(RmpKms *k) { (void)k; return (struct gbm_surface *)&the_kms; }
uint32_t rmp_kms_format(const RmpKms *k) { (void)k; return 0x34325258u; }
int rmp_kms_present_gl(RmpKms *k)
{
    (void)k;
    if (h_present_fails) return -1;
    mock_log_external(X_PRESENT, 0);
    if (!modeset_done) { modeset_done = 1; h_modesets++; return 0; }
    if (h_pending) {   /* waits only for the previous flip */
        if (h_pending_land > h_now) h_now = h_pending_land;
        complete();
    }
    /* The frame's rendering runs after its swap, one frame at a time. */
    double start = mock_last_swap_time > gpu_free ? mock_last_swap_time : gpu_free;
    gpu_free = start + h_gpu;
    h_pending = 1;
    h_pending_land = land(h_now > gpu_free ? h_now : gpu_free);
    h_presents++;
    return 0;
}
int rmp_kms_dispatch(RmpKms *k) { (void)k; if (h_pending && h_pending_land <= h_now) complete(); return 0; }
int rmp_kms_has_free_buffer(RmpKms *k) { (void)k; return !h_no_free_buffer; }
int rmp_kms_wait_free_buffer(RmpKms *k) { (void)k; return h_no_free_buffer ? -1 : 0; }
bool rmp_kms_flip_pending(const RmpKms *k) { (void)k; return h_pending != 0; }
/* The counter reaches `sequence` at that vblank's time (vblanks at k*period). */
int rmp_kms_wait_vblank(RmpKms *k, unsigned sequence)
{
    (void)k;
    h_vblank_waits++;
    if (h_pending) h_wait_with_pending++;
    if (h_vblank_fails) { errno = EINVAL; return -1; }
    double t = sequence * H_PERIOD;
    if (t > h_now) { h_wait_ms += (t - h_now) * 1000; h_now = t; }
    return 0;
}
double rmp_kms_refresh_period(const RmpKms *k) { (void)k; return H_PERIOD; }
unsigned long rmp_kms_flip_count(const RmpKms *k) { (void)k; return (unsigned long)h_flips; }
int rmp_kms_flip_record(const RmpKms *k, unsigned long index, double *time, unsigned *sequence)
{
    (void)k;
    if (index >= (unsigned long)h_flips) return -1;
    double t = h_flip_times[index % 4096];
    if (time) *time = t;
    if (sequence) *sequence = (unsigned)lround(t / H_PERIOD);
    return 0;
}
int rmp_kms_restore(RmpKms *k)
{
    (void)k;
    if (h_pending) { h_now = h_pending_land; complete(); }
    h_restored = 1;
    mock_log_external(X_RESTORE, 0);
    return 0;
}
void rmp_kms_close(RmpKms *k) { (void)k; h_closed = 1; mock_log_external(X_CLOSE, 0); }

/* ---- fake rmp_input ---- */
static rmp_key_event keys[256];
static int key_n;
static unsigned char down[1024];
void h_key(int code, int value) { keys[key_n++] = (rmp_key_event){ code, value }; }
void rmp_input_open(int grab, double now) { (void)grab; (void)now; }
void rmp_input_close(void) { key_n = 0; }
int rmp_input_fd(void) { return h_keyboard ? 7 : -1; }
int rmp_input_extra_fd(void) { return -1; }
int rmp_input_connected(void) { return h_keyboard; }
int rmp_input_pending(void) { return key_n > 0; }
int rmp_input_poll(rmp_key_event *out, int max, double now)
{
    (void)now;
    int n = key_n < max ? key_n : max;
    memcpy(out, keys, sizeof keys[0] * (size_t)n);
    memmove(keys, keys + n, sizeof keys[0] * (size_t)(key_n - n));
    key_n -= n;
    for (int i = 0; i < n; i++) if (out[i].code >= 0 && out[i].code < 1024) down[out[i].code] = out[i].value != 0;
    return n;
}
int rmp_input_key_down(int code) { return code >= 0 && code < 1024 && down[code]; }

/* ---- poll: sleep on the virtual clock until timeout or a flip lands ---- */
int mock_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    for (nfds_t i = 0; i < n; i++) fds[i].revents = 0;
    if (n > 1 && key_n) { fds[1].revents = POLLIN; return 1; }
    double wake = h_now + (timeout < 0 ? 10.0 : timeout / 1000.0);
    if (h_pending && h_pending_land <= wake) {
        if (h_pending_land > h_now) h_now = h_pending_land;
        fds[0].revents = POLLIN;
        return 1;
    }
    h_now = wake;
    return 0;
}
