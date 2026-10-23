// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Publisher format and the Web/manuscript forms (novel_format.h). Rules 2-5
 * replay syosetu_format.pl step by step: each step is Perl's s/A/B/g (left
 * to right, not overlapping) on the line, logged when it matched, and the
 * line end of /！　\n/ is the end of the line. */
#include "i18n.h"
#include "novel_format.h"
#include "novel_ruby.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *const novel_format_names[NOVEL_FORMAT_COUNT] = {
#define NF_NAME(name, on, help) #name,
    NOVEL_FORMAT_RULES(NF_NAME)
#undef NF_NAME
};
const int novel_format_defaults[NOVEL_FORMAT_COUNT] = {
#define NF_ON(name, on, help) on,
    NOVEL_FORMAT_RULES(NF_ON)
#undef NF_ON
};

/* ---- strings and the log ---------------------------------------------- */
typedef struct { char *p; size_t n, cap; int fail; } Str;
static void put(Str *s, const char *t, size_t n)
{
    if (s->fail) return;
    if (s->n + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 64;
        while (cap < s->n + n + 1) cap *= 2;
        char *p = realloc(s->p, cap);
        if (!p) { s->fail = 1; return; }
        s->p = p; s->cap = cap;
    }
    if (n) memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void puts_(Str *s, const char *t) { put(s, t, strlen(t)); }
static void putcp(Str *s, uint32_t c) { char b[4]; put(s, b, novel_encode(c, b)); }

void nf_log_free(NFLog *log)
{
    for (size_t i = 0; i < log->count; ++i) free(log->items[i].text);
    free(log->items);
    memset(log, 0, sizeof(*log));
}
static void note(NFLog *log, size_t line, const char *text)
{
    if (!log || log->failed) return;
    if (log->count == log->cap) {
        size_t cap = log->cap ? log->cap * 2 : 32;
        NFMessage *p = realloc(log->items, cap * sizeof(*p));
        if (!p) { log->failed = 1; return; }
        log->items = p; log->cap = cap;
    }
    char *t = strdup(text);
    if (!t) { log->failed = 1; return; }
    log->items[log->count++] = (NFMessage){line, t};
}

/* ---- classifying lines -------------------------------------------------- */
static int is_space(uint32_t c) { return c == ' ' || c == 0x3000 || c == '\t'; }
int novel_blank_line(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t next;
        if (!is_space(novel_decode(s, n, i, &next))) return 0;
        i = next;
    }
    return 1;
}
static int is_letter(uint32_t c)
{
    if (novel_is_kana(c) || novel_is_kanji(c) || c == 0x30fc || c == 0xfffd) return 1;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return 1;
    if ((c >= 0xff10 && c <= 0xff19) || (c >= 0xff21 && c <= 0xff3a) || (c >= 0xff41 && c <= 0xff5a)) return 1;
    /* Latin with accents, Greek, Cyrillic. */
    return (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) || (c >= 0x370 && c <= 0x52f);
}
/* Brackets and the marks of speech or a pause: a line of 「……」 or ！？ is
 * dialogue, not a scene break. */
