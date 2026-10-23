// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Row-23 input line with the input method (see doc_input.h). */
#include "doc_input.h"
#include "font24.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reserve(DocInput *in, size_t need)
{
    if (need <= in->cap) return 0;
    size_t c = in->cap ? in->cap : 64;
    while (c < need) c *= 2;
    uint32_t *p = realloc(in->text, c * sizeof(*p));
    if (!p) { in->failed = 1; return -1; }
    in->text = p;
    in->cap = c;
    return 0;
}
static void insert(DocInput *in, uint32_t c)
{
    if (reserve(in, in->len + 1)) return;
    memmove(in->text + in->cursor + 1, in->text + in->cursor, (in->len - in->cursor) * sizeof(uint32_t));
    in->text[in->cursor++] = c;
    in->len++;
}
static void insert_utf8(DocInput *in, const char *s)
{
    while (*s) {
        uint32_t c = font24_decode(&s);
        if (c == IME_CH_ZENKAKU_HANKAKU) continue;
        if (c >= 0x20 || c == '\t' || c == '\n' || c == '\f') insert(in, c);
    }
}
int doc_input_open(DocInput *in, Ime *ime, const char *label, const char *utf8)
{
    free(in->text);
    memset(in, 0, sizeof(*in));
    in->ime = ime;
    snprintf(in->label, sizeof(in->label), "%s", label);
    insert_utf8(in, utf8 ? utf8 : "");
    in->cursor = in->len;     /* at the end, as widgets_input() */
    in->active = 1;
    if (in->ime) ime_reset(in->ime);
    return in->failed ? -1 : 0;
}
void doc_input_close(DocInput *in)
{
    if (in->ime) ime_reset(in->ime);
    free(in->text);
    memset(in, 0, sizeof(*in));
}
char *doc_input_text(const DocInput *in)
{
    size_t n = in->len * 4 + 1;
    char *s = malloc(n), *p = s;
    if (!s) return NULL;
    for (size_t i = 0; i < in->len; ++i) {
        uint32_t c = in->text[i];
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = (char)(0xc0 | (c >> 6)); *p++ = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { *p++ = (char)(0xe0 | (c >> 12)); *p++ = (char)(0x80 | ((c >> 6) & 0x3f)); *p++ = (char)(0x80 | (c & 0x3f)); }
        else { *p++ = (char)(0xf0 | (c >> 18)); *p++ = (char)(0x80 | ((c >> 12) & 0x3f)); *p++ = (char)(0x80 | ((c >> 6) & 0x3f)); *p++ = (char)(0x80 | (c & 0x3f)); }
    }
    *p = 0;
    return s;
}

static int control(const Key *k, uint32_t ch)
{
    return k->key == KCHAR && (k->ch == ch || k->ch == ch - 32) && (k->mods & (MOD_CTRL | MOD_ALT)) == MOD_CTRL;
}
int doc_input_key(DocInput *in, const Key *k)
{
    int consumed = in->ime ? ime_key(in->ime, k) : 0;
    if (in->ime) {
        const char *t = ime_commit(in->ime);
        if (*t) { insert_utf8(in, t); ime_commit_done(in->ime); }
        if (consumed || ime_busy(in->ime)) return DOC_INPUT_MORE;
    }
    switch (k->key) {
    case KRET: return DOC_INPUT_ACCEPT;
    case KESC: return DOC_INPUT_CANCEL;
    case KLEFT:
        if (in->cursor) in->cursor--;
        return DOC_INPUT_MORE;
    case KRIGHT:
        if (in->cursor < in->len) in->cursor++;
        return DOC_INPUT_MORE;
    case KHOME: in->cursor = 0; return DOC_INPUT_MORE;
    case KEND: in->cursor = in->len; return DOC_INPUT_MORE;
    case KBS:
        if (in->cursor) {
            memmove(in->text + in->cursor - 1, in->text + in->cursor, (in->len - in->cursor) * sizeof(uint32_t));
            in->cursor--;
            in->len--;
        }
        return DOC_INPUT_MORE;
    case KDEL:
        if (in->cursor < in->len) {
            memmove(in->text + in->cursor, in->text + in->cursor + 1, (in->len - in->cursor - 1) * sizeof(uint32_t));
            in->len--;
        }
        return DOC_INPUT_MORE;
    case KTAB: insert(in, '\t'); return DOC_INPUT_MORE;
    default: break;
    }
    if (k->key != KCHAR) return DOC_INPUT_MORE;
    if (control(k, 'a')) in->cursor = 0;
    else if (control(k, 'e')) in->cursor = in->len;
    else if (control(k, 'b')) { if (in->cursor) in->cursor--; }
    else if (control(k, 'f')) { if (in->cursor < in->len) in->cursor++; }
    else if (control(k, 'h')) { Key bs = {KBS, 0, 0}; return doc_input_key(in, &bs); }
    else if (control(k, 'd')) { Key del = {KDEL, 0, 0}; return doc_input_key(in, &del); }
    else if (control(k, 'k')) in->len = in->cursor;
    else if (control(k, 'u')) {
        memmove(in->text, in->text + in->cursor, (in->len - in->cursor) * sizeof(uint32_t));
        in->len -= in->cursor;
        in->cursor = 0;
        in->scroll = 0;
    } else if (!(k->mods & (MOD_CTRL | MOD_ALT)) && k->ch >= 0x20 && k->ch != 0x7f && k->ch != IME_CH_ZENKAKU_HANKAKU)
        insert(in, k->ch);
    return DOC_INPUT_MORE;
}

