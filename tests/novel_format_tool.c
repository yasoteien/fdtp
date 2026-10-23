// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Test entrance for novel_format.c and novel_docx.c (tests/check_novel_format.py):
 *   novel-format-tool format RULES IN OUT LOG    整形 (RULES: 12 digits 0/1 in NF_* order)
 *   novel-format-tool manuscript SCENE IN OUT LOG
 *   novel-format-tool web IN OUT LOG
 *   novel-format-tool pieces RULES IN            per line: JSON with the text after
 *       ZENKAKU/HALFSPACE, scene, header and the protected code point ranges
 *   novel-format-tool docx RUBY IN TEMPLATE OUT   (TEMPLATE "-": the built-in one)
 *   novel-format-tool undo RULES IN              format, undo, redo, compare (exit 0 ok)
 * OUT is written with the file's encoding and line end (novel_doc_save), LOG
 * gets one "N行目..." line per message. */
#include "novel_doc.h"
#include "novel_docx.h"
#include "novel_format.h"
#include "novel_ruby.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rules_of(const char *s, int *rules)
{
    if (strlen(s) != NOVEL_FORMAT_COUNT) return -1;
    for (int i = 0; i < NOVEL_FORMAT_COUNT; ++i) rules[i] = s[i] == '1';
    return 0;
}
static int write_log(const char *path, const NFLog *log)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (size_t i = 0; i < log->count; ++i) fprintf(f, "%zu行目%s\n", log->items[i].line, log->items[i].text);
    return fclose(f);
}
static void json(const char *s, size_t n)
{
    putchar('"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}
static char *text_of(NovelDoc *d, size_t *n) { return novel_doc_copy(d, (NPos){0, 0}, novel_doc_end(d), n); }
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
    const char *mode = argv[1];
    NovelDoc d;
    novel_doc_init(&d);
    NFLog log = {0};
    int rules[NOVEL_FORMAT_COUNT];
    int rc = 0;
    if (!strcmp(mode, "format") || !strcmp(mode, "manuscript") || !strcmp(mode, "web")) {
        int web = !strcmp(mode, "web");
        if (argc != (web ? 5 : 6)) return 2;
        const char *in = argv[web ? 2 : 3], *out = argv[web ? 3 : 4], *logp = argv[web ? 4 : 5];
        if (novel_doc_load(&d, in, 64u << 20)) { perror(in); return 2; }
        long changed;
        size_t cursor = d.count / 2;
        if (!strcmp(mode, "format")) {
            if (rules_of(argv[2], rules)) return 2;
            changed = novel_format_doc(&d, rules, &log, &cursor);
        } else if (web) changed = novel_form_web(&d, &log, &cursor);
        else changed = novel_form_manuscript(&d, argv[2][0] == '1', &log, &cursor);
        if (changed < 0 || log.failed) { fprintf(stderr, "out of memory\n"); return 3; }
        if (cursor >= d.count) { fprintf(stderr, "cursor %zu outside %zu lines\n", cursor, d.count); return 3; }
        if (novel_doc_save(&d, out, 0)) { perror(out); return 2; }
        if (write_log(logp, &log)) { perror(logp); return 2; }
        printf("%ld\n", changed);
    } else if (!strcmp(mode, "pieces") && argc == 4) {
        if (rules_of(argv[2], rules) || novel_doc_load(&d, argv[3], 64u << 20)) return 2;
        size_t body = novel_header(&d, NULL);
        for (size_t i = 0; i < d.count; ++i) {
            const NLine *l = &d.lines[i];
            int header = i < body;
            /* The text the rules after HALFSPACE see: format with only
             * ZENKAKU, HALFSPACE and SCENE as given. */
            int only[NOVEL_FORMAT_COUNT] = {0};
            only[NF_ZENKAKU] = rules[NF_ZENKAKU]; only[NF_HALFSPACE] = rules[NF_HALFSPACE]; only[NF_SCENE] = rules[NF_SCENE];
            size_t n;
            char *pre = novel_format_line(l->s, l->len, only, header, i + 1, NULL, &n);
            if (!pre) return 3;
            int scene = !header && rules[NF_SCENE] && novel_scene_line(pre, n);
            size_t count = 0;
            for (size_t k = 0; k < n; k = novel_next(pre, n, k)) ++count;
            uint32_t *cp = malloc((count + 1) * sizeof(*cp));
            count = 0;
            for (size_t k = 0; k < n;) cp[count++] = novel_decode(pre, n, k, &k);
            NRuby *spans = NULL; size_t cap = 0;
            size_t ns = novel_ruby_parse(cp, count, &spans, &cap);
            printf("{\"line\":%zu,\"header\":%d,\"scene\":%d,\"blank\":%d,\"pre\":", i + 1, header, scene,
                   novel_blank_line(l->s, l->len));
            json(pre, n);
            printf(",\"protect\":[");
            int first = 1;
            for (size_t k = 0; k < ns; ++k) {
                size_t a, b;
                if (spans[k].kind == NR_BAR || spans[k].kind == NR_KANJI) { a = spans[k].ruby; b = spans[k].ruby_end; }
                else if (spans[k].kind == NR_EMPHASIS) { a = spans[k].base; b = spans[k].base_end; }
                else continue;
                printf("%s[%zu,%zu]", first ? "" : ",", a, b);
                first = 0;
            }
            printf("]}\n");
            free(spans); free(cp); free(pre);
        }
    } else if (!strcmp(mode, "docx") && argc == 6) {
        if (novel_doc_load(&d, argv[3], 64u << 20)) { perror(argv[3]); return 2; }
        char err[512] = "";
        if (novel_docx_write(&d, strcmp(argv[4], "-") ? argv[4] : NULL, argv[5], argv[2][0] == '1', err, sizeof(err))) {
            fprintf(stderr, "docx: %s\n", err);
            rc = 1;
        }
    } else if (!strcmp(mode, "undo") && argc == 4) {
        if (rules_of(argv[2], rules) || novel_doc_load(&d, argv[3], 64u << 20)) return 2;
        size_t before_n, after_n, again_n;
        char *before = text_of(&d, &before_n);
        long changed = novel_format_doc(&d, rules, &log, NULL);
        char *after = text_of(&d, &after_n);
        NPos p;
        int undone = novel_doc_undo(&d, &p);
        char *again = text_of(&d, &again_n);
        int same = again_n == before_n && !memcmp(again, before, before_n);
        int redone = novel_doc_redo(&d, &p);
        char *redo = text_of(&d, &again_n);
        int same2 = again_n == after_n && !memcmp(redo, after, after_n);
        /* One more undo goes before the load: nothing. */
        novel_doc_undo(&d, &p);
        int more = novel_doc_undo(&d, &p);
        printf("{\"changed\":%ld,\"undone\":%d,\"restored\":%d,\"redone\":%d,\"same\":%d,\"more\":%d,\"modified\":%d}\n",
               changed, undone, same, redone, same2, more, novel_doc_modified(&d));
        free(before); free(after); free(again); free(redo);
    } else { fprintf(stderr, "usage\n"); rc = 2; }
    nf_log_free(&log);
    novel_doc_free(&d);
    return rc;
}