static int not_a_break(uint32_t c)
{
    static const uint32_t marks[] = {
        0x300c, 0x300d, 0x300e, 0x300f, 0xff08, 0xff09, '(', ')', 0x3010, 0x3011, 0x3008, 0x3009,
        0x300a, 0x300b, 0x3014, 0x3015, 0xff3b, 0xff3d, '[', ']', 0xff5b, 0xff5d, '{', '}', '<', '>',
        0x201c, 0x201d, 0x2018, 0x2019, '"', '\'', 0xff62, 0xff63, 0x3001, 0x3002, 0xff0c, 0xff0e,
        ',', '.', 0xff01, 0xff1f, '!', '?', 0x2026, 0x2025, 0x30fb, 0xff65
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) if (marks[i] == c) return 1;
    return 0;
}
int novel_scene_line(const char *s, size_t n)
{
    int count = 0;
    for (size_t i = 0; i < n;) {
        size_t next;
        uint32_t c = novel_decode(s, n, i, &next);
        i = next;
        if (is_space(c)) continue;
        if (c < 0x20 || is_letter(c) || not_a_break(c) || ++count > 10) return 0;
    }
    return count > 0;
}
static int line_is(const NLine *l, const char *text) { return l->len == strlen(text) && !memcmp(l->s, text, l->len); }
/* 【本文（N行）】 with N in ASCII or full-width digits. */
static int body_mark(const NLine *l)
{
    static const char head[] = "【本文（", tail[] = "行）】"; /* i18n:keep */
    size_t h = sizeof(head) - 1, t = sizeof(tail) - 1;
    if (l->len < h + t + 1 || memcmp(l->s, head, h) || memcmp(l->s + l->len - t, tail, t)) return 0;
    for (size_t i = h; i < l->len - t;) {
        size_t next;
        uint32_t c = novel_decode(l->s, l->len - t, i, &next);
        if (!((c >= '0' && c <= '9') || (c >= 0xff10 && c <= 0xff19))) return 0;
        i = next;
    }
    return 1;
}
size_t novel_header(const NovelDoc *d, size_t *title)
{
    if (d->count < 2 || !line_is(&d->lines[0], "【タイトル】")) return 0; /* i18n:keep */
    for (size_t i = 1; i < d->count && i < 200; ++i)
        if (body_mark(&d->lines[i])) { if (title) *title = 1; return i + 1; }
    return 0;
}

/* ---- one line ------------------------------------------------------------ */
/* Perl's s/from/to/g on one piece; returns the number of replacements. */
static int replace_all(Str *s, const char *from, const char *to)
{
    size_t fn = strlen(from), tn = strlen(to);
    if (s->n < fn) return 0;
    Str out = {0};
    int hits = 0;
    size_t i = 0;
    while (i < s->n) {
        if (i + fn <= s->n && !memcmp(s->p + i, from, fn)) { put(&out, to, tn); i += fn; ++hits; }
        else { put(&out, s->p + i, 1); ++i; }
    }
    if (!hits) { free(out.p); return 0; }
    if (out.fail) { free(out.p); s->fail = 1; return hits; }
    if (!out.p) put(&out, "", 0);
    free(s->p);
    *s = out;
    return hits;
}
/* s/from\n/to\n/: the end of the line. */
static int replace_end(Str *s, const char *from, const char *to)
{
    size_t fn = strlen(from), tn = strlen(to);
    if (s->n < fn || memcmp(s->p + s->n - fn, from, fn)) return 0;
    s->n -= fn;
    put(s, to, tn);
    return 1;
}
/* The line in pieces: even indices outside, odd ones the text inside 《》. */
#define MAX_PIECES 513
typedef struct { Str piece[MAX_PIECES]; size_t count; } Pieces;
static int pieces_fail(const Pieces *p)
{
    for (size_t i = 0; i < p->count; ++i) if (p->piece[i].fail) return 1;
    return 0;
}
static int step(Pieces *p, const char *from, const char *to, int end)
{
    int hits = 0;
    for (size_t i = 0; i < p->count; i += 2) {
        if (end) { if (i + 1 == p->count) hits += replace_end(&p->piece[i], from, to); }
        else hits += replace_all(&p->piece[i], from, to);
    }
    return hits;
}
static void stepped(Pieces *p, const char *from, const char *to, int end, NFLog *log, size_t line, const char *message)
{
    if (step(p, from, to, end)) note(log, line, message);
}
/* Rule 11: a run of … ・ ． with a … or two dots becomes an even number of
 * … (the dots counted three to one, rounded up). */
