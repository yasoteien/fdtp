// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 5: editing paragraphs and saving a docx.
 *
 * An edited paragraph keeps its start and end tags, its w:pPr, and every
 * child that is not a text run (bookmarks, comment ranges, image and note
 * runs) byte for byte where it was; the first text run keeps its start tag
 * and w:rPr and takes all of the new text; the other text runs go
 * (docs/FILER_PLAN.md). Paragraphs that could not keep their content this
 * way were refused when the document was read (doc_parse.c). The main part
 * is the original bytes with only the changed paragraphs spliced in; the
 * other zip entries are copied unchanged by zip_rewrite(). */
#include "doc_model.h"
#include "cells.h"
#include "i18n.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

typedef struct { char *data; size_t len, cap; int failed; } Buf;
static void add(Buf *b, const void *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->len + n + 1) {
            if (c > SIZE_MAX / 2) { b->failed = 1; return; }
            c *= 2;
        }
        char *p = realloc(b->data, c);
        if (!p) { b->failed = 1; return; }
        b->data = p;
        b->cap = c;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void adds(Buf *b, const char *s) { add(b, s, strlen(s)); }
static void addq(Buf *b, const char *prefix, const char *local)
{
    if (*prefix) { adds(b, prefix); adds(b, ":"); }
    adds(b, local);
}
static void add_range(Buf *b, const DocFile *d, uint64_t start, uint64_t end)
{
    if (end > start) add(b, d->xml + start, (size_t)(end - start));
}
/* A start tag of an empty element (<w:r .../>) as a start tag (<w:r ...>). */
static void add_opened(Buf *b, const DocFile *d, uint64_t start, uint64_t end, int empty)
{
    if (!empty) { add_range(b, d, start, end); return; }
    uint64_t e = end;
    if (e > start && d->xml[e - 1] == '>') --e;
    if (e > start && d->xml[e - 1] == '/') --e;
    while (e > start && (d->xml[e - 1] == ' ' || d->xml[e - 1] == '\t' || d->xml[e - 1] == '\r' || d->xml[e - 1] == '\n')) --e;
    add_range(b, d, start, e);
    adds(b, ">");
}

static uint32_t next_code(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = s[0];
    int k = 1;
    if (c >= 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        c = ((c & 7u) << 18) | ((s[1] & 0x3fu) << 12) | ((s[2] & 0x3fu) << 6) | (s[3] & 0x3fu); k = 4;
    } else if (c >= 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        c = ((c & 15u) << 12) | ((s[1] & 0x3fu) << 6) | (s[2] & 0x3fu); k = 3;
    } else if (c >= 0xc0 && (s[1] & 0xc0) == 0x80) {
        c = ((c & 31u) << 6) | (s[1] & 0x3fu); k = 2;
    } else if (c >= 0x80) c = 0xfffd;
    *p = s + k;
    return c;
}
/* The run's content: w:t for the characters, w:tab w:br for the controls. */
static void add_content(Buf *b, const char *prefix, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    int open = 0;
    while (*p) {
        const unsigned char *start = p;
        uint32_t c = next_code(&p);
        const char *element = NULL;
        if (c == '\t') element = "tab";
        else if (c == '\n') element = "br";
        else if (c == '\f') element = "br page";
        else if (c == 0x2011) element = "noBreakHyphen";
        else if (c == 0x00ad) element = "softHyphen";
        else if (c < 0x20 || c == 0xfffe || c == 0xffff) continue;
        if (element) {
            if (open) { adds(b, "</"); addq(b, prefix, "t"); adds(b, ">"); open = 0; }
            adds(b, "<");
            if (!strcmp(element, "br page")) {
                addq(b, prefix, "br"); adds(b, " "); addq(b, prefix, "type"); adds(b, "=\"page\"");
            } else addq(b, prefix, element);
            adds(b, "/>");
            continue;
        }
        if (!open) { adds(b, "<"); addq(b, prefix, "t"); adds(b, " xml:space=\"preserve\">"); open = 1; }
        if (c == '&') adds(b, "&amp;");
        else if (c == '<') adds(b, "&lt;");
        else if (c == '>') adds(b, "&gt;");
        else add(b, start, (size_t)(p - start));
    }
    if (open) { adds(b, "</"); addq(b, prefix, "t"); adds(b, ">"); }
}

