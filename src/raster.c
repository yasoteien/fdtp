// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "raster.h"
#include "font24.h"
/* Shadow of a floating box (revision 1 §7): black ground, grey characters. */
#define SHADOW_BG 0x000000
#define SHADOW_FG 0x808080
static void put_rgb(Raster *r, int x, int y, uint32_t rgb)
{
    if (x < 0 || x >= FD_W || y < 0 || y >= FD_H) return;
    unsigned char *p = CANVAS_PX(r, CANVAS_GRID_X+x, y);
    p[0] = rgb >> 16; p[1] = rgb >> 8; p[2] = rgb; p[3] = 255;
}
/* Source bit of a 16-dot glyph; outside is blank. */
static int ink(const Font24Glyph *g, int x, int y)
{
    return x >= 0 && x < g->width && y >= 0 && y < 16 && ((g->rows[y] >> (g->width - 1 - x)) & 1);
}
/* fg over bg with coverage n/4 (0..4). */
static void blend(Raster *r, int x, int y, uint32_t f, uint32_t b, int n)
{
    if (n == 0) { put_rgb(r, x, y, b); return; }
    if (n == 4) { put_rgb(r, x, y, f); return; }
    unsigned char *p = CANVAS_PX(r, CANVAS_GRID_X+x, y);
    for (int i = 0; i < 3; ++i) {
        int sh = 16 - 8 * i;
        p[i] = (unsigned char)((((f >> sh) & 255) * n + ((b >> sh) & 255) * (4 - n)) / 4);
    }
    p[3] = 255;
}
/* What a line pixel at grid pixel (x, y) may do: 0 draw, 1 hidden under a
 * box, 2 in a box shadow (drawn grey). */
static int covered(const Cells *c, int x, int y)
{
    if (!c->box_count || x < 0 || y < 0 || x >= FD_W || y >= FD_H) return 0;
    int col = x / 12, row = y / 24;
    if (cells_box_at(c, col, row) >= 0) return 1;
    return cells_shadowed(c, col, row) ? 2 : 0;
}
static void line_pixel(Raster *r, const Cells *c, int x, int y)
{
    int k = covered(c, x, y);
    if (k == 1) return;
    put_rgb(r, x, y, k == 2 ? SHADOW_FG : fd_palette[CYAN]);
}
static int top_gap(const Cells *c, int x)
{
    return (x >= c->title_start * 12 && x < c->title_end * 12) ||
           (x >= c->date_start * 12 && x < c->date_end * 12);
}
/* Integer pixel-centre annulus: radius six, two pixels thick. The right
 * side follows the grid's width (953 = FD_W - 7 at 80 columns), the row 1
 * dividers the header's columns (fd_divider, at 162 438 690 there). */
