// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Platform glue: EGL on GBM (KMS) or on an X11 window (XCB), the frame loop
 * (pacing, damage, buffer age, partial update, swap + flip), keyboard,
 * terminal, signals and stats.
 * EGL setup and the per-frame order come from poc/pomera-scope src/gl.c
 * and src/main.c (the 60 fps / MISSED 0 version). */
#define _POSIX_C_SOURCE 200809L
#include "rmp_platform.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
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

#include "rmp_damage.h"
#include "rmp_gfx.h"
#include "rmp_input.h"
#include "rmp_kms.h"
#include "rmp_pacing.h"
#if RMP_WITH_X11
#include "rmp_x11.h"
#include "rmp_xdnd.h"
#else
/* Built without libxcb: the X11 output is never opened, so these are never
 * reached past rmp_x11_open. */
typedef struct RmpX11 RmpX11;
static RmpX11 *rmp_x11_open(int w, int h, int fullscreen, int quiet, int resizable,
                            const rmp_platform_options *names)
{
    (void)w; (void)h; (void)fullscreen; (void)resizable; (void)names;
    if (!quiet) fprintf(stderr, "x11: not built in (no libxcb1-dev when this was built)\n");
    return NULL;
}
static void rmp_x11_close(RmpX11 *x) { (void)x; }
static void *rmp_x11_connection(RmpX11 *x) { (void)x; return NULL; }
static int rmp_x11_screen(const RmpX11 *x) { (void)x; return 0; }
static unsigned rmp_x11_visual(const RmpX11 *x) { (void)x; return 0; }
static void *rmp_x11_window(RmpX11 *x) { (void)x; return NULL; }
static void rmp_x11_show(RmpX11 *x) { (void)x; }
static int rmp_x11_is_fullscreen(const RmpX11 *x) { (void)x; return 0; }
static void rmp_x11_set_fullscreen(RmpX11 *x, int fullscreen) { (void)x; (void)fullscreen; }
static int rmp_x11_fd(const RmpX11 *x) { (void)x; return -1; }
static int rmp_x11_dispatch(RmpX11 *x) { (void)x; return -1; }
static void rmp_x11_size(const RmpX11 *x, int *w, int *h) { (void)x; *w = *h = 0; }
static int rmp_x11_take_resized(RmpX11 *x) { (void)x; return 0; }
static int rmp_x11_take_exposed(RmpX11 *x) { (void)x; return 0; }
static int rmp_x11_keys_pending(const RmpX11 *x) { (void)x; return 0; }
static int rmp_x11_poll_key(RmpX11 *x, rmp_key_event *out) { (void)x; (void)out; return 0; }
static int rmp_x11_key_down(const RmpX11 *x, int code) { (void)x; (void)code; return 0; }
static int rmp_x11_pointer_pending(const RmpX11 *x) { (void)x; return 0; }
static int rmp_x11_poll_pointer(RmpX11 *x, rmp_pointer_event *out) { (void)x; (void)out; return 0; }
static void rmp_x11_hide(RmpX11 *x) { (void)x; }
static void rmp_x11_activate(RmpX11 *x) { (void)x; }
static int rmp_x11_take_close(RmpX11 *x) { (void)x; return 0; }
static int rmp_x11_close_pending(const RmpX11 *x) { (void)x; return 0; }
typedef struct RmpXdnd RmpXdnd;
typedef int (*rmp_xdnd_over_fn)(void *, int, int, const char *const *, size_t, unsigned, unsigned);
typedef int (*rmp_xdnd_drop_fn)(void *, int, int, const char *const *, size_t, int);
static RmpXdnd *rmp_xdnd_open(RmpX11 *x) { (void)x; return NULL; }
static void rmp_xdnd_close(RmpXdnd *d) { (void)d; }
static void rmp_xdnd_set_target(RmpXdnd *d, rmp_xdnd_over_fn over, rmp_xdnd_drop_fn drop, void *ctx)
{
    (void)d; (void)over; (void)drop; (void)ctx;
}
static int rmp_xdnd_drag(RmpXdnd *d, const char *const *paths, size_t count)
{
    (void)d; (void)paths; (void)count;
    return -1;
}
static int rmp_xdnd_take_result(RmpXdnd *d) { (void)d; return -1; }
#endif

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_PLATFORM_XCB_EXT
#define EGL_PLATFORM_XCB_EXT 0x31DC
#define EGL_PLATFORM_XCB_SCREEN_EXT 0x31DE
#endif
#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT 0x313D
#endif
typedef EGLBoolean (EGLAPIENTRY *SetDamageRegionFn)(EGLDisplay, EGLSurface, EGLint *, EGLint);
typedef EGLDisplay (EGLAPIENTRY *GetPlatformDisplayFn)(EGLenum, void *, const EGLint *);
typedef EGLSurface (EGLAPIENTRY *CreatePlatformWindowSurfaceFn)(EGLDisplay, EGLConfig, void *, const EGLint *);
/* X11: no flip events; frames are paced by the clock at this rate. */
#define X11_PERIOD (1.0 / 60.0)

enum { FRAME_NONE, FRAME_ACTIVE, FRAME_SKIP_IDLE, FRAME_SKIP_TIME };
#define STATS_INTERVALS 512

static volatile sig_atomic_t quit_flag;
static int atexit_done;

/* One window (rmp_window_open). The SELECTED window's x11, surface, width,
 * height, scaled and damage are P's own fields below (so the single-window
 * code is unchanged); a window not selected keeps them here. */
typedef struct {
    int used, shown;
    int interval;                 /* its rmp_set_frame_interval */
    int have_target;              /* last_target is valid */
    double last_target;           /* display time of its last frame (several windows) */
    RmpX11 *x11;
    EGLSurface surface;
    int width, height, scaled;
    rmp_damage damage;
} Window;

static struct {
    int initialized;
    rmp_platform_options options;
    RmpKms *kms;                  /* the KMS output, or */
    RmpX11 *x11;                  /* the X11 output (exactly one is set); the selected window's */
    RmpXdnd *xdnd;                /* X11: drag and drop */
    double last_present;          /* X11: when the previous swap returned */
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;           /* the selected window's */
    EGLConfig config;             /* for the surfaces of more windows */
    Window win[RMP_WINDOWS_MAX];  /* 0: the first window (KMS: the only one) */
    int cur;                      /* the selected window */
    /* rmp_frames_begin .. rmp_frames_end: one pass over the windows. */
    int tick_open, tick_presented, tick_held;
    double tick_target;
    int has_age;
    SetDamageRegionFn set_damage;
    int damage_error_reported;
    int width, height;            /* what the app draws: RMP_DISPLAY_W x H */
    int scaled;                   /* the mode differs: rmp_gfx_set_output */
    rmp_damage damage;
    rmp_pacing pacing;
    unsigned long flips_seen;
    unsigned last_sequence;
    int have_sequence;
    int frame_state, frame_full;
    unsigned clear_rgb;
    long frame_px;
    double frame_begin;
    /* The frame being built: interval, display time and vblank sequence. */
    int frame_interval, frame_has_seq;
    double frame_target;
    unsigned frame_target_seq, frame_deadline_seq;
    /* The queued flip: its deadline, judged when it completes (MISSED). */
    int queued_valid, queued_interval;
    unsigned queued_deadline_seq;
    unsigned long missed_total;
    int vblank_error_reported;
    struct termios saved_terminal;
    int terminal_saved;
    /* Statistics of the running one-second window. */
    double stats_start, stats_cpu;
    double intervals[STATS_INTERVALS];
    size_t interval_count;
    unsigned missed, frames;
    double build_sum, swap_sum, hold_sum, px_sum;
    rmp_frame_stats stats;
} P;

