// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
/* Stage 4: tables (csv, tsv, xlsx) on the FD grid. The model lives in
 * sheet_model.h; only the cells on the screen are formatted when drawn. */
#include "sheetview.h"
#include "sheet_model.h"
#include "doc_undo.h"
#include "app.h"
#include "font24.h"
#include "raster.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int sheet_write_file(const char *path, const void *data, size_t n, char *err, size_t errlen);

/* Revision 1 item 29: the screen's frame (cells.h frame_open). Row 1 the
 * column names, rows 2 .. FRAME_BOTTOM - 1 (20 at 25 rows) the data, from
 * column 1 the row numbers, the cells up to column FD_COLS - 2. Row FD_LINE is
 * the cell, FD_BAND the band. The grid's lines run to the frame's lines. */
#define DATA_TOP 2
#define DATA_ROWS (FRAME_BODY_ROWS - 1)
#define LEFT 1
#define LIMIT FRAME_RIGHT   /* the first column the cells may not use */
#define DEFAULT_WIDTH 10
#define MIN_WIDTH 2
#define MAX_WIDTH 60

typedef enum { PROMPT_NONE, PROMPT_EDIT, PROMPT_GOTO, PROMPT_QUIT } Prompt;
typedef struct {
    Screen screen;
    Book book;
    Prompt prompt;
    uint32_t edit_row, edit_col;
    char *edit_initial;
    unsigned char *gutters; /* per-sheet high-water widths, including the gap */
    /* Revision 1 item 38: C-z / C-y, one cell's edit a step (sheet_undo.c).
     * 「変更あり」 is whether the state differs from the one saved. */
    UndoHist undo;
    int saves;
} SheetView;

static int keep_backup = 1;
void sheetview_set_backup(int keep) { keep_backup = keep != 0; }

static Sheet *current(SheetView *v) { return &v->book.sheets[v->book.current]; }
static int any_dirty(const SheetView *v) { return undo_dirty(&v->undo); }
static int width_of(const Sheet *s, uint32_t col)
{
    return s->widths && s->widths[col] ? s->widths[col] : DEFAULT_WIDTH;
}

/* ---- dates ---- */
static long days_from_civil(long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civil_from_days(long z, long *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}
/* Serial number (1900 system with its 29 Feb 1900, or 1904) to text. */
static int date_text(const Book *b, double v, CellFormat f, char *out, size_t n)
{
    if (!(v >= 0) || v > 2958466.0) return 0;
    long days = (long)floor(v);
    long secs = lround((v - (double)days) * 86400.0);
    if (secs >= 86400) { ++days; secs -= 86400; }
    long y; unsigned m, d;
    if (b->date1904) civil_from_days(days_from_civil(1904, 1, 1) + days, &y, &m, &d);
    else if (days == 60) { y = 1900; m = 2; d = 29; }
    else civil_from_days(days_from_civil(1899, 12, 31) + days - (days > 60), &y, &m, &d);
    int hh = (int)(secs / 3600), mm = (int)(secs / 60 % 60), ss = (int)(secs % 60);
    char time[16];
    if (f.seconds) snprintf(time, sizeof(time), "%02d:%02d:%02d", hh, mm, ss);
    else snprintf(time, sizeof(time), "%02d:%02d", hh, mm);
    if (f.kind == FMT_TIME) snprintf(out, n, "%s", time);
    else if (f.kind == FMT_DATETIME) snprintf(out, n, "%04ld/%02u/%02u %s", y, m, d, time);
    else snprintf(out, n, "%04ld/%02u/%02u", y, m, d);
    return 1;
}
/* "2024/1/31", "2024-01-31 9:05", "9:05:30" back to a serial number. */
static int parse_date(const Book *b, const char *s, CellFormat f, double *out)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, n = 0, have_date = 0;
    char sep;
    if (sscanf(s, "%d%c%d%c%d%n", &y, &sep, &mo, &sep, &d, &n) == 5 && (sep == '/' || sep == '-') && y >= 1900 && y <= 9999) {
        have_date = 1;
        s += n;
        while (*s == ' ') ++s;
    } else if (f.kind != FMT_TIME) return 0;
    if (*s) {
        n = 0;
        if (sscanf(s, "%d:%d%n", &h, &mi, &n) < 2) return 0;
        s += n;
        if (*s == ':') {
            n = 0;
            if (sscanf(s + 1, "%d%n", &se, &n) < 1) return 0;
            s += 1 + n;
        }
        if (*s || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) return 0;
    } else if (!have_date) return 0;
    double serial = 0;
    if (have_date) {
        if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
        long days = days_from_civil(y, (unsigned)mo, (unsigned)d);
        long yy; unsigned mm, dd;
        civil_from_days(days, &yy, &mm, &dd);
        if ((int)mm != mo) return 0;
        if (b->date1904) serial = (double)(days - days_from_civil(1904, 1, 1));
        else { long k = days - days_from_civil(1899, 12, 31); serial = (double)(k >= 60 ? k + 1 : k); }
        if (serial < 0) return 0;
    }
    *out = serial + (h * 3600 + mi * 60 + se) / 86400.0;
    return 1;
}