static void lines(Raster *r, const Cells *c, int row)
{
    const int right = FD_W - 7, inner = FD_W - 12;
    const int d0 = fd_divider(0) * 12 + 6, d1 = fd_divider(1) * 12 + 6, d2 = fd_divider(2) * 12 + 6;
    for (int y = row * 24; y < (row + 1) * 24; ++y) {
        for (int x = 5; x <= right + 1; ++x) {
            int on = 0;
            if ((y == 11 || y == 12) && x >= 12 && x < inner && !top_gap(c,x)) on = 1;
            if ((y == 131 || y == 132) && x >= 12 && x < inner) on = 1;
            if ((x == 5 || x == 6 || x == right - 1 || x == right) && y >= 18 && y < 126) on = 1;
            if ((y == 59 || y == 60) && x >= 6 && x <= right) on = 1;
            /* Under the title or the date there is no top line to join:
             * the divider starts at row 1, off the text (85 columns). */
            if (y >= (top_gap(c,x) ? 24 : 12) && y <= 60 &&
                (x == d0 - 1 || x == d0 || x == d1 - 1 || x == d1 || x == d2 - 1 || x == d2)) on = 1;
            int cx = x < 12 ? 12 : inner - 1;
            int cy = y < 18 ? 18 : 126;
            if ((x < 12 || x >= inner) && (y < 18 || y >= 126) && y <= 133) {
                int dx = 2*x + 1 - 2*cx, dy = 2*y + 1 - 2*cy;
                int d = dx*dx + dy*dy;
                if (d >= 100 && d < 196) on = 1;
            }
            if (on) line_pixel(r,c,x,y);
        }
    }
}
static void glyph_cell(Raster *r, const Cell *v, int col, int row, uint32_t f, uint32_t b)
{
    const Font24Glyph *g = font24_lookup(v->code);
    /* 1.5x by area: output pixel (x, y) covers the 2x2 block of the
     * source upscaled 3x, i.e. source pixels ((2x+dx)/3, (2y+dy)/3). */
    int start = v->part == 2 ? 12 : 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 12; ++x) {
            int ox = start + x, n = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    n += ink(g, (2 * ox + dx) / 3, (2 * y + dy) / 3);
            blend(r, col * 12 + x, row * 24 + y, f, b, n);
        }
}
static void render_cell(Raster *r, const Cells *c, int col, int row)
{
    const Cell *v = &c->data[row][col];
    if (c->box_count && cells_shadowed(c, col, row)) glyph_cell(r, v, col, row, SHADOW_FG, SHADOW_BG);
    else glyph_cell(r, v, col, row, fd_palette[v->fg], fd_palette[v->bg]);
}
/* Box i's frame in grid pixel row y. Lines pass through the centres of the
 * border cells (pixels centre-1 and centre, as the top FD frame); corners are
 * the same two-dot annulus of radius six. Pixels under a later box are left. */
