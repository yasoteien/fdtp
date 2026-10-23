// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVEL_RUBY_H
#define FDTP_NOVEL_RUBY_H
/* Ruby notation of Shosetsuka ni Naro and Kakuyomu (docs/FILER_PLAN.md 8.1),
 * read for display only; the file keeps the notation as written.
 *   ｜親《ルビ》 |親《ルビ》  base from the bar to 《
 *   漢字《ルビ》              the kanji (with 々〆ヶ) right before 《
 *   漢字（かな） 漢字(かな)  Naro only: kana-only text after kanji
 *   《《傍点》》              Kakuyomu only: emphasis dots
 *   ｜《 ｜( ｜（            the bar is hidden and the symbol is literal
 * Spans never cross a line end: the parser sees one line at a time. */
#include <stddef.h>
#include <stdint.h>

typedef enum { NR_BAR, NR_KANJI, NR_PAREN, NR_EMPHASIS, NR_ESCAPE } NRubyKind;
enum { NR_OK, NR_MAGENTA, NR_RED };
/* Limits. Longer bases/rubies are left as plain text: Kakuyomu's maximum
 * (base 20, ruby 50) for the bar and kanji forms, Naro's kana limit (20) for
 * the bracket form, and (an assumption) 100 characters for emphasis. */
#define NR_BASE_MAX 20
#define NR_RUBY_MAX 50
#define NR_PAREN_MAX 20
#define NR_EMPHASIS_MAX 100
/* Naro's limits (a users' survey): base 10, ruby 10, or 20 if kana only. */
#define NR_NARO_BASE 10
#define NR_NARO_RUBY 10
#define NR_NARO_KANA 20

typedef struct {
    NRubyKind kind;
    size_t start, end;           /* the whole notation, code point indices */
    size_t base, base_end;       /* base text (escape: the literal symbol) */
    size_t ruby, ruby_end;       /* ruby text; empty for emphasis/escape */
    int warn;                    /* NR_OK, NR_MAGENTA (one site only), NR_RED (over Naro's limits) */
} NRuby;

int novel_is_kanji(uint32_t c);
int novel_is_kana(uint32_t c);
/* Spans in order, not overlapping. *out is grown with realloc (owned by the
 * caller, *cap its capacity). Returns the count, or (size_t)-1 when out of memory. */
size_t novel_ruby_parse(const uint32_t *cp, size_t n, NRuby **out, size_t *cap);
#endif
