// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Revision 1 item 41: the text of a slide's paragraphs, edited as the
 * docx view edits its paragraphs (doc_save.c). A paragraph keeps its start
 * tag, its a:pPr, its a:endParaRPr and its end tag byte for byte; its runs
 * become one a:r with the first run's start tag and a:rPr (the format of
 * its first characters), and an a:br (with the same a:rPr) for each line
 * break. Where the paragraph is was read by slide_xml.c (keep_text 2). */
#include "slide_model.h"
#include "i18n.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char R_CODE[] = N_("文字コードが UTF-8 でないため、このスライドは直せません");

/* UTF-16, or an encoding other than UTF-8 in the XML declaration. */
static int not_utf8(const char *xml, size_t len)
{
    const unsigned char *u = (const unsigned char *)xml;
    if (len >= 2 && ((u[0] == 0xff && u[1] == 0xfe) || (u[0] == 0xfe && u[1] == 0xff))) return 1;
    size_t at = len >= 3 && u[0] == 0xef && u[1] == 0xbb && u[2] == 0xbf ? 3 : 0;
    if (len - at < 5 || strncmp(xml + at, "<?xml", 5)) return 0;
    const char *end = memchr(xml + at, '>', len - at);
    if (!end) return 0;
    for (const char *p = xml + at; p + 8 < end; ++p) {
        if (strncmp(p, "encoding", 8)) continue;
        const char *q = p + 8;
        while (q < end && (*q == ' ' || *q == '=' || *q == '"' || *q == '\'')) ++q;
        return !(end - q >= 5 && (!strncmp(q, "UTF-8", 5) || !strncmp(q, "utf-8", 5)));
    }
    return 0;
}
static int add(SPara ***list, int *n, int *cap, SPara *p)
{
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 32;
        SPara **q = realloc(*list, (size_t)c * sizeof(*q));
        if (!q) return -1;
        *list = q;
        *cap = c;
    }
    (*list)[(*n)++] = p;
    return 0;
}
static int by_gid(const void *a, const void *b)
{
    const SPara *x = *(SPara *const *)a, *y = *(SPara *const *)b;
    return (x->gid > y->gid) - (x->gid < y->gid);
}
int slide_edit_list(const char *xml, size_t len, SPart *part, SPara ***list, int *n)
{
    *list = NULL;
    *n = 0;
    if (spart_parse(xml, len, 2, part)) return -1;
    int cap = 0, bad = not_utf8(xml, len);
    for (int i = 0; i < part->nshapes; ++i) {
        SShape *s = &part->shapes[i];
        for (int j = 0; s->text && j < s->text->npara; ++j)
            if (add(list, n, &cap, &s->text->paras[j])) goto nomem;
        if (!s->table) continue;
        for (int c = 0; c < s->table->ncells; ++c) {
            SCell *cell = &s->table->cells[c];
            if (cell->merged || !cell->text) continue;   /* hidden under the merge */
            for (int j = 0; j < cell->text->npara; ++j)
                if (add(list, n, &cap, &cell->text->paras[j])) goto nomem;
        }
    }
    if (*n) qsort(*list, (size_t)*n, sizeof(**list), by_gid);
    for (int i = 0; bad && i < *n; ++i) (*list)[i]->src.refuse = R_CODE;
    return 0;
nomem:
    free(*list);
    *list = NULL;
    *n = 0;
    spart_free(part);
    return -1;
}

char *slide_edit_text(const SPara *p)
{
    size_t len = 0;
    for (int i = 0; i < p->nruns; ++i) len += strlen(p->runs[i].text);
    char *s = malloc(len + 1);
    if (!s) return NULL;
    s[0] = 0;
    char *at = s;
    for (int i = 0; i < p->nruns; ++i) {
        size_t k = strlen(p->runs[i].text);
        memcpy(at, p->runs[i].text, k);
        at += k;
    }
    *at = 0;
    return s;
}

/* ---- the new paragraph ---- */
typedef struct { char *data; size_t len, cap; int failed; } Buf;
static void put(Buf *b, const void *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->len + n + 1) c *= 2;
        char *p = realloc(b->data, c);
        if (!p) { b->failed = 1; return; }
        b->data = p;
        b->cap = c;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }
static void qname(Buf *b, const char *prefix, const char *local)
{
    if (*prefix) { puts_(b, prefix); puts_(b, ":"); }
    puts_(b, local);
}
/* An element's start tag as written, opened when it was empty (<a:r/>). */
static void opened(Buf *b, const char *xml, size_t start, size_t end, int empty)
{
    if (!empty) { put(b, xml + start, end - start); return; }
    size_t e = end;
    if (e > start && xml[e - 1] == '>') --e;
    if (e > start && xml[e - 1] == '/') --e;
    while (e > start && (xml[e - 1] == ' ' || xml[e - 1] == '\t' || xml[e - 1] == '\r' || xml[e - 1] == '\n')) --e;
    put(b, xml + start, e - start);
    puts_(b, ">");
}
/* The characters as a:t content: & < > escaped, characters XML 1.0 does
 * not allow left out (tabs are kept: PowerPoint writes them so). */
static void escaped(Buf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '&') puts_(b, "&amp;");
        else if (c == '<') puts_(b, "&lt;");
        else if (c == '>') puts_(b, "&gt;");
        else if (c < 0x20 && c != '\t') continue;
        else if (c == 0xef && i + 2 < n && (unsigned char)s[i + 1] == 0xbf && ((unsigned char)s[i + 2] == 0xbe || (unsigned char)s[i + 2] == 0xbf)) i += 2;
        else put(b, s + i, 1);
    }
}
/* The format of the new run: the first run's a:rPr, or (no run) the
 * endParaRPr renamed, both of type CT_TextCharacterProperties. */
