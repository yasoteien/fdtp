// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_WIDGETS_H
#define FDTP_WIDGETS_H
#include "cells.h"
#include "keys.h"
#define WIDGET_CAP 4096
#define WIDGET_HISTORY 32
#define WIDGET_OPTIONS 16
/* Rows of an information box; more than fit on the screen scroll (item 25). */
#define WIDGET_INFO_LINES 256
/* Kept per history kind, newest first (revision 1 §12). */
#define WIDGET_HISTORY_KEEP 20
/* WIDGET_INFO and WIDGET_ERROR are boxes closed by any key; App consumes that
 * key (app_key), so the screen below does not see it. */
typedef enum { WIDGET_NONE, WIDGET_NOTICE, WIDGET_CONFIRM, WIDGET_INPUT, WIDGET_CHOICE,
               WIDGET_INFO, WIDGET_ERROR, WIDGET_HELP } WidgetKind;
/* One row of a help box (widgets_help): key NULL is a section heading
 * (text), key "" continues the item above (text only). */
typedef struct { const char *key, *text; } WidgetHelpItem;
/* What an input asks for; inputs of one kind share a remembered list.
 * Names in the history file: destination path command search name. */
typedef enum { WIDGET_HISTORY_NONE, WIDGET_HISTORY_DESTINATION, WIDGET_HISTORY_PATH,
               WIDGET_HISTORY_COMMAND, WIDGET_HISTORY_SEARCH, WIDGET_HISTORY_NAME,
               WIDGET_HISTORY_KINDS } WidgetHistoryKind;
typedef struct {
    WidgetKind kind;
    char label[256], text[WIDGET_CAP], directory[WIDGET_CAP];
    size_t cursor, scroll;
    char history[WIDGET_HISTORY][WIDGET_CAP], draft[WIDGET_CAP];
    int history_count, history_index;
    const char *options[WIDGET_OPTIONS];
    int option_count, selected;
    /* Input with a history kind: the pull-down list under the field. */
    WidgetHistoryKind history_kind;
    int list_open, list_selected, list_top, edited;
    /* Revision 1 §2: F8 in an input of the destination or path kind calls
     * tree_hook(tree_context) (widgets_tree_hook); NULL: F8 does nothing. */
    void (*tree_hook)(void *context);
    void *tree_context;
    /* Information box; info_top is the first row shown when it scrolls. */
    int info_count, info_top;
    /* Help box (widgets_help): the caller's items (not copied). */
    const WidgetHelpItem *help;
    int help_count, help_top;
    char info_name[WIDGET_INFO_LINES][64], info_value[WIDGET_INFO_LINES][512];
} Widgets;
typedef enum { WIDGET_PASS, WIDGET_HANDLED, WIDGET_ACCEPT, WIDGET_CANCEL } WidgetResult;
void widgets_init(Widgets *w);
/* A short notice on row 23 that the next key clears (the key still works).
 * A message that reads as an error (…ません, 失敗, 違います, 長すぎ, …, but
 * not 見つかりません or 〜はありません) becomes widgets_error instead. */
void widgets_notice(Widgets *w, const char *message);
/* An error box with 「何かキーを押してください」; any key closes it. */
void widgets_error(Widgets *w, const char *message);
void widgets_confirm(Widgets *w, const char *message);
void widgets_input(Widgets *w, const char *label, const char *initial, const char *directory);
/* widgets_input with a remembered list of earlier answers of that kind
 * (revision 1 §12). ↓, ↑ or Alt+↓ opens the list under the field, showing the
 * entries that begin with what was typed (all of them before any typing);
 * ↑↓ PgUp PgDn choose, RET puts the entry in the field (RET again accepts),
 * Esc or Alt+↓ closes the list. An accepted non-empty answer goes to the head
 * of its kind (an equal older entry is dropped, at most 20 kept) and the
 * history file is written. kind WIDGET_HISTORY_NONE is widgets_input. */
void widgets_input_history(Widgets *w, const char *label, const char *initial, const char *directory,
                           WidgetHistoryKind kind);
/* A box: label (wrapped, up to 3 rows), then the options side by side when
 * they fit in 66 columns, otherwise one per row. ←→ or ↑↓ move, an initial
 * letter jumps, RET picks. An option with an ASCII mnemonic "(T)" is picked
 * at once by that letter (either case), like 卓駆★'s buttons. */
void widgets_choice(Widgets *w, const char *label, const char *const *options, int count);
/* An information box: title, a rule, then 「名前: 値」 rows (names aligned,
 * long values wrapped). Strings are copied; at most WIDGET_INFO_LINES rows,
 * names up to 63 bytes and values up to 511. Any key closes it, except that
 * a box with more rows than fit (FD_ROWS - 7) scrolls (item 25): ↑↓ one
 * row, PgUp PgDn Space a box's height, Home End to the ends; the bottom line
 * of the frame shows 「↑↓ 先頭-最後/全部」. */
void widgets_info(Widgets *w, const char *title, const char *const *names, const char *const *values, int count);
/* A help box (revision 1 item 25): title in the top line of the frame;
 * sections (a yellow heading in a line) with the keys (CYAN, left aligned
 * in one column) and their descriptions (WHITE) aligned. As large as the
 * screen allows, in the middle; on a wide screen the sections go into two
 * or more columns side by side. ↑↓ scroll one row, PgUp PgDn Space a box's
 * height, Home End to the ends; Esc q h ? F1 RET close it. The bottom line
 * shows where the view is (「↑↓ PgUp PgDn  1-20/63  Esc: 戻る」). items stay
 * the caller's (static). */
void widgets_help(Widgets *w, const char *title, const WidgetHelpItem *items, int count);
/* The F8 hook of destination/path inputs (set by fileops_tree_install). The
 * hook may hide the input (kind NONE) and open it again later. */
void widgets_tree_hook(Widgets *w, void (*hook)(void *context), void *context);
/* Notice dismisses and returns PASS. Confirm ACCEPT only for y; n/Esc CANCEL.
 * Input/choice accepted result stays in text/selected, kind becomes NONE.
 * Info/error: any key closes them, HANDLED (an info box that scrolls keeps
 * the scrolling keys). Help: HANDLED; the closing keys close it. */
WidgetResult widgets_key(Widgets *w, const Key *k);
void widgets_draw(Widgets *w, Cells *cells);
/* 1 while a notice or an error box shows; the message is in label. */
int widgets_message(const Widgets *w);
/* 1 when widgets_draw writes row 23 (a plain notice), for pixel screens that
 * copy that row; boxes are drawn with raster_boxes. */
int widgets_row23(const Widgets *w);
/* Where the input box's field text goes (row 11, columns 8 to 72 at 80 x 25;
 * in the middle of a larger screen): the row, the first column and the
 * column after the last. */
void widgets_input_field(int *row, int *start, int *end);
/* The history file. Without a call, the first use opens the default:
 * $FDTP_HISTORY when set (empty: kept in memory only), otherwise
 * $HOME/.fdtp/history — unless widgets_history_home(0) was called (the
 * headless test runner), then memory only. path NULL: that default.
 * Returns 0, or -1 when an existing file could not be read. */
int widgets_history_open(const char *path);
void widgets_history_home(int allowed);
int widgets_history_count(WidgetHistoryKind kind);
const char *widgets_history_entry(WidgetHistoryKind kind, int index);
/* Frees the remembered lists (the next use opens the file again). */
void widgets_history_close(void);
#endif