/* ---- the new bytes of one paragraph ---- */
static const DocChild *first_text_run(const DocItem *it)
{
    for (size_t i = 0; i < it->nchildren; ++i)
        if (it->children[i].kind == DOC_CHILD_TEXT_RUN) return &it->children[i];
    return NULL;
}
static void add_run(Buf *b, const DocFile *d, const DocItem *it, const char *text)
{
    const DocChild *run = first_text_run(it);
    if (run) {
        add_opened(b, d, run->range.start, run->tag_end, run->empty);
        if (!run->empty) add_range(b, d, run->rpr.start, run->rpr.end);
    } else {
        adds(b, "<"); addq(b, it->prefix, "r"); adds(b, ">");
        if (it->ppr_rpr_clean) add_range(b, d, it->ppr_rpr.start, it->ppr_rpr.end);
    }
    add_content(b, it->prefix, text);
    adds(b, "</"); addq(b, it->prefix, "r"); adds(b, ">");
}
static void edited_xml(Buf *b, const DocFile *d, const DocItem *it)
{
    const DocChild *first = first_text_run(it);
    int need_run = first || *it->edit;
    if (it->empty_tag) {
        add_opened(b, d, it->whole.start, it->tag_end, 1);
        if (need_run) add_run(b, d, it, it->edit);
        adds(b, "</"); addq(b, it->prefix, "p"); adds(b, ">");
        return;
    }
    uint64_t at = it->whole.start;
    for (size_t i = 0; i < it->nchildren; ++i) {
        const DocChild *ch = &it->children[i];
        if (ch->kind != DOC_CHILD_TEXT_RUN) continue;
        add_range(b, d, at, ch->range.start);
        if (ch == first) add_run(b, d, it, it->edit);
        at = ch->range.end;
    }
    if (!first) {
        add_range(b, d, at, it->end_tag);
        if (need_run) add_run(b, d, it, it->edit);
        at = it->end_tag;
    }
    add_range(b, d, at, it->whole.end);
}
static void new_xml(Buf *b, const DocItem *it)
{
    adds(b, "<"); addq(b, it->prefix, "p"); adds(b, ">");
    if (it->new_ppr) adds(b, it->new_ppr);
    if (*it->edit || it->new_rpr) {
        adds(b, "<"); addq(b, it->prefix, "r"); adds(b, ">");
        if (it->new_rpr) adds(b, it->new_rpr);
        add_content(b, it->prefix, it->edit);
        adds(b, "</"); addq(b, it->prefix, "r"); adds(b, ">");
    }
    adds(b, "</"); addq(b, it->prefix, "p"); adds(b, ">");
}

unsigned char *doc_build(const DocFile *d, size_t *len, char *err, size_t errlen)
{
    Buf b = {0};
    add(&b, "", 0);
    uint64_t at = 0, anchor = 0;
    int have_anchor = 0;
    for (size_t i = 0; i < d->nitems; ++i) {
        const DocItem *it = &d->items[i];
        if (it->is_new) {
            if (!have_anchor) { free(b.data); fail(err, errlen, T("段落を足す場所がありません")); return NULL; }
            if (it->deleted) continue;
            add_range(&b, d, at, anchor);
            if (anchor > at) at = anchor;
            new_xml(&b, it);
            continue;
        }
        if (it->deleted || it->edited) {
            add_range(&b, d, at, it->whole.start);
            if (!it->deleted) edited_xml(&b, d, it);
            at = it->whole.end;
        }
        anchor = it->whole.end;
        have_anchor = 1;
    }
    add_range(&b, d, at, d->xml_len);
    if (b.failed) { free(b.data); fail(err, errlen, T("メモリが足りません")); return NULL; }
    *len = b.len;
    return (unsigned char *)b.data;
}