static int even_leaders(Str *s)
{
    Str out = {0};
    int changed = 0;
    for (size_t i = 0; i < s->n;) {
        size_t next, j = i;
        uint32_t c = novel_decode(s->p, s->n, i, &next);
        if (c != 0x2026 && c != 0x30fb && c != 0xff0e) { put(&out, s->p + i, next - i); i = next; continue; }
        size_t leaders = 0, dots = 0;
        while (j < s->n) {
            size_t k;
            uint32_t d = novel_decode(s->p, s->n, j, &k);
            if (d == 0x2026) ++leaders;
            else if (d == 0x30fb || d == 0xff0e) ++dots;
            else break;
            j = k;
        }
        if (!leaders && dots < 2) { put(&out, s->p + i, j - i); i = j; continue; }
        size_t want = leaders + (dots + 2) / 3;
        if (want % 2) ++want;
        Str run = {0};
        for (size_t k = 0; k < want; ++k) puts_(&run, "…");
        if (run.n != j - i || memcmp(run.p, s->p + i, run.n)) changed = 1;
        put(&out, run.p ? run.p : "", run.n);
        if (run.fail) out.fail = 1;
        free(run.p);
        i = j;
    }
    if (out.fail) { free(out.p); s->fail = 1; return 0; }
    if (!changed) { free(out.p); return 0; }
    free(s->p);
    *s = out;
    return 1;
}
static int dash_like(uint32_t c) { return c == 0x2015 || c == 0x2014 || c == 0x2500 || c == 0x30fc || c == 0xff0d; }
static int true_dash(uint32_t c) { return c == 0x2015 || c == 0x2014 || c == 0x2500; }
/* Rule 10: a run of dashes (― — ─, or two or more ー －) becomes an even
 * number of ―. ー right after kana are the long vowel and stay (ァーーー！). */