static void box_frame_row(Raster *r, const Cells *c, int i, int y)
{
    const CellBox *b = &c->boxes[i];
    if (!(b->kind & CELL_BOX_FRAME)) return;
    int xl = b->col * 12 + 5, xr = (b->col + b->w - 1) * 12 + 5;
    int yt = b->row * 24 + 11, yb = (b->row + b->h - 1) * 24 + 11;
    if (y < yt || y > yb + 1) return;
    int cxl = xl + 7, cxr = xr - 5, cyt = yt + 7, cyb = yb - 5;
    int yr = b->rule ? b->rule * 24 + 11 : -100;
    for (int x = xl; x <= xr + 1; ++x) {
        int on = 0;
        int xcorner = x < cxl || x >= cxr, ycorner = y < cyt || y >= cyb;
        if (xcorner && ycorner) {
            int cx = x < cxl ? cxl : cxr, cy = y < cyt ? cyt : cyb;
            int dx = 2*x + 1 - 2*cx, dy = 2*y + 1 - 2*cy, d = dx*dx + dy*dy;
            on = d >= 100 && d < 196;
        } else if (!xcorner) {
            int top = (y == yt || y == yt + 1) && !(x >= b->top_from * 12 && x < b->top_to * 12);
            int bottom = (y == yb || y == yb + 1) && !(x >= b->bottom_from * 12 && x < b->bottom_to * 12);
            on = top || bottom || y == yr || y == yr + 1;
        }
        else on = x == xl || x == xl + 1 || x == xr || x == xr + 1 || y == yr || y == yr + 1;
        if (!on) continue;
        int top = cells_box_at(c, x / 12, y / 24);
        if (top > i) continue;
        put_rgb(r, x, y, fd_palette[CYAN]);
    }
}
/* The extra lines of box i (CellLine) in grid pixel row y. */
static void box_lines_row(Raster *r, const Cells *c, int i, int y)
{
    for (int n = 0; n < c->line_count; ++n) {
        const CellLine *l = &c->lines[n];
        if (l->box != i) continue;
        int x0, x1;
        if (l->vertical) {
            if (y < l->from * 24 + 11 || y > l->to * 24 + 12) continue;
            x0 = l->at * 12 + 5; x1 = x0 + 1;
        } else {
            if (y != l->at * 24 + 11 && y != l->at * 24 + 12) continue;
            x0 = l->from * 12 + 5; x1 = l->to * 12 + 6;
        }
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || x >= FD_W) continue;
            if (cells_box_at(c, x / 12, y / 24) > i) continue;
            put_rgb(r, x, y, fd_palette[CYAN]);
        }
    }
}
static void boxes_row(Raster *r, const Cells *c, int row)
{
    for (int i = 0; i < c->box_count; ++i)
        for (int y = row * 24; y < (row + 1) * 24; ++y) { box_frame_row(r, c, i, y); box_lines_row(r, c, i, y); }
}
int raster_update(Raster *r, Cells *c, FDRect out[FD_ROWS_MAX])
{
    int count = 0;
    for (int row = 0; row < FD_ROWS; ++row) {
        int changed = c->lines_dirty || r->full_dirty;
        for (int col = 0; col < FD_COLS; ++col) changed |= c->dirty[row][col];
        if (!changed) continue;
        for (int col = 0; col < FD_COLS; ++col) {
            render_cell(r, c, col, row);
            c->dirty[row][col] = 0;
        }
        if (c->frame && row <= 5) lines(r,c,row);
        if (cells_has_separator(c, row))
            for (int y=row*24+11;y<=row*24+12;++y)
                for (int x=0;x<FD_W;++x) line_pixel(r,c,x,y);
        if (c->box_count) boxes_row(r,c,row);
        out[count++] = (FDRect){0,row*24,CANVAS_W,24};
    }
    c->lines_dirty = 0;
    r->full_dirty = 0;
    return count;
}
void raster_bands(Canvas *c, Canvas *scratch, Cells *cells, int line)
{
    canvas_init(scratch);
    FDRect rect[FD_ROWS_MAX];
    raster_update(scratch, cells, rect);
    size_t bytes = (size_t)c->w * 4 * 24;
    memcpy(CANVAS_ROW(c, 0), CANVAS_ROW(scratch, 0), bytes);
    memcpy(CANVAS_ROW(c, FRAME_BOTTOM * 24), CANVAS_ROW(scratch, FRAME_BOTTOM * 24), bytes);
    memcpy(CANVAS_ROW(c, FD_BAND * 24), CANVAS_ROW(scratch, FD_BAND * 24), bytes);
    if (line) memcpy(CANVAS_ROW(c, FD_LINE * 24), CANVAS_ROW(scratch, FD_LINE * 24), bytes);
}
void raster_boxes(Canvas *r, const Cells *c)
{
    if (!c->box_count) return;
    for (int row = 0; row < FD_ROWS; ++row)
        for (int col = 0; col < FD_COLS; ++col) {
            int i = cells_box_at(c, col, row);
            if (i >= 0) {
                const CellBox *b = &c->boxes[i];
                int border = col == b->col || col == b->col + b->w - 1 || row == b->row || row == b->row + b->h - 1;
                if ((b->kind & CELL_BOX_SHADOW) || border) render_cell(r, c, col, row);
                continue;
            }
            if (!cells_shadowed(c, col, row)) continue;
            /* Over a picture the shadow darkens what is there. */
            for (int y = row * 24; y < row * 24 + 24; ++y)
                for (int x = col * 12; x < col * 12 + 12; ++x) {
                    unsigned char *p = CANVAS_PX(r, CANVAS_GRID_X + x, y);
                    p[0] /= 4; p[1] /= 4; p[2] /= 4; p[3] = 255;
                }
        }
    for (int row = 0; row < FD_ROWS; ++row) boxes_row(r, c, row);
}
void raster_frame_line(Canvas *r, const Cells *c, int y)
{
    for (int dy = 0; dy < 2; ++dy)
        for (int x = 5; x <= FD_W - 6; ++x) {
            if (cells_box_at(c, x / 12, (y + dy) / 24) > 0) continue;
            put_rgb(r, x, y + dy, fd_palette[CYAN]);
        }
}