/* The X window of window i (the selected one's is P.x11). */
static RmpX11 *win_x11(int i) { return i == P.cur ? P.x11 : P.win[i].x11; }

double rmp_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int  rmp_quit_requested(void) { return quit_flag != 0; }
void rmp_request_quit(void) { quit_flag = 1; }
static void on_signal(int sig) { (void)sig; quit_flag = 1; }

void rmp_platform_default_options(rmp_platform_options *o)
{
    o->output = RMP_OUTPUT_AUTO;
    o->fullscreen = 0;
    o->card = NULL;
    o->grab_keyboard = 1;
    o->manage_terminal = 1;
    o->install_signal_handlers = 1;
    o->log_stats = 0;
    o->display_w = 0;
    o->display_h = 0;
    o->resizable = 0;
    o->title = NULL;
    o->wm_instance = NULL;
    o->wm_class = NULL;
    o->icon = NULL;
    o->icon_words = 0;
    o->place_apart = 0;
    o->close_key = 0;
    o->window_w = o->window_h = 0;
    o->positioned = o->x = o->y = 0;
    o->map_unfocused = 0;
}

int rmp_platform_output(void) { return !P.initialized ? 0 : P.x11 ? RMP_OUTPUT_X11 : RMP_OUTPUT_KMS; }

int rmp_platform_probe(const rmp_platform_options *options, int *screen_w, int *screen_h, rmp_rect *work)
{
    rmp_platform_options defaults;
    if (!options) {
        rmp_platform_default_options(&defaults);
        options = &defaults;
    }
    if (options->output == RMP_OUTPUT_KMS) return RMP_OUTPUT_KMS;
#if RMP_WITH_X11
    int w = 0, h = 0, area[4];
    if (rmp_x11_probe(&w, &h, area) == 0) {
        if (screen_w) *screen_w = w;
        if (screen_h) *screen_h = h;
        if (work) *work = (rmp_rect){ area[0], area[1], area[2], area[3] };
        return RMP_OUTPUT_X11;
    }
#else
    (void)screen_w; (void)screen_h; (void)work;
#endif
    return RMP_OUTPUT_KMS;
}

int rmp_display_width(void) { return P.width; }
int rmp_display_height(void) { return P.height; }

void rmp_output_size(int *w, int *h)
{
    *w = *h = 0;
    if (P.x11) rmp_x11_size(P.x11, w, h);
    else if (P.kms) {
        *w = rmp_kms_width(P.kms);
        *h = rmp_kms_height(P.kms);
    }
}

int rmp_platform_is_fullscreen(void)
{
    return P.x11 ? rmp_x11_is_fullscreen(P.x11) : 0;
}

void rmp_platform_set_fullscreen(int fullscreen)
{
    if (P.x11) rmp_x11_set_fullscreen(P.x11, fullscreen);
}

int rmp_set_display_size(int w, int h)
{
    if (!P.initialized || P.frame_state == FRAME_ACTIVE || w < 1 || h < 1 || w > 4096 || h > 4096) return -1;
    if (w == P.width && h == P.height) return 0;
    int out_w, out_h;
    rmp_output_size(&out_w, &out_h);
    if (rmp_gfx_set_display(w, h, 1) < 0) return -1;
    int scaled = rmp_gfx_set_output(out_w, out_h);
    if (scaled < 0) return -1;
    P.width = w;
    P.height = h;
    P.scaled = scaled;
    rmp_damage_init(&P.damage, w, h);
    rmp_damage_add_full(&P.damage);
    return 0;
}

/* ---- terminal ---------------------------------------------------------- */
static void terminal_setup(void)
{
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &P.saved_terminal) < 0) return;
    struct termios t = P.saved_terminal;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);   /* ISIG stays: Ctrl-C still works */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) P.terminal_saved = 1;
}

static void terminal_restore(void)
{
    if (!P.terminal_saved) return;
    /* Keys typed while the app ran must not reach the shell afterwards. */
    tcflush(STDIN_FILENO, TCIFLUSH);
    int rc;
    do { rc = tcsetattr(STDIN_FILENO, TCSANOW, &P.saved_terminal); } while (rc < 0 && errno == EINTR);
    if (rc < 0) perror("restore terminal");
    P.terminal_saved = 0;
}

/* ---- statistics -------------------------------------------------------- */
/* /proc stat comm may contain spaces and parentheses; fields follow the last ')'. */
static double cpu_seconds(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return NAN;
    char buf[4096];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return NAN; }
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return NAN;
    p++;
    unsigned long long utime = 0, stime = 0;
    for (int field = 3; field <= 15; field++) {
        while (*p == ' ') p++;
        if (!*p) return NAN;
        char *end = p;
        while (*end && *end != ' ') end++;
        if (field == 14) utime = strtoull(p, NULL, 10);
        if (field == 15) stime = strtoull(p, NULL, 10);
        p = end;
    }
    long ticks = sysconf(_SC_CLK_TCK);
    return ticks > 0 ? (double)(utime + stime) / (double)ticks : NAN;
}