/* ---- formatting ---- */
static void general(double v, int room, char *out, size_t n)
{
    if (room < 1) room = 1;
    if (v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%.0f", v);
        if ((int)strlen(out) <= room) return;
    }
    for (int p = 10; p >= 1; --p) {
        snprintf(out, n, "%.*g", p, v);
        if ((int)strlen(out) <= room) return;
    }
    snprintf(out, n, "#");
}
static int numeric_text(const char *s)
{
    double v;
    while (*s == ' ') ++s;
    return *s && sheet_parse_number(s, &v);
}
/* Text shown in the grid; *right is set for right-aligned values. */
static void display(const Book *b, const Sheet *s, const SheetCell *c, int room, char *out, size_t n, int *right)
{
    *right = 0;
    out[0] = 0;
    switch (c->kind) {
    case SK_NUM: {
        *right = 1;
        CellFormat f = xlsx_format(b, c->style);
        if ((f.kind == FMT_DATE || f.kind == FMT_TIME || f.kind == FMT_DATETIME) && date_text(b, c->num, f, out, n)) return;
        if (f.kind == FMT_FIXED) { snprintf(out, n, "%.*f", f.decimals, c->num); return; }
        if (f.kind == FMT_PERCENT) { snprintf(out, n, "%.*f%%", f.decimals, c->num * 100); return; }
        general(c->num, room, out, n);
        return;
    }
    case SK_BOOL: *right = 1; snprintf(out, n, "%s", c->num ? "TRUE" : "FALSE"); return;
    case SK_ERR: *right = 1; snprintf(out, n, "%s", cell_string(b, s, c)); return;
    case SK_STR: case SK_SST:
        snprintf(out, n, "%s", cell_string(b, s, c));
        if (b->type == BOOK_CSV && numeric_text(out)) *right = 1;
        return;
    default: return;
    }
}
/* Content for row 23 and the editor: the formula, or the value in full. */
static char *content(const Book *b, const Sheet *s, const SheetCell *c)
{
    char tmp[128];
    if (!c) return strdup("");
    if (c->flags & SF_FORMULA) {
        const char *f = arena_str(&s->arena, c->formula);
        size_t n = strlen(f);
        char *p = malloc(n + 2);
        if (p) { p[0] = '='; memcpy(p + 1, f, n + 1); }
        return p;
    }
    switch (c->kind) {
    case SK_NUM: {
        CellFormat f = xlsx_format(b, c->style);
        if (!((f.kind == FMT_DATE || f.kind == FMT_TIME || f.kind == FMT_DATETIME) && date_text(b, c->num, f, tmp, sizeof(tmp))))
            sheet_number_text(c->num, tmp, sizeof(tmp));
        return strdup(tmp);
    }
    case SK_BOOL: return strdup(c->num ? "TRUE" : "FALSE");
    case SK_STR: case SK_SST: case SK_ERR: return strdup(cell_string(b, s, c));
    default: return strdup("");
    }
}
/* Control characters (line breaks in a cell) are shown as spaces. */
static void clean(char *s)
{
    for (; *s; ++s) if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
}

static const SheetRange *merge_at(const Sheet *s, uint32_t row, uint32_t col)
{
    for (size_t i = 0; i < s->nmerges; ++i) {
        const SheetRange *r = &s->merges[i];
        if (row >= r->r1 && row <= r->r2 && col >= r->c1 && col <= r->c2) return r;
    }
    return NULL;
}
static int in_array(const Sheet *s, uint32_t row, uint32_t col)
{
    for (size_t i = 0; i < s->narrays; ++i) {
        const SheetRange *r = &s->arrays[i];
        if (row >= r->r1 && row <= r->r2 && col >= r->c1 && col <= r->c2) return 1;
    }
    return 0;
}

/* ---- drawing ---- */
static int gutter_width(SheetView *v, const Sheet *s)
{
    char tmp[16];
    uint32_t rows = (uint32_t)s->nrows;
    if (s->count && s->cells[s->count - 1].row + 1 > rows)
        rows = s->cells[s->count - 1].row + 1;
    /* Use the whole sheet from the start. Empty rows remain navigable, so
     * visiting farther digits can widen the gutter, but never shrink it. */
    uint32_t visible = s->top_row + DATA_ROWS;
    if (visible > SHEET_MAX_ROWS) visible = SHEET_MAX_ROWS;
    if (rows < visible) rows = visible;
    int n = snprintf(tmp, sizeof(tmp), "%u", rows);
    int width = n + 1 > 5 ? n + 1 : 5;
    unsigned char *held = &v->gutters[v->book.current];
    if (width > *held) *held = (unsigned char)width;
    return *held;
}
static void scroll_into_view(SheetView *v, Sheet *s)
{
    if (s->cur_row < s->top_row) s->top_row = s->cur_row;
    if (s->cur_row >= s->top_row + DATA_ROWS) s->top_row = s->cur_row - DATA_ROWS + 1;
    int room = LIMIT - LEFT - gutter_width(v, s);
    if (s->cur_col < s->left_col) { s->left_col = s->cur_col; return; }
    /* From the cursor leftwards, as many columns as fit (at most 40). */
    uint32_t c = s->cur_col;
    int used = width_of(s, c);
    while (c > s->left_col && used + width_of(s, c - 1) <= room) used += width_of(s, --c);
    if (c > s->left_col) s->left_col = c;
}
static void fill(Cells *c, int x, int y, int w, int fg, int bg)
{
    for (int i = 0; i < w && x + i < LIMIT; ++i) cells_put(c, x + i, y, ' ', fg, bg);
}
/* Returns 1 when the text took the gap column, where the grid line goes. */
static int put_cell(Cells *c, int x, int y, int w, const char *text, int right, int fg, int bg)
{
    int room = w - 1, gap = 0;
    if (x + w > LIMIT) w = LIMIT - x;
    fill(c, x, y, w, fg, bg);
    if (room < 1 || !*text) return 0;
    if (room > w) room = w;
    int width = cells_width(text);
    if (right) {
        /* A value one column too wide may use the gap kept between cells. */
        if (width == room + 1 && width <= w) { room = width; gap = 1; }
        if (width > room) {
            for (int i = 0; i < room; ++i) cells_put(c, x + i, y, '#', fg, bg);
            return 0;
        }
        cells_text(c, x + room - width, y, text, fg, bg, width);
    } else cells_text(c, x, y, text, fg, bg, room);
    return gap;
}
/* The grid (revision 1 §3 17): FD-frame cyan dots on the right and bottom
 * edges of every cell (the gap column kept on the right of each cell, and the
 * last pixel rows of each row), so the cursor block and the highlighted names
 * sit exactly between lines. Dots go only where the pixel is still the ground
 * colour: characters and the reversed cursor are never painted over. No dots
 * under the floating boxes or their shadows, inside a merged range, or where
 * a value overflows into the gap. */