static void format(Buf *b, const char *xml, const SParaSrc *s, const char *rpr_name)
{
    if (s->has_run) { put(b, xml + s->rpr, s->rpr_end - s->rpr); return; }
    if (s->epr_end <= s->epr) return;
    size_t nl = strlen(s->epr_name), start = s->epr + 1 + nl, end = s->epr_end;
    size_t close = 3 + nl;   /* </name> */
    int full = end - s->epr >= close + 1 + nl && xml[end - 1] == '>' && xml[end - close] == '<' && xml[end - close + 1] == '/' &&
               !strncmp(xml + end - close + 2, s->epr_name, nl);
    puts_(b, "<");
    puts_(b, rpr_name);
    put(b, xml + start, (full ? end - close : end) - start);
    if (full) { puts_(b, "</"); puts_(b, rpr_name); puts_(b, ">"); }
}
char *slide_edit_rewrite(const char *xml, size_t len, const SPara *p, const char *text, size_t *out, char *err, size_t errlen)
{
    const SParaSrc *s = &p->src;
    if (s->refuse) { snprintf(err, errlen, "%s", T(s->refuse)); return NULL; }
    if (s->end > len || s->start >= s->end || s->head > s->tail || s->tail > s->end) {
        snprintf(err, errlen, "%s", T("段落の場所が正しくありません"));
        return NULL;
    }
    /* Names: the run's own (its prefix), else the paragraph's prefix. */
    char rprefix[32] = "";
    if (s->has_run) {
        const char *colon = strchr(s->run_name, ':');
        size_t k = colon ? (size_t)(colon - s->run_name) : 0;
        memcpy(rprefix, s->run_name, k);
        rprefix[k] = 0;
    } else snprintf(rprefix, sizeof(rprefix), "%s", s->prefix);
    char rpr_name[48];
    snprintf(rpr_name, sizeof(rpr_name), "%s%srPr", rprefix, *rprefix ? ":" : "");
    Buf fmt = { 0 }, b = { 0 };
    format(&fmt, xml, s, rpr_name);
    put(&b, xml, s->start);
    if (s->empty) opened(&b, xml, s->start, s->tag_end, 1);
    else put(&b, xml + s->start, s->head - s->start);
    /* The runs: one per line, a:br between. An empty text keeps the first
     * run (with no characters), so its format stays. */
    const char *at = text;
    int first = 1;
    for (;;) {
        const char *nl = strchr(at, '\n');
        size_t n = nl ? (size_t)(nl - at) : strlen(at);
        if (!first) {
            puts_(&b, "<"); qname(&b, rprefix, "br");
            if (fmt.len) { puts_(&b, ">"); put(&b, fmt.data, fmt.len); puts_(&b, "</"); qname(&b, rprefix, "br"); puts_(&b, ">"); }
            else puts_(&b, "/>");
        }
        if (n || (first && !nl && s->has_run)) {
            if (s->has_run) opened(&b, xml, s->run, s->run_tag, s->run_empty);
            else { puts_(&b, "<"); qname(&b, rprefix, "r"); puts_(&b, ">"); }
            put(&b, fmt.data ? fmt.data : "", fmt.len);
            puts_(&b, "<"); qname(&b, rprefix, "t"); puts_(&b, ">");
            escaped(&b, at, n);
            puts_(&b, "</"); qname(&b, rprefix, "t"); puts_(&b, ">");
            puts_(&b, "</"); qname(&b, rprefix, "r"); puts_(&b, ">");
        }
        first = 0;
        if (!nl) break;
        at = nl + 1;
    }
    if (s->empty) { puts_(&b, "</"); qname(&b, s->prefix, "p"); puts_(&b, ">"); }
    else put(&b, xml + s->tail, s->end - s->tail);
    put(&b, xml + s->end, len - s->end);
    free(fmt.data);
    if (b.failed || fmt.failed) {
        free(b.data);
        snprintf(err, errlen, "%s", T("メモリが足りません"));
        return NULL;
    }
    *out = b.len;
    return b.data;
}

/* ---- NAME.bak (as doc_save.c) ---- */
static int write_all(int fd, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
int slide_backup(const char *path, char *err, size_t errlen)
{
    int in = open(path, O_RDONLY);
    if (in < 0) { snprintf(err, errlen, "%s", T("元のファイルを読めません")); return -1; }
    size_t n = strlen(path) + 32;
    char *bak = malloc(n), *tmp = malloc(n);
    if (!bak || !tmp) { close(in); free(bak); free(tmp); snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
    snprintf(bak, n, "%s.bak", path);
    snprintf(tmp, n, "%s.bak.XXXXXX", path);
    int out = mkstemp(tmp);
    int rc = -1;
    if (out >= 0) {
        unsigned char buf[65536];
        rc = 0;
        for (;;) {
            ssize_t r = read(in, buf, sizeof(buf));
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) { rc = -1; break; }
            if (!r) break;
            if (write_all(out, buf, (size_t)r)) { rc = -1; break; }
        }
        struct stat st;
        if (!rc && !fstat(in, &st)) (void)fchmod(out, st.st_mode & 07777);
        if (!rc && fsync(out)) rc = -1;
        if (close(out)) rc = -1;
        if (!rc && rename(tmp, bak)) rc = -1;
        if (rc) unlink(tmp);
    }
    close(in);
    if (rc) snprintf(err, errlen, "%s", T(".bak を書けません"));
    free(bak);
    free(tmp);
    return rc;
}
