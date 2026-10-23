// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* rmp-jukebox platform: display (DRM/KMS + GBM + EGL/GLES2, or an X11
 * window through XCB + EGL), frame pacing, partial redraw (EGL_EXT_buffer_age
 * + EGL_KHR_partial_update), keyboard (evdev keyd virtual keyboard on KMS,
 * the window's keys on X11) and the monotonic clock.
 *
 * Everything here runs on the display (main) thread. Contract and loop:
 * docs/PORT_APP_SKELETON.md section 3. */
#ifndef RMP_PLATFORM_H
#define RMP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel rectangle, top-left origin. Empty when w <= 0 or h <= 0. */
typedef struct { int x, y, w, h; } rmp_rect;

/* ---- Initialisation ---------------------------------------------------- */
/* Where the display goes. AUTO: X11 when $DISPLAY is set and the X server
 * answers (and the X11 output is built in), else KMS. docs/PORT_X11.md. */
enum { RMP_OUTPUT_AUTO, RMP_OUTPUT_KMS, RMP_OUTPUT_X11 };

typedef struct {
    int output;                /* RMP_OUTPUT_* */
    int fullscreen;            /* X11: start full screen (F11 toggles) */
    const char *card;          /* KMS. NULL: "/dev/dri/card0" */
    /* KMS. 1: EVIOCGRAB the keyd virtual keyboard, so keys typed into the
     * app never reach the console or a shell behind it. Ctrl(left)+C then
     * sets rmp_quit_requested() (on X11 always, while the window has the
     * focus). 0: read without grabbing. */
    int grab_keyboard;
    /* KMS. 1: stdin (if a tty) loses ECHO/ICANON (ISIG stays), its input
     * queue is discarded on shutdown and the settings restored. */
    int manage_terminal;
    /* 1: SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGPIPE set rmp_quit_requested(). */
    int install_signal_handlers;
    /* 1: one statistics line per second on stderr. */
    int log_stats;
    /* What the app draws at first. 0: RMP_DISPLAY_W x RMP_DISPLAY_H (the
     * Pomera's panel). An app that sizes itself to the surface (FDTP on a
     * wide screen) sets it, and may change it with rmp_set_display_size. On
     * X11 it is also the window's first size. */
    int display_w, display_h;
    /* X11: 1 = the window may take any size (no aspect hint; the app
     * follows it with rmp_output_size and rmp_set_display_size). */
    int resizable;
    /* X11: the window's name and icon (KMS ignores them). NULL / 0 keep
     * rmp-jukebox's: WM_NAME "rmp-jukebox", WM_CLASS "rmp-jukebox" twice,
     * no _NET_WM_NAME and no icon.
     * title: WM_NAME (STRING) and _NET_WM_NAME (UTF8_STRING).
     * wm_instance, wm_class: the two parts of WM_CLASS (one NULL: the
     * other one for both; both NULL: "rmp-jukebox").
     * icon: _NET_WM_ICON as it is set (CARDINAL, 32 bits): for each size
     * width, height, then width x height ARGB pixels (rows top down);
     * icon_words counts every uint32_t. Read during rmp_platform_init only. */
    const char *title;
    const char *wm_instance, *wm_class;
    const uint32_t *icon;
    size_t icon_words;
} rmp_platform_options;

/* Defaults: auto, windowed, card0, grab 1, terminal 1, signals 1, log_stats 0,
 * display 0 x 0 (1024 x 600), resizable 0, rmp-jukebox's name, no icon. */
void rmp_platform_default_options(rmp_platform_options *options);
/* NULL options = defaults. 0 on success. A missing keyboard is not an error
 * (it is looked for again once per second). */
int  rmp_platform_init(const rmp_platform_options *options);
/* Waits for a queued flip, restores the original CRTC and the terminal, then
 * frees EGL/GBM. Destroy screens (their GL objects) BEFORE this. Idempotent;
 * also registered with atexit(). */
void rmp_platform_shutdown(void);
/* The app always draws 1024 x 600 (the Pomera's panel). Another mode
 * (e.g. a PC on 1920x1080 HDMI) shows it scaled to fit, aspect kept and centred. */
#define RMP_DISPLAY_W 1024
#define RMP_DISPLAY_H 600
int  rmp_display_width(void);    /* RMP_DISPLAY_W, or what the app set */
int  rmp_display_height(void);   /* RMP_DISPLAY_H, or what the app set */
/* The surface: the KMS mode, or the X window as it is now. */
void rmp_output_size(int *w, int *h);
/* The app draws w x h from now on (each 1..4096). Not inside a frame; the
 * next frame repaints everything. Shown 1:1 when it is the surface's size,
 * otherwise scaled to fit as above; a display set by the app (here or in the
 * options) is sampled sharp (nearest) when the scale is a whole number.
 * 0 on success, -1 on error. */
int  rmp_set_display_size(int w, int h);
/* X11 full screen state: initially the option, then the last request or
 * _NET_WM_STATE update received from the window manager. Requests are
 * asynchronous; the window manager may refuse them. KMS and uninitialized:
 * query returns 0 and set does nothing. Nonzero means full screen. */
int  rmp_platform_is_fullscreen(void);
void rmp_platform_set_fullscreen(int fullscreen);

/* ---- Frame loop ----------------------------------------------------------
 *   while (!rmp_quit_requested()) {
 *       rmp_wait(0.1);                        // sleeps until due / key / flip
 *       n = rmp_poll_keys(ev, 32); ...        // may switch screens
 *       if (!rmp_frame_ready()) continue;
 *       screen->update(screen->self, rmp_frame_time());   // rmp_frame_damage()
 *       ...app registers its own damage (F-key band, overlays)...
 *       n = rmp_frame_begin(rects, RMP_REPAINT_MAX);
 *       if (n > 0) { screen->draw(screen->self, rects, n); ...app draws... }
 *       if (rmp_frame_end() < 0) break;
 *   }
 */
#define RMP_REPAINT_MAX 32

/* Vblanks per frame: 1 = 60 fps, 2 = 30, 3 = 20, 4 = 15 (clamped to 1..4).
 * Flips stay vsync-aligned (never ASYNC) and never come earlier than this.
 * The visible screen sets it in activate(). Default 2. Applies to frames
 * begun after the call. */
void rmp_set_frame_interval(int vblanks);
int  rmp_frame_interval(void);
/* Sleeps (poll on the DRM fd and the keyboard) until a frame may begin, a
 * key arrives or max_seconds pass. Handles flip events. 1 = a frame may
 * begin now, 0 = not yet (e.g. a key arrived), -1 = error. */
int  rmp_wait(double max_seconds);
/* Non-blocking: 1 when a frame may begin now. */
int  rmp_frame_ready(void);
/* Expected display time (rmp_now_seconds() clock) of the frame about to be
 * built: use it as `now` for animation. */
double rmp_frame_time(void);

/* Registers a rectangle that changes in the NEXT frame to be drawn. Call it
 * BEFORE rmp_frame_begin (from screen update / app logic). Damage
 * accumulates until a frame is actually presented. Clipped to the display. */
void rmp_frame_damage(const rmp_rect *r);
/* The next frame repaints the whole display (screen switch, overlay gone). */
void rmp_frame_invalidate_all(void);

/* Starts a frame. Returns the number of rectangles to repaint (<= max,
 * non-overlapping): this frame's damage plus whatever the back buffer is
 * missing (buffer age). Whole display when the age is unknown.
 * 0 = nothing to draw (no damage, or not due yet): draw nothing, still call
 * rmp_frame_end. -1 = error. eglSetDamageRegionKHR is set from the result.
 * Draw ONLY inside the rectangles: call rmp_frame_clip(&rects[i]) before
 * drawing into each one. The first clip of a whole-display frame clears it. */
int  rmp_frame_begin(rmp_rect *repaint, int max_rects);
/* Binds the display framebuffer, flushes pending rmp_gfx batches and sets
 * glScissor to r (NULL: scissor off; then only draw if the frame is full). */
void rmp_frame_clip(const rmp_rect *r);
/* 1 when the current frame repaints the whole display. */
int  rmp_frame_is_full(void);
/* Clear colour of whole-display frames (0xRRGGBB). Default 0x080C10. */
void rmp_frame_set_clear_color(unsigned rgb);
/* eglSwapBuffers + vsync-aligned page flip. Never waits for the flip it
 * queues. Interval 1: waits only while the PREVIOUS flip is still pending
 * (one frame ahead at most). Interval >= 2: holds the page flip until the
 * vblank before the frame's target (drmWaitVBlank; at most about one
 * vblank), so it is shown on the target and never earlier, while building,
 * swap and GPU work get about two vblanks. 0 on success, -1 on error. */
int  rmp_frame_end(void);

/* Updated once per second. */
typedef struct {
    float fps;          /* completed flips per second */
    float frame_ms;     /* mean flip interval */
    float p99_ms;       /* 99th percentile flip interval */
    float build_ms;     /* rmp_frame_begin .. rmp_frame_end (CPU) */
    float swap_ms;      /* inside rmp_frame_end (swap + flip queueing), without the hold */
    float redraw_px;    /* mean repainted pixels per presented frame */
    float cpu_pct;      /* this process, 100 = one core */
    unsigned missed;    /* flips later than their target vblank (interval after the
                           previous flip, or the slot after an idle frame) */
    int interval;       /* vblanks per frame */
    float hold_ms;      /* interval >= 2: rmp_frame_end waiting for the vblank
                           before the target (mean per presented frame) */
} rmp_frame_stats;
rmp_frame_stats rmp_get_frame_stats(void);

/* ---- Keyboard (evdev) -------------------------------------------------- */
/* code: linux/input-event-codes.h (KEY_*). value: 0 release, 1 press,
 * 2 autorepeat. When the device disappears every held key gets a release. */
typedef struct { int code; int value; } rmp_key_event;
/* Non-blocking. Returns the number of events written (<= max); the rest
 * stay queued for the next call. */
int  rmp_poll_keys(rmp_key_event *out, int max);
/* Current state (1 = held) as of the events returned so far. */
int  rmp_key_down(int code);
int  rmp_keyboard_connected(void);

/* ---- Misc -------------------------------------------------------------- */
double rmp_now_seconds(void);           /* CLOCK_MONOTONIC, seconds */
int  rmp_quit_requested(void);          /* signal or grabbed Ctrl+C */
void rmp_request_quit(void);

#ifdef __cplusplus
}
#endif
#endif
