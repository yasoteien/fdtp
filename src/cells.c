// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "cells.h"
#include "font24.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const uint32_t fd_palette[9] = {0x000055,0x00ffff,0xffffff,0xffff00,0xff00ff,
                                0x000000,0x0000ff,0xff0000,0x00ff00};
int fd_cols = 80, fd_rows = 25;
void fd_set_size(int cols, int rows)
{
    fd_cols = cols < 80 ? 80 : cols > FD_COLS_MAX ? FD_COLS_MAX : cols;
    fd_rows = rows < 25 ? 25 : rows > FD_ROWS_MAX ? FD_ROWS_MAX : rows;
}
static void assign(Cells *c, int x, int y, Cell v)
{
    Cell *old = &c->data[y][x];
    if (old->code != v.code || old->fg != v.fg || old->bg != v.bg || old->part != v.part) {
        *old = v;
        c->dirty[y][x] = 1;
    }
}
void cells_invalidate(Cells *c) { memset(c->dirty, 1, sizeof(c->dirty)); c->lines_dirty = 1; }
/* Only the grid in use is cleared and copied (the storage is for 320 x 90):
 * on the Pomera a draw moves the 80 x 25 it always did. */
static void clear_fields(Cells *c)
{
    c->title_start = c->title_end = c->date_start = c->date_end = 0;
    c->lines_dirty = 0;
    c->frame = 1;
    memset(c->separators, 0, sizeof(c->separators));
    c->box_count = 0;
    memset(c->boxes, 0, sizeof(c->boxes));
    c->line_count = 0;
    memset(c->lines, 0, sizeof(c->lines));
}
void cells_init(Cells *c)
{
    clear_fields(c);
    for (int y = 0; y < FD_ROWS; ++y) memset(c->data[y], 0, sizeof(Cell) * (size_t)FD_COLS);
    cells_clear(c);
    cells_invalidate(c);
}
void cells_copy(Cells *dst, const Cells *src)
{
    for (int y = 0; y < FD_ROWS; ++y) {
        memcpy(dst->data[y], src->data[y], sizeof(Cell) * (size_t)FD_COLS);
        memcpy(dst->dirty[y], src->dirty[y], (size_t)FD_COLS);
    }
    dst->title_start = src->title_start; dst->title_end = src->title_end;
    dst->date_start = src->date_start; dst->date_end = src->date_end;
    dst->lines_dirty = src->lines_dirty;
    dst->frame = src->frame;
    memcpy(dst->separators, src->separators, sizeof(dst->separators));
    dst->box_count = src->box_count;
    memcpy(dst->boxes, src->boxes, sizeof(dst->boxes));
    dst->line_count = src->line_count;
    memcpy(dst->lines, src->lines, sizeof(dst->lines));
}
void cells_clear(Cells *c)
{
    Cell v = {' ', WHITE, BG, 0};
    for (int y = 0; y < FD_ROWS; ++y)
        for (int x = 0; x < FD_COLS; ++x) assign(c, x, y, v);
}
/* Clear both halves when replacing either half of a wide character. */
static void erase_wide(Cells *c, int x, int y)
{
    Cell v = {' ', WHITE, BG, 0};
    int part = c->data[y][x].part;
    if (part == 1 && x + 1 < FD_COLS) assign(c, x + 1, y, v);
    if (part == 2 && x > 0) assign(c, x - 1, y, v);
}
void cells_put(Cells *c, int col, int row, uint32_t code, int fg, int bg)
{
    int w = font24_width(code);
    if (row < 0 || row >= FD_ROWS || col < 0 || col + w > FD_COLS) return;
    erase_wide(c, col, row);
    if (w == 2) erase_wide(c, col + 1, row);
    Cell v = {code, (unsigned char)fg, (unsigned char)bg, w == 2 ? 1 : 0};
    assign(c, col, row, v);
    if (w == 2) { v.part = 2; assign(c, col + 1, row, v); }
}
int cells_text(Cells *c, int col, int row, const char *s, int fg, int bg, int limit)
{
    int n = 0;
    while (*s) {
        uint32_t code = font24_decode(&s);
        int w = font24_width(code);
        if (n + w > limit || col + n + w > FD_COLS) break;
        cells_put(c, col + n, row, code, fg, bg);
        n += w;
    }
    return n;
}
const char *cells_home_path(const char *path, char *buf, size_t size)
{
    const char *home = getenv("HOME");
    size_t home_len = home ? strlen(home) : 0;
    while (home_len > 1 && home[home_len-1] == '/') --home_len;
    if (path && home_len > 1 && strncmp(path,home,home_len) == 0 &&
        (path[home_len] == 0 || path[home_len] == '/')) {
        snprintf(buf,size,"~%s",path+home_len);
        return buf;
    }
    return path ? path : "";
}
const char *cells_tail(const char *s, int max)
{
    while (*s && cells_width(s) > max) font24_decode(&s);
    return s;
}
int cells_path(Cells *c, int col, int row, const char *path, int fg, int bg, int limit)
{
    char buf[4096];
    path = cells_home_path(path,buf,sizeof(buf));
    if (cells_width(path) <= limit || limit < 3) return cells_text(c,col,row,path,fg,bg,limit);
    int n = cells_text(c,col,row,"…",fg,bg,2);
    return n + cells_text(c,col+2,row,cells_tail(path,limit-2),fg,bg,limit-2);
}
int cells_width(const char *s)
{
    int n = 0;
    while (*s) n += font24_width(font24_decode(&s));
    return n;
}
int cells_box(Cells *c, int col, int row, int w, int h, int kind)
{
    if (c->box_count >= CELLS_BOXES) return -1;
    if (col < 0) { w += col; col = 0; }
    if (row < 0) { h += row; row = 0; }
    if (col + w > FD_COLS) w = FD_COLS - col;
    if (row + h > FD_ROWS) h = FD_ROWS - row;
    if (w < 2 || h < 2) return -1;
    for (int y = row; y < row + h; ++y)
        for (int x = col; x < col + w; ++x) cells_put(c, x, y, ' ', WHITE, BG);
    CellBox *b = &c->boxes[c->box_count];
    memset(b, 0, sizeof(*b));   /* padding too: cells_same_boxes compares bytes */
    b->col = (short)col; b->row = (short)row; b->w = (short)w; b->h = (short)h; b->kind = (unsigned char)kind;
    return c->box_count++;
}
static int inside(const CellBox *b, int col, int row)
{
    return col >= b->col && col < b->col + b->w && row >= b->row && row < b->row + b->h;
}
int cells_box_at(const Cells *c, int col, int row)
{
    for (int i = c->box_count - 1; i >= 0; --i) if (inside(&c->boxes[i], col, row)) return i;
    return -1;
}
int cells_shadowed(const Cells *c, int col, int row)
{
    int top = cells_box_at(c, col, row);
    for (int i = c->box_count - 1; i > top; --i) {
        const CellBox *b = &c->boxes[i];
        if (!(b->kind & CELL_BOX_SHADOW)) continue;
        if ((col == b->col + b->w && row > b->row && row <= b->row + b->h) ||
            (row == b->row + b->h && col > b->col && col <= b->col + b->w)) return 1;
    }
    return 0;
}
void cells_band_item(Cells *c, int x80, int w80, const char *label, int hot)
{
    int x = x80 * FD_COLS / 80, w = w80 * FD_COLS / 80, lw = cells_width(label);
    int pad = w > lw ? (w - lw) / 2 : 0;
    for (int i = 0; i < w; ++i) cells_put(c, x + i, FD_BAND, ' ', BG, CYAN);
    cells_text(c, x + pad, FD_BAND, label, BG, CYAN, w - pad);
    if (hot >= 0) cells_put(c, x + pad + hot, FD_BAND, (unsigned char)label[hot], BG, YELLOW);
}
void cells_separator_text(const Cells *c, char *out, size_t size)
{
    uint32_t w[CELLS_SEPARATOR_WORDS];
    memcpy(w, c->separators, sizeof(w));
    char digits[16 * CELLS_SEPARATOR_WORDS];
    size_t n = 0;
    for (;;) {   /* divide the little-endian words by 10 until they are 0 */
        uint64_t rest = 0;
        int nonzero = 0;
        for (int i = CELLS_SEPARATOR_WORDS - 1; i >= 0; --i) {
            uint64_t v = (rest << 32) | w[i];
            w[i] = (uint32_t)(v / 10);
            rest = v % 10;
            nonzero |= w[i] != 0;
        }
        digits[n++] = (char)('0' + rest);
        if (!nonzero) break;
    }
    size_t k = 0;
    while (n && k + 1 < size) out[k++] = digits[--n];
    if (size) out[k] = 0;
}
int cells_same_boxes(const Cells *a, const Cells *b)
{
    if (a->box_count != b->box_count || a->line_count != b->line_count) return 0;
    if (a->line_count && memcmp(a->lines, b->lines, sizeof(a->lines[0]) * (size_t)a->line_count)) return 0;
    return !a->box_count || !memcmp(a->boxes, b->boxes, sizeof(a->boxes[0]) * (size_t)a->box_count);
}
int cells_box_line(Cells *c, int vertical, int at, int from, int to)
{
    if (!c->box_count || c->line_count >= CELLS_LINES || to < from) return -1;
    CellLine *l = &c->lines[c->line_count];
    memset(l, 0, sizeof(*l));
    l->at = (short)at; l->from = (short)from; l->to = (short)to;
    l->vertical = (unsigned char)(vertical != 0); l->box = (signed char)(c->box_count - 1);
    return c->line_count++;
}