#define GRID_DOTS 1
static void grid_dot(Canvas *canvas, const Cells *c, int x, int y)
{
    if (x < 0 || x >= FD_W || y < 0 || y >= FD_H) return;
    /* Box 0 is the screen's frame; not under the floating boxes over it. */
    if (c->box_count && (cells_box_at(c, x / 12, y / 24) > 0 || cells_shadowed(c, x / 12, y / 24))) return;
    unsigned char *p = CANVAS_PX(canvas, CANVAS_GRID_X + x, y);
    uint32_t bg = fd_palette[BG], line = fd_palette[CYAN];
    if (p[0] != (unsigned char)(bg >> 16) || p[1] != (unsigned char)(bg >> 8) || p[2] != (unsigned char)bg) return;
    p[0] = (unsigned char)(line >> 16); p[1] = (unsigned char)(line >> 8); p[2] = (unsigned char)line; p[3] = 255;
}
static void grid_vline(Canvas *canvas, const Cells *c, int edge, int y0, int y1)
{
    for (int y = y0; y < y1; ++y)
        for (int d = 1; d <= GRID_DOTS; ++d) grid_dot(canvas, c, edge - d, y);
}
static void grid_hline(Canvas *canvas, const Cells *c, int edge, int x0, int x1)
{
    for (int x = x0; x < x1; ++x)
        for (int d = 1; d <= GRID_DOTS; ++d) grid_dot(canvas, c, x, edge - d);
}
/* The frame's lines (raster.c): inner pixels x 7 and FD_W - 8, the top line's
 * lower pixel y 12, the bottom line's upper pixel FRAME_BOTTOM * 24 + 11. A
 * vertical line of the grid runs from the top line to the bottom line, but
 * not through the title or the status written in their gaps. */