/* ---- edits ---- */
static void rebuild_display(DocItem *it)
{
    doc_text_free(&it->disp);
    for (int i = 0; i < it->images_before; ++i) doc_text_utf8(&it->disp, T("[画像]"), strlen(T("[画像]")), CYAN);
    const unsigned char *p = (const unsigned char *)it->edit;
    int color = it->heading ? YELLOW : WHITE;
    while (*p) {
        uint32_t c = next_code(&p);
        if (c != 0x00ad) doc_text_add(&it->disp, c, color);
    }
    for (int i = 0; i < it->images_after; ++i) doc_text_utf8(&it->disp, T("[画像]"), strlen(T("[画像]")), CYAN);
}
int doc_set_text(DocFile *d, size_t index, const char *text, char *err, size_t errlen)
{
    if (index >= d->nitems) { fail(err, errlen, T("段落がありません")); return -1; }
    DocItem *it = &d->items[index];
    if (it->kind != DOC_PARA || it->deleted) { fail(err, errlen, "%s", it->refuse ? T(it->refuse) : T("段落がありません")); return -1; }
    if (it->refuse) { fail(err, errlen, "%s", T(it->refuse)); return -1; }
    if (it->edit && !strcmp(it->edit, text)) return 0;
    char *copy = strdup(text);
    if (!copy) { fail(err, errlen, T("メモリが足りません")); return -1; }
    free(it->edit);
    it->edit = copy;
    if (!it->is_new) it->edited = 1;
    rebuild_display(it);
    if (it->disp.failed) { fail(err, errlen, T("メモリが足りません")); return -1; }
    return 0;
}
static char *copy_without(const DocFile *d, DocRange r, const DocRange *drops, int ndrops)
{
    Buf b = {0};
    add(&b, "", 0);
    uint64_t at = r.start;
    for (;;) {
        /* The next drop range at or after at. */
        const DocRange *next = NULL;
        for (int i = 0; i < ndrops; ++i)
            if (drops[i].start >= at && drops[i].end > drops[i].start && drops[i].end <= r.end && (!next || drops[i].start < next->start))
                next = &drops[i];
        if (!next) break;
        add_range(&b, d, at, next->start);
        at = next->end;
    }
    add_range(&b, d, at, r.end);
    if (b.failed) { free(b.data); return NULL; }
    return b.data;
}
long doc_insert_after(DocFile *d, size_t index, char *err, size_t errlen)
{
    if (!d->nitems) { fail(err, errlen, T("段落を足す場所がありません")); return -1; }
    if (index >= d->nitems) index = d->nitems - 1;
    const DocItem *src = NULL;
    for (size_t j = index + 1; j-- > 0;) {
        const DocItem *x = &d->items[j];
        if (x->kind == DOC_PARA && !x->deleted) { src = x; break; }
    }
    DocItem it;
    memset(&it, 0, sizeof(it));
    it.kind = DOC_PARA;
    it.is_new = 1;
    it.edit = strdup("");
    snprintf(it.prefix, sizeof(it.prefix), "%s", src ? src->prefix : d->root_prefix);
    int nomem = !it.edit;
    if (src) {
        it.heading = src->heading;
        if (src->is_new) {
            if (src->new_ppr && !(it.new_ppr = strdup(src->new_ppr))) nomem = 1;
            if (src->new_rpr && !(it.new_rpr = strdup(src->new_rpr))) nomem = 1;
        } else {
            if (src->has_ppr && !(it.new_ppr = copy_without(d, src->ppr, src->drops, src->ndrops))) nomem = 1;
            const DocChild *run = first_text_run(src);
            DocRange r = {0, 0};
            if (run && !run->empty && run->rpr.end > run->rpr.start && !run->rpr_change) r = run->rpr;
            else if (!run && src->ppr_rpr_clean && src->ppr_rpr.end > src->ppr_rpr.start) r = src->ppr_rpr;
            if (r.end > r.start && !(it.new_rpr = copy_without(d, r, NULL, 0))) nomem = 1;
        }
    }
    if (nomem) { doc_item_free(&it); fail(err, errlen, T("メモリが足りません")); return -1; }
    if (d->nitems + 1 > d->citems) {
        size_t c = d->citems ? d->citems * 2 : 16;
        DocItem *p = realloc(d->items, c * sizeof(*p));
        if (!p) { doc_item_free(&it); fail(err, errlen, T("メモリが足りません")); return -1; }
        d->items = p;
        d->citems = c;
    }
    memmove(&d->items[index + 2], &d->items[index + 1], (d->nitems - index - 1) * sizeof(DocItem));
    d->items[index + 1] = it;
    d->nitems++;
    return (long)index + 1;
}
int doc_can_delete(const DocFile *d, size_t index, char *err, size_t errlen)
{
    if (index >= d->nitems || d->items[index].deleted) { fail(err, errlen, T("段落がありません")); return -1; }
    const DocItem *it = &d->items[index];
    if (it->kind != DOC_PARA) { fail(err, errlen, "%s", it->refuse_delete ? T(it->refuse_delete) : T("表は消せません")); return -1; }
    if (it->is_new) return 0;
    if (it->refuse_delete) { fail(err, errlen, "%s", T(it->refuse_delete)); return -1; }
    for (size_t j = index + 1; j < d->nitems; ++j) if (!d->items[j].deleted) return 0;
    fail(err, errlen, T("文書の最後の段落は消せません"));
    return -1;
}
int doc_delete(DocFile *d, size_t index, char *err, size_t errlen)
{
    if (doc_can_delete(d, index, err, errlen)) return -1;
    DocItem *it = &d->items[index];
    if (it->is_new) {
        doc_item_free(it);
        memmove(&d->items[index], &d->items[index + 1], (d->nitems - index - 1) * sizeof(DocItem));
        d->nitems--;
        return 0;
    }
    it->deleted = 1;
    return 0;
}