/* ---- the frame of every full screen (cells.h) ---- */
static CellBox *frame_box(Cells *c)
{
    return c->box_count && c->boxes[0].kind == CELL_BOX_FRAME ? &c->boxes[0] : NULL;
}
int frame_open(Cells *c)
{
    c->frame = 0;
    cells_no_separators(c);
    if (c->box_count) return -1;
    return cells_box(c, 0, 0, FD_COLS, FRAME_BOTTOM + 1, CELL_BOX_FRAME);
}
int frame_title(Cells *c, int col, const char *title, int limit, const char *extra, int extra_fg)
{
    CellBox *b = frame_box(c);
    int room = FRAME_RIGHT - 2 - col;
    if (limit > room) limit = room;
    if (!b || limit <= 0) return col;
    int w = cells_text(c, col, 0, title, YELLOW, BG, limit);
    if (extra && *extra && w + 1 < room) {
        cells_put(c, col + w, 0, ' ', WHITE, BG);
        w += 1 + cells_text(c, col + w + 1, 0, extra, extra_fg, BG, room - w - 1);
    }
    b->top_from = (short)(col - 1); b->top_to = (short)(col + w + 1);
    return col + w;
}
int frame_status(Cells *c, int end, const char *text, int limit)
{
    CellBox *b = frame_box(c);
    if (!b || !text || !*text) return end;
    if (end > FRAME_STATUS_END) end = FRAME_STATUS_END;
    if (limit > end - 2) limit = end - 2;
    /* Cut on the left (the end of the status, where the screen is, stays). */
    while (*text && cells_width(text) > limit) { ++text; while ((*text & 0xc0) == 0x80) ++text; }
    int w = cells_width(text);
    if (w <= 0) return end;
    int x = end - w;
    cells_text(c, x, FRAME_BOTTOM, text, CYAN, BG, w);
    b->bottom_from = (short)(x - 1); b->bottom_to = (short)(end + 1);
    return x;
}
int frame_hline(Cells *c, int row)
{
    if (!frame_box(c) || row <= 0 || row >= FRAME_BOTTOM) return -1;
    int box = c->box_count;   /* cells_box_line adds to the last box: frame lines belong to box 0 */
    c->box_count = 1;
    int r = cells_box_line(c, 0, row, 0, FRAME_RIGHT);
    c->box_count = box;
    return r;
}
int frame_vline(Cells *c, int col, int from, int to)
{
    if (!frame_box(c) || col <= 0 || col >= FRAME_RIGHT) return -1;
    if (from < 0) from = 0;
    if (to > FRAME_BOTTOM) to = FRAME_BOTTOM;
    int box = c->box_count;
    c->box_count = 1;
    int r = cells_box_line(c, 1, col, from, to);
    c->box_count = box;
    return r;
}
void frame_keys(Cells *c, int col, const FrameKey *keys, int n)
{
    int total = 0;
    for (int i = 0; i < n; ++i) total += cells_width(keys[i].key) + cells_width(keys[i].name) + (i ? 2 : 0);
    int gap = col + total <= FD_COLS ? 2 : 1, x = col;
    for (int i = 0; i < n && x < FD_COLS; ++i) {
        if (i) x += gap;
        x += cells_text(c, x, FD_BAND, keys[i].key, BG, YELLOW, FD_COLS - x);
        x += cells_text(c, x, FD_BAND, keys[i].name, BG, CYAN, FD_COLS - x);
    }
}
