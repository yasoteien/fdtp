// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Recording EGL/GLES2 mock (Mac tests). Every call that matters for the
 * frame contract is appended to a log with the GL state at that moment. */
#ifndef MOCK_GL_H
#define MOCK_GL_H

#include <EGL/egl.h>
#include <GLES2/gl2.h>

enum {
    EV_AGE, EV_DAMAGE, EV_BINDFB, EV_CLEAR, EV_SCISSOR, EV_SCISSOR_OFF, EV_DRAW,
    EV_SWAP, EV_TEXIMAGE, EV_GENFB, EV_DELPROGRAM, EV_TERMINATE, EV_EXTERNAL,
    EV_MAGFILTER             /* a: GL_TEXTURE_MAG_FILTER value */
};
typedef struct {
    int type;
    int a, b, c, d;          /* event specific */
    GLuint fb;               /* bound framebuffer */
    int scissor_on;
    int sx, sy, sw, sh;      /* scissor box (GL, bottom-left) */
    GLuint program;
    int blend;
    double time;
} mock_event;

#define MOCK_LOG 20000
extern mock_event mock_log[MOCK_LOG];
extern int mock_log_n;
extern int mock_age;                 /* value of EGL_BUFFER_AGE_EXT */
extern int mock_swap_fails;
extern int mock_partial_update;      /* advertise EGL_KHR_partial_update */
extern EGLint mock_damage_rects[4 * 64];
extern int mock_damage_n;
extern double (*mock_time)(void);    /* stamped into events */
extern double mock_last_swap_time;   /* mock_time() of the latest eglSwapBuffers */
extern GLuint mock_programs_linked;

void mock_gl_reset_log(void);
int  mock_find(int type, int from);  /* index of the next event of type, or -1 */
int  mock_count(int type, int from, int to);
/* Called by an external logger (fake kms) to put its own events in order. */
void mock_log_external(int what, int a);

#endif