/* ---- saving ---- */
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
/* NAME.bak, through a temporary file beside it. */
static int make_backup(const char *path, char *err, size_t errlen)
{
    int in = open(path, O_RDONLY);
    if (in < 0) { fail(err, errlen, T("元のファイルを読めません")); return -1; }
    size_t n = strlen(path) + 32;
    char *bak = malloc(n), *tmp = malloc(n);
    if (!bak || !tmp) { close(in); free(bak); free(tmp); fail(err, errlen, T("メモリが足りません")); return -1; }
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
    if (rc) fail(err, errlen, T(".bak を書けません"));
    free(bak);
    free(tmp);
    return rc;
}
int doc_save(DocFile *d, int backup, int *backed, char *err, size_t errlen)
{
    if (backed) *backed = 0;
    if (d->readonly) { fail(err, errlen, "%s", d->readonly); return -1; }
    struct stat st;
    if (stat(d->path, &st) || st.st_mtime != d->mtime || st.st_size != d->size) {
        fail(err, errlen, T("ファイルが外で書き換えられたため保存しません"));
        return -1;
    }
    size_t len;
    unsigned char *xml = doc_build(d, &len, err, errlen);
    if (!xml) return -1;
    if (backup && !d->backup_done) {
        if (make_backup(d->path, err, errlen)) { free(xml); return -1; }
        d->backup_done = 1;
        if (backed) *backed = 1;
    }
    long index = zip_find(d->zip, d->part);
    int method = ZIP_DEFLATE;
    if (index >= 0 && zip_entry(d->zip, (size_t)index)->method == ZIP_STORE) method = ZIP_STORE;
    ZipChange change = {d->part, 0, method, xml, len, NULL, NULL};
    int rc = zip_rewrite(d->zip, d->path, &change, 1, err, errlen);
    free(xml);
    if (rc) return -1;
    /* Read the saved file again, so later edits refer to its bytes. */
    DocFile fresh;
    char path[4096];
    snprintf(path, sizeof(path), "%s", d->path);
    int done = d->backup_done;
    if (doc_open(&fresh, path, err, errlen)) {
        char why[200];
        snprintf(why, sizeof(why), "%s", err ? err : "");
        fail(err, errlen, T("保存しましたが開き直せません: %s"), why);
        return -1;
    }
    doc_free(d);
    *d = fresh;
    d->backup_done = done;
    return 0;
}
