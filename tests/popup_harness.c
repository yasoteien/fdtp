// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Revision 1 §7 and §12 (tests/check_popup.py builds this with widgets, cells,
 * raster, canvas, font24 and keys only). Writes PPM and cells-free checks;
 * argv[1] is a scratch directory under build/. */
#define _POSIX_C_SOURCE 200809L
#include "widgets.h"
#include "raster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static char dir[2048];
static const char *in_dir(const char *name)
{
    static char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return path;
}
static void press(Widgets *w, const char *token)
{
    Key k;
    assert(keys_parse(token, &k));
    widgets_key(w, &k);
}
static void type(Widgets *w, const char *s) { for (; *s; ++s) { char t[2] = {*s, 0}; press(w, t); } }
static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    assert(!fclose(f));
}
static char *read_file(const char *path)
{
    static char buffer[1 << 16];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[n] = 0;
    fclose(f);
    return buffer;
}
static const unsigned char *px(const Canvas *c, int gx, int gy) { return CANVAS_PX(c, CANVAS_GRID_X + gx, gy); }
static int is_rgb(const Canvas *c, int gx, int gy, unsigned rgb)
{
    const unsigned char *p = px(c, gx, gy);
    return p[0] == (rgb >> 16 & 255) && p[1] == (rgb >> 8 & 255) && p[2] == (rgb & 255);
}
/* History file: parsing boundaries, escapes, the 20-entry limit, dedupe. */
static void history_tests(void)
{
    char longline[5000];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = 0;
    char text[8192];
    snprintf(text, sizeof(text),
             "# comment line\n"
             "\n"
             "no tab here\n"
             "unknown\tskipped\n"
             "path\t\n"                      /* empty text: skipped */
             "path\tbad\\qescape\n"          /* invalid escape: skipped */
             "path\ttab\\there\r\n"          /* CRLF and an escaped tab */
             "path\tback\\\\slash\n"
             "path\ttab\\there\n"            /* duplicate: the first stays */
             "destination\t/d/one\n"
             "path\t%s\n"                    /* longer than WIDGET_CAP: skipped */
             "command\tnl\\nin\n",
             longline);
    write_file(in_dir("parse"), text);
    assert(widgets_history_open(in_dir("parse")) == 0);
    assert(widgets_history_count(WIDGET_HISTORY_PATH) == 2);
    assert(!strcmp(widgets_history_entry(WIDGET_HISTORY_PATH, 0), "tab\there"));
    assert(!strcmp(widgets_history_entry(WIDGET_HISTORY_PATH, 1), "back\\slash"));
    assert(widgets_history_count(WIDGET_HISTORY_DESTINATION) == 1);
    assert(!strcmp(widgets_history_entry(WIDGET_HISTORY_COMMAND, 0), "nl\nin"));
    assert(!widgets_history_entry(WIDGET_HISTORY_PATH, 2) && !widgets_history_entry(WIDGET_HISTORY_NONE, 0));
    assert(!widgets_history_entry(WIDGET_HISTORY_PATH, -1) && widgets_history_count(WIDGET_HISTORY_KINDS) == 0);
    /* Accepting saves; escapes round-trip; an equal entry moves to the head. */
    Widgets *w = calloc(1, sizeof(*w));
    assert(w);
    widgets_init(w);
    widgets_input_history(w, "パス", "back\\slash", ".", WIDGET_HISTORY_PATH);
    press(w, "RET");
    assert(w->kind == WIDGET_NONE && widgets_history_count(WIDGET_HISTORY_PATH) == 2);
    assert(!strcmp(widgets_history_entry(WIDGET_HISTORY_PATH, 0), "back\\slash"));
    const char *saved = read_file(in_dir("parse"));
    assert(saved && strstr(saved, "path\tback\\\\slash\npath\ttab\\there\n") && strstr(saved, "command\tnl\\nin\n"));
    assert(!strstr(saved, "unknown") && !strstr(saved, "bad"));
    assert(widgets_history_open(in_dir("parse")) == 0 && widgets_history_count(WIDGET_HISTORY_PATH) == 2 &&
           !strcmp(widgets_history_entry(WIDGET_HISTORY_PATH, 1), "tab\there"));
    /* 21 different answers: the oldest goes, 20 stay, newest first. */
    assert(widgets_history_open(in_dir("limit")) == 0 && widgets_history_count(WIDGET_HISTORY_NAME) == 0);
    for (int i = 0; i < 21; ++i) {
        char answer[16];
        snprintf(answer, sizeof(answer), "n%02d", i);
        widgets_input_history(w, "名前", answer, ".", WIDGET_HISTORY_NAME);
        press(w, "RET");
    }
    assert(widgets_history_count(WIDGET_HISTORY_NAME) == WIDGET_HISTORY_KEEP);
    assert(!strcmp(widgets_history_entry(WIDGET_HISTORY_NAME, 0), "n20") && !strcmp(widgets_history_entry(WIDGET_HISTORY_NAME, 19), "n01"));
    /* Empty answers and cancelled inputs are not remembered. */
    widgets_input_history(w, "名前", "", ".", WIDGET_HISTORY_NAME); press(w, "RET");
    widgets_input_history(w, "名前", "never", ".", WIDGET_HISTORY_NAME); press(w, "ESC");
    assert(widgets_history_count(WIDGET_HISTORY_NAME) == 20 && strcmp(widgets_history_entry(WIDGET_HISTORY_NAME, 0), "never"));
    /* More than 20 of a kind in the file: the first 20 are kept. */
    char many[4096] = "";
    for (int i = 0; i < 25; ++i) { char line[32]; snprintf(line, sizeof(line), "search\ts%02d\n", i); strcat(many, line); }
    write_file(in_dir("many"), many);
    assert(widgets_history_open(in_dir("many")) == 0 && widgets_history_count(WIDGET_HISTORY_SEARCH) == 20 &&
           !strcmp(widgets_history_entry(WIDGET_HISTORY_SEARCH, 19), "s19"));
    /* Memory only: nothing is written. */
    assert(widgets_history_open("") == 0);
    widgets_input_history(w, "パス", "kept", ".", WIDGET_HISTORY_PATH); press(w, "RET");
    assert(widgets_history_count(WIDGET_HISTORY_PATH) == 1);
    /* The default path: $FDTP_HISTORY, else $HOME/.fdtp/history (made). */
    setenv("HOME", in_dir("home"), 1);
    mkdir(in_dir("home"), 0700);
    unsetenv("FDTP_HISTORY");
    widgets_history_home(1);
    assert(widgets_history_open(NULL) == 0);
    widgets_input_history(w, "パス", "/x", ".", WIDGET_HISTORY_PATH); press(w, "RET");
    assert(read_file(in_dir("home/.fdtp/history")) && strstr(read_file(in_dir("home/.fdtp/history")), "path\t/x\n"));
    widgets_history_home(0);
    assert(widgets_history_open(NULL) == 0 && widgets_history_count(WIDGET_HISTORY_PATH) == 0);
    setenv("FDTP_HISTORY", in_dir("env-history"), 1);
    assert(widgets_history_open(NULL) == 0);
    widgets_input_history(w, "パス", "/y", ".", WIDGET_HISTORY_PATH); press(w, "RET");
    assert(read_file(in_dir("env-history")) && strstr(read_file(in_dir("env-history")), "path\t/y\n"));
    /* The pull-down: filter by what was typed, choose, RET puts it in the field. */
    write_file(in_dir("pull"), "destination\t/srv/b\ndestination\t/mnt/a\ndestination\t/srv/a\n");
    assert(widgets_history_open(in_dir("pull")) == 0);
    widgets_input_history(w, "コピー先", "/prefilled", ".", WIDGET_HISTORY_DESTINATION);
    press(w, "DOWN");
    assert(w->list_open && w->list_selected == 0);          /* all three: nothing typed yet */
    press(w, "DOWN"); press(w, "DOWN"); press(w, "DOWN");   /* clamps at the last */
    assert(w->list_selected == 2);
    press(w, "RET");
    assert(!w->list_open && w->kind == WIDGET_INPUT && !strcmp(w->text, "/srv/a"));
    press(w, "C-u"); type(w, "/s"); press(w, "M-DOWN");
    assert(w->list_open);
    press(w, "DOWN"); press(w, "RET");
    assert(!strcmp(w->text, "/srv/a"));                     /* /srv/b, /srv/a match "/s" */
    press(w, "UP");
    assert(w->list_open);                                    /* ↑ opens it too */
    press(w, "ESC");
    assert(!w->list_open && w->kind == WIDGET_INPUT);        /* Esc closes only the list */
    press(w, "C-u"); type(w, "/zzz"); press(w, "DOWN");
    assert(w->list_open);
    press(w, "RET");                                         /* nothing matches: text kept */
    assert(!w->list_open && !strcmp(w->text, "/zzz"));
    press(w, "ESC");
    assert(w->kind == WIDGET_NONE);
    /* Plain inputs keep ↑↓ for the session's earlier answers. */
    widgets_input(w, "入力", "", ".");
    press(w, "DOWN");
    assert(!w->list_open);
    press(w, "ESC");
    widgets_history_close();
    free(w);
}
/* Drawing: frame and shadow pixels, the field, the information box, and the
 * overlay for pixel screens. */
