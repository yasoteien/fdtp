// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "sheet_model.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t arena_add(Arena *a, const char *s, size_t n)
{
    if (a->len + n + 1 >= SHEET_NONE) return SHEET_NONE;
    if (a->len + n + 1 > a->cap) {
        size_t cap = a->cap ? a->cap : 4096;
        while (cap < a->len + n + 1) cap *= 2;
        if (cap >= SHEET_NONE) cap = SHEET_NONE - 1;
        char *p = realloc(a->data, cap);
        if (!p) return SHEET_NONE;
        a->data = p; a->cap = cap;
    }
    uint32_t at = (uint32_t)a->len;
    if (n) memcpy(a->data + at, s, n);
    a->data[at + n] = 0;
    a->len += n + 1;
    return at;
}
void arena_free(Arena *a) { free(a->data); memset(a, 0, sizeof(*a)); }

static int compare(uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2)
{
    if (r1 != r2) return r1 < r2 ? -1 : 1;
    if (c1 != c2) return c1 < c2 ? -1 : 1;
    return 0;
}
size_t sheet_lower(const Sheet *s, uint32_t row, uint32_t col)
{
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare(s->cells[mid].row, s->cells[mid].col, row, col) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
SheetCell *sheet_find(Sheet *s, uint32_t row, uint32_t col)
{
    size_t i = sheet_lower(s, row, col);
    return i < s->count && s->cells[i].row == row && s->cells[i].col == col ? &s->cells[i] : NULL;
}
SheetCell *sheet_insert(Sheet *s, uint32_t row, uint32_t col)
{
    size_t i = sheet_lower(s, row, col);
    if (i < s->count && s->cells[i].row == row && s->cells[i].col == col) return &s->cells[i];
    if (s->count >= SHEET_MAX_CELLS) return NULL;
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        SheetCell *p = realloc(s->cells, cap * sizeof(*p));
        if (!p) return NULL;
        s->cells = p; s->cap = cap;
    }
    memmove(s->cells + i + 1, s->cells + i, (s->count - i) * sizeof(*s->cells));
    s->count++;
    SheetCell *c = &s->cells[i];
    memset(c, 0, sizeof(*c));
    c->row = row; c->col = col; c->text = SHEET_NONE; c->formula = SHEET_NONE;
    return c;
}
void sheet_free(Sheet *s)
{
    free(s->cells); arena_free(&s->arena); free(s->merges); free(s->arrays); free(s->shared);
    free(s->row_fields);
    s->cells = NULL; s->count = s->cap = 0; s->merges = s->arrays = NULL; s->nmerges = s->narrays = 0;
    s->shared = NULL; s->nshared = 0; s->row_fields = NULL; s->nrows = 0; s->loaded = 0; s->dirty = 0;
}
void book_free(Book *b)
{
    for (int i = 0; i < b->nsheets; ++i) {
        sheet_free(&b->sheets[i]);
        free(b->sheets[i].name); free(b->sheets[i].part); free(b->sheets[i].widths);
    }
    free(b->sheets); free(b->path); zip_close(b->zip); arena_free(&b->sst); free(b->sst_off);
    free(b->xf); free(b->calc_chain); free(b->workbook_rels);
    memset(b, 0, sizeof(*b));
}
const char *cell_string(const Book *b, const Sheet *s, const SheetCell *c)
{
    if (c->kind == SK_SST) return c->text < b->nsst ? arena_str(&b->sst, b->sst_off[c->text]) : "";
    if (c->kind == SK_STR || c->kind == SK_ERR) return arena_str(&s->arena, c->text);
    return "";
}
void sheet_column_name(uint32_t col, char *out, size_t n)
{
    char tmp[8];
    int k = 0;
    uint32_t v = col + 1;
    while (v && k < 7) { v -= 1; tmp[k++] = (char)('A' + v % 26); v /= 26; }
    size_t j = 0;
    while (k && j + 1 < n) out[j++] = tmp[--k];
    if (n) out[j] = 0;
}
void sheet_address(uint32_t row, uint32_t col, char *out, size_t n)
{
    char name[8];
    sheet_column_name(col, name, sizeof(name));
    snprintf(out, n, "%s%u", name, row + 1);
}
size_t sheet_parse_address(const char *s, uint32_t *row, uint32_t *col)
{
    const char *p = s;
    uint64_t c = 0, r = 0;
    int letters = 0, digits = 0;
    if (*p == '$') ++p;
    while (isalpha((unsigned char)*p) && letters < 4) { c = c * 26 + (uint64_t)(toupper((unsigned char)*p) - 'A' + 1); ++p; ++letters; }
    if (!letters || letters > 3) return 0;
    if (*p == '$') ++p;
    while (isdigit((unsigned char)*p) && digits < 8) { r = r * 10 + (uint64_t)(*p - '0'); ++p; ++digits; }
    if (!digits || digits > 7 || r < 1 || r > SHEET_MAX_ROWS || c > SHEET_MAX_COLS) return 0;
    *row = (uint32_t)r - 1; *col = (uint32_t)c - 1;
    return (size_t)(p - s);
}
int sheet_parse_range(const char *s, SheetRange *r)
{
    size_t n = sheet_parse_address(s, &r->r1, &r->c1);
    if (!n) return -1;
    if (s[n] == ':') {
        size_t m = sheet_parse_address(s + n + 1, &r->r2, &r->c2);
        if (!m || s[n + 1 + m]) return -1;
    } else if (!s[n]) { r->r2 = r->r1; r->c2 = r->c1; }
    else return -1;
    if (r->r2 < r->r1) { uint32_t t = r->r1; r->r1 = r->r2; r->r2 = t; }
    if (r->c2 < r->c1) { uint32_t t = r->c1; r->c1 = r->c2; r->c2 = t; }
    return 0;
}
void sheet_number_text(double v, char *out, size_t n)
{
    if (v == 0) { snprintf(out, n, "0"); return; }
    for (int p = 15; p <= 17; ++p) {
        snprintf(out, n, "%.*g", p, v);
        if (strtod(out, NULL) == v) break;
    }
}
int sheet_parse_number(const char *s, double *v)
{
    const char *p = s;
    int digits = 0;
    if (*p == '+' || *p == '-') ++p;
    while (isdigit((unsigned char)*p)) { ++p; ++digits; }
    if (*p == '.') { ++p; while (isdigit((unsigned char)*p)) { ++p; ++digits; } }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p) return 0;
    char *end;
    double d = strtod(s, &end);
    if (*end || !isfinite(d)) return 0;
    *v = d;
    return 1;
}
