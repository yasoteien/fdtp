// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_KEYS_H
#define FDTP_KEYS_H
#include <stddef.h>
#include <stdint.h>
enum { KCHAR, KUP, KDOWN, KLEFT, KRIGHT, KPGUP, KPGDN, KHOME, KEND,
       KINS, KDEL, KBS, KRET, KTAB, KESC, KF1, KF2, KF3, KF4, KF5, KF6,
       KF7, KF8, KF9, KF10, KF11, KF12 };
enum { MOD_SHIFT=1, MOD_CTRL=2, MOD_ALT=4 };
typedef struct { int key; uint32_t ch; int mods; } Key;
/* evdev numeric codes, independent of Linux headers; Caps Lock ignored. */
/* Conversion/parsing return 1 on success, 0 on invalid/ignored key. */
int keys_evdev(int code, int mods, Key *out);
int keys_parse(const char *token, Key *out);
int keys_equal(const Key *a, const Key *b);
/* SCRIPT_MODS: HOLD-SHIFT / RELEASE-SHIFT (Shift pressed or released alone,
 * mods = the modifiers held afterwards). */
/* SCRIPT_REALWAIT: REALWAIT:seconds, real time with ticks for child jobs. */
/* SCRIPT_CLICK: CLICK:COL,ROW or CLICK:COL,ROW,CMD / SHIFT / POINT, a mouse
 * click on the filer's cell (filer_mouse; the macOS window's). width and
 * height carry the cell, mods the FILER_* action. */
/* SCRIPT_DISPLAY: DISPLAY:WxH, the headless run's screen becomes W x H
 * pixels (as an X window resized; WIDE_NOTES.md). */
typedef enum { SCRIPT_KEY, SCRIPT_SNAP, SCRIPT_WAIT, SCRIPT_END, SCRIPT_ERROR, SCRIPT_MODS, SCRIPT_DISPLAY, SCRIPT_REALWAIT, SCRIPT_CLICK } ScriptKind;
typedef struct { const char *text; size_t pos; int quoted; char error[128]; int mods; } KeyScript;
typedef struct { ScriptKind kind; Key key; double seconds; int mods; int width, height; } ScriptEvent;
void keys_script_init(KeyScript *s, const char *text);
ScriptEvent keys_script_next(KeyScript *s);
#endif