static void stats_reset(double now)
{
    P.stats_start = now;
    P.stats_cpu = cpu_seconds();
    P.interval_count = 0;
    P.missed = P.frames = 0;
    P.build_sum = P.swap_sum = P.hold_sum = P.px_sum = 0;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void stats_report(double now)
{
    double elapsed = now - P.stats_start;
    if (elapsed < 1) return;
    rmp_frame_stats s;
    memset(&s, 0, sizeof s);
    size_t n = P.interval_count;
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += P.intervals[i];
    if (n) {
        qsort(P.intervals, n, sizeof P.intervals[0], compare_double);
        s.p99_ms = (float)P.intervals[(size_t)ceil((double)n * .99) - 1];
        s.frame_ms = (float)(sum / (double)n);
    }
    s.fps = (float)((double)n / elapsed);
    if (P.frames) {
        s.build_ms = (float)(P.build_sum / P.frames);
        s.swap_ms = (float)(P.swap_sum / P.frames);
        s.hold_ms = (float)(P.hold_sum / P.frames);
        s.redraw_px = (float)(P.px_sum / P.frames);
    }
    s.cpu_pct = (float)(100 * (cpu_seconds() - P.stats_cpu) / elapsed);
    s.missed = P.missed;
    s.interval = P.pacing.interval;
    P.stats = s;
    if (P.options.log_stats)
        fprintf(stderr, "FPS %.2f INTERVAL %d FLIPS %zu FRAME %.2fMS P99 %.2fMS BUILD %.2fMS SWAP %.2fMS "
                "HOLD %.2fMS MISSED %u CPU %.1f%% DRAW %.0fPX FRAMES %u\n",
                s.fps, s.interval, n, s.frame_ms, s.p99_ms, s.build_ms, s.swap_ms, s.hold_ms, s.missed,
                s.cpu_pct, s.redraw_px, P.frames);
    stats_reset(now);
}

rmp_frame_stats rmp_get_frame_stats(void) { return P.stats; }

/* Consume completed flip records: pacing clock, intervals, missed vblanks.
 * MISSED: the flip landed later than its deadline, which is the later of the
 * vblank planned when its frame began (rmp_pacing_deadline: the target -
 * interval after the last flip, or the slot after an idle frame - or, for a
 * frame begun after its slot had passed, the vblank after the next one) and
 * `interval` vblanks after the flip before it. A gap left by idle
 * (undamaged) frames or by the app being busy is not a miss. */
static void sync_flips(void)
{
    if (!P.kms) return;
    unsigned long count = rmp_kms_flip_count(P.kms);
    for (; P.flips_seen < count; P.flips_seen++) {
        double t;
        unsigned seq;
        if (rmp_kms_flip_record(P.kms, P.flips_seen, &t, &seq) < 0) { P.queued_valid = 0; continue; }
        if (P.pacing.have_flip && P.have_sequence) {
            if (P.interval_count < STATS_INTERVALS)
                P.intervals[P.interval_count++] = (t - P.pacing.last_flip) * 1000;
            if (P.queued_valid) {
                unsigned want = P.queued_deadline_seq, after_last = P.last_sequence + (unsigned)P.queued_interval;
                if ((int)(want - after_last) < 0) want = after_last;
                if ((int)(seq - want) > 0) { P.missed++; P.missed_total++; }
            }
        }
        P.queued_valid = 0;
        rmp_pacing_flip_done(&P.pacing, t);
        P.last_sequence = seq;
        P.have_sequence = 1;
    }
    P.pacing.pending = rmp_kms_flip_pending(P.kms);
}

/* ---- EGL --------------------------------------------------------------- */
static int egl_open(void)
{
    GetPlatformDisplayFn get_display = (GetPlatformDisplayFn)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) return -1;
    if (P.x11) {
        const EGLint screen[] = { EGL_PLATFORM_XCB_SCREEN_EXT, rmp_x11_screen(P.x11), EGL_NONE };
        P.display = get_display(EGL_PLATFORM_XCB_EXT, rmp_x11_connection(P.x11), screen);
    } else {
        P.display = get_display(EGL_PLATFORM_GBM_KHR, rmp_kms_gbm_device(P.kms), NULL);
    }
    if (P.display == EGL_NO_DISPLAY || !eglInitialize(P.display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
        return -1;
    EGLint count = 0;
    EGLConfig chosen = NULL;
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (!eglChooseConfig(P.display, attrs, NULL, 0, &count) || count <= 0) return -1;
    EGLConfig *configs = calloc((size_t)count, sizeof *configs);
    if (!configs) return -1;
    EGLint actual = 0;
    /* The config whose native visual is the GBM fourcc / the window's visual. */
    const uint32_t want = P.x11 ? (uint32_t)rmp_x11_visual(P.x11) : rmp_kms_format(P.kms);
    if (eglChooseConfig(P.display, attrs, configs, count, &actual))
        for (EGLint i = 0; i < actual; i++) {
            EGLint visual = 0;
            eglGetConfigAttrib(P.display, configs[i], EGL_NATIVE_VISUAL_ID, &visual);
            if ((uint32_t)visual == want) { chosen = configs[i]; break; }
        }
    free(configs);
    if (!chosen) {
        fprintf(stderr, "EGL: no config matching the %s\n", P.x11 ? "X window's visual" : "GBM fourcc");
        return -1;
    }
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    P.config = chosen;
    P.context = eglCreateContext(P.display, chosen, EGL_NO_CONTEXT, context_attrs);
    if (P.context == EGL_NO_CONTEXT) return -1;
    if (P.x11) {
        CreatePlatformWindowSurfaceFn create =
            (CreatePlatformWindowSurfaceFn)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
        if (!create) return -1;
        P.surface = create(P.display, chosen, rmp_x11_window(P.x11), NULL);
    } else {
        P.surface = eglCreateWindowSurface(P.display, chosen, (EGLNativeWindowType)rmp_kms_gbm_surface(P.kms), NULL);
    }
    if (P.surface == EGL_NO_SURFACE || !eglMakeCurrent(P.display, P.surface, P.surface, P.context)) return -1;
    (void)eglSwapInterval(P.display, 1);
    /* Refuse Mesa's software fallback (llvmpipe/softpipe): it "works" at
     * ~14 fps with 280% CPU. Happens when /dev/dri/renderD128 (root:render
     * 0660) cannot be opened, e.g. over SSH or from a boot service without
     * a console login (no logind uaccess ACL). RMP_ALLOW_SOFTWARE_GL=1 skips. */
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    fprintf(stderr, "GL renderer: %s\n", renderer ? renderer : "(null)");
    if (renderer && (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) &&
        !getenv("RMP_ALLOW_SOFTWARE_GL")) {
        fprintf(stderr, "GL: software renderer, not the GPU. /dev/dri/renderD128 is not accessible;"
                        " add the user to the render group: sudo usermod -aG render $USER"
                        " (RMP_ALLOW_SOFTWARE_GL=1 runs anyway)\n");
        return -1;
    }
    const char *ext = eglQueryString(P.display, EGL_EXTENSIONS);
    int partial = ext && strstr(ext, "EGL_KHR_partial_update") != NULL;
    P.has_age = ext && (strstr(ext, "EGL_EXT_buffer_age") != NULL || partial);
    /* Damage regions need the age query each frame (Mesa: EGL_BAD_ACCESS otherwise). */
    if (partial && P.has_age) P.set_damage = (SetDamageRegionFn)eglGetProcAddress("eglSetDamageRegionKHR");
    fprintf(stderr, "EGL partial redraw: buffer_age %s, partial_update %s\n",
            P.has_age ? "yes" : "no", P.set_damage ? "yes" : "no");
    return 0;
}

static void egl_close(void)
{
    if (P.display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(P.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (P.surface != EGL_NO_SURFACE) eglDestroySurface(P.display, P.surface);
    if (P.context != EGL_NO_CONTEXT) eglDestroyContext(P.display, P.context);
    eglTerminate(P.display);
    P.display = EGL_NO_DISPLAY;
    P.surface = EGL_NO_SURFACE;
    P.context = EGL_NO_CONTEXT;
}

/* ---- drag and drop (X11) ------------------------------------------------- */
static int x11_events(void);
/* The app's callbacks: kept across rmp_platform_init (which clears P). */
static struct {
    rmp_dnd_over_fn over;
    rmp_dnd_drop_fn drop;
    void *ctx;
} DND;

/* Window pixels of window `win` to its display pixels (floor, also left of /
 * above it), as the pointer's (the window's scaling and centring undone). */
static void window_to_display_of(int win, int *x, int *y)
{
    rmp_rect d;
    int dw, dh;
    rmp_gfx_output_rect(win, &d, &dw, &dh);
    if (d.w <= 0 || d.h <= 0) return;
    long px = (long)(*x - d.x) * dw, py = (long)(*y - d.y) * dh;
    *x = (int)(px >= 0 ? px / d.w : (px - d.w + 1) / d.w);
    *y = (int)(py >= 0 ? py / d.h : (py - d.h + 1) / d.h);
}

static int dnd_over(void *ctx, int x, int y, const char *const *paths, size_t count, unsigned mods, unsigned allowed)
{
    (void)ctx;
    if (!DND.over) return RMP_DND_NONE;
    window_to_display_of(0, &x, &y);
    return DND.over(DND.ctx, x, y, paths, count, mods, allowed);
}

static int dnd_drop(void *ctx, int x, int y, const char *const *paths, size_t count, int action)
{
    (void)ctx;
    if (!DND.drop) return RMP_DND_NONE;
    window_to_display_of(0, &x, &y);
    return DND.drop(DND.ctx, x, y, paths, count, action);
}

int rmp_dnd_accept(rmp_dnd_over_fn over, rmp_dnd_drop_fn drop, void *ctx)
{
    DND.over = over && drop ? over : NULL;
    DND.drop = over && drop ? drop : NULL;
    DND.ctx = ctx;
    if (!P.x11 || !P.xdnd) return -1;
    if (DND.over) rmp_xdnd_set_target(P.xdnd, dnd_over, dnd_drop, NULL);
    else rmp_xdnd_set_target(P.xdnd, NULL, NULL, NULL);
    return 0;
}

int rmp_dnd_drag(const char *const *paths, size_t count)
{
    return P.initialized && P.x11 ? rmp_xdnd_drag(P.xdnd, paths, count) : -1;
}

int rmp_dnd_take_result(void)
{
    if (!P.initialized || !P.x11) return -1;
    x11_events();
    return rmp_xdnd_take_result(P.xdnd);
}

/* ---- init / shutdown --------------------------------------------------- */
int rmp_platform_init(const rmp_platform_options *options)
{
    if (P.initialized) return 0;
    memset(&P, 0, sizeof P);
    P.display = EGL_NO_DISPLAY;
    P.context = EGL_NO_CONTEXT;
    P.surface = EGL_NO_SURFACE;
    P.win[0].used = P.win[0].shown = 1;
    P.win[0].interval = 2;        /* rmp_pacing_init's */
    if (options) P.options = *options;
    else rmp_platform_default_options(&P.options);
    P.clear_rgb = 0x080C10;
    if (P.options.install_signal_handlers) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        const int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
        for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) sigaction(signals[i], &sa, NULL);
    }
    P.initialized = 1;
    if (!atexit_done && atexit(rmp_platform_shutdown) == 0) atexit_done = 1;
    int sized = P.options.display_w > 0 && P.options.display_h > 0 &&
                P.options.display_w <= 4096 && P.options.display_h <= 4096;
    P.width = sized ? P.options.display_w : RMP_DISPLAY_W;
    P.height = sized ? P.options.display_h : RMP_DISPLAY_H;
    /* X11 when asked, or (auto) when an X server answers on $DISPLAY. */
    if (P.options.output != RMP_OUTPUT_KMS) {
        P.x11 = rmp_x11_open(P.width, P.height, P.options.fullscreen, P.options.output == RMP_OUTPUT_AUTO,
                             P.options.resizable, &P.options);
        if (!P.x11 && P.options.output == RMP_OUTPUT_X11) goto fail;
    }
    int out_w, out_h;
    if (P.x11) {
        fprintf(stderr, "x11: window %dx%d on %s\n", P.width, P.height, getenv("DISPLAY"));
        rmp_x11_size(P.x11, &out_w, &out_h);
    } else {
        if (P.options.manage_terminal) terminal_setup();
        P.kms = rmp_kms_open(P.options.card);
        if (!P.kms) goto fail;
        out_w = rmp_kms_width(P.kms);
        out_h = rmp_kms_height(P.kms);
    }
    if (egl_open() < 0) {
        fprintf(stderr, "EGL initialization failed (0x%x)\n", (unsigned)eglGetError());
        goto fail;
    }
    if (rmp_gfx_init(P.width, P.height) < 0) goto fail;
    if (sized && rmp_gfx_set_display(P.width, P.height, 1) < 0) goto fail;
    P.scaled = rmp_gfx_set_output(out_w, out_h);
    if (P.scaled < 0) goto fail;
    if (P.scaled)
        fprintf(stderr, "display: %dx%d scaled to fit %dx%d (aspect kept, centred)\n",
                P.width, P.height, out_w, out_h);
    rmp_damage_init(&P.damage, P.width, P.height);
    rmp_damage_add_full(&P.damage);
    rmp_pacing_init(&P.pacing, P.x11 ? X11_PERIOD : rmp_kms_refresh_period(P.kms));
    double now = rmp_now_seconds();
    if (P.x11) {
        P.xdnd = rmp_xdnd_open(P.x11);
        if (DND.over) rmp_xdnd_set_target(P.xdnd, dnd_over, dnd_drop, NULL);
        rmp_x11_show(P.x11);
    } else {
        rmp_input_open(P.options.grab_keyboard, now);
    }
    stats_reset(now);
    return 0;
fail:
    rmp_platform_shutdown();
    return -1;
}

static void win_save(int i);
static void win_load(int i);

void rmp_platform_shutdown(void)
{
    if (!P.initialized) return;
    P.initialized = 0;
    if (P.cur != 0) {   /* window 0's surface for the GL clean-up below */
        rmp_gfx_select_output(0);
        win_save(P.cur);
        win_load(0);
        P.cur = 0;
    }
    rmp_input_close();
    /* Restore the display first so the screen is never left black. */
    int restored = P.kms ? rmp_kms_restore(P.kms) : 0;
    terminal_restore();
    if (restored == 0) {
        if (P.display != EGL_NO_DISPLAY && P.context != EGL_NO_CONTEXT &&
            eglMakeCurrent(P.display, P.surface, P.surface, P.context))
            rmp_gfx_shutdown();
        for (int i = 1; i < RMP_WINDOWS_MAX; i++)
            if (P.win[i].used && P.win[i].surface != EGL_NO_SURFACE && P.display != EGL_NO_DISPLAY)
                eglDestroySurface(P.display, P.win[i].surface);
        egl_close();
        rmp_kms_close(P.kms);
        rmp_xdnd_close(P.xdnd);
        rmp_x11_close(P.x11);   /* after EGL: the surface is on its window */
        for (int i = 1; i < RMP_WINDOWS_MAX; i++)
            if (P.win[i].used) rmp_x11_close(P.win[i].x11);
    } else {
        fprintf(stderr, "display restore failed; retaining EGL/GBM until process exit\n");
    }
    P.kms = NULL;
    P.x11 = NULL;
    P.xdnd = NULL;
    memset(P.win, 0, sizeof P.win);
}

/* ---- frame loop -------------------------------------------------------- */
/* The pacing runs at the fastest shown window (the selected one when none
 * is shown). One window: exactly its interval, as before. */
static void apply_interval(void)
{
    int n = 0;
    for (int i = 0; i < RMP_WINDOWS_MAX; i++)
        if (P.win[i].used && P.win[i].shown && (!n || P.win[i].interval < n)) n = P.win[i].interval;
    rmp_pacing_set_interval(&P.pacing, n ? n : P.win[P.cur].interval);
}

void rmp_set_frame_interval(int vblanks)
{
    if (!P.initialized) {
        rmp_pacing_set_interval(&P.pacing, vblanks);
        return;
    }
    P.win[P.cur].interval = vblanks < 1 ? 1 : vblanks > 4 ? 4 : vblanks;
    apply_interval();
}
int  rmp_frame_interval(void) { return P.pacing.interval; }

void rmp_frame_damage(const rmp_rect *r) { if (r && P.initialized) rmp_damage_add(&P.damage, *r); }
void rmp_frame_invalidate_all(void) { if (P.initialized) rmp_damage_add_full(&P.damage); }
void rmp_frame_set_clear_color(unsigned rgb) { P.clear_rgb = rgb & 0xFFFFFFu; }
int  rmp_frame_is_full(void) { return P.frame_state == FRAME_ACTIVE && P.frame_full; }

static double due_time(double now)
{
    sync_flips();
    return rmp_pacing_due(&P.pacing, now);
}

/* X11: read the window's events; a new size or an expose repaints all.
 * Not inside a frame (the scaled texture may be re-created). */
static int x11_events(void)
{
    /* The other windows: only read (a new size or an expose is applied when
     * the window is selected, before its frame: the flags wait in RmpX11). */
    for (int i = 0; i < RMP_WINDOWS_MAX; i++)
        if (i != P.cur && P.win[i].used && rmp_x11_dispatch(P.win[i].x11) < 0) {
            fprintf(stderr, "x11: connection lost\n");
            quit_flag = 1;
            return -1;
        }
    if (rmp_x11_dispatch(P.x11) < 0) {
        fprintf(stderr, "x11: connection lost\n");
        quit_flag = 1;
        return -1;
    }
    if (P.frame_state != FRAME_ACTIVE && rmp_x11_take_resized(P.x11)) {
        int w, h;
        rmp_x11_size(P.x11, &w, &h);
        int scaled = rmp_gfx_set_output(w, h);
        if (scaled < 0) return -1;
        P.scaled = scaled;
        rmp_damage_add_full(&P.damage);
    }
    if (rmp_x11_take_exposed(P.x11)) rmp_damage_add_full(&P.damage);
    return 0;
}

int rmp_frame_ready(void)
{
    if (P.x11) {
        if (x11_events() < 0) return 0;
        double now = rmp_now_seconds();
        return due_time(now) <= now;
    }
    if (!P.kms) return 0;
    if (rmp_kms_dispatch(P.kms) < 0) return 0;
    double now = rmp_now_seconds();
    return due_time(now) <= now && rmp_kms_has_free_buffer(P.kms);
}

double rmp_frame_time(void)
{
    sync_flips();
    return rmp_pacing_target(&P.pacing, rmp_now_seconds());
}

/* A queued key, pointer event or close request in any window. */
static int x11_input_pending(void)
{
    for (int i = 0; i < RMP_WINDOWS_MAX; i++) {
        if (!P.win[i].used) continue;
        RmpX11 *x = win_x11(i);
        if (rmp_x11_keys_pending(x) || rmp_x11_pointer_pending(x) || rmp_x11_close_pending(x)) return 1;
    }
    return 0;
}

/* X11: like the KMS loop below, without flip events (the swap blocks). */
static int x11_wait(double max_seconds)
{
    double start = rmp_now_seconds(), limit = start + (max_seconds > 0 ? max_seconds : 0);
    for (;;) {
        if (x11_events() < 0) return -1;
        double now = rmp_now_seconds(), due = due_time(now);
        if (due <= now) return 1;
        if (x11_input_pending() || quit_flag) return 0;
        if (now >= limit) return 0;
        double until = due < limit ? due : limit;
        int timeout = (int)ceil((until - now) * 1000);
        if (timeout < 0) timeout = 0;
        struct pollfd fds[RMP_WINDOWS_MAX];
        nfds_t nfds = 0;
        for (int i = 0; i < RMP_WINDOWS_MAX; i++)
            if (P.win[i].used) fds[nfds++] = (struct pollfd){ rmp_x11_fd(win_x11(i)), POLLIN, 0 };
        if (poll(fds, nfds, timeout) < 0 && errno != EINTR) return -1;
    }
}

int rmp_wait(double max_seconds)
{
    if (P.x11) return x11_wait(max_seconds);
    if (!P.kms) return -1;
    double start = rmp_now_seconds(), limit = start + (max_seconds > 0 ? max_seconds : 0);
    for (;;) {
        if (rmp_kms_dispatch(P.kms) < 0) return -1;
        double now = rmp_now_seconds(), due = due_time(now);
        int free_buffer = rmp_kms_has_free_buffer(P.kms);
        if (due <= now && free_buffer) return 1;
        if (rmp_input_pending() || quit_flag) return 0;
        if (now >= limit) return 0;
        double until = limit;
        if (free_buffer && due < until) until = due;   /* else: wait for the flip event */
        int timeout = (int)ceil((until - now) * 1000);
        if (timeout < 0) timeout = 0;
        struct pollfd fds[3] = { { rmp_kms_fd(P.kms), POLLIN, 0 }, { rmp_input_fd(), POLLIN, 0 },
                                 { rmp_input_extra_fd(), POLLIN, 0 } };
        int nfds = 3;   /* a negative fd is skipped by poll */
        int ret = poll(fds, (nfds_t)nfds, timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if ((fds[1].revents | fds[2].revents) & (POLLIN | POLLERR | POLLHUP)) return 0;
    }
}

/* Age, damage region and partial update of the selected window, for a frame
 * whose pacing fields (frame_begin, target...) are set. */
static int begin_region(rmp_rect *out, int max)
{
    /* Age first, then the damage region, then any GL command (partial_update). */
    EGLint age = 0;
    if (P.has_age && !eglQuerySurface(P.display, P.surface, EGL_BUFFER_AGE_EXT, &age)) age = 0;
    /* Scaled: the app draws into one texture that keeps the last frame
     * (age 1); the surface itself is redrawn whole (no damage region). */
    if (P.scaled) age = 1;
    int full = 0;
    int n = rmp_damage_region(&P.damage, (int)age, out, max, &full);
    if (P.set_damage && !P.scaled) {
        EGLint region[4 * RMP_REPAINT_MAX];
        for (int i = 0; i < n; i++) {   /* EGL rectangles are bottom-left origin */
            region[i * 4] = out[i].x;
            region[i * 4 + 1] = P.height - (out[i].y + out[i].h);
            region[i * 4 + 2] = out[i].w;
            region[i * 4 + 3] = out[i].h;
        }
        if (!P.set_damage(P.display, P.surface, region, n) && !P.damage_error_reported) {
            fprintf(stderr, "eglSetDamageRegionKHR failed: 0x%x (drawing stays partial)\n", (unsigned)eglGetError());
            P.damage_error_reported = 1;
        }
    }
    P.frame_px = 0;
    for (int i = 0; i < n; i++) P.frame_px += rmp_rect_area(out[i]);
    P.frame_full = full;
    P.frame_state = FRAME_ACTIVE;
    rmp_gfx_frame_start(full, P.clear_rgb);
    return n;
}

/* Inside rmp_frames_begin .. rmp_frames_end: the pass is due already; this
 * window draws when it has damage and its own interval has come round (a
 * 30 fps window beside a 60 fps one draws every other pass). */
static int tick_frame_begin(rmp_rect *out, int max, double now)
{
    Window *w = &P.win[P.cur];
    if (!rmp_damage_pending(&P.damage)) { P.frame_state = FRAME_SKIP_IDLE; return 0; }
    if (w->interval > P.pacing.interval && w->have_target &&
        P.tick_target < w->last_target + ((double)w->interval - 0.5) * P.pacing.period) {
        P.frame_state = FRAME_SKIP_TIME;   /* the damage waits for its turn */
        return 0;
    }
    P.frame_begin = now;
    P.frame_interval = P.pacing.interval;
    P.frame_target = P.tick_target;
    P.frame_has_seq = 0;
    return begin_region(out, max);
}

int rmp_frame_begin(rmp_rect *out, int max)
{
    if ((!P.kms && !P.x11) || P.frame_state == FRAME_ACTIVE) return -1;
    if (max > RMP_REPAINT_MAX) max = RMP_REPAINT_MAX;
    if (max < 1) return -1;
    if (P.x11 ? x11_events() < 0 : rmp_kms_dispatch(P.kms) < 0) return -1;
    double now = rmp_now_seconds();
    if (P.tick_open) return tick_frame_begin(out, max, now);
    if (due_time(now) > now) { P.frame_state = FRAME_SKIP_TIME; return 0; }
    if (!rmp_damage_pending(&P.damage)) { P.frame_state = FRAME_SKIP_IDLE; return 0; }
    /* Normally immediate (three GBM buffers); blocks only if all are busy. */
    if (P.kms && rmp_kms_wait_free_buffer(P.kms) < 0) return -1;
    P.frame_begin = rmp_now_seconds();
    /* Where this frame is to be shown: interval >= 2 holds its flip until
     * the vblank before (rmp_frame_end). Sequences count like flip records. */
    P.frame_interval = P.pacing.interval;
    P.frame_target = rmp_pacing_target(&P.pacing, P.frame_begin);
    P.frame_has_seq = P.pacing.have_flip && P.have_sequence;
    if (P.frame_has_seq) {
        P.frame_target_seq = P.last_sequence +
            (unsigned)lround((P.frame_target - P.pacing.last_flip) / P.pacing.period);
        P.frame_deadline_seq = P.last_sequence +
            (unsigned)lround((rmp_pacing_deadline(&P.pacing, P.frame_begin) - P.pacing.last_flip) / P.pacing.period);
    }
    return begin_region(out, max);
}

void rmp_frame_clip(const rmp_rect *r)
{
    if (P.frame_state == FRAME_ACTIVE) rmp_gfx_clip(r);
}

/* Sleeps on the monotonic clock (poll with no descriptor). */
static void sleep_until(double t)
{
    for (;;) {
        double now = rmp_now_seconds();
        if (now >= t) return;
        struct pollfd none = { -1, 0, 0 };
        if (poll(&none, 1, (int)ceil((t - now) * 1000)) < 0 && errno != EINTR) return;
    }
}

/* Interval >= 2: hold the swapped frame's page flip until the vblank before
 * its target, so it latches on the target and never earlier. The frame was
 * begun at most about two vblanks before the target, so this waits at most
 * about one vblank. Keys wait meanwhile (rmp_poll_keys after the frame). */
static void hold_flip(void)
{
    if (P.frame_interval < 2 || !P.frame_has_seq) return;
    /* After an idle spell (FDTP waiting for a key) the kernel has turned the
     * vblank interrupt off, and a wait for an absolute sequence then blocks
     * until its 3 s timeout (FDTP 2026-10-23: every key after 5 s idle took
     * 3.03 s). Nothing to pace against then: present at once. */
    if (!P.pacing.have_flip || rmp_now_seconds() - P.pacing.last_flip > 0.25) return;
    unsigned hold = P.frame_target_seq - 1;
    if ((int)(hold - P.last_sequence) <= 0) return;   /* a completed flip is already there */
    if (rmp_kms_wait_vblank(P.kms, hold) == 0) return;
    if (!P.vblank_error_reported) {
        fprintf(stderr, "drmWaitVBlank failed (%s): holding page flips by the clock\n", strerror(errno));
        P.vblank_error_reported = 1;
    }
    /* By time: the vblank before the target plus the margin that keeps a
     * flip from latching one vblank early. */
    sleep_until(P.frame_target - P.pacing.period + RMP_PACING_MARGIN);
}

/* X11: no page flip of our own, and the swap does not wait for a vblank
 * (XWayland returns at once). So the clock does the pacing, like hold_flip
 * without drmWaitVBlank: wait until the vblank before the frame's target,
 * swap, and take the target as its display time (the flip of KMS). The
 * next frame then begins about one vblank before that time, as on KMS
 * while a flip is queued. FPS/P99 come from those times; MISSED is not
 * judged. */
static int x11_present(double swap_start)
{
    double hold_start = rmp_now_seconds();
    sleep_until(P.frame_target - P.pacing.period + RMP_PACING_MARGIN);
    double hold = rmp_now_seconds() - hold_start;
    if (!eglSwapBuffers(P.display, P.surface)) {
        fprintf(stderr, "eglSwapBuffers failed: 0x%x\n", (unsigned)eglGetError());
        return -1;
    }
    double now = rmp_now_seconds();
    /* A slow frame, past its target: from now on, not the grid. */
    double shown = now <= P.frame_target ? P.frame_target : now;
    rmp_damage_commit(&P.damage);
    if (P.last_present > 0 && P.interval_count < STATS_INTERVALS)
        P.intervals[P.interval_count++] = (shown - P.last_present) * 1000;
    P.last_present = shown;
    rmp_pacing_presented(&P.pacing, 0);
    rmp_pacing_flip_done(&P.pacing, shown);
    P.frames++;
    P.build_sum += (swap_start - P.frame_begin) * 1000;
    P.swap_sum += (now - swap_start - hold) * 1000;
    P.hold_sum += hold * 1000;
    P.px_sum += (double)P.frame_px;
    stats_report(now);
    return 0;
}

/* One window's frame inside a pass: the first swap of the pass waits for the
 * vblank before the target (as x11_present), the others follow at once, so
 * that they all land on the target. The pacing moves at rmp_frames_end. */
static int x11_present_tick(double swap_start)
{
    double hold_start = rmp_now_seconds(), hold = 0;
    if (!P.tick_held) {
        sleep_until(P.frame_target - P.pacing.period + RMP_PACING_MARGIN);
        hold = rmp_now_seconds() - hold_start;
        P.tick_held = 1;
    }
    if (!eglSwapBuffers(P.display, P.surface)) {
        fprintf(stderr, "eglSwapBuffers failed: 0x%x\n", (unsigned)eglGetError());
        return -1;
    }
    double now = rmp_now_seconds();
    rmp_damage_commit(&P.damage);
    P.win[P.cur].last_target = P.frame_target;
    P.win[P.cur].have_target = 1;
    P.tick_presented++;
    P.frames++;
    P.build_sum += (swap_start - P.frame_begin) * 1000;
    P.swap_sum += (now - swap_start - hold) * 1000;
    P.hold_sum += hold * 1000;
    P.px_sum += (double)P.frame_px;
    return 0;
}

int rmp_frames_begin(void)
{
    if (!P.initialized || !P.x11 || P.frame_state == FRAME_ACTIVE || P.tick_open) return -1;
    if (x11_events() < 0) return -1;
    double now = rmp_now_seconds();
    if (due_time(now) > now) return 0;
    P.tick_open = 1;
    P.tick_presented = P.tick_held = 0;
    P.tick_target = rmp_pacing_target(&P.pacing, now);
    return 1;
}

int rmp_frames_end(void)
{
    if (!P.tick_open || P.frame_state == FRAME_ACTIVE) return -1;
    P.tick_open = 0;
    double now = rmp_now_seconds();
    if (!P.tick_presented) {   /* no window had anything to draw */
        rmp_pacing_idle(&P.pacing, now);
        return 0;
    }
    double shown = now <= P.tick_target ? P.tick_target : now;
    if (P.last_present > 0 && P.interval_count < STATS_INTERVALS)
        P.intervals[P.interval_count++] = (shown - P.last_present) * 1000;
    P.last_present = shown;
    rmp_pacing_presented(&P.pacing, 0);
    rmp_pacing_flip_done(&P.pacing, shown);
    stats_report(now);
    return 0;
}

int rmp_frame_end(void)
{
    int state = P.frame_state;
    P.frame_state = FRAME_NONE;
    if (state == FRAME_SKIP_IDLE) {
        if (!P.tick_open) rmp_pacing_idle(&P.pacing, rmp_now_seconds());
        return 0;
    }
    if (state != FRAME_ACTIVE) return 0;
    rmp_gfx_frame_finish();
    if (rmp_gfx_check_error("frame") < 0) return -1;
    double swap_start = rmp_now_seconds();
    if (P.x11) return P.tick_open ? x11_present_tick(swap_start) : x11_present(swap_start);
    /* No glFinish: the GPU finishes while the CPU builds the next frame; the
     * kernel's implicit fence holds the pageflip until rendering is done. */
    if (!eglSwapBuffers(P.display, P.surface)) {
        fprintf(stderr, "eglSwapBuffers failed: 0x%x\n", (unsigned)eglGetError());
        return -1;
    }
    double hold_start = rmp_now_seconds();
    hold_flip();
    double hold = rmp_now_seconds() - hold_start;
    if (rmp_kms_present_gl(P.kms) < 0) return -1;
    double now = rmp_now_seconds();
    rmp_damage_commit(&P.damage);
    sync_flips();   /* the previous flip (interval 1: completed inside present) */
    int pending = rmp_kms_flip_pending(P.kms);
    P.queued_valid = pending && P.frame_has_seq;
    P.queued_deadline_seq = P.frame_deadline_seq;
    P.queued_interval = P.frame_interval;
    rmp_pacing_presented(&P.pacing, pending);
    P.frames++;
    P.build_sum += (swap_start - P.frame_begin) * 1000;
    P.swap_sum += (now - swap_start - hold) * 1000;
    P.hold_sum += hold * 1000;
    P.px_sum += (double)P.frame_px;
    stats_report(now);
    return 0;
}

/* ---- keyboard ---------------------------------------------------------- */
#ifndef KEY_C
#define KEY_C 46
#endif
#ifndef KEY_LEFTCTRL
#define KEY_LEFTCTRL 29
#endif

int rmp_poll_keys(rmp_key_event *out, int max)
{
    if (!P.initialized || max <= 0) return 0;
    if (P.x11) {
        /* Ctrl(left)+C quits here too, like the grabbed keyboard on KMS. */
        x11_events();
        int n = 0;
        for (int i = 0; i < RMP_WINDOWS_MAX && n < max; i++) {
            if (!P.win[i].used) continue;
            RmpX11 *x = win_x11(i);
            while (n < max && rmp_x11_poll_key(x, out + n)) {
                if (out[n].code == KEY_C && out[n].value == 1 && rmp_x11_key_down(x, KEY_LEFTCTRL)) quit_flag = 1;
                out[n].window = i;
                n++;
            }
        }
        return n;
    }
    double now = rmp_now_seconds();
    int n = 0;
    /* One event at a time so the Ctrl state is the one at that event.
     * Grabbed keyboard: the tty never sees Ctrl-C, so honour it here. */
    while (n < max && rmp_input_poll(out + n, 1, now) == 1) {
        if (P.options.grab_keyboard && out[n].code == KEY_C && out[n].value == 1 &&
            rmp_input_key_down(KEY_LEFTCTRL))
            quit_flag = 1;
        out[n].window = 0;
        n++;
    }
    return n;
}

int rmp_poll_pointer(rmp_pointer_event *out, int max)
{
    if (!P.initialized || max <= 0 || !P.x11) return 0;
    x11_events();
    const double now = rmp_now_seconds();
    int n = 0;
    for (int i = 0; i < RMP_WINDOWS_MAX && n < max; i++) {
        if (!P.win[i].used) continue;
        RmpX11 *x = win_x11(i);
        while (n < max && rmp_x11_poll_pointer(x, out + n)) {
            rmp_pointer_event *e = out + n++;
            window_to_display_of(i, &e->x, &e->y);
            e->time = now;
            e->window = i;
        }
    }
    return n;
}

int rmp_key_down(int code) { return P.x11 ? rmp_x11_key_down(P.x11, code) : rmp_input_key_down(code); }
int rmp_keyboard_connected(void) { return P.x11 ? 1 : rmp_input_connected(); }

/* ---- several windows ------------------------------------------------------ */
static void win_save(int i)
{
    Window *w = &P.win[i];
    w->x11 = P.x11;
    w->surface = P.surface;
    w->width = P.width;
    w->height = P.height;
    w->scaled = P.scaled;
    w->damage = P.damage;
}

static void win_load(int i)
{
    const Window *w = &P.win[i];
    P.x11 = w->x11;
    P.surface = w->surface;
    P.width = w->width;
    P.height = w->height;
    P.scaled = w->scaled;
    P.damage = w->damage;
}

static int valid_window(int i) { return P.initialized && i >= 0 && i < RMP_WINDOWS_MAX && P.win[i].used; }

static rmp_damage *win_damage(int i) { return i == P.cur ? &P.damage : &P.win[i].damage; }

int rmp_window_selected(void) { return P.cur; }

int rmp_window_select(int i)
{
    if (!valid_window(i) || P.frame_state == FRAME_ACTIVE) return -1;
    if (i == P.cur) return 0;
    rmp_gfx_select_output(i);   /* flushes into the old surface first */
    win_save(P.cur);
    win_load(i);
    P.cur = i;
    if (!eglMakeCurrent(P.display, P.surface, P.surface, P.context)) {
        fprintf(stderr, "eglMakeCurrent (window %d) failed: 0x%x\n", i, (unsigned)eglGetError());
        return -1;
    }
    rmp_gfx_invalidate_state();   /* the display framebuffer and viewport of this window */
    return 0;
}

int rmp_window_open(const rmp_window_options *o)
{
    if (!P.initialized || !P.x11 || !o || P.frame_state == FRAME_ACTIVE || P.display == EGL_NO_DISPLAY) return -1;
    if (o->display_w < 1 || o->display_h < 1 || o->display_w > 4096 || o->display_h > 4096) return -1;
    int id = -1;
    for (int i = 1; i < RMP_WINDOWS_MAX && id < 0; i++)
        if (!P.win[i].used) id = i;
    if (id < 0) return -1;
    rmp_platform_options names = P.options;
    if (o->title) names.title = o->title;
    names.window_w = o->window_w;
    names.window_h = o->window_h;
    names.positioned = o->positioned;
    names.x = o->x;
    names.y = o->y;
    names.place_apart = 0;
    names.close_key = -1;   /* rmp_window_take_close */
    names.map_unfocused = 1;   /* the focus moves only with rmp_window_raise */
    RmpX11 *x = rmp_x11_open(o->display_w, o->display_h, 0, 0, 0, &names);
    if (!x) return -1;
    CreatePlatformWindowSurfaceFn create =
        (CreatePlatformWindowSurfaceFn)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    EGLSurface surface = create ? create(P.display, P.config, rmp_x11_window(x), NULL) : EGL_NO_SURFACE;
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "EGL: no surface for another window (0x%x)\n", (unsigned)eglGetError());
        rmp_x11_close(x);
        return -1;
    }
    Window *w = &P.win[id];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->interval = 2;
    w->x11 = x;
    w->surface = surface;
    w->width = o->display_w;
    w->height = o->display_h;
    rmp_damage_init(&w->damage, w->width, w->height);
    rmp_damage_add_full(&w->damage);
    const int prev = P.cur;
    int ok = rmp_window_select(id) == 0;
    if (ok) {
        (void)eglSwapInterval(P.display, 1);
        int ow, oh;
        rmp_x11_size(P.x11, &ow, &oh);
        int scaled = rmp_gfx_set_display(P.width, P.height, 0) < 0 ? -1 : rmp_gfx_set_output(ow, oh);
        ok = scaled >= 0;
        if (ok) P.scaled = scaled;
    }
    if (!ok) {
        fprintf(stderr, "x11: cannot set up window %d\n", id);
        rmp_window_close(id);
        if (P.win[prev].used) rmp_window_select(prev);
        return -1;
    }
    rmp_window_select(prev);
    return id;
}

void rmp_window_close(int i)
{
    if (!valid_window(i) || i == 0 || P.frame_state == FRAME_ACTIVE) return;
    if (P.cur == i && rmp_window_select(0) < 0) return;
    /* Its scaled texture: drop it while its slot is selected in rmp_gfx. */
    rmp_gfx_select_output(i);
    rmp_gfx_release_output();
    rmp_gfx_select_output(0);
    rmp_gfx_invalidate_state();
    Window *w = &P.win[i];
    if (w->surface != EGL_NO_SURFACE) eglDestroySurface(P.display, w->surface);
    rmp_x11_close(w->x11);
    memset(w, 0, sizeof *w);
    apply_interval();
}

void rmp_window_show(int i, int show)
{
    if (!valid_window(i) || !P.x11) return;
    show = show != 0;
    if (show == P.win[i].shown) return;
    P.win[i].shown = show;
    P.win[i].have_target = 0;
    if (show) {
        rmp_x11_show(win_x11(i));
        rmp_damage_add_full(win_damage(i));
    } else {
        rmp_x11_hide(win_x11(i));
    }
    apply_interval();
}

int rmp_window_shown(int i) { return valid_window(i) && P.win[i].shown; }

void rmp_window_raise(int i)
{
    if (!valid_window(i) || !P.x11) return;
    if (!P.win[i].shown) {
        P.win[i].shown = 1;
        P.win[i].have_target = 0;
        rmp_damage_add_full(win_damage(i));
        apply_interval();
    }
    rmp_x11_activate(win_x11(i));
}

int rmp_window_take_close(int i) { return valid_window(i) && P.x11 ? rmp_x11_take_close(win_x11(i)) : 0; }
