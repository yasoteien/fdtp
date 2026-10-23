// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _XOPEN_SOURCE 700
/* Unit checks for the novel editor's text model, ruby parser and layout
 * (tests/check_novel.py): novel-unit TMPDIR. Exits non-zero on failure. */
#include "novel_doc.h"
#include "novel_ruby.h"
#include "novel_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(unsigned n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng % n); }

/* ---- document vs a flat-string model --------------------------------- */
static char *all(const NovelDoc *d, size_t *n) { return novel_doc_copy(d, (NPos){0, 0}, novel_doc_end(d), n); }
static size_t to_offset(const NovelDoc *d, NPos p)
{
    size_t o = 0;
    for (size_t i = 0; i < p.line; ++i) o += d->lines[i].len + 1;
    return o + p.off;
}
static NPos random_pos(const NovelDoc *d)
{
    NPos p = {rnd((unsigned)d->count), 0};
    size_t len = d->lines[p.line].len;
    p.off = len ? rnd((unsigned)len + 1) : 0;
    return novel_doc_clamp(d, p);
}
static void model_check(const NovelDoc *d, const char *model, const char *where)
{
    size_t n;
    char *t = all(d, &n);
    CHECK(t && n == strlen(model) && !memcmp(t, model, n), "%s: doc '%.40s' model '%.40s'", where, t ? t : "", model);
    free(t);
}
static void doc_random(void)
{
    static const char *pieces[] = {"a", "あ", "\n", "｜", "《", "》", "漢字", "\n\n", "x\ny", "😀"};
    NovelDoc d; novel_doc_init(&d);
    CHECK(!novel_doc_set_text(&d, "", 0), "empty");
    char **hist = calloc(20001, sizeof(char *));
    size_t top = 0, cur = 0, clean = 0;
    hist[0] = strdup("");
    for (int step = 0; step < 6000; ++step) {
        unsigned op = rnd(10);
        if (op < 4 || op == 9) {
            novel_doc_group(&d);
            int ops = op == 9 ? 2 : 1;
            for (int k = 0; k < ops; ++k) {
                const char *p = pieces[rnd(10)];
                NPos at = random_pos(&d);
                int ok; novel_doc_insert(&d, at, p, strlen(p), &ok); CHECK(ok, "insert");
            }
        } else if (op < 7) {
            NPos a = random_pos(&d), b = random_pos(&d);
            if (!npos_cmp(a, b)) continue;
            novel_doc_group(&d);
            int ok; novel_doc_delete(&d, a, b, &ok); CHECK(ok, "delete");
        } else if (op == 7) {
            NPos c;
            int did = novel_doc_undo(&d, &c);
            CHECK(did == (cur > 0), "undo availability at %d", step);
            if (did) {
                --cur; size_t n; char *t = all(&d, &n);
                CHECK(npos_cmp(c, novel_doc_end(&d)) <= 0, "undo caret in range");
                CHECK(to_offset(&d, c) <= n, "undo caret");
                free(t);
            }
            model_check(&d, hist[cur], "undo");
            CHECK(novel_doc_modified(&d) == (cur != clean), "modified after undo (%zu %zu)", cur, clean);
            continue;
        } else {
            if (rnd(2)) {
                NPos c;
                int did = novel_doc_redo(&d, &c);
                CHECK(did == (cur < top), "redo availability");
                if (did) ++cur;
                model_check(&d, hist[cur], "redo");
            } else { novel_doc_mark_clean(&d); clean = cur; }
            CHECK(novel_doc_modified(&d) == (cur != clean), "modified");
            continue;
        }
        for (size_t i = cur + 1; i <= top; ++i) { free(hist[i]); hist[i] = NULL; }
        if (clean > cur) clean = (size_t)-1;
        size_t n; hist[++cur] = all(&d, &n); top = cur;
        CHECK(novel_doc_modified(&d) == (cur != clean), "modified after edit");
        if (cur == 20000) break;
    }
    /* Undo everything, then redo everything. */
    NPos c;
    while (novel_doc_undo(&d, &c)) --cur;
    CHECK(cur == 0, "undo to the start"); model_check(&d, "", "all undone");
    while (novel_doc_redo(&d, &c)) ++cur;
    CHECK(cur == top, "redo to the end"); model_check(&d, hist[top], "all redone");
    for (size_t i = 0; i <= top; ++i) free(hist[i]);
    free(hist);
    novel_doc_free(&d);
}

