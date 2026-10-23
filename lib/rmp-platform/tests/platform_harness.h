// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Harness around rmp_platform.c for Mac tests: virtual clock, a fake
 * vsync display behind the rmp_kms API and a scripted keyboard behind the
 * rmp_input API. Links with mock_gl.c and the real rmp_gfx.c. */
#ifndef PLATFORM_HARNESS_H
#define PLATFORM_HARNESS_H

#include "rmp_platform.h"

#define H_PERIOD (1344.0 * 635.0 / 51200000.0)

extern double h_now;                 /* virtual CLOCK_MONOTONIC */
extern int h_flips;                  /* completed flips */
extern double h_flip_times[4096];
extern int h_presents, h_modesets, h_present_fails, h_no_free_buffer;
extern int h_restored, h_closed, h_pending;
extern double h_pending_land;
/* GPU: each frame's rendering takes h_gpu seconds, starting at its
 * eglSwapBuffers (or when the previous frame's rendering ends); its flip
 * cannot latch before that (the kernel's implicit fence). */
extern double h_gpu;
/* rmp_kms_wait_vblank: calls, failures to inject, a flip pending meanwhile. */
extern int h_vblank_waits, h_vblank_fails, h_wait_with_pending;
extern double h_wait_ms;             /* total time spent inside it */
unsigned long h_missed_total(void);  /* platform MISSED, never reset */
unsigned h_sequence(double t);       /* vblank sequence of a flip time */
/* Scripted keyboard. */
void h_key(int code, int value);
extern int h_keyboard;               /* connected */
void h_clear_quit(void);

enum { X_RESTORE = 1, X_CLOSE, X_PRESENT };

#endif