static int dashes(Str *s, uint32_t before)
{
    Str out = {0};
    int changed = 0;
    uint32_t prev = before;
    for (size_t i = 0; i < s->n;) {
        size_t next;
        uint32_t c = novel_decode(s->p, s->n, i, &next);
        if (!dash_like(c)) { put(&out, s->p + i, next - i); i = next; prev = c; continue; }
        size_t j = i;
        if (novel_is_kana(prev) && prev != 0x30fc)
            while (j < s->n && novel_decode(s->p, s->n, j, NULL) == 0x30fc) j = novel_next(s->p, s->n, j);
        put(&out, s->p + i, j - i);
        size_t start = j, len = 0;
        int real = 0;
        uint32_t last = c;
        while (j < s->n) {
            size_t k;
            uint32_t d = novel_decode(s->p, s->n, j, &k);
            if (!dash_like(d)) break;
            real |= true_dash(d);
            ++len; last = d; j = k;
        }
        if (len && (real || len >= 2)) {
            size_t want = len % 2 ? len + 1 : len;
            Str run = {0};
            for (size_t k = 0; k < want; ++k) puts_(&run, "―");
            if (run.n != j - start || memcmp(run.p, s->p + start, run.n)) changed = 1;
            put(&out, run.p ? run.p : "", run.n);
            if (run.fail) out.fail = 1;
            free(run.p);
        } else put(&out, s->p + start, j - start);
        prev = last;
        i = j;
    }
    if (out.fail) { free(out.p); s->fail = 1; return 0; }
    if (!changed) { free(out.p); return 0; }
    free(s->p);
    *s = out;
    return 1;
}
static int zenkaku(Str *s)
{
    Str out = {0};
    int changed = 0;
    for (size_t i = 0; i < s->n; ++i) {
        unsigned char c = (unsigned char)s->p[i];
        if (c >= 0x21 && c <= 0x7e) { putcp(&out, 0xff01u + (c - 0x21u)); changed = 1; }
        else put(&out, s->p + i, 1);
    }
    if (out.fail) { free(out.p); s->fail = 1; return 0; }
    if (!changed) { free(out.p); return 0; }
    free(s->p);
    *s = out;
    return 1;
}
static int drop_half_spaces(Str *s)
{
    size_t w = 0;
    for (size_t i = 0; i < s->n; ++i) if (s->p[i] != ' ') s->p[w++] = s->p[i];
    int changed = w != s->n;
    s->n = w;
    if (s->p) s->p[w] = 0;
    return changed;
}
/* Cut the line into pieces around the text in 《》 of ruby and emphasis. */
static int cut(Pieces *p, const Str *line)
{
    size_t n = 0;
    for (size_t i = 0; i < line->n; i = novel_next(line->p, line->n, i)) ++n;
    uint32_t *cp = malloc((n + 1) * sizeof(*cp));
    size_t *at = malloc((n + 1) * sizeof(*at));
    NRuby *spans = NULL; size_t cap = 0;
    if (!cp || !at) { free(cp); free(at); return -1; }
    n = 0;
    for (size_t i = 0; i < line->n;) { at[n] = i; cp[n++] = novel_decode(line->p, line->n, i, &i); }
    at[n] = line->n;
    size_t count = novel_ruby_parse(cp, n, &spans, &cap);
    int rc = count == (size_t)-1 ? -1 : 0;
    size_t from = 0;
    p->count = 0;
    for (size_t k = 0; !rc && k < count; ++k) {
        size_t a, b;
        if (spans[k].kind == NR_BAR || spans[k].kind == NR_KANJI) { a = spans[k].ruby; b = spans[k].ruby_end; }
        else if (spans[k].kind == NR_EMPHASIS) { a = spans[k].base; b = spans[k].base_end; }
        else continue;
        if (p->count + 3 > MAX_PIECES) break;
        put(&p->piece[p->count++], line->p + at[from], at[a] - at[from]);
        put(&p->piece[p->count++], line->p + at[a], at[b] - at[a]);
        from = b;
    }
    if (!rc) put(&p->piece[p->count++], line->p + at[from], at[n] - at[from]);
    free(spans); free(cp); free(at);
    return rc || pieces_fail(p) ? -1 : 0;
}
static int indent_ok(const Str *s)
{
    if (!s->n) return 0;
    uint32_t c = novel_decode(s->p, s->n, 0, NULL);
    return !(is_space(c) || c == 0x300c || c == 0x300e || c == 0xff08 || c == '(' || c == 0xff62);
}
char *novel_format_line(const char *s, size_t n, const int *rules, int header, size_t lineno,
                        NFLog *log, size_t *outlen)
{
    Str line = {0};
    put(&line, s, n);
    if (rules[NF_ZENKAKU] && zenkaku(&line)) note(log, lineno, T("の半角を全角に修正"));
    int scene = !header && rules[NF_SCENE] && novel_scene_line(line.p, line.n);
    if (!scene && !header && rules[NF_HALFSPACE] && drop_half_spaces(&line)) note(log, lineno, T("の半角の空白を除きました"));
    if (scene || line.fail) {
        if (line.fail) { free(line.p); return NULL; }
        *outlen = line.n;
        return line.p;
    }
    Pieces *p = calloc(1, sizeof(*p));
    if (!p || cut(p, &line)) {
        if (p) for (size_t i = 0; i < p->count; ++i) free(p->piece[i].p);
        free(p); free(line.p);
        return NULL;
    }
    free(line.p);
    int two = !header && rules[NF_LEADER2];
    if (rules[NF_LEADER])
        stepped(p, "・・・", two ? "……" : "…", 0, log, lineno, two ? T("の・・・を……に修正") : T("の・・・を…に修正")); /* i18n:keep */
    int keep = rules[NF_EXCLAIM] && rules[NF_EXCLAIM_KEEP];
    if (rules[NF_EXCLAIM]) {
        stepped(p, "！", "！　", 0, log, lineno, T("の！を！　に修正")); /* i18n:keep */
        stepped(p, "？", "？　", 0, log, lineno, T("の？を？　に修正")); /* i18n:keep */
    }
    if (keep) {
        stepped(p, "！　", "！", 1, log, lineno, T("は行末！だったので元に戻しました")); /* i18n:keep */
        stepped(p, "？　", "？", 1, log, lineno, T("は行末？だったので元に戻しました")); /* i18n:keep */
    }
    if (rules[NF_SPACES]) stepped(p, "　　", "　", 0, log, lineno, T("で空白が連続していたので一つに戻しました")); /* i18n:keep */
    if (keep) {
        stepped(p, "！　！　！", "！！！", 0, log, lineno, T("は行末は感嘆符が三つ連続していたので元に戻しました")); /* i18n:keep */
        stepped(p, "！　！", "！！", 0, log, lineno, T("は行末は感嘆符が二つ連続していたので元に戻しました")); /* i18n:keep */
        stepped(p, "！　？", "！？", 0, log, lineno, T("は台詞の！？だったので元に戻しました")); /* i18n:keep */
        stepped(p, "？　！", "？！", 0, log, lineno, T("は台詞の？！だったので元に戻しました")); /* i18n:keep */
        stepped(p, "！　」", "！」", 0, log, lineno, T("は台詞の末尾！だったので元に戻しました")); /* i18n:keep */
        stepped(p, "！　』", "！』", 0, log, lineno, T("は台詞の末尾！だったので元に戻しました")); /* i18n:keep */
        stepped(p, "！　）", "！）", 0, log, lineno, T("はカッコの末尾！だったので元に戻しました")); /* i18n:keep */
        stepped(p, "？　」", "？」", 0, log, lineno, T("は台詞の末尾？だったので元に戻しました")); /* i18n:keep */
        stepped(p, "？　』", "？』", 0, log, lineno, T("は台詞の末尾？だったので元に戻しました")); /* i18n:keep */
        stepped(p, "？　）", "？）", 0, log, lineno, T("はカッコの末尾？だったので元に戻しました")); /* i18n:keep */
    }
    if (!header) {
        int hit = 0;
        if (rules[NF_LEADER_EVEN]) {
            for (size_t i = 0; i < p->count; i += 2) hit |= even_leaders(&p->piece[i]);
            if (hit) note(log, lineno, T("の三点リーダを偶数個にそろえました"));
        }
        if (rules[NF_DASH]) {
            hit = 0;
            /* A piece after 《》 text starts after 》 (not kana). */
            for (size_t i = 0; i < p->count; i += 2) hit |= dashes(&p->piece[i], 0);
            if (hit) note(log, lineno, T("のダッシュを――にそろえました"));
        }
        if (rules[NF_PERIOD]) {
            hit = step(p, "。」", "」", 0); /* i18n:keep */
            hit += step(p, "。』", "』", 0); /* i18n:keep */
            if (hit) note(log, lineno, T("の括弧の中の最後の。を取りました"));
        }
    }
    Str out = {0};
    for (size_t i = 0; i < p->count; ++i) { put(&out, p->piece[i].p ? p->piece[i].p : "", p->piece[i].n); }
    int failed = pieces_fail(p) || out.fail;
    for (size_t i = 0; i < p->count; ++i) free(p->piece[i].p);
    free(p);
    if (!failed && !header && rules[NF_INDENT] && indent_ok(&out)) {
        Str indented = {0};
        puts_(&indented, "　"); /* i18n:keep */
        put(&indented, out.p, out.n);
        free(out.p);
        out = indented;
        note(log, lineno, T("の段落の頭を字下げしました"));
    }
    if (failed || out.fail) { free(out.p); return NULL; }
    if (!out.p) put(&out, "", 0);
    if (out.fail) { free(out.p); return NULL; }
    *outlen = out.n;
    return out.p;
}