#define FRAME_IN_LEFT 7
#define FRAME_IN_RIGHT (FD_W - 7)
static int in_gap(int x, int from, int to) { return x >= from * 12 && x < to * 12; }
static void grid_vline_framed(Canvas *canvas, const Cells *c, int edge, int y0, int y1)
{
    const CellBox *f = c->box_count ? &c->boxes[0] : NULL;
    int x = edge - 1;
    if (f && y0 <= 24) y0 = in_gap(x, f->top_from, f->top_to) ? 24 : 13;
    if (f && y1 >= FRAME_BOTTOM * 24) y1 = in_gap(x, f->bottom_from, f->bottom_to) ? FRAME_BOTTOM * 24 : FRAME_BOTTOM * 24 + 11;
    grid_vline(canvas, c, edge, y0, y1);
}
static void draw_grid(Canvas *canvas, const Cells *c, const Sheet *s, int gutter, const uint32_t *cols,
                      const int *xs, const int *ws, int ncols, int nrows, unsigned char gap[][FD_COLS_MAX])
{
    int top = 1 * 24, bottom = (DATA_TOP + nrows) * 24, inner = LIMIT * 12;
    int right = ncols ? (xs[ncols - 1] + ws[ncols - 1]) * 12 : (LEFT + gutter) * 12;
    /* The last column cut by the frame: its lines reach the frame's line. */
    if (right >= inner) right = FRAME_IN_RIGHT;
    /* Under the column names, and under every row (row numbers included),
     * from the frame's left line. */
    grid_hline(canvas, c, DATA_TOP * 24, FRAME_IN_LEFT, right);
    for (int y = 0; y < nrows; ++y) {
        uint32_t row = s->top_row + (uint32_t)y;
        int edge = (DATA_TOP + y + 1) * 24;
        grid_hline(canvas, c, edge, FRAME_IN_LEFT, (LEFT + gutter) * 12);
        for (int i = 0; i < ncols; ++i) {
            const SheetRange *m = merge_at(s, row, cols[i]);
            if (m && row < m->r2) continue;
            int x1 = (xs[i] + ws[i]) * 12;
            grid_hline(canvas, c, edge, xs[i] * 12, x1 < inner ? x1 : FRAME_IN_RIGHT);
        }
    }
    /* Between the row numbers and the cells, and right of every column; from
     * the top line, and on to the bottom line under the last row. */
    int tail = bottom < FRAME_BOTTOM * 24;
    grid_vline_framed(canvas, c, (LEFT + gutter) * 12, top, bottom);
    if (tail) grid_vline_framed(canvas, c, (LEFT + gutter) * 12, bottom, FRAME_BOTTOM * 24);
    for (int i = 0; i < ncols; ++i) {
        int edge = (xs[i] + ws[i]) * 12;
        if (edge >= inner) break;
        grid_vline_framed(canvas, c, edge, top, DATA_TOP * 24);
        for (int y = 0; y < nrows; ++y) {
            const SheetRange *m = merge_at(s, s->top_row + (uint32_t)y, cols[i]);
            if (gap[y][i] || (m && cols[i] < m->c2)) continue;
            int y1 = (DATA_TOP + y + 1) * 24;
            grid_vline_framed(canvas, c, edge, (DATA_TOP + y) * 24, y + 1 == nrows && !tail ? FRAME_BOTTOM * 24 : y1);
        }
        if (tail) grid_vline_framed(canvas, c, edge, bottom, FRAME_BOTTOM * 24);
    }
}
static void sheet_draw(Screen *screen, App *app, Canvas *canvas, Cells *c)
{
    SheetView *v = screen->self;
    Book *b = &v->book;
    Sheet *s = current(v);
    /* A pixel screen: the cells are drawn here, then the grid over them. */
    cells_init(c);
    scroll_into_view(v, s);
    if (frame_open(c) < 0) return;
    /* The name in the top line, the sheet or the code in the bottom line. */
    const char *name = strrchr(b->path, '/');
    name = name ? name + 1 : b->path;
    frame_title(c, FRAME_TITLE_COL, name, FRAME_STATUS_END - FRAME_TITLE_COL - 9, any_dirty(v) ? T("変更あり") : NULL, MAGENTA);
    char status[256];
    if (b->type == BOOK_XLSX) snprintf(status, sizeof(status), "%s  %d/%d", s->name, b->current + 1, b->nsheets);
    else snprintf(status, sizeof(status), "%s%s %s", jis_encoding_name(b->encoding), b->bom ? " BOM" : "",
                  !strcmp(b->newline, "\r\n") ? "CRLF" : !strcmp(b->newline, "\r") ? "CR" : "LF");
    frame_status(c, FRAME_STATUS_END, status, FD_COLS - 8);
    /* Row 1: column names; left 5 (or more): row numbers. */
    int gutter = gutter_width(v, s);
    uint32_t cols[FD_COLS_MAX];
    int xs[FD_COLS_MAX], ws[FD_COLS_MAX], ncols = 0;
    for (uint32_t col = s->left_col, at = (uint32_t)(LEFT + gutter); at < (uint32_t)LIMIT && col < SHEET_MAX_COLS; ++col) {
        cols[ncols] = col; xs[ncols] = (int)at; ws[ncols] = width_of(s, col);
        at += (uint32_t)ws[ncols];
        ncols++;
    }
    for (int i = 0; i < ncols; ++i) {
        char label[8];
        sheet_column_name(cols[i], label, sizeof(label));
        int w = ws[i], hl = cols[i] == s->cur_col;
        if (xs[i] + w > LIMIT) w = LIMIT - xs[i];
        fill(c, xs[i], 1, w, hl ? BG : CYAN, hl ? CYAN : BG);
        int lw = (int)strlen(label), lx = xs[i] + (ws[i] - lw) / 2;
        if (lx + lw <= LIMIT) cells_text(c, lx, 1, label, hl ? BG : CYAN, hl ? CYAN : BG, lw);
    }
    unsigned char gap[FD_ROWS_MAX][FD_COLS_MAX];
    memset(gap, 0, sizeof(gap));
    int nrows = 0;
    for (int y = 0; y < DATA_ROWS; ++y) {
        uint32_t row = s->top_row + (uint32_t)y;
        if (row >= SHEET_MAX_ROWS) break;
        nrows = y + 1;
        char number[16];
        snprintf(number, sizeof(number), "%u", row + 1);
        int hl = row == s->cur_row, nw = (int)strlen(number);
        fill(c, LEFT, DATA_TOP + y, gutter - 1, hl ? BG : CYAN, hl ? CYAN : BG);
        cells_text(c, LEFT + gutter - 1 - nw, DATA_TOP + y, number, hl ? BG : CYAN, hl ? CYAN : BG, nw);
        /* Walk the stored cells of this row once. */
        size_t k = sheet_lower(s, row, s->left_col);
        for (int i = 0; i < ncols; ++i) {
            while (k < s->count && s->cells[k].row == row && s->cells[k].col < cols[i]) ++k;
            SheetCell *cell = k < s->count && s->cells[k].row == row && s->cells[k].col == cols[i] ? &s->cells[k] : NULL;
            int cursor = row == s->cur_row && cols[i] == s->cur_col;
            int fg = cursor ? BG : WHITE, bg = cursor ? WHITE : BG;
            const SheetRange *m = merge_at(s, row, cols[i]);
            char text[512];
            int right = 0;
            text[0] = 0;
            if (cell && (!m || (m->r1 == row && m->c1 == cols[i]))) {
                display(b, s, cell, ws[i] - 1, text, sizeof(text), &right);
                clean(text);
            }
            gap[y][i] = (unsigned char)put_cell(c, xs[i], DATA_TOP + y, ws[i], text, right, fg, bg);
        }
    }
    /* Row 23: address and content (widgets draw over it when active). */
    char address[16];
    sheet_address(s->cur_row, s->cur_col, address, sizeof(address));
    int x = 2 + cells_text(c, 2, FD_LINE, address, CYAN, BG, 12);
    x += cells_text(c, x, FD_LINE, ": ", CYAN, BG, 2);
    const SheetRange *m = merge_at(s, s->cur_row, s->cur_col);
    if (!m || (m->r1 == s->cur_row && m->c1 == s->cur_col)) {
        char *text = content(b, s, sheet_find(s, s->cur_row, s->cur_col));
        if (text) { clean(text); cells_text(c, x, FD_LINE, text, WHITE, BG, FD_COLS - x); free(text); }
    }
    /* Row 24: the band. */
    static const char *const bands[] = {" Edit ", " Save ", "Sheet ", "Width ", " Goto ", "Clear ", "Undo^Z", " Quit "};
    static const int hot[] = {1, 1, 0, 0, 1, 0, 5, 1};
    for (int i = 0; i < 8; ++i) cells_band_item(c, 2 + i * 10, 6, bands[i], hot[i]);
    widgets_draw(&app->widgets, c);
    canvas->full_dirty = 1;
    FDRect rects[FD_ROWS_MAX];
    raster_update(canvas, c, rects);
    draw_grid(canvas, c, s, gutter, cols, xs, ws, ncols, nrows, gap);
}

