// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_APP_H
#define FDTP_APP_H
#include "screen.h"
#include "widgets.h"
#include "commands.h"
#define APP_SCREENS 16
#define APP_EXTERNAL_ARGS 72
enum { APP_EXTERNAL_PAUSE=1, APP_EXTERNAL_TERMINAL=2 };
typedef struct { char *argv[APP_EXTERNAL_ARGS]; char *cwd; unsigned flags; int pending; } ExternalRequest;
/* The ~/.fdtprc variables (stage 1c, config.c). Empty strings: not set.
 * sorttype uses FDclone's numbers: 0 none, 1 name, 2 extension, 3 size,
 * 4 date, 5 length, +100 descending. displaymode is the column count.
 * Filers take them when pushed (filer_apply_settings); editor is tried
 * before $VISUAL/$EDITOR; shell is exported as $SHELL. pager is unused. */
typedef struct {
    char editor[256], pager[256], shell[256];
    int sorttype, displaymode, dotfile, symlink, filetype;
    /* The novel editor (revision 1 4.3, 4.1): NOVEL_WORD_RUBY, and the
     * NOVEL_FORMAT_<NAME> switches in novel_format.h's order (default on). */
    int novel_word_ruby;
    signed char novel_format[16];
    /* SCREENSIZE (WIDE_NOTES.md): 0 x 0 = auto, else columns x rows. */
    int screen_cols, screen_rows;
    /* AWAKE_MINUTES (revision 1 item 28): minutes after the last key during
     * which the display is kept awake (fdtp_main.c); 0: never. */
    int awake_minutes;
    /* SPLIT_SIZE (item 37): Alt+n's default piece size in KB ("10M" = 10240). */
    int split_kb;
    /* UNDO_LEVELS (item 38): Ctrl+z / Ctrl+y steps in the docx and xlsx views. */
    int undo_levels;
    /* LANGUAGE: auto / ja / en (resolved by config.c). */
    char language[5];
} AppSettings;
#define APP_AWAKE_DEFAULT 15
#define APP_SPLIT_DEFAULT_KB (10 * 1024)
/* 1 KB .. 1023 MB: no GB pieces (item 37). */
#define APP_SPLIT_MAX_KB (1023 * 1024)
#define APP_UNDO_DEFAULT 100
/* Item 28: the keep-awake repaints come this often (the Pomera's GPU sleeps
 * after 0.2 s, its vblank interrupt after a few seconds without use). */
#define APP_AWAKE_PERIOD 0.15
struct ConfigState;
struct App {
    Screen *screens[APP_SCREENS];
    int depth, quit, changed, last_pixel;
    int mods; /* modifier keys held now (MOD_SHIFT ...), from app_modifiers */
    double now;
    Widgets widgets;
    char deferred_notice[256];
    int deferred_error; /* the waiting message came from app_error */
    CommandTable commands;
    ExternalRequest external;
    AppSettings settings;
    struct ConfigState *config; /* owned by config.c */
    /* Installed only while config's first-run picker is visible. Keeping
     * these optional lets isolated viewer harnesses omit config.c. */
    int (*first_run_key)(App *, const Key *);
    void (*first_run_draw)(App *, Cells *);
    /* For the system information (revision 1 item 31), from the platform
     * adapter (fdtp_main.c): the output ("KMS", "X"; NULL headless) and the
     * time (as now) of the last key, 0 when not known. */
    const char *output;
    double last_key;
    /* Optional window control; NULL in the preview/headless adapter. */
    int (*is_fullscreen)(void);
    void (*set_fullscreen)(int on);
};
void app_init(App *app);
/* The screen size to use (WIDE_NOTES.md): cols x rows from the command line
 * (-1: not given, then SCREENSIZE; 0 x 0: auto) on an output of out_w x
 * out_h pixels (0 x 0: not known). */
ScreenSize app_screen_choice(const App *app, int cols, int rows, int out_w, int out_h);
/* 1 when s is not the size in use (fd_cols ... canvas_h). */
int app_screen_differs(ScreenSize s);
void app_destroy(App *app);
/* Item 28: 1 when a keep-awake repaint is due at now: minutes > 0, now
 * before last_key + minutes, and APP_AWAKE_PERIOD past last_poke. *wait
 * (may be NULL) gets the seconds until the next one is due, -1 when none
 * will be before another key. Times in seconds of one clock. */
int app_awake_due(int minutes, double last_key, double last_poke, double now, double *wait);
/* On failure caller retains ownership; on success app owns Screen. */
int app_push(App *app, Screen *screen);
void app_pop(App *app);
Screen *app_top(App *app);
void app_key(App *app, const Key *key);
void app_tick(App *app, double now);
/* The platform reports the modifiers held after each press or release (the
 * filer's band shows FD's Shift band while Shift is down). */
void app_modifiers(App *app, int mods);
void app_draw(App *app, Canvas *canvas, Cells *cells);
/* Row 23, or an error box when the message reads as an error (widgets.h).
 * While another widget is open the message waits until it closes. */
void app_notice(App *app, const char *message);
/* Always the error box (「何かキーを押してください」). */
void app_error(App *app, const char *message);
/* argv/cwd deep copied. A single pending request, returns -1 on error. */
int app_request_external(App *app, const char *const *argv, unsigned flags);
int app_request_external_at(App *app, const char *const *argv, unsigned flags, const char *cwd);
void app_external_clear(App *app);
void app_external_done(App *app, int status);
#endif