/* ---- files ----------------------------------------------------------- */
static void write_bytes(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb"); fwrite(data, 1, n, f); fclose(f);
}
static size_t read_bytes(const char *path, char *buffer, size_t cap)
{
    FILE *f = fopen(path, "rb"); if (!f) return (size_t)-1;
    size_t n = fread(buffer, 1, cap, f); fclose(f); return n;
}
static void roundtrip(const char *dir, const char *name, const char *data, size_t n, JisEncoding enc, const char *text)
{
    char path[1024], back[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    write_bytes(path, data, n);
    NovelDoc d; novel_doc_init(&d);
    CHECK(!novel_doc_load(&d, path, 1 << 20), "%s: load", name);
    CHECK(d.encoding == enc, "%s: encoding %d", name, d.encoding);
    size_t len; char *t = all(&d, &len);
    size_t tn = !strcmp(name, "nul.txt") ? 4 : strlen(text);
    CHECK(len == tn && !memcmp(t, text, len), "%s: text '%s'", name, t);
    free(t);
    CHECK(!novel_doc_modified(&d), "%s: unmodified after load", name);
    CHECK(!novel_doc_save(&d, path, 0), "%s: save", name);
    size_t got = read_bytes(path, back, sizeof(back));
    CHECK(got == n && !memcmp(back, data, n), "%s: identical bytes after save (%zu/%zu)", name, got, n);
    novel_doc_free(&d);
}
static void files(const char *dir)
{
    roundtrip(dir, "lf.txt", "a\nb\n", 4, JIS_UTF8, "a\nb\n");
    roundtrip(dir, "crlf.txt", "a\r\nb\r\n", 6, JIS_UTF8, "a\nb\n");
    roundtrip(dir, "cr.txt", "a\rb", 3, JIS_UTF8, "a\nb");
    roundtrip(dir, "empty.txt", "", 0, JIS_UTF8, "");
    roundtrip(dir, "nl.txt", "\n", 1, JIS_UTF8, "\n");
    roundtrip(dir, "crlfonly.txt", "\r\n\r\n", 4, JIS_UTF8, "\n\n");
    roundtrip(dir, "bom.txt", "\xef\xbb\xbfあ\n", 7, JIS_UTF8, "あ\n");
    roundtrip(dir, "nul.txt", "a\0b\n", 4, JIS_UTF8, "a\0b\n");
    roundtrip(dir, "sjis.txt", "\x8a\xbf\x8e\x9a\r\n\x82\xa0", 8, JIS_SHIFT_JIS, "漢字\nあ");
    roundtrip(dir, "euc.txt", "\xb4\xc1\xbb\xfa\xa4\xce\n", 7, JIS_EUC_JP, "漢字の\n");
    /* Mixed line ends become the first one (documented). */
    char path[1024], back[64];
    snprintf(path, sizeof(path), "%s/mixed.txt", dir);
    write_bytes(path, "a\r\nb\nc", 6);
    NovelDoc d; novel_doc_init(&d);
    CHECK(!novel_doc_load(&d, path, 1 << 20) && !strcmp(d.newline, "\r\n"), "mixed: first line end");
    CHECK(!novel_doc_save(&d, path, 0), "mixed save");
    CHECK(read_bytes(path, back, sizeof(back)) == 7 && !memcmp(back, "a\r\nb\r\nc", 7), "mixed: normalised");
    /* Unrepresentable in Shift_JIS: -2 and the file is untouched; UTF-8 works. */
    snprintf(path, sizeof(path), "%s/sjis.txt", dir);
    novel_doc_free(&d); novel_doc_init(&d);
    CHECK(!novel_doc_load(&d, path, 1 << 20), "sjis reload");
    novel_doc_insert(&d, (NPos){0, 0}, "😀", 4, NULL);
    CHECK(novel_doc_save(&d, path, 0) == -2, "unrepresentable");
    CHECK(read_bytes(path, back, sizeof(back)) == 8, "untouched");
    CHECK(!novel_doc_save(&d, path, 1) && d.encoding == JIS_UTF8, "UTF-8 fallback");
    CHECK(read_bytes(path, back, sizeof(back)) == 4 + 6 + 2 + 3 && !memcmp(back, "😀漢字\r\nあ", 15), "UTF-8 bytes");
    novel_doc_free(&d);
    /* A read-only file is not replaced. */
    snprintf(path, sizeof(path), "%s/readonly.txt", dir);
    write_bytes(path, "ro\n", 3);
    chmod(path, 0444);
    novel_doc_init(&d);
    CHECK(!novel_doc_load(&d, path, 1 << 20), "readonly load");
    novel_doc_insert(&d, (NPos){0, 0}, "x", 1, NULL);
    CHECK(novel_doc_save(&d, path, 0) == -1, "readonly refused");
    CHECK(read_bytes(path, back, sizeof(back)) == 3 && !memcmp(back, "ro\n", 3), "readonly untouched");
    chmod(path, 0644);
    novel_doc_free(&d);
    /* Size limit and missing files. */
    novel_doc_init(&d);
    snprintf(path, sizeof(path), "%s/sjis.txt", dir);
    CHECK(novel_doc_load(&d, path, 4) == -1, "too big");
    snprintf(path, sizeof(path), "%s/missing.txt", dir);
    CHECK(novel_doc_load(&d, path, 1 << 20) == -1, "missing");
    novel_doc_free(&d);
}

/* ---- search ---------------------------------------------------------- */
static void search(void)
{
    NovelDoc d; novel_doc_init(&d);
    novel_doc_set_text(&d, "ab ab\ncab\n", 10);
    NPos s, e; int w;
    CHECK(novel_doc_find(&d, (NPos){0, 0}, "ab", 2, &s, &e, &w) && s.off == 0 && !w, "first");
    CHECK(novel_doc_find(&d, (NPos){0, 1}, "ab", 2, &s, &e, &w) && s.off == 3 && !w, "second");
    CHECK(novel_doc_find(&d, (NPos){0, 4}, "ab", 2, &s, &e, &w) && s.line == 1 && s.off == 1, "next line");
    CHECK(novel_doc_find(&d, (NPos){1, 2}, "ab", 2, &s, &e, &w) && s.line == 0 && s.off == 0 && w, "wrap");
    CHECK(novel_doc_find(&d, (NPos){0, 4}, "b a", 3, &s, &e, &w) && s.off == 1 && w, "wrap to the same line");
    CHECK(!novel_doc_find(&d, (NPos){0, 0}, "zz", 2, &s, &e, &w), "absent");
    CHECK(!novel_doc_find(&d, (NPos){0, 0}, "", 0, &s, &e, &w), "empty pattern");
    novel_doc_free(&d);
}

/* ---- ruby ------------------------------------------------------------ */
static size_t decode(const char *s, uint32_t *cp)
{
    size_t n = 0, len = strlen(s);
    for (size_t at = 0; at < len;) cp[n++] = novel_decode(s, len, at, &at);
    return n;
}
/* Expect: kind, base text, ruby text, warn; or kind -1 for no span. */
static void ruby(const char *text, int kind, const char *base, const char *rb, int warn)
{
    uint32_t cp[512], want[128];
    size_t n = decode(text, cp), cap = 0;
    NRuby *spans = NULL;
    size_t count = novel_ruby_parse(cp, n, &spans, &cap);
    if (kind < 0) { CHECK(count == 0, "'%s': no span expected, got %zu", text, count); free(spans); return; }
    CHECK(count >= 1, "'%s': span expected", text);
    if (count >= 1) {
        NRuby r = spans[0];
        CHECK((int)r.kind == kind, "'%s': kind %d", text, r.kind);
        size_t bn = decode(base, want);
        CHECK(r.base_end - r.base == bn && !memcmp(cp + r.base, want, bn * 4), "'%s': base", text);
        size_t rn = decode(rb, want);
        CHECK(r.ruby_end - r.ruby == rn && !memcmp(cp + r.ruby, want, rn * 4), "'%s': ruby", text);
        CHECK(r.warn == warn, "'%s': warn %d", text, r.warn);
    }
    free(spans);
}
static void rubies(void)
{
    ruby("｜本気《マジ》", NR_BAR, "本気", "マジ", NR_OK);
    ruby("|本気《マジ》", NR_BAR, "本気", "マジ", NR_OK);
    ruby("あ漢字《かんじ》", NR_KANJI, "漢字", "かんじ", NR_OK);
    ruby("人々《ひとびと》", NR_KANJI, "人々", "ひとびと", NR_OK);
    ruby("東京（とうきょう）", NR_PAREN, "東京", "とうきょう", NR_MAGENTA);
    ruby("東京(トーキョー)", NR_PAREN, "東京", "トーキョー", NR_MAGENTA);
    ruby("東京(Tokyo)", -1, "", "", 0);
    ruby("東京(とうきょう）", -1, "", "", 0);
    ruby("あ（かな）", -1, "", "", 0);
    ruby("《《傍点》》", NR_EMPHASIS, "傍点", "", NR_MAGENTA);
    ruby("《《》》", -1, "", "", 0);
    ruby("｜《", NR_ESCAPE, "《", "", NR_OK);
    ruby("｜(", NR_ESCAPE, "(", "", NR_OK);
    ruby("｜（", NR_ESCAPE, "（", "", NR_OK);
    ruby("漢字《》", -1, "", "", 0);
    ruby("｜《》", NR_ESCAPE, "《", "", NR_OK);
    ruby("｜", -1, "", "", 0);
    ruby("｜あ《", -1, "", "", 0);
    ruby("｜あ《い", -1, "", "", 0);
    ruby("｜あ｜い《う》", NR_BAR, "い", "う", NR_OK);
    ruby("かな《ふりがな》", -1, "", "", 0);
    /* Limits: base 20 (Kakuyomu) ruby, 21 plain; Naro's 10 -> red. */
    ruby("｜一二三四五六七八九十《かず》", NR_BAR, "一二三四五六七八九十", "かず", NR_OK);
    ruby("｜一二三四五六七八九十百《かず》", NR_BAR, "一二三四五六七八九十百", "かず", NR_RED);
    ruby("｜一二三四五六七八九十一二三四五六七八九十《か》", NR_BAR, "一二三四五六七八九十一二三四五六七八九十", "か", NR_RED);
    ruby("｜一二三四五六七八九十一二三四五六七八九十一《か》", -1, "", "", 0);
    ruby("｜親《abcdefghij》", NR_BAR, "親", "abcdefghij", NR_OK);
    ruby("｜親《abcdefghijk》", NR_BAR, "親", "abcdefghijk", NR_RED);
    ruby("｜親《あいうえおかきくけこさしすせそたちつてと》", NR_BAR, "親", "あいうえおかきくけこさしすせそたちつてと", NR_OK);
    ruby("｜親《あいうえおかきくけこさしすせそたちつてとな》", NR_BAR, "親", "あいうえおかきくけこさしすせそたちつてとな", NR_RED);
    char fifty[256] = "｜親《", fiftyone[256];
    for (int i = 0; i < 50; ++i) strcat(fifty, "a");
    strcpy(fiftyone, fifty); strcat(fiftyone, "a》"); strcat(fifty, "》");
    char as[64] = "";
    for (int i = 0; i < 50; ++i) strcat(as, "a");
    ruby(fifty, NR_BAR, "親", as, NR_RED);
    ruby(fiftyone, -1, "", "", 0);
    ruby("漢字(あいうえおかきくけこさしすせそたちつてと)", NR_PAREN, "漢字", "あいうえおかきくけこさしすせそたちつてと", NR_MAGENTA);
    ruby("漢字(あいうえおかきくけこさしすせそたちつてとな)", -1, "", "", 0);
    /* The escape protects the kanji before it from a later 《. */
    uint32_t cp[64]; size_t n = decode("漢｜《字《じ》", cp), cap = 0; NRuby *s = NULL;
    size_t count = novel_ruby_parse(cp, n, &s, &cap);
    CHECK(count == 2 && s[0].kind == NR_ESCAPE && s[1].kind == NR_KANJI && s[1].base == 3,
          "kanji before an escape stay plain: %zu", count);
    free(s);
}

/* ---- layout ---------------------------------------------------------- */
static void layout_one_width(NLayout *l, const char *text, int vertical, int width)
{
    size_t len = strlen(text);
    l->width = width;
    int cap = width ? width : vertical ? NL_ROW_V : NL_ROW_H;
    for (size_t at = 0;; at = novel_next(text, len, at)) {
        CHECK(!nl_run(l, text, len, vertical, at, 0, 0, 0, 0), "nl_run");
        /* Every character once, in order; rows within the width. */
        size_t seen = 0, prev = 0;
        for (size_t k = 0; k < l->nu; ++k) {
            CHECK(k == 0 || l->u[k].off > prev, "'%s': unit order", text);
            prev = l->u[k].off; ++seen;
        }
        size_t chars = 0;
        for (size_t i = 0; i < len; i = novel_next(text, len, i)) ++chars;
        CHECK(seen == chars, "'%s': %zu units for %zu chars", text, seen, chars);
        for (size_t r = 0; r < l->nrow; ++r) {
            CHECK(l->row[r].length <= cap, "'%s': row %zu is %d px", text, r, l->row[r].length);
            int end = -1000;
            for (size_t k = l->row[r].unit0; k < l->row[r].unit0 + l->row[r].units; ++k) {
                const NUnit *u = &l->u[k];
                if ((u->flags & NU_HIDDEN) || !u->adv) continue;
                CHECK(u->pos >= end, "'%s': overlap at unit %zu", text, k);
                end = u->pos + u->adv;
            }
        }
        /* The caret's position maps back to the caret. */
        size_t row; int pos;
        nl_locate(l, at, &row, &pos);
        size_t hit = nl_hit(l, row, pos);
        CHECK(hit == at, "'%s' %s: caret %zu -> row %zu pos %d -> %zu", text, vertical ? "v" : "h", at, row, pos, hit);
        if (at >= len) break;
    }
}
static void layout_one(NLayout *l, const char *text, int vertical) { layout_one_width(l, text, vertical, 0); }
static void layouts(void)
{
    static const char *texts[] = {
        "", "a", "あ", "｜本気《マジ》", "あ｜本気《マジ》い漢字《かんじ》う東京（とうきょう）え《《傍点》》か",
        "長い｜例《ながいながいながいるびです》の", "ABCー12。ぁX", "1 22 333 4444", "\tタブ\t",
        "｜一二三四五六七八九十一二三四五六七八九十《あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんあいう》",
        "ああああああああああああああああああああああああああああああああああああああ｜漢字《かんじ》ああ",
        "a\x01" "b\x7f" "c",
    };
    NLayout l; nl_init(&l);
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) { layout_one(&l, texts[i], 0); layout_one(&l, texts[i], 1); }
    /* Random mixes. */
    static const char *parts[] = {"あ", "漢", "字", "｜", "《", "》", "（", "）", "(", ")", "a", "1", "ー", "。", " ", "ぁ", "\t", "《《", "》》"};
    char text[600];
    for (int t = 0; t < 300; ++t) {
        text[0] = 0;
        int count = 1 + (int)rnd(80);
        for (int i = 0; i < count; ++i) strcat(text, parts[rnd(sizeof(parts) / sizeof(parts[0]))]);
        layout_one(&l, text, (int)rnd(2));
    }
    /* Wrapping: 41 full-width characters make two rows of 40 and 1. */
    char wide[200] = "";
    for (int i = 0; i < 41; ++i) strcat(wide, "あ");
    nl_run(&l, wide, strlen(wide), 0, (size_t)-1, 0, 0, 0, 0);
    CHECK(l.nrow == 2 && l.row[0].units == 40 && l.row[1].units == 1, "40 per row: %zu rows", l.nrow);
    nl_run(&l, wide, strlen(wide), 1, (size_t)-1, 0, 0, 0, 0);
    CHECK(l.nrow == 2 && l.row[0].units == 23 && l.row[1].units == 18, "23 per column");
    /* The editor's narrower rows (rules and line numbers, noveledit.c). */
    l.width = 864;
    nl_run(&l, wide, strlen(wide), 0, (size_t)-1, 0, 0, 0, 0);
    CHECK(l.nrow == 2 && l.row[0].units == 36 && l.row[1].units == 5, "36 per row with numbers");
    l.width = 528;
    nl_run(&l, wide, strlen(wide), 1, (size_t)-1, 0, 0, 0, 0);
    CHECK(l.nrow == 2 && l.row[0].units == 22 && l.row[1].units == 19, "22 per column");
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) layout_one_width(&l, texts[i], 0, 864);
    l.width = 0;
    /* A ruby group is never split across rows. */
    char group[300] = "";
    for (int i = 0; i < 39; ++i) strcat(group, "あ");
    strcat(group, "｜漢字《かんじ》");
    nl_run(&l, group, strlen(group), 0, (size_t)-1, 0, 0, 0, 0);
    CHECK(l.nrow == 2 && l.row[1].rubies == 3, "group moved whole to the next row");
    nl_free(&l);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s TMPDIR\n", argv[0]); return 2; }
    doc_random();
    files(argv[1]);
    search();
    rubies();
    layouts();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    puts("novel unit checks passed");
    return 0;
}