/* ---- editing ---- */
static int editable(SheetView *v, App *app, uint32_t row, uint32_t col)
{
    Book *b = &v->book;
    Sheet *s = current(v);
    if (b->readonly) { app_notice(app, b->readonly_reason); return 0; }
    const SheetRange *m = merge_at(s, row, col);
    if (m && (m->r1 != row || m->c1 != col)) { app_notice(app, T("結合されたセルは左上だけ直せます")); return 0; }
    if (in_array(s, row, col)) { app_notice(app, T("配列数式のセルは直せません")); return 0; }
    return 1;
}
static void set_text(Sheet *s, SheetCell *c, const char *text, int *nomem)
{
    c->text = arena_add(&s->arena, text, strlen(text));
    if (c->text == SHEET_NONE) { *nomem = 1; c->kind = SK_EMPTY; }
    else c->kind = SK_STR;
}
static void apply(SheetView *v, App *app, uint32_t row, uint32_t col, const char *text)
{
    Book *b = &v->book;
    Sheet *s = current(v);
    int nomem = 0;
    if (b->type == BOOK_CSV) {
        SheetCell *c = *text ? sheet_insert(s, row, col) : sheet_find(s, row, col);
        if (!c) {
            if (*text) app_notice(app, T("メモリが足りません"));
            return;
        }
        if (*text) set_text(s, c, text, &nomem); else c->kind = SK_EMPTY;
        c->flags |= SF_EDITED;
        s->dirty = 1;
        if (nomem) app_notice(app, T("メモリが足りません"));
        return;
    }
    SheetCell *c = sheet_find(s, row, col);
    /* Editing the master of a shared formula: its members become plain
     * formulas, since they take their text from the master. */
    if (c && (c->flags & SF_SHARED)) {
        uint32_t si = c->aux;
        SheetShared *master = NULL;
        for (size_t i = 0; i < s->nshared; ++i) if (s->shared[i].si == si) master = &s->shared[i];
        if (master && master->row == row && master->col == col) {
            for (size_t i = 0; i < s->count; ++i) {
                SheetCell *x = &s->cells[i];
                if (x != c && (x->flags & SF_SHARED) && x->aux == si && !(x->flags & SF_FORMULA)) {
                    app_notice(app, T("この共有数式の元のセルは直せません")); return;
                }
            }
            for (size_t i = 0; i < s->count; ++i) {
                SheetCell *x = &s->cells[i];
                if (x != c && (x->flags & SF_SHARED) && x->aux == si) { x->flags = (uint16_t)((x->flags & ~SF_SHARED) | SF_REWRITE); }
            }
            master->si = SHEET_NONE;
        }
        c->flags &= (uint16_t)~SF_SHARED;
    }
    const char *message = NULL;
    const char *formula = NULL;
    double number = 0;
    int kind = SK_STR;
    CellFormat f = xlsx_format(b, c ? c->style : 0);
    if (!*text) kind = SK_EMPTY;
    else if (text[0] == '=' && text[1]) {
        switch (formula_check(text + 1)) {
        case FORMULA_OK: formula = text + 1; kind = SK_EMPTY; break;
        case FORMULA_FUNCTION: message = T("この関数は計算できません"); break;
        case FORMULA_UNSUPPORTED: message = T("この数式は計算できません"); break;
        default: message = T("数式が正しくありません"); break;
        }
    } else if ((f.kind == FMT_DATE || f.kind == FMT_TIME || f.kind == FMT_DATETIME) && parse_date(b, text, f, &number)) kind = SK_NUM;
    else if (sheet_parse_number(text, &number)) kind = SK_NUM;
    if (!c && kind == SK_EMPTY && !formula) return;
    c = sheet_insert(s, row, col);
    if (!c) { app_notice(app, T("メモリが足りません")); return; }
    c->flags &= (uint16_t)~(SF_FORMULA | SF_CALC | SF_RECALC | SF_REWRITE | SF_SHARED);
    c->flags |= SF_EDITED;
    c->formula = SHEET_NONE;
    c->kind = (uint8_t)kind;
    c->num = number;
    if (kind == SK_STR) set_text(s, c, text, &nomem);
    if (formula) {
        /* As Excel writes them: names and references in capitals. A
         * computable formula has no strings, so all of it can be raised. */
        char *upper = strdup(formula);
        if (upper) for (char *p = upper; *p; ++p) *p = (char)toupper((unsigned char)*p);
        c->formula = upper ? arena_add(&s->arena, upper, strlen(upper)) : SHEET_NONE;
        free(upper);
        if (c->formula == SHEET_NONE) nomem = 1;
        else c->flags |= SF_FORMULA | SF_CALC;
    }
    s->dirty = 1;
    int r = sheet_recalc(b, s);
    if (nomem) app_notice(app, T("メモリが足りません"));
    else if (message) app_notice(app, message);
    else if (r == 1) app_notice(app, T("循環する参照があります"));
    else if (r == 2) app_notice(app, T("メモリが足りないため計算できない所があります"));
}
/* apply() as one undo step (revision 1 item 38). */
static void edit_cell(SheetView *v, App *app, uint32_t row, uint32_t col, const char *text)
{
    Book *b = &v->book;
    Sheet *s = current(v);
    SheetCapture *cap = sheet_capture(b, s, row, col);
    apply(v, app, row, col, text);
    int same = 0;
    SheetStep *st = cap ? sheet_step_make(b, s, b->current, row, col, cap, &same) : NULL;
    if (same) return;
    UndoStep step = {st, -1, -1, v->saves};
    if (!st || undo_push(&v->undo, app->settings.undo_levels, &step)) {
        if (!st) undo_lost(&v->undo);
        app_notice(app, T("メモリが足りないため、取り消しの記録を消しました"));
    }
}
static void undo_redo(SheetView *v, App *app, int redo)
{
    UndoStep *u = redo ? undo_next_redo(&v->undo) : undo_next_undo(&v->undo);
    if (!u) { app_notice(app, redo ? T("やり直す物はありません") : T("取り消す物はありません")); return; }
    Book *b = &v->book;
    int sheet;
    uint32_t row, col;
    sheet_step_where(u->data, &sheet, &row, &col);
    char err[256] = "", message[400];
    int moved = sheet != b->current;
    if (moved) {
        /* The step is on another sheet: go there (read it again when it was let go). */
        if (sheet < 0 || sheet >= b->nsheets || xlsx_load_sheet(b, sheet, err, sizeof(err))) {
            undo_clear(&v->undo);
            snprintf(message, sizeof(message), "%s: %s", redo ? T("やり直せません") : T("取り消せません"), err[0] ? err : T("シートがありません"));
            app_notice(app, message);
            return;
        }
        xlsx_unload_sheet(b, b->current);
        b->current = sheet;
    }
    Sheet *s = current(v);
    int force = b->type == BOOK_XLSX && u->saves != v->saves;
    s->cur_row = row;
    s->cur_col = col;
    if (sheet_step_apply(b, s, u->data, !redo, force)) {
        undo_lost(&v->undo);
        app_notice(app, T("メモリが足りないため、取り消しの記録を消しました"));
        return;
    }
    if (redo) undo_done_redo(&v->undo);
    else undo_done_undo(&v->undo);
    char address[16];
    sheet_address(row, col, address, sizeof(address));
    if (moved) snprintf(message, sizeof(message), T("%s（%s!%s）"), redo ? T("やり直しました") : T("取り消しました"), s->name, address);
    else snprintf(message, sizeof(message), T("%s（%s）"), redo ? T("やり直しました") : T("取り消しました"), address);
    app_notice(app, message);
}
static void begin_edit(SheetView *v, App *app, const Key *first)
{
    Sheet *s = current(v);
    if (!editable(v, app, s->cur_row, s->cur_col)) return;
    char *text = first ? strdup("") : content(&v->book, s, sheet_find(s, s->cur_row, s->cur_col));
    if (!text) { app_notice(app, T("メモリが足りません")); return; }
    if (strlen(text) >= WIDGET_CAP - 1) { free(text); app_notice(app, T("長すぎて直せません")); return; }
    char address[16];
    sheet_address(s->cur_row, s->cur_col, address, sizeof(address));
    free(v->edit_initial);
    v->edit_initial = text;
    v->edit_row = s->cur_row; v->edit_col = s->cur_col;
    widgets_input(&app->widgets, address, text, NULL);
    v->prompt = PROMPT_EDIT;
    if (first) widgets_key(&app->widgets, first);
}