/* ---- documents ------------------------------------------------------------ */
static void undo_partial(NovelDoc *d, int edited)
{
    NPos p;
    novel_doc_group(d);
    if (edited) novel_doc_undo(d, &p);
}
long novel_format_doc(NovelDoc *d, const int *rules, NFLog *log, size_t *cursor_line)
{
    size_t body = novel_header(d, NULL);
    long changed = 0;
    (void)cursor_line; /* lines stay where they are */
    novel_doc_group(d);
    for (size_t i = 0; i < d->count; ++i) {
        size_t n;
        char *line = novel_format_line(d->lines[i].s, d->lines[i].len, rules, i < body, i + 1, log, &n);
        if (!line) { undo_partial(d, changed > 0); return -1; }
        if (n != d->lines[i].len || memcmp(line, d->lines[i].s, n)) {
            int ok = 1;
            novel_doc_delete(d, (NPos){i, 0}, (NPos){i, d->lines[i].len}, &ok);
            if (ok) novel_doc_insert(d, (NPos){i, 0}, line, n, &ok);
            ++changed;
            if (!ok) { free(line); undo_partial(d, 1); return -1; }
        }
        free(line);
    }
    novel_doc_group(d);
    return changed;
}
/* The body (after the Kakuyomu header) without the last empty line, which
 * is the line end of the last line. */
