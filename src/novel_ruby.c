// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "novel_ruby.h"
#include <stdlib.h>

#define BAR_FULL 0xff5cu
#define OPEN 0x300au   /* 《 */
#define CLOSE 0x300bu  /* 》 */

int novel_is_kanji(uint32_t c)
{
    return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) ||
           (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x3ffff) ||
           c == 0x3005 || c == 0x3006 || c == 0x30f6;
}
int novel_is_kana(uint32_t c)
{
    return (c >= 0x3041 && c <= 0x3096) || (c >= 0x309b && c <= 0x309f) ||
           (c >= 0x30a1 && c <= 0x30fa) || (c >= 0x30fc && c <= 0x30ff) ||
           (c >= 0xff66 && c <= 0xff9f);
}
static int is_bar(uint32_t c) { return c == BAR_FULL || c == '|'; }

static int add(NRuby **out, size_t *cap, size_t *count, NRuby r)
{
    if (*count == *cap) {
        size_t next = *cap ? *cap * 2 : 16;
        NRuby *p = realloc(*out, next * sizeof(*p));
        if (!p) return -1;
        *out = p; *cap = next;
    }
    (*out)[(*count)++] = r;
    return 0;
}
/* Kanji run ending right before i, not reaching back into an earlier span. */
static size_t kanji_start(const uint32_t *cp, size_t i, size_t floor)
{
    size_t b = i;
    while (b > floor && novel_is_kanji(cp[b - 1]) && i - b <= NR_BASE_MAX) --b;
    return b;
}
/* Closing 》 for ruby text starting at from: non-empty, no 《, within max. */
static size_t ruby_close(const uint32_t *cp, size_t n, size_t from, size_t max)
{
    for (size_t k = from; k < n && k - from <= max; ++k) {
        if (cp[k] == CLOSE) return k > from ? k : (size_t)-1;
        if (cp[k] == OPEN) return (size_t)-1;
    }
    return (size_t)-1;
}
static int warning(const uint32_t *cp, NRuby r)
{
    size_t base = r.base_end - r.base, ruby = r.ruby_end - r.ruby;
    if (r.kind == NR_EMPHASIS) return NR_MAGENTA;
    int kana = 1;
    for (size_t i = r.ruby; i < r.ruby_end; ++i) kana &= novel_is_kana(cp[i]);
    if (base > NR_NARO_BASE || ruby > (kana ? NR_NARO_KANA : NR_NARO_RUBY)) return NR_RED;
    return r.kind == NR_PAREN ? NR_MAGENTA : NR_OK;
}
size_t novel_ruby_parse(const uint32_t *cp, size_t n, NRuby **out, size_t *cap)
{
    size_t count = 0, floor = 0, i = 0;
    while (i < n) {
        uint32_t c = cp[i];
        NRuby r = {0};
        int found = 0;
        if (is_bar(c)) {
            if (i + 1 < n && (cp[i + 1] == OPEN || cp[i + 1] == '(' || cp[i + 1] == 0xff08)) {
                r = (NRuby){NR_ESCAPE, i, i + 2, i + 1, i + 2, i + 2, i + 2, NR_OK};
                found = 1;
            } else {
                size_t j = i + 1;
                while (j < n && j - i - 1 <= NR_BASE_MAX && cp[j] != OPEN && !is_bar(cp[j])) ++j;
                if (j < n && cp[j] == OPEN && j > i + 1 && j - i - 1 <= NR_BASE_MAX) {
                    size_t k = ruby_close(cp, n, j + 1, NR_RUBY_MAX);
                    if (k != (size_t)-1) {
                        r = (NRuby){NR_BAR, i, k + 1, i + 1, j, j + 1, k, NR_OK};
                        found = 1;
                    }
                }
            }
        } else if (c == OPEN && i + 1 < n && cp[i + 1] == OPEN) {
            size_t k = i + 2;
            while (k + 1 < n && k - (i + 2) <= NR_EMPHASIS_MAX && !(cp[k] == CLOSE && cp[k + 1] == CLOSE)) ++k;
            if (k + 1 < n && cp[k] == CLOSE && cp[k + 1] == CLOSE && k > i + 2 && k - (i + 2) <= NR_EMPHASIS_MAX) {
                r = (NRuby){NR_EMPHASIS, i, k + 2, i + 2, k, k, k, NR_OK};
                found = 1;
            }
        } else if (c == OPEN) {
            size_t b = kanji_start(cp, i, floor);
            if (b < i && i - b <= NR_BASE_MAX) {
                size_t k = ruby_close(cp, n, i + 1, NR_RUBY_MAX);
                if (k != (size_t)-1) {
                    r = (NRuby){NR_KANJI, b, k + 1, b, i, i + 1, k, NR_OK};
                    found = 1;
                }
            }
        } else if (c == '(' || c == 0xff08) {
            uint32_t close = c == '(' ? ')' : 0xff09;
            size_t b = kanji_start(cp, i, floor);
            if (b < i && i - b <= NR_BASE_MAX) {
                size_t k = i + 1;
                while (k < n && k - i - 1 <= NR_PAREN_MAX && novel_is_kana(cp[k])) ++k;
                if (k < n && cp[k] == close && k > i + 1 && k - i - 1 <= NR_PAREN_MAX) {
                    r = (NRuby){NR_PAREN, b, k + 1, b, i, i + 1, k, NR_OK};
                    found = 1;
                }
            }
        }
        if (!found) { ++i; continue; }
        r.warn = warning(cp, r);
        if (add(out, cap, &count, r)) return (size_t)-1;
        i = floor = r.end;
    }
    return count;
}