/* ---- saving ---- */
static int copy_backup(const char *path, char *err, size_t errlen)
{
    size_t n = strlen(path) + 5;
    char *bak = malloc(n);
    if (!bak) { snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
    snprintf(bak, n, "%s.bak", path);
    int in = open(path, O_RDONLY);
    if (in < 0) { free(bak); snprintf(err, errlen, "%s", T("元のファイルを読めません")); return -1; }
    struct stat st;
    unsigned char *data = NULL;
    size_t size = 0;
    int rc = -1;
    if (!fstat(in, &st) && st.st_size >= 0 && (uint64_t)st.st_size < SIZE_MAX) {
        size = (size_t)st.st_size;
        data = malloc(size ? size : 1);
        size_t got = 0;
        while (data && got < size) {
            ssize_t r = read(in, data + got, size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (data && got == size) rc = sheet_write_file(bak, data, size, err, errlen);
        else snprintf(err, errlen, "%s", T("元のファイルを読めません"));
    }
    close(in);
    free(data); free(bak);
    return rc;
}
static void save(SheetView *v, App *app)
{
    Book *b = &v->book;
    if (b->readonly) { app_notice(app, b->readonly_reason); return; }
    if (!any_dirty(v)) { app_notice(app, T("変更はありません")); return; }
    struct stat st;
    if (stat(b->path, &st) || st.st_mtime != b->mtime || st.st_size != b->size) {
        app_notice(app, T("ファイルが外で書き換えられたため保存しません")); return;
    }
    char err[256] = "", message[320];
    int backed = 0;
    if (keep_backup && !b->backup_done) {
        if (copy_backup(b->path, err, sizeof(err))) {
            snprintf(message, sizeof(message), T(".bak を作れないため保存しません: %s"), err);
            app_notice(app, message); return;
        }
        b->backup_done = backed = 1;
    }
    int rc = b->type == BOOK_XLSX ? xlsx_save(b, err, sizeof(err)) : csv_save(b, err, sizeof(err));
    if (rc) {
        snprintf(message, sizeof(message), T("保存できません: %s"), err[0] ? err : T("書き込めません"));
        app_notice(app, message); return;
    }
    if (b->type == BOOK_CSV) for (int i = 0; i < b->nsheets; ++i) {
        Sheet *s = &b->sheets[i];
        for (size_t k = 0; k < s->count; ++k) s->cells[k].flags &= (uint16_t)~SF_EDITED;
        s->dirty = 0;
    }
    if (!stat(b->path, &st)) { b->mtime = st.st_mtime; b->size = st.st_size; }
    undo_saved(&v->undo);
    v->saves++;
    app_notice(app, backed ? T("保存しました（元のファイルは .bak に残しました）") : T("保存しました"));
}

/* ---- keys ---- */
static void switch_sheet(SheetView *v, App *app, int direction)
{
    Book *b = &v->book;
    if (b->type != BOOK_XLSX || b->nsheets < 2) return;
    char err[256] = "";
    for (int k = 1; k < b->nsheets; ++k) {
        int j = ((b->current + direction * k) % b->nsheets + b->nsheets) % b->nsheets;
        if (!xlsx_load_sheet(b, j, err, sizeof(err))) {
            xlsx_unload_sheet(b, b->current);
            b->current = j;
            return;
        }
    }
    app_notice(app, err[0] ? err : T("ほかのシートを開けません"));
}
static void move(Sheet *s, long dr, long dc)
{
    long r = (long)s->cur_row + dr, c = (long)s->cur_col + dc;
    if (r < 0) r = 0;
    if (r >= (long)SHEET_MAX_ROWS) r = SHEET_MAX_ROWS - 1;
    if (c < 0) c = 0;
    if (c >= (long)SHEET_MAX_COLS) c = SHEET_MAX_COLS - 1;
    s->cur_row = (uint32_t)r; s->cur_col = (uint32_t)c;
}
static int prompt_key(SheetView *v, App *app, const Key *k)
{
    Sheet *s = current(v);
    if (v->prompt == PROMPT_EDIT && k->key == KTAB) return 0;   /* no file-name completion here */
    WidgetResult r = widgets_key(&app->widgets, k);
    Prompt p = v->prompt;
    if (r != WIDGET_ACCEPT && r != WIDGET_CANCEL) return 0;
    v->prompt = PROMPT_NONE;
    if (r == WIDGET_CANCEL) return 0;
    if (p == PROMPT_QUIT) return 1;
    if (p == PROMPT_GOTO) {
        uint32_t row, col;
        const char *t = app->widgets.text;
        while (*t == ' ') ++t;
        size_t n = sheet_parse_address(t, &row, &col);
        if (!n || t[n]) app_notice(app, T("番地が正しくありません"));
        else { s->cur_row = row; s->cur_col = col; }
        return 0;
    }
    if (p == PROMPT_EDIT && v->edit_initial && strcmp(app->widgets.text, v->edit_initial))
        edit_cell(v, app, v->edit_row, v->edit_col, app->widgets.text);
    return 0;
}
static int sheet_key(Screen *screen, App *app, const Key *k)
{
    SheetView *v = screen->self;
    if (v->prompt != PROMPT_NONE) return prompt_key(v, app, k);
    Sheet *s = current(v);
    int ctrl = (k->mods & MOD_CTRL) != 0, alt = (k->mods & MOD_ALT) != 0;
    switch (k->key) {
    case KESC:
        if (any_dirty(v)) { widgets_confirm(&app->widgets, T("保存せずに戻りますか？")); v->prompt = PROMPT_QUIT; return 0; }
        return 1;
    case KUP: move(s, -1, 0); return 0;
    case KDOWN: move(s, 1, 0); return 0;
    case KLEFT: move(s, 0, -1); return 0;
    case KRIGHT: move(s, 0, 1); return 0;
    case KPGUP: move(s, -DATA_ROWS, 0); s->top_row = s->top_row >= (uint32_t)DATA_ROWS ? s->top_row - (uint32_t)DATA_ROWS : 0; return 0;
    case KPGDN:
        move(s, DATA_ROWS, 0);
        if (s->top_row + 2 * DATA_ROWS <= SHEET_MAX_ROWS) s->top_row += DATA_ROWS;
        return 0;
    case KHOME: s->cur_row = 0; s->cur_col = 0; return 0;
    case KEND:
        if (s->count) {
            uint32_t col = 0;
            for (size_t i = 0; i < s->count; ++i) if (s->cells[i].col > col) col = s->cells[i].col;
            s->cur_row = s->cells[s->count - 1].row; s->cur_col = col;
        }
        return 0;
    case KTAB: switch_sheet(v, app, (k->mods & MOD_SHIFT) ? -1 : 1); return 0;
    case KRET: begin_edit(v, app, NULL); return 0;
    case KDEL:
        if (editable(v, app, s->cur_row, s->cur_col) && sheet_find(s, s->cur_row, s->cur_col))
            edit_cell(v, app, s->cur_row, s->cur_col, "");
        return 0;
    default: break;
    }
    if (k->key != KCHAR) return 0;
    if (ctrl && !alt) {
        if (k->ch == 's' || k->ch == 'S') save(v, app);
        else if (k->ch == 'g' || k->ch == 'G') { widgets_input(&app->widgets, i18n_lang() == I18N_EN ? "Go to cell" : "移動先", "", NULL); v->prompt = PROMPT_GOTO; } /* i18n:keep */
        else if (k->ch == 'z' || k->ch == 'Z') undo_redo(v, app, 0);
        else if (k->ch == 'y' || k->ch == 'Y') undo_redo(v, app, 1);
        return 0;
    }
    if (alt) return 0;
    if (k->ch == '<' || k->ch == '>') {
        if (!s->widths) { s->widths = calloc(SHEET_MAX_COLS, 1); if (!s->widths) { app_notice(app, T("メモリが足りません")); return 0; } }
        int w = width_of(s, s->cur_col) + (k->ch == '>' ? 1 : -1);
        if (w < MIN_WIDTH) w = MIN_WIDTH;
        if (w > MAX_WIDTH) w = MAX_WIDTH;
        s->widths[s->cur_col] = (unsigned char)w;
        return 0;
    }
    if (k->ch >= 0x20 && k->ch != 0x7f) begin_edit(v, app, k);
    return 0;
}
static void sheet_destroy(Screen *screen)
{
    SheetView *v = screen->self;
    book_free(&v->book);
    undo_free(&v->undo);
    free(v->edit_initial);
    free(v->gutters);
    free(v);
}

static int has_suffix(const char *path, const char *suffix)
{
    size_t a = strlen(path), b = strlen(suffix);
    return a >= b && !strcasecmp(path + a - b, suffix);
}
Screen *sheetview_create(App *app, const char *path)
{
    SheetView *v = calloc(1, sizeof(*v));
    if (!v) { app_notice(app, T("メモリが足りません")); return NULL; }
    Book *b = &v->book;
    b->path = strdup(path);
    struct stat st;
    if (!b->path || stat(path, &st) || !S_ISREG(st.st_mode)) {
        free(b->path); free(v); app_notice(app, T("ファイルを開けません")); return NULL;
    }
    b->mtime = st.st_mtime; b->size = st.st_size;
    unsigned char magic[4] = {0};
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        if (read(fd, magic, 4) != 4) memset(magic, 0, 4);
        close(fd);
    }
    int zip = magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
    char err[256] = "", message[320] = "";
    int rc;
    if (zip || has_suffix(path, ".xlsx")) {
        rc = xlsx_open(b, path, err, sizeof(err));
        if (!rc) {
            /* A damaged first sheet: say so, and show the next one that loads. */
            char first[256] = "";
            rc = -1;
            for (int i = 0; i < b->nsheets && rc; ++i) {
                if (!xlsx_load_sheet(b, i, err, sizeof(err))) { b->current = i; rc = 0; }
                else if (!first[0]) snprintf(first, sizeof(first), T("「%s」を開けません: %s"), b->sheets[i].name, err);
            }
            if (!rc && first[0]) snprintf(message, sizeof(message), "%s", first);
        }
    } else rc = csv_open(b, path, has_suffix(path, ".tsv") || has_suffix(path, ".tab") ? '\t' : ',', err, sizeof(err));
    if (rc) {
        snprintf(message, sizeof(message), T("表を開けません: %s"), err[0] ? err : T("形式が違います"));
        app_notice(app, message);
        book_free(b); free(v);
        return NULL;
    }
    v->gutters = calloc((size_t)b->nsheets, sizeof(*v->gutters));
    if (!v->gutters) {
        book_free(b); free(v); app_notice(app, T("メモリが足りません")); return NULL;
    }
    undo_init(&v->undo, sheet_step_free);
    v->screen = (Screen){v, sheet_key, sheet_draw, 1, NULL, sheet_destroy};
    if (message[0]) app_notice(app, message);
    return &v->screen;
}