static size_t body_lines(const NovelDoc *d, size_t body, int *terminator)
{
    size_t m = d->count - body;
    *terminator = m > 0 && d->count > 1 && d->lines[d->count - 1].len == 0;
    return m - (size_t)*terminator;
}
typedef struct { const char **s; size_t *n; size_t count, cap; int fail; } Out;
static void out_push(Out *o, const char *s, size_t n)
{
    if (o->fail) return;
    if (o->count == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        const char **a = realloc(o->s, cap * sizeof(*a));
        if (!a) { o->fail = 1; return; }
        o->s = a;
        size_t *b = realloc(o->n, cap * sizeof(*b));
        if (!b) { o->fail = 1; return; }
        o->n = b; o->cap = cap;
    }
    o->s[o->count] = s; o->n[o->count++] = n;
}
/* Replace the body with the lines of o (plus the final line end). */
static long replace_body(NovelDoc *d, size_t body, int terminator, Out *o, long changes)
{
    if (o->fail) return -1;
    if (terminator) out_push(o, "", 0);
    if (o->fail) return -1;
    Str text = {0};
    for (size_t i = 0; i < o->count; ++i) { if (i) put(&text, "\n", 1); put(&text, o->s[i], o->n[i]); }
    if (text.fail) { free(text.p); return -1; }
    size_t old_n;
    char *old = novel_doc_copy(d, (NPos){body, 0}, novel_doc_end(d), &old_n);
    if (!old) { free(text.p); return -1; }
    int same = old_n == text.n && (!old_n || !memcmp(old, text.p, old_n));
    free(old);
    if (same) { free(text.p); return 0; }
    int ok = 1;
    novel_doc_group(d);
    novel_doc_delete(d, (NPos){body, 0}, novel_doc_end(d), &ok);
    if (ok && text.n) novel_doc_insert(d, (NPos){body, 0}, text.p, text.n, &ok);
    free(text.p);
    if (!ok) { undo_partial(d, 1); return -1; }
    novel_doc_group(d);
    return changes ? changes : 1;
}
long novel_form_manuscript(NovelDoc *d, int scene, NFLog *log, size_t *cursor_line)
{
    size_t body = novel_header(d, NULL);
    int terminator;
    size_t m = body_lines(d, body, &terminator);
    const NLine *B = d->lines + body;
    Out o = {0};
    long changes = 0;
    size_t cursor = cursor_line ? *cursor_line : 0, moved = cursor;
    int have_prev = 0, prev_scene = 0;
    for (size_t i = 0; i < m;) {
        if (novel_blank_line(B[i].s, B[i].len)) {
            size_t j = i;
            while (j < m && novel_blank_line(B[j].s, B[j].len)) ++j;
            size_t run = j - i;
            int next_scene = scene && j < m && novel_scene_line(B[j].s, B[j].len);
            int keep = run >= 2 || (have_prev && prev_scene) || next_scene;
            if (cursor >= body + i && cursor < body + j) moved = body + o.count;
            if (keep) out_push(&o, "", 0);
            char message[96];
            if (run >= 2) {
                snprintf(message, sizeof(message), T("から空行が%zu行続いていたので一つにしました"), run);
                note(log, body + i + 1, message); ++changes;
            } else if (!keep) { note(log, body + i + 1, T("の空行を除きました")); ++changes; }
            else if (B[i].len) ++changes; /* spaces only: now empty */
            i = j;
            continue;
        }
        int is_scene = scene && novel_scene_line(B[i].s, B[i].len);
        if (i > 0 && !novel_blank_line(B[i - 1].s, B[i - 1].len) && (is_scene || prev_scene)) {
            out_push(&o, "", 0);
            if (is_scene) note(log, body + i + 1, T("の区切りの前に空行を入れました"));
            else note(log, body + i, T("の区切りの後に空行を入れました"));
            ++changes;
        }
        if (cursor == body + i) moved = body + o.count;
        out_push(&o, B[i].s, B[i].len);
        have_prev = 1; prev_scene = is_scene;
        ++i;
    }
    if (cursor >= body + m) moved = body + o.count + (cursor - body - m);
    long rc = replace_body(d, body, terminator, &o, changes);
    free(o.s); free(o.n);
    if (rc > 0 && cursor_line) *cursor_line = moved < d->count ? moved : d->count - 1;
    return rc;
}
long novel_form_web(NovelDoc *d, NFLog *log, size_t *cursor_line)
{
    size_t body = novel_header(d, NULL);
    int terminator;
    size_t m = body_lines(d, body, &terminator);
    const NLine *B = d->lines + body;
    Out o = {0};
    long changes = 0;
    size_t cursor = cursor_line ? *cursor_line : 0, moved = cursor;
    for (size_t i = 0; i < m; ++i) {
        if (cursor == body + i) moved = body + o.count;
        out_push(&o, B[i].s, B[i].len);
        if (!novel_blank_line(B[i].s, B[i].len) && i + 1 < m && !novel_blank_line(B[i + 1].s, B[i + 1].len)) {
            out_push(&o, "", 0);
            note(log, body + i + 1, T("の後に空行を入れました"));
            ++changes;
        }
    }
    if (cursor >= body + m) moved = body + o.count + (cursor - body - m);
    long rc = replace_body(d, body, terminator, &o, changes);
    free(o.s); free(o.n);
    if (rc > 0 && cursor_line) *cursor_line = moved < d->count ? moved : d->count - 1;
    return rc;
}

