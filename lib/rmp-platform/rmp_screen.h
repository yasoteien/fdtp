// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Screen contract shared by the app body and the screens (scope, main...).
 * docs/PORT_APP_SKELETON.md section 4. */
#ifndef RMP_SCREEN_H
#define RMP_SCREEN_H

#include "rmp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rmp_screen {
    void *self;
    /* Became visible: call rmp_set_frame_interval(), restart heavy work,
     * damage everything it owns. GL context is current. */
    void (*activate)(void *self);
    /* Hidden: may stop heavy work (audio capture, analysis). */
    void (*deactivate)(void *self);
    /* Every frame, BEFORE rmp_frame_begin: advance state to `now`
     * (rmp_frame_time()) and rmp_frame_damage() what changed. No drawing. */
    void (*update)(void *self, double now);
    /* After rmp_frame_begin (count > 0): for each repaint rectangle,
     * rmp_frame_clip() it and draw what lies inside. Off-screen (FBO) work
     * must be done before the first rmp_frame_clip of the frame. */
    void (*draw)(void *self, const rmp_rect *repaint, int count);
    /* Frees everything; call before rmp_platform_shutdown(). */
    void (*destroy)(void *self);
} rmp_screen;

#ifdef __cplusplus
}
#endif
#endif