static void draw_tests(void)
{
    Widgets *w = calloc(1, sizeof(*w));
    Canvas *canvas = malloc(sizeof(*canvas));
    Cells *cells = malloc(sizeof(*cells));
    assert(w && canvas && cells);
    widgets_init(w);
    unsigned cyan = 0x00ffff, navy = 0x000055, black = 0, grey = 0x808080, white = 0xffffff;
    /* A confirmation over a screen full of yellow X. */
    cells_init(cells);
    for (int y = 0; y < FD_ROWS; ++y) for (int x = 0; x < FD_COLS; ++x) cells_put(cells, x, y, 'X', YELLOW, BG);
    widgets_confirm(w, "表示ディレクトリをディスクに書き込みますか？");
    widgets_draw(w, cells);
    assert(cells->box_count == 1);
    CellBox b = cells->boxes[0];
    assert(b.h == 3 && b.row == 11 && b.col == (80 - b.w) / 2 && b.kind == (CELL_BOX_FRAME | CELL_BOX_SHADOW));
    canvas_init(canvas);
    FDRect rects[FD_ROWS_MAX];
    raster_update(canvas, cells, rects);
    assert(canvas_write_ppm(canvas, in_dir("confirm.ppm")) == 0);
    int left = b.col * 12 + 5, right = (b.col + b.w - 1) * 12 + 5, top = b.row * 24 + 11, bottom = (b.row + b.h - 1) * 24 + 11;
    int mid_x = (b.col + b.w / 2) * 12 + 3, mid_y = (b.row + 1) * 24 + 12;
    assert(is_rgb(canvas, mid_x, top, cyan) && is_rgb(canvas, mid_x, top + 1, cyan) && !is_rgb(canvas, mid_x, top + 2, cyan));
    assert(is_rgb(canvas, mid_x, bottom, cyan) && is_rgb(canvas, mid_x, bottom + 1, cyan));
    assert(is_rgb(canvas, left, mid_y, cyan) && is_rgb(canvas, left + 1, mid_y, cyan) && is_rgb(canvas, right + 1, mid_y, cyan));
    assert(is_rgb(canvas, left - 1, mid_y, navy) && is_rgb(canvas, left + 2, mid_y, navy));   /* two dots wide */
    assert(!is_rgb(canvas, left, top, cyan) && !is_rgb(canvas, left + 1, top + 1, cyan));     /* the corner is round */
    /* The X under the box are gone; the shadow is black with grey letters. */
    int shadow_x = (b.col + b.w) * 12, shadow_y = (b.row + b.h) * 24;
    int greys = 0, blacks = 0;
    for (int y = shadow_y; y < shadow_y + 24; ++y)
        for (int x = (b.col + 1) * 12; x < (b.col + b.w + 1) * 12; ++x) {
            if (is_rgb(canvas, x, y, black)) ++blacks;
            else if (is_rgb(canvas, x, y, grey)) ++greys;
            else { const unsigned char *q = px(canvas, x, y); assert(q[0] == q[1] && q[1] == q[2] && q[0] < 128); }
        }
    assert(blacks && greys);
    for (int y = (b.row + 1) * 24; y < (b.row + b.h) * 24; ++y)
        for (int x = shadow_x; x < shadow_x + 12; ++x) assert(!is_rgb(canvas, x, y, navy) && !is_rgb(canvas, x, y, 0xffff00));
    assert(is_rgb(canvas, shadow_x, b.row * 24 + 2, navy) || is_rgb(canvas, shadow_x, b.row * 24 + 2, 0xffff00));  /* not on the top row */
    for (int y = b.row * 24; y < (b.row + b.h) * 24; ++y)
        for (int x = (b.col + b.w + 1) * 12; x < (b.col + b.w + 2) * 12; ++x)
            assert(!is_rgb(canvas, x, y, black) && !is_rgb(canvas, x, y, grey));   /* one cell wide */
    /* Unchanged frame: nothing redrawn; closing the box redraws every row. */
    Cells again = *cells;
    assert(raster_update(canvas, &again, rects) == 0);
    /* The input box: white field, navy text, caret reversed. */
    cells_init(cells);
    widgets_input(w, "起動オプションを入力して下さい.", "abc", ".");
    widgets_draw(w, cells);
    assert(cells->box_count == 1 && cells->boxes[0].row == 9 && cells->boxes[0].h == 4 && cells->boxes[0].w == 70);
    for (int x = 7; x < 73; ++x) assert(cells->data[11][x].bg == (x == 11 ? BG : WHITE));
    assert(cells->data[11][8].code == 'a' && cells->data[11][8].fg == BG && cells->data[11][11].fg == WHITE);
    assert(cells->data[10][7].fg == CYAN);
    canvas_init(canvas);
    raster_update(canvas, cells, rects);
    assert(canvas_write_ppm(canvas, in_dir("input.ppm")) == 0);
    assert(is_rgb(canvas, 7 * 12 + 1, 11 * 24 + 1, white) && is_rgb(canvas, 11 * 12 + 1, 11 * 24 + 1, navy));
    /* With the list open the box grows and a rule parts field and list. */
    assert(widgets_history_open("") == 0);
    widgets_input_history(w, "コピー先", "", ".", WIDGET_HISTORY_DESTINATION);
    type(w, "/a"); press(w, "RET");
    widgets_input_history(w, "コピー先", "", ".", WIDGET_HISTORY_DESTINATION);
    type(w, "/b"); press(w, "RET");
    widgets_input_history(w, "コピー先", "", ".", WIDGET_HISTORY_DESTINATION);
    press(w, "DOWN");
    cells_init(cells);
    widgets_draw(w, cells);
    assert(cells->box_count == 1 && cells->boxes[0].h == 7 && cells->boxes[0].rule == 12);
    assert(cells->data[13][8].code == '/' && cells->data[13][9].code == 'b' && cells->data[13][9].bg == CYAN);
    assert(cells->data[14][9].code == 'a' && cells->data[14][9].bg == BG);
    canvas_init(canvas);
    raster_update(canvas, cells, rects);
    assert(canvas_write_ppm(canvas, in_dir("pulldown.ppm")) == 0);
    assert(is_rgb(canvas, 40 * 12, 12 * 24 + 11, cyan) && is_rgb(canvas, 40 * 12, 12 * 24 + 12, cyan));   /* the rule */
    assert(is_rgb(canvas, 5 * 12 + 5, 12 * 24 + 11, cyan));                                             /* joins the frame */
    widgets_history_close();
    press(w, "ESC"); press(w, "ESC");
    /* The information box: title, rule, aligned names, wrapped values. */
    const char *names[] = {"場所", "種類", "サイズ", "更新日時"};
    char long_value[300];
    memset(long_value, 'p', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = 0;
    const char *values[] = {long_value, "テキスト（.txt）", "1,234 バイト（1.2 KB）", "2026-10-22 21:30:00"};
    widgets_info(w, "ファイルの情報", names, values, 4);
    assert(w->kind == WIDGET_INFO && w->info_count == 4);
    cells_init(cells);
    widgets_draw(w, cells);
    b = cells->boxes[0];
    assert(cells->box_count == 1 && b.w == 70 && b.rule == b.row + 2);
    /* 299 p wrapped at 66-8-2=56 columns: six rows, then one row each. */
    assert(b.h == 4 + 6 + 3);
    assert(cells->data[b.row + 1][b.col + 2].fg == YELLOW);
    assert(cells->data[b.row + 3][b.col + 2].fg == CYAN && cells->data[b.row + 3][b.col + 2 + 8].code == ':');
    assert(cells->data[b.row + 3][b.col + 2 + 10].code == 'p' && cells->data[b.row + 3][b.col + 2 + 10].fg == WHITE);
    assert(cells->data[b.row + 4][b.col + 2].code == ' ' && cells->data[b.row + 4][b.col + 12].code == 'p');
    assert(cells->data[b.row + 9][b.col + 2 + 8].code == ':' && cells->data[b.row + 11][b.col + 2].code == 0x66f4);
    canvas_init(canvas);
    raster_update(canvas, cells, rects);
    assert(canvas_write_ppm(canvas, in_dir("info.ppm")) == 0);
    assert(is_rgb(canvas, 40 * 12, (b.row + 2) * 24 + 11, cyan));
    press(w, "x");
    assert(w->kind == WIDGET_NONE);
    /* Choice: side by side, or one per row when they do not fit. */
    const char *few[] = {"名前", "拡張子", "サイズ"};
    widgets_choice(w, "並べ替え", few, 3);
    press(w, "DOWN");
    assert(w->selected == 1);
    cells_init(cells);
    widgets_draw(w, cells);
    assert(cells->box_count == 1 && cells->boxes[0].h == 4);
    const char *many[] = {"/very/long/directory/name/number/one/that/goes/on", "/another/long/directory/name/that/goes/on/too", "/c"};
    widgets_choice(w, "移動履歴", many, 3);
    cells_init(cells);
    widgets_draw(w, cells);
    assert(cells->box_count == 1 && cells->boxes[0].h == 6);
    b = cells->boxes[0];
    assert(cells->data[b.row + 2][b.col + 2].bg == WHITE && cells->data[b.row + 2][b.col + b.w - 3].bg == WHITE);
    assert(cells->data[b.row + 3][b.col + 2].bg == BG);
    canvas_init(canvas);
    raster_update(canvas, cells, rects);
    assert(canvas_write_ppm(canvas, in_dir("choice.ppm")) == 0);
    press(w, "ESC");
    /* An error: message and the fixed hint; any key closes it. */
    widgets_notice(w, "ディレクトリを開けません");
    assert(w->kind == WIDGET_ERROR);
    cells_init(cells);
    widgets_draw(w, cells);
    b = cells->boxes[0];
    assert(b.h == 4 && cells->data[b.row + 2][b.col + 2].code == 0x4f55 && cells->data[b.row + 2][b.col + 2].fg == CYAN);
    canvas_init(canvas);
    raster_update(canvas, cells, rects);
    assert(canvas_write_ppm(canvas, in_dir("error.ppm")) == 0);
    press(w, "DOWN");
    assert(w->kind == WIDGET_NONE);
    /* A plain notice stays on row 23 without a box. */
    widgets_notice(w, "保存しました");
    cells_init(cells);
    widgets_draw(w, cells);
    assert(!cells->box_count && cells->data[23][2].code == 0x4fdd);
    /* Pixel screens: the box over a red picture; its shadow darkens it. */
    for (int y = 0; y < CANVAS_H; ++y)
        for (int x = 0; x < CANVAS_W; ++x) { unsigned char *p = CANVAS_PX(canvas, x, y); p[0] = 200; p[1] = 40; p[2] = 40; p[3] = 255; }
    cells_init(cells);
    widgets_confirm(w, "上書きしますか？");
    widgets_draw(w, cells);
    raster_boxes(canvas, cells);
    assert(canvas_write_ppm(canvas, in_dir("overlay.ppm")) == 0);
    b = cells->boxes[0];
    mid_x = (b.col + b.w / 2) * 12 + 3;
    assert(is_rgb(canvas, mid_x, b.row * 24 + 11, cyan) && is_rgb(canvas, b.col * 12 + 13, (b.row + 1) * 24 + 1, navy));
    assert(is_rgb(canvas, (b.col + b.w) * 12 + 1, (b.row + 1) * 24 + 1, 0x320a0a));     /* 200/4, 40/4 */
    assert(is_rgb(canvas, (b.col + b.w + 1) * 12 + 1, (b.row + 1) * 24 + 1, 0xc82828)); /* beyond the shadow */
    assert(is_rgb(canvas, 2, 2, 0xc82828));
    press(w, "y");
    free(w); free(canvas); free(cells);
}
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: popup-harness SCRATCH_DIR\n"); return 2; }
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    unsetenv("FDTP_HISTORY");
    widgets_history_home(0);
    history_tests();
    draw_tests();
    puts("popup harness: history file, pull-down, boxes, information, overlay passed");
    return 0;
}