/* ---- format.log ------------------------------------------------------------ */
int novel_format_log_append(const char *log_path, const char *command, const char *file, const NFLog *log)
{
    if (!log_path || !*log_path) return -1;
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", log_path);
    for (char *p = dir + 1; *p; ++p)
        if (*p == '/') { *p = 0; mkdir(dir, 0700); *p = '/'; }
    FILE *f = fopen(log_path, "a");
    if (!f) return -1;
    static const char rule[] = "=======================================================\n";
    char host[256] = "";
    if (gethostname(host, sizeof(host) - 1)) host[0] = 0;
    host[sizeof(host) - 1] = 0;
    time_t now = time(NULL);
    struct tm tm;
    char when[64] = "";
    if (localtime_r(&now, &tm)) strftime(when, sizeof(when), "%m/%d/%y %H:%M:%S", &tm);
    const char *slash = strrchr(file, '/');
    char pwd[4096];
    if (slash) snprintf(pwd, sizeof(pwd), "%.*s", slash == file ? 1 : (int)(slash - file), file);
    else if (!getcwd(pwd, sizeof(pwd))) pwd[0] = 0;
    fputs(rule, f);
    fprintf(f, "HOST : %s\n", host);
    fprintf(f, "fdtp  %s\n\n", command);
    fprintf(f, "Executed time is %s\n", when);
    if (getenv("OSTYPE")) fprintf(f, "OSTYPE : %s\n", getenv("OSTYPE"));
    if (getenv("USER")) fprintf(f, "USER : %s\n", getenv("USER"));
    fprintf(f, "PWD : %s\n", pwd);
    fprintf(f, "Change TXT : %s\n", slash ? slash + 1 : file);
    for (size_t i = 0; i < log->count; ++i) fprintf(f, T("%zu行目%s\n"), log->items[i].line, log->items[i].text);
    fputs(rule, f);
    return fclose(f) ? -1 : 0;
}