/* What a character looks like on the line: the controls get a mark. */
static uint32_t shown(uint32_t c, int *color)
{
    *color = WHITE;
    switch (c) {
    case '\t': *color = CYAN; return 0x2192;   /* → */
    case '\n': *color = CYAN; return 0x2193;   /* ↓ */
    case '\f': *color = CYAN; return 0x2261;   /* ≡ */
    case 0x2011: return '-';
    case 0x00ad: *color = CYAN; return '-';
    default: return c;
    }
}
static int shown_width(uint32_t c)
{
    int color;
    return font24_width(shown(c, &color));
}
void doc_input_draw(DocInput *in, Cells *c)
{
    if (!in->active) return;
    for (int x = 0; x < FD_COLS; ++x) cells_put(c, x, FD_LINE, ' ', WHITE, BG);
    int col = 2 + cells_text(c, 2, FD_LINE, in->label, CYAN, BG, 40);
    col += cells_text(c, col, FD_LINE, ": ", CYAN, BG, 2);
    int available = FD_COLS - col;
    if (available < 4) return;
    ImePreedit pe = {"", 0, 0, 0};
    if (in->ime && ime_busy(in->ime)) ime_preedit(in->ime, &pe);
    int pre_w = cells_width(pe.text);
    int cur_w = in->cursor < in->len ? shown_width(in->text[in->cursor]) : 1;
    if (in->scroll > in->cursor) in->scroll = in->cursor;
    for (;;) {
        int w = 0;
        for (size_t i = in->scroll; i < in->cursor; ++i) w += shown_width(in->text[i]);
        if (in->scroll >= in->cursor || w + pre_w + cur_w <= available) break;
        in->scroll++;
    }
    int x = col;
    for (size_t i = in->scroll; i < in->cursor; ++i) {
        int color;
        uint32_t s = shown(in->text[i], &color);
        cells_put(c, x, FD_LINE, s, color, BG);
        x += font24_width(s);
    }
    int caret = x;
    if (*pe.text) {
        const char *p = pe.text;
        size_t off = 0;
        while (*p && x < FD_COLS) {
            const char *before = p;
            uint32_t ch = font24_decode(&p);
            int hl = pe.hl_end > pe.hl_start && off >= pe.hl_start && off < pe.hl_end;
            if (off == pe.caret) caret = x;
            if (x + font24_width(ch) > FD_COLS) break;
            cells_put(c, x, FD_LINE, ch, BG, hl ? YELLOW : CYAN);
            x += font24_width(ch);
            off += (size_t)(p - before);
        }
        if (pe.caret >= strlen(pe.text)) caret = x;
    }
    for (size_t i = in->cursor; i < in->len && x < FD_COLS; ++i) {
        int color;
        uint32_t s = shown(in->text[i], &color);
        if (x + font24_width(s) > FD_COLS) break;
        int at_cursor = i == in->cursor && !*pe.text;
        cells_put(c, x, FD_LINE, s, at_cursor ? BG : color, at_cursor ? WHITE : BG);
        x += font24_width(s);
    }
    if (in->cursor >= in->len && !*pe.text && x < FD_COLS) cells_put(c, x, FD_LINE, ' ', BG, WHITE);
    /* The candidate list, above the line at the caret. */
    ImeCandidates cand = {0};
    if (in->ime) ime_candidates(in->ime, &cand);
    if (!cand.active || cand.shown <= 0) return;
    int width = 0;
    char items[IME_PAGE_MAX][160];
    for (int i = 0; i < cand.shown; ++i) {
        snprintf(items[i], sizeof(items[i]), " %d %s ", (i + 1) % 10, cand.items[i] ? cand.items[i] : "");
        int w = cells_width(items[i]);
        if (w > width) width = w;
    }
    if (width > FD_COLS) width = FD_COLS;
    int left = caret + width > FD_COLS ? FD_COLS - width : caret;
    int shown_rows = cand.shown > FD_ROWS - 3 ? FD_ROWS - 3 : cand.shown;
    for (int i = 0; i < shown_rows; ++i) {
        int row = FD_LINE - shown_rows + i;
        int sel = cand.first + i == cand.index;
        for (int k = 0; k < width; ++k) cells_put(c, left + k, row, ' ', sel ? BG : WHITE, sel ? WHITE : BLUE);
        cells_text(c, left, row, items[i], sel ? BG : WHITE, sel ? WHITE : BLUE, width);
    }
}
