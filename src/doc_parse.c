// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 5: reading a docx. The relationships lead to the main document
 * part and its styles; the main part is read whole and parsed once with
 * expat (namespace triplets, so the prefixes in the file are known). */
#include "doc_model.h"
#include "cells.h"
#include "i18n.h"
#include "expat_compat.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define NS_SEP '\x01'
#define PART_LIMIT (256u << 20)
#define STYLE_LIMIT (32u << 20)
#define MAX_DEPTH 2048

static const char W_TRANSITIONAL[] = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
static const char W_STRICT[] = "http://purl.oclc.org/ooxml/wordprocessingml/main";
static const char MC_URI[] = "http://schemas.openxmlformats.org/markup-compatibility/2006";
static const char M_URI[] = "http://schemas.openxmlformats.org/officeDocument/2006/math";
static const char M_STRICT[] = "http://purl.oclc.org/ooxml/officeDocument/math";

static void fail(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- text ---- */
static int grow(void **p, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 16;
    while (c < need) {
        if (c > SIZE_MAX / 2 / size) return -1;
        c *= 2;
    }
    void *q = realloc(*p, c * size);
    if (!q) return -1;
    *p = q;
    *cap = c;
    return 0;
}
void doc_text_add(DocText *t, uint32_t code, int color)
{
    if (t->failed) return;
    if (t->n + 1 > t->cap) {
        size_t c = t->cap ? t->cap * 2 : 32;
        uint32_t *a = realloc(t->code, c * sizeof(*a));
        if (!a) { t->failed = 1; return; }
        t->code = a;
        unsigned char *b = realloc(t->color, c);
        if (!b) { t->failed = 1; return; }
        t->color = b;
        t->cap = c;
    }
    t->code[t->n] = code;
    t->color[t->n] = (unsigned char)color;
    t->n++;
}
/* UTF-8 to code points; bytes that are not UTF-8 become U+FFFD. */
static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *used)
{
    unsigned char c = s[0];
    uint32_t code, min;
    size_t k;
    if (c < 0x80) { *used = 1; return c; }
    if (c >= 0xc2 && c <= 0xdf) { code = c & 0x1f; k = 2; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { code = c & 0x0f; k = 3; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { code = c & 7; k = 4; min = 0x10000; }
    else { *used = 1; return 0xfffd; }
    if (k > n) { *used = 1; return 0xfffd; }
    for (size_t i = 1; i < k; ++i) {
        if ((s[i] & 0xc0) != 0x80) { *used = 1; return 0xfffd; }
        code = (code << 6) | (s[i] & 0x3f);
    }
    if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) { *used = 1; return 0xfffd; }
    *used = k;
    return code;
}
void doc_text_utf8(DocText *t, const char *s, size_t n, int color)
{
    const unsigned char *p = (const unsigned char *)s;
    while (n) {
        size_t used;
        uint32_t c = utf8_next(p, n, &used);
        doc_text_add(t, c, color);
        p += used;
        n -= used;
    }
}
void doc_text_free(DocText *t)
{
    free(t->code);
    free(t->color);
    memset(t, 0, sizeof(*t));
}

typedef struct { char *data; size_t len, cap; int failed; } Buf;
static void buf_add(Buf *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (grow((void **)&b->data, &b->cap, b->len + n + 1, 1)) { b->failed = 1; return; }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

/* ---- names ---- */
enum { NS_NONE, NS_W, NS_MC, NS_M, NS_OTHER };
enum {
    E_OTHER, E_document, E_body, E_p, E_pPr, E_r, E_rPr, E_t, E_tab, E_br, E_cr, E_noBreakHyphen,
    E_softHyphen, E_lastRenderedPageBreak, E_tbl, E_tr, E_tc, E_tcPr, E_gridSpan, E_vMerge, E_drawing,
    E_pict, E_object, E_sdt, E_sdtContent, E_customXml, E_bookmarkStart, E_bookmarkEnd,
    E_commentRangeStart, E_commentRangeEnd, E_proofErr, E_permStart, E_permEnd, E_pStyle,
    E_pageBreakBefore, E_sectPr, E_type, E_pPrChange, E_rPrChange, E_ins, E_del, E_moveFrom, E_moveTo,
    E_moveFromRangeStart, E_moveFromRangeEnd, E_moveToRangeStart, E_moveToRangeEnd, E_hyperlink,
    E_fldSimple, E_fldChar, E_instrText, E_delInstrText, E_delText, E_smartTag, E_sym, E_ruby,
    E_footnoteReference, E_endnoteReference, E_commentReference, E_style, E_name, E_basedOn, E_dir,
    E_bdo, E_subDoc, E_ptab, E_customXmlInsRangeStart, E_customXmlDelRangeStart,
    E_customXmlMoveFromRangeStart, E_customXmlMoveToRangeStart,
    E_COUNT,
    E_AlternateContent = 100, E_Choice, E_Fallback, E_MATH
};
static const char *const w_names[E_COUNT] = {
    "", "document", "body", "p", "pPr", "r", "rPr", "t", "tab", "br", "cr", "noBreakHyphen",
    "softHyphen", "lastRenderedPageBreak", "tbl", "tr", "tc", "tcPr", "gridSpan", "vMerge", "drawing",
    "pict", "object", "sdt", "sdtContent", "customXml", "bookmarkStart", "bookmarkEnd",
    "commentRangeStart", "commentRangeEnd", "proofErr", "permStart", "permEnd", "pStyle",
    "pageBreakBefore", "sectPr", "type", "pPrChange", "rPrChange", "ins", "del", "moveFrom", "moveTo",
    "moveFromRangeStart", "moveFromRangeEnd", "moveToRangeStart", "moveToRangeEnd", "hyperlink",
    "fldSimple", "fldChar", "instrText", "delInstrText", "delText", "smartTag", "sym", "ruby",
    "footnoteReference", "endnoteReference", "commentReference", "style", "name", "basedOn", "dir",
    "bdo", "subDoc", "ptab", "customXmlInsRangeStart", "customXmlDelRangeStart",
    "customXmlMoveFromRangeStart", "customXmlMoveToRangeStart",
};
typedef struct {
    int ns, id;
    const char *local, *prefix;
    size_t local_len, prefix_len;
} QName;
static int uri_is(const char *uri, size_t n, const char *s) { return strlen(s) == n && !memcmp(uri, s, n); }
static void qname(const char *name, QName *q)
{
    memset(q, 0, sizeof(*q));
    const char *a = strchr(name, NS_SEP);
    if (!a) {
        q->ns = NS_NONE; q->local = name; q->local_len = strlen(name); q->prefix = "";
    } else {
        size_t ulen = (size_t)(a - name);
        q->local = a + 1;
        const char *b = strchr(q->local, NS_SEP);
        q->local_len = b ? (size_t)(b - q->local) : strlen(q->local);
        q->prefix = b ? b + 1 : "";
        q->prefix_len = strlen(q->prefix);
        if (uri_is(name, ulen, W_TRANSITIONAL) || uri_is(name, ulen, W_STRICT)) q->ns = NS_W;
        else if (uri_is(name, ulen, MC_URI)) q->ns = NS_MC;
        else if (uri_is(name, ulen, M_URI) || uri_is(name, ulen, M_STRICT)) q->ns = NS_M;
        else q->ns = NS_OTHER;
    }
    q->id = E_OTHER;
    if (q->ns == NS_W) {
        for (int i = 1; i < E_COUNT; ++i)
            if (strlen(w_names[i]) == q->local_len && !memcmp(w_names[i], q->local, q->local_len)) { q->id = i; break; }
    } else if (q->ns == NS_MC) {
        if (q->local_len == 16 && !memcmp(q->local, "AlternateContent", 16)) q->id = E_AlternateContent;
        else if (q->local_len == 6 && !memcmp(q->local, "Choice", 6)) q->id = E_Choice;
        else if (q->local_len == 8 && !memcmp(q->local, "Fallback", 8)) q->id = E_Fallback;
    } else if (q->ns == NS_M) q->id = E_MATH;
}
/* A WordprocessingML attribute (w:val ...); unqualified ones as a fallback. */
static const char *wattr(const char **a, const char *local)
{
    const char *plain = NULL;
    for (; a && a[0]; a += 2) {
        QName q;
        qname(a[0], &q);
        if (strlen(local) != q.local_len || memcmp(q.local, local, q.local_len)) continue;
        if (q.ns == NS_W) return a[1];
        if (q.ns == NS_NONE) plain = a[1];
    }
    return plain;
}
static int on_off(const char *v) { return !v || !(!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off")); }

/* ---- relationships ---- */
typedef struct { char *type, *target, *mode; } Rel;
typedef struct { Rel *r; size_t n, cap; int failed; } Rels;
static void rels_start(void *u, const char *name, const char **a)
{
    Rels *r = u;
    const char *colon = strrchr(name, ':');
    const char *ln = colon ? colon + 1 : name;
    if (strcmp(ln, "Relationship") || r->failed) return;
    const char *type = NULL, *target = NULL, *mode = NULL;
    for (; a[0]; a += 2) {
        if (!strcmp(a[0], "Type")) type = a[1];
        else if (!strcmp(a[0], "Target")) target = a[1];
        else if (!strcmp(a[0], "TargetMode")) mode = a[1];
    }
    if (!type || !target) return;
    if (grow((void **)&r->r, &r->cap, r->n + 1, sizeof(Rel))) { r->failed = 1; return; }
    Rel *x = &r->r[r->n];
    x->type = strdup(type); x->target = strdup(target); x->mode = mode ? strdup(mode) : NULL;
    if (!x->type || !x->target || (mode && !x->mode)) { free(x->type); free(x->target); free(x->mode); r->failed = 1; return; }
    r->n++;
}
static void rels_end(void *u, const char *name) { (void)u; (void)name; }
static void rels_free(Rels *r)
{
    for (size_t i = 0; i < r->n; ++i) { free(r->r[i].type); free(r->r[i].target); free(r->r[i].mode); }
    free(r->r);
    memset(r, 0, sizeof(*r));
}
static int read_part(ZipArchive *z, const char *name, size_t limit, unsigned char **data, size_t *n, char *err, size_t errlen)
{
    long i = zip_find(z, name);
    if (i < 0) { fail(err, errlen, T("%s がありません"), name); return -1; }
    return zip_read(z, (size_t)i, limit, data, n, err, errlen);
}
static int read_rels(ZipArchive *z, const char *name, Rels *out)
{
    unsigned char *data;
    size_t n;
    memset(out, 0, sizeof(*out));
    if (zip_find(z, name) < 0 || read_part(z, name, 16u << 20, &data, &n, NULL, 0)) return -1;
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) { free(data); return -1; }
    XML_SetUserData(p, out);
    XML_SetElementHandler(p, rels_start, rels_end);
    int ok = n <= INT32_MAX && XML_Parse(p, (const char *)data, (int)n, 1) == XML_STATUS_OK && !out->failed;
    XML_ParserFree(p);
    free(data);
    if (!ok) { rels_free(out); return -1; }
    return 0;
}
static const Rel *rel_type(const Rels *r, const char *tail)
{
    size_t t = strlen(tail);
    for (size_t i = 0; i < r->n; ++i) {
        size_t n = strlen(r->r[i].type);
        if (n >= t && !strcmp(r->r[i].type + n - t, tail) && !(r->r[i].mode && !strcmp(r->r[i].mode, "External"))) return &r->r[i];
    }
    return NULL;
}
/* Target relative to the directory of source (or absolute from the root). */
static char *resolve(const char *source, const char *target)
{
    size_t n = strlen(source) + strlen(target) + 2;
    char *joined = malloc(n), *out = malloc(n);
    if (!joined || !out) { free(joined); free(out); return NULL; }
    if (target[0] == '/') snprintf(joined, n, "%s", target + 1);
    else {
        const char *slash = strrchr(source, '/');
        int dir = slash ? (int)(slash - source + 1) : 0;
        snprintf(joined, n, "%.*s%s", dir, source, target);
    }
    size_t len = 0;
    for (char *seg = joined; *seg;) {
        char *end = strchr(seg, '/');
        size_t sl = end ? (size_t)(end - seg) : strlen(seg);
        if (sl == 2 && !strncmp(seg, "..", 2)) {
            while (len && out[len - 1] == '/') --len;
            while (len && out[len - 1] != '/') --len;
        } else if (sl && !(sl == 1 && seg[0] == '.')) {
            memcpy(out + len, seg, sl);
            len += sl;
            if (end) out[len++] = '/';
        }
        seg = end ? end + 1 : seg + sl;
    }
    out[len] = 0;
    free(joined);
    return out;
}
static char *rels_name(const char *part)
{
    const char *slash = strrchr(part, '/');
    size_t dir = slash ? (size_t)(slash - part + 1) : 0;
    size_t n = strlen(part) + 16;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%.*s_rels/%s.rels", (int)dir, part, part + dir);
    return s;
}

/* ---- styles ---- */
typedef struct { char *id, *name, *based; } Style;
typedef struct { Style *s; size_t n, cap; int depth, style_depth, failed; } Styles;
static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }
static void styles_start(void *u, const char *name, const char **a)
{
    Styles *st = u;
    QName q;
    qname(name, &q);
    st->depth++;
    if (st->failed || q.ns != NS_W) return;
    if (q.id == E_style && st->depth == 2) {
        if (grow((void **)&st->s, &st->cap, st->n + 1, sizeof(Style))) { st->failed = 1; return; }
        Style *s = &st->s[st->n++];
        memset(s, 0, sizeof(*s));
        s->id = dup_or_null(wattr(a, "styleId"));
        st->style_depth = st->depth;
        return;
    }
    if (st->style_depth && st->depth == st->style_depth + 1 && st->n) {
        Style *s = &st->s[st->n - 1];
        if (q.id == E_name && !s->name) s->name = dup_or_null(wattr(a, "val"));
        else if (q.id == E_basedOn && !s->based) s->based = dup_or_null(wattr(a, "val"));
    }
}
static void styles_end(void *u, const char *name)
{
    (void)name;
    Styles *st = u;
    if (st->depth == st->style_depth) st->style_depth = 0;
    st->depth--;
}
static void styles_free(Styles *st)
{
    for (size_t i = 0; i < st->n; ++i) { free(st->s[i].id); free(st->s[i].name); free(st->s[i].based); }
    free(st->s);
    memset(st, 0, sizeof(*st));
}
static int looks_heading(const char *s)
{
    return s && (!strncasecmp(s, "heading", 7) || strstr(s, "見出し") != NULL); /* i18n:keep */
}
static int is_heading(const Styles *st, const char *id)
{
    for (int hop = 0; id && hop < 16; ++hop) {
        if (looks_heading(id)) return 1;
        const Style *found = NULL;
        for (size_t i = 0; i < st->n; ++i) if (st->s[i].id && !strcmp(st->s[i].id, id)) { found = &st->s[i]; break; }
        if (!found) return 0;
        if (looks_heading(found->name)) return 1;
        id = found->based;
    }
    return 0;
}

/* ---- the main part ---- */
static const char *const R_TRACK = N_("変更履歴を含むため、この段落は直せません");
static const char *const R_FIELD = N_("フィールドを含むため、この段落は直せません");
static const char *const R_LINK = N_("リンクを含むため、この段落は直せません");
static const char *const R_TAG = N_("タグやコンテンツコントロールを含むため、この段落は直せません");
static const char *const R_MATH = N_("数式を含むため、この段落は直せません");
static const char *const R_RUBY = N_("ルビを含むため、この段落は直せません");
static const char *const R_SYM = N_("記号やタブ位置の要素を含むため、この段落は直せません");
static const char *const R_MID = N_("画像や注が文の途中にあるため、この段落は直せません");
static const char *const R_MIXED = N_("画像や注と文字が同じ部分にあるため、この段落は直せません");
static const char *const R_BREAK = N_("段組みの区切りを含むため、この段落は直せません");
static const char *const R_OTHER = N_("知らない要素を含むため、この段落は直せません");
static const char *const R_TABLE = N_("表の中は直せません");
static const char *const D_SECT = N_("セクションの区切りを含むため、この段落は消せません");
static const char *const D_MARK = N_("ブックマークやコメントの範囲を含むため、この段落は消せません");
static const char *const D_REF = N_("注やコメントを含むため、この段落は消せません");
static const char *const D_FIELD = N_("フィールドを含むため、この段落は消せません");
static const char *const D_TRACK = N_("変更履歴を含むため、この段落は消せません");
static const char *const D_TABLE = N_("表は消せません");

typedef struct { short ns, id; unsigned char block; } Frame;
typedef struct {
    XML_Parser parser;
    DocFile *d;
    const Styles *styles;
    char err[200];
    int stop;
    Frame *stack;
    int depth;
    int skip;                   /* depth of the skipped element, 0 none */
    long para;                  /* current body-level paragraph or -1 */
    int para_depth;
    long child;                 /* current direct child of para or -1 */
    int child_text, child_special;
    int ppr_rpr_depth, sect_depth, sect_continuous;
    char *pstyle;
    Buf edit;
    int in_t, t_edit;
    DocText *target;
    long table;
    int table_depth, tbl_nest, cell_depth, cell_paras;
    int root_seen, body_depth;
    int bad_encoding;
} Ctx;

static uint64_t here(Ctx *c) { return (uint64_t)fdtp_xml_byte_index(c->parser); }
static uint64_t here_end(Ctx *c) { return here(c) + (uint64_t)fdtp_xml_byte_count(c->parser); }
static void ctx_fail(Ctx *c, const char *message)
{
    if (!c->err[0]) snprintf(c->err, sizeof(c->err), "%s", message);
    c->stop = 1;
    XML_StopParser(c->parser, XML_FALSE);
}
static DocItem *item(Ctx *c, long i) { return &c->d->items[i]; }
static DocItem *new_item(Ctx *c, int kind)
{
    DocFile *d = c->d;
    if (grow((void **)&d->items, &d->citems, d->nitems + 1, sizeof(DocItem))) { ctx_fail(c, T("メモリが足りません")); return NULL; }
    DocItem *it = &d->items[d->nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    return it;
}
static void refuse(DocItem *it, const char *why) { if (!it->refuse) it->refuse = why; }
static void refuse_delete(DocItem *it, const char *why) { if (!it->refuse_delete) it->refuse_delete = why; }
static void add_image(Ctx *c)
{
    if (c->target) doc_text_utf8(c->target, T("[画像]"), strlen(T("[画像]")), CYAN);
    if (c->para >= 0 && c->child >= 0 && c->depth > c->para_depth + 1) item(c, c->para)->children[c->child].images++;
}
static void edit_char(Ctx *c, const char *s, size_t n)
{
    if (c->t_edit) buf_add(&c->edit, s, n);
}
static void text_control(Ctx *c, uint32_t code, const char *utf8)
{
    if (c->target) doc_text_add(c->target, code, WHITE);
    edit_char(c, utf8, strlen(utf8));
}
static int in_run_child(Ctx *c)
{
    /* Inside a direct child run of the paragraph (at any depth below it). */
    return c->para >= 0 && c->child >= 0 && item(c, c->para)->children[c->child].kind == DOC_CHILD_TEXT_RUN &&
           c->stack[c->para_depth + 1].id == E_r && c->stack[c->para_depth + 1].ns == NS_W;
}
static void add_drop(DocItem *it, uint64_t start, uint64_t end)
{
    if (it->ndrops < (int)(sizeof(it->drops) / sizeof(it->drops[0]))) it->drops[it->ndrops++] = (DocRange){start, end};
    else it->ppr_rpr_clean = 0;
}

/* Elements that make a paragraph not editable, wherever they are in it. */
static void paragraph_element(Ctx *c, DocItem *it, const QName *q)
{
    switch (q->id) {
    case E_ins: case E_del: case E_moveFrom: case E_moveTo: case E_rPrChange: case E_pPrChange:
    case E_moveFromRangeStart: case E_moveFromRangeEnd: case E_moveToRangeStart: case E_moveToRangeEnd:
    case E_delText: case E_delInstrText: case E_customXmlInsRangeStart: case E_customXmlDelRangeStart:
    case E_customXmlMoveFromRangeStart: case E_customXmlMoveToRangeStart:
        refuse(it, R_TRACK); refuse_delete(it, D_TRACK); break;
    case E_fldSimple: case E_fldChar: case E_instrText:
        refuse(it, R_FIELD); refuse_delete(it, D_FIELD); break;
    case E_hyperlink: refuse(it, R_LINK); break;
    case E_smartTag: case E_customXml: case E_sdt: refuse(it, R_TAG); break;
    case E_MATH: refuse(it, R_MATH); break;
    case E_ruby: refuse(it, R_RUBY); break;
    case E_sym: case E_ptab: refuse(it, R_SYM); break;
    case E_dir: case E_bdo: case E_subDoc: refuse(it, R_OTHER); break;
    case E_bookmarkStart: case E_bookmarkEnd: case E_commentRangeStart: case E_commentRangeEnd:
    case E_permStart: case E_permEnd:
        refuse_delete(it, D_MARK); break;
    case E_footnoteReference: case E_endnoteReference: case E_commentReference:
        refuse_delete(it, D_REF); break;
    default: break;
    }
    (void)c;
}
static void para_child_start(Ctx *c, DocItem *it, const QName *q)
{
    if (grow((void **)&it->children, &it->cchildren, it->nchildren + 1, sizeof(DocChild))) { ctx_fail(c, T("メモリが足りません")); return; }
    DocChild *ch = &it->children[it->nchildren];
    memset(ch, 0, sizeof(*ch));
    ch->range.start = here(c);
    ch->tag_end = here_end(c);
    c->child = (long)it->nchildren++;
    c->child_text = c->child_special = 0;
    if (q->ns == NS_W && q->id == E_r) ch->kind = DOC_CHILD_TEXT_RUN;
    else if (q->ns == NS_W && q->id == E_pPr) {
        ch->kind = DOC_CHILD_PPR;
        it->has_ppr = 1;
        it->ppr.start = ch->range.start;
    } else {
        ch->kind = DOC_CHILD_MARK;
        switch (q->ns == NS_W ? q->id : -1) {
        case E_bookmarkStart: case E_bookmarkEnd: case E_commentRangeStart: case E_commentRangeEnd:
        case E_proofErr: case E_permStart: case E_permEnd:
            break;
        default:
            if (q->ns == NS_W && (q->id == E_hyperlink || q->id == E_fldSimple || q->id == E_ins || q->id == E_del ||
                                  q->id == E_moveFrom || q->id == E_moveTo || q->id == E_smartTag || q->id == E_customXml ||
                                  q->id == E_sdt || q->id == E_dir || q->id == E_bdo || q->id == E_subDoc ||
                                  q->id == E_moveFromRangeStart || q->id == E_moveFromRangeEnd ||
                                  q->id == E_moveToRangeStart || q->id == E_moveToRangeEnd))
                break;  /* the reason comes from paragraph_element */
            if (q->id == E_MATH) break;
            refuse(it, R_OTHER);
        }
    }
}
static void run_child_start(Ctx *c, DocItem *it, const QName *q, const char **a)
{
    DocChild *ch = &it->children[c->child];
    if (q->ns == NS_MC && q->id == E_AlternateContent) { c->child_special = 1; return; }
    if (q->ns != NS_W) { refuse(it, R_OTHER); return; }
    switch (q->id) {
    case E_rPr: ch->rpr.start = here(c); return;
    case E_t: case E_tab: case E_cr: case E_noBreakHyphen: case E_softHyphen: c->child_text = 1; return;
    case E_lastRenderedPageBreak: return;
    case E_br: {
        c->child_text = 1;
        const char *type = wattr(a, "type");
        if ((type && strcmp(type, "page") && strcmp(type, "textWrapping")) || wattr(a, "clear")) refuse(it, R_BREAK);
        return;
    }
    case E_drawing: case E_pict: case E_object:
    case E_footnoteReference: case E_endnoteReference: case E_commentReference:
        c->child_special = 1; return;
    case E_fldChar: case E_instrText: case E_delInstrText: case E_delText: case E_sym: case E_ruby: case E_ptab:
        return;     /* paragraph_element gives the reason */
    default: refuse(it, R_OTHER); return;
    }
}

static void doc_start(void *u, const char *name, const char **a)
{
    Ctx *c = u;
    if (c->stop) return;
    if (c->depth + 1 >= MAX_DEPTH) { ctx_fail(c, T("入れ子が深すぎます")); return; }
    QName q;
    qname(name, &q);
    Frame parent = c->stack[c->depth];
    c->depth++;
    Frame *f = &c->stack[c->depth];
    f->ns = (short)q.ns; f->id = (short)q.id; f->block = 0;
    if (c->skip) return;
    if (c->depth == 1) {
        c->root_seen = 1;
        if (q.ns != NS_W || q.id != E_document) { ctx_fail(c, T("Word の文書ではありません")); return; }
        snprintf(c->d->root_prefix, sizeof(c->d->root_prefix), "%.*s", (int)q.prefix_len, q.prefix);
        return;
    }
    if (q.ns == NS_W && q.id == E_body && c->depth == 2) { f->block = 1; c->body_depth = c->depth; return; }
    int w = q.ns == NS_W;
    /* Block-level containers that are transparent for the items. */
    if (parent.block && w && (q.id == E_sdt || q.id == E_sdtContent || q.id == E_customXml) && c->para < 0 && c->table < 0) {
        f->block = 1; return;
    }
    if (q.ns == NS_MC && q.id == E_Fallback) { c->skip = c->depth; return; }
    if (w && (q.id == E_drawing || q.id == E_pict || q.id == E_object)) {
        if (c->para >= 0) paragraph_element(c, item(c, c->para), &q);
        if (c->para >= 0 && c->child >= 0 && c->depth == c->para_depth + 2 &&
            item(c, c->para)->children[c->child].kind == DOC_CHILD_TEXT_RUN)
            run_child_start(c, item(c, c->para), &q, a);
        add_image(c);
        c->skip = c->depth;
        return;
    }
    /* A body-level paragraph or table. */
    if (parent.block && c->para < 0 && c->table < 0) {
        if (w && q.id == E_p) {
            DocItem *it = new_item(c, DOC_PARA);
            if (!it) return;
            c->para = (long)(c->d->nitems - 1);
            c->para_depth = c->depth;
            c->child = -1;
            it->whole.start = here(c);
            it->tag_end = here_end(c);
            it->ppr_rpr_clean = 1;
            snprintf(it->prefix, sizeof(it->prefix), "%.*s", (int)q.prefix_len, q.prefix);
            c->target = &it->disp;
            c->edit.len = 0;
            if (c->edit.data) c->edit.data[0] = 0;
            free(c->pstyle); c->pstyle = NULL;
            return;
        }
        if (w && q.id == E_tbl) {
            DocItem *it = new_item(c, DOC_TABLE);
            if (!it) return;
            it->table = calloc(1, sizeof(DocTable));
            if (!it->table) { ctx_fail(c, T("メモリが足りません")); return; }
            it->whole.start = here(c);
            it->refuse = R_TABLE;
            it->refuse_delete = D_TABLE;
            snprintf(it->prefix, sizeof(it->prefix), "%.*s", (int)q.prefix_len, q.prefix);
            c->table = (long)(c->d->nitems - 1);
            c->table_depth = c->depth;
            c->tbl_nest = 1;
            c->target = NULL;
            return;
        }
        return;
    }
    if (c->table >= 0) {
        DocTable *t = item(c, c->table)->table;
        if (w && q.id == E_tbl) { c->tbl_nest++; return; }
        if (c->tbl_nest == 1 && w && q.id == E_tr && c->depth == c->table_depth + 1) {
            if (grow((void **)&t->rows, &t->cap, t->n + 1, sizeof(DocRow))) { ctx_fail(c, T("メモリが足りません")); return; }
            memset(&t->rows[t->n++], 0, sizeof(DocRow));
            return;
        }
        if (c->tbl_nest == 1 && w && q.id == E_tc && c->depth == c->table_depth + 2 && t->n) {
            DocRow *r = &t->rows[t->n - 1];
            if (grow((void **)&r->cells, &r->cap, r->n + 1, sizeof(DocCell))) { ctx_fail(c, T("メモリが足りません")); return; }
            DocCell *cell = &r->cells[r->n++];
            memset(cell, 0, sizeof(*cell));
            cell->span = 1;
            c->cell_depth = c->depth;
            c->cell_paras = 0;
            c->target = &cell->text;
            return;
        }
        if (c->tbl_nest == 1 && w && c->cell_depth && c->depth == c->cell_depth + 2 && c->stack[c->depth - 1].id == E_tcPr && t->n) {
            DocRow *r = &t->rows[t->n - 1];
            if (r->n && q.id == E_gridSpan) {
                const char *v = wattr(a, "val");
                long span = v ? strtol(v, NULL, 10) : 1;
                r->cells[r->n - 1].span = span >= 1 && span <= 64 ? (int)span : 1;
            } else if (r->n && q.id == E_vMerge) {
                const char *v = wattr(a, "val");
                r->cells[r->n - 1].vcont = !v || strcmp(v, "restart");
            }
            return;
        }
        if (w && q.id == E_p && c->target) {
            if (c->cell_paras++) doc_text_add(c->target, '\n', WHITE);
            return;
        }
        if (w && q.id == E_t) { c->in_t = c->depth; c->t_edit = 0; return; }
        if (w && (q.id == E_tab || q.id == E_br || q.id == E_cr) && c->target)
            doc_text_add(c->target, q.id == E_tab ? '\t' : '\n', WHITE);
        return;
    }
    if (c->para < 0) return;
    DocItem *it = item(c, c->para);
    paragraph_element(c, it, &q);
    if (c->depth == c->para_depth + 1) { para_child_start(c, it, &q); return; }
    if (c->child < 0) return;
    DocChild *ch = &it->children[c->child];
    if (c->depth == c->para_depth + 2 && ch->kind == DOC_CHILD_TEXT_RUN) run_child_start(c, it, &q, a);
    if (ch->kind == DOC_CHILD_PPR) {
        if (c->depth == c->para_depth + 2 && w) {
            if (q.id == E_pStyle) { free(c->pstyle); c->pstyle = dup_or_null(wattr(a, "val")); }
            else if (q.id == E_pageBreakBefore) {
                if (on_off(wattr(a, "val"))) it->sep_before = 1;
                add_drop(it, here(c), 0);
            } else if (q.id == E_sectPr) {
                it->has_sect = 1;
                c->sect_depth = c->depth;
                c->sect_continuous = 0;
                refuse_delete(it, D_SECT);
                add_drop(it, here(c), 0);
            } else if (q.id == E_rPr) {
                it->ppr_rpr.start = here(c);
                c->ppr_rpr_depth = c->depth;
            } else if (q.id == E_pPrChange) add_drop(it, here(c), 0);
        } else if (c->sect_depth && c->depth == c->sect_depth + 1 && w && q.id == E_type) {
            const char *v = wattr(a, "val");
            c->sect_continuous = v && (!strcmp(v, "continuous") || !strcmp(v, "nextColumn"));
        } else if (c->ppr_rpr_depth && c->depth == c->ppr_rpr_depth + 1 && w &&
                   (q.id == E_ins || q.id == E_del || q.id == E_moveFrom || q.id == E_moveTo || q.id == E_rPrChange)) {
            it->ppr_rpr_clean = 0;
            add_drop(it, here(c), 0);
        }
        return;
    }
    if (ch->kind == DOC_CHILD_TEXT_RUN && w && q.id == E_rPrChange) ch->rpr_change = 1;
    /* Text, anywhere in the paragraph (runs inside links, fields, insertions). */
    if (w && q.id == E_t) { c->in_t = c->depth; c->t_edit = in_run_child(c); return; }
    c->t_edit = in_run_child(c);
    if (w && q.id == E_tab) text_control(c, '\t', "\t");
    else if (w && q.id == E_cr) text_control(c, '\n', "\n");
    else if (w && q.id == E_br) {
        const char *type = wattr(a, "type");
        if (type && !strcmp(type, "page")) text_control(c, '\f', "\f");
        else if (!type || !strcmp(type, "textWrapping")) text_control(c, '\n', "\n");
    } else if (w && q.id == E_noBreakHyphen) text_control(c, 0x2011, "\xe2\x80\x91");
    else if (w && q.id == E_softHyphen) { edit_char(c, "\xc2\xad", 2); }
    c->t_edit = 0;
}
/* Closes the last drop range opened at this depth. */
static void close_drop(DocItem *it, uint64_t end)
{
    for (int i = it->ndrops - 1; i >= 0; --i)
        if (!it->drops[i].end) { it->drops[i].end = end; return; }
}
static void para_end(Ctx *c, DocItem *it)
{
    it->whole.end = fdtp_xml_byte_count(c->parser) ? here_end(c) : it->tag_end;
    it->empty_tag = !fdtp_xml_byte_count(c->parser);
    it->end_tag = it->empty_tag ? it->tag_end : here(c);
    it->heading = is_heading(c->styles, c->pstyle);
    if (it->heading)
        for (size_t i = 0; i < it->disp.n; ++i) if (it->disp.color[i] == WHITE) it->disp.color[i] = YELLOW;
    it->edit = strdup(c->edit.data ? c->edit.data : "");
    if (!it->edit || c->edit.failed || it->disp.failed) { ctx_fail(c, T("メモリが足りません")); return; }
    /* Image and note runs must lie outside the text runs. */
    long first = -1, last = -1;
    for (size_t i = 0; i < it->nchildren; ++i) {
        if (it->children[i].kind != DOC_CHILD_TEXT_RUN) continue;
        if (first < 0) first = (long)i;
        last = (long)i;
    }
    for (size_t i = 0; i < it->nchildren; ++i) {
        DocChild *ch = &it->children[i];
        if (ch->kind != DOC_CHILD_SPECIAL_RUN) continue;
        if (first >= 0 && (long)i > first && (long)i < last) refuse(it, R_MID);
        if (first < 0 || (long)i < first) it->images_before += ch->images;
        else it->images_after += ch->images;
    }
    c->sect_depth = 0;
    c->para = -1;
    c->child = -1;
    c->target = NULL;
}
static void doc_end(void *u, const char *name)
{
    (void)name;
    Ctx *c = u;
    if (c->stop) return;
    int depth = c->depth;
    Frame f = c->stack[depth];
    c->depth--;
    if (c->skip) {
        if (depth == c->skip) c->skip = 0;
        return;
    }
    if (c->in_t == depth) { c->in_t = 0; c->t_edit = 0; }
    if (c->table >= 0) {
        if (f.ns == NS_W && f.id == E_tbl) {
            if (--c->tbl_nest == 0) {
                item(c, c->table)->whole.end = here_end(c);
                c->table = -1;
                c->target = NULL;
            }
        } else if (c->tbl_nest == 1 && depth == c->cell_depth && f.ns == NS_W && f.id == E_tc) {
            c->cell_depth = 0;
            c->target = NULL;
        }
        return;
    }
    if (c->para < 0) return;
    DocItem *it = item(c, c->para);
    if (depth == c->para_depth) { para_end(c, it); return; }
    if (c->child < 0) return;
    DocChild *ch = &it->children[c->child];
    uint64_t end = fdtp_xml_byte_count(c->parser) ? here_end(c) : here(c);
    if (depth == c->para_depth + 1) {
        ch->range.end = end;
        ch->empty = !fdtp_xml_byte_count(c->parser);
        if (ch->kind == DOC_CHILD_TEXT_RUN && c->child_special) {
            if (c->child_text) refuse(it, R_MIXED);
            ch->kind = DOC_CHILD_SPECIAL_RUN;
        }
        if (ch->kind == DOC_CHILD_PPR) it->ppr.end = end;
        c->child = -1;
        return;
    }
    if (ch->kind == DOC_CHILD_TEXT_RUN && depth == c->para_depth + 2 && f.ns == NS_W && f.id == E_rPr) ch->rpr.end = end;
    if (ch->kind == DOC_CHILD_PPR) {
        if (depth == c->para_depth + 2 && f.ns == NS_W && f.id == E_rPr) { it->ppr_rpr.end = end; c->ppr_rpr_depth = 0; }
        else if (depth == c->para_depth + 2 && f.ns == NS_W && (f.id == E_sectPr || f.id == E_pageBreakBefore || f.id == E_pPrChange)) {
            close_drop(it, end);
            if (f.id == E_sectPr) {
                if (!c->sect_continuous) it->sep_after = 1;
                c->sect_depth = 0;
            }
        } else if (c->ppr_rpr_depth && depth == c->ppr_rpr_depth + 1 && f.ns == NS_W &&
                   (f.id == E_ins || f.id == E_del || f.id == E_moveFrom || f.id == E_moveTo || f.id == E_rPrChange))
            close_drop(it, end);
    }
}
static void doc_chars(void *u, const char *s, int n)
{
    Ctx *c = u;
    if (c->stop || c->skip || !c->in_t || n <= 0) return;
    if (c->target) doc_text_utf8(c->target, s, (size_t)n, WHITE);
    if (c->t_edit) buf_add(&c->edit, s, (size_t)n);
}
static void doc_decl(void *u, const char *version, const char *encoding, int standalone)
{
    (void)version; (void)standalone;
    Ctx *c = u;
    if (encoding && strcasecmp(encoding, "UTF-8")) c->bad_encoding = 1;
}

static int parse_main(DocFile *d, const Styles *styles, char *err, size_t errlen)
{
    if (d->xml_len > INT32_MAX) { fail(err, errlen, T("文書が大きすぎます")); return -1; }
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.d = d; c.styles = styles; c.para = -1; c.table = -1; c.child = -1;
    c.stack = calloc(MAX_DEPTH + 1, sizeof(Frame));
    c.parser = XML_ParserCreateNS(NULL, NS_SEP);
    if (!c.stack || !c.parser) {
        free(c.stack);
        if (c.parser) XML_ParserFree(c.parser);
        fail(err, errlen, T("メモリが足りません"));
        return -1;
    }
    XML_SetReturnNSTriplet(c.parser, 1);
    XML_SetUserData(c.parser, &c);
    XML_SetElementHandler(c.parser, doc_start, doc_end);
    XML_SetCharacterDataHandler(c.parser, doc_chars);
    XML_SetXmlDeclHandler(c.parser, doc_decl);
    int ok = XML_Parse(c.parser, (const char *)d->xml, (int)d->xml_len, 1) == XML_STATUS_OK;
    if (!ok && !c.err[0])
        snprintf(c.err, sizeof(c.err), T("XML が正しくありません（%llu 行目）"), (unsigned long long)fdtp_xml_line_number(c.parser));
    if (ok && !c.err[0] && !c.root_seen) snprintf(c.err, sizeof(c.err), "%s", T("Word の文書ではありません"));
    XML_ParserFree(c.parser);
    free(c.stack);
    free(c.pstyle);
    free(c.edit.data);
    if (c.err[0]) { fail(err, errlen, "%s", c.err); return -1; }
    if (c.bad_encoding || (d->xml_len >= 2 && ((d->xml[0] == 0xff && d->xml[1] == 0xfe) || (d->xml[0] == 0xfe && d->xml[1] == 0xff))))
        d->readonly = T("文書の文字コードが UTF-8 でないため保存できません");
    return 0;
}

void doc_item_free(DocItem *it)
{
    doc_text_free(&it->disp);
    free(it->edit);
    free(it->children);
    free(it->new_ppr);
    free(it->new_rpr);
    if (it->table) {
        for (size_t r = 0; r < it->table->n; ++r) {
            for (size_t i = 0; i < it->table->rows[r].n; ++i) doc_text_free(&it->table->rows[r].cells[i].text);
            free(it->table->rows[r].cells);
        }
        free(it->table->rows);
        free(it->table);
    }
    memset(it, 0, sizeof(*it));
}
void doc_free(DocFile *d)
{
    for (size_t i = 0; i < d->nitems; ++i) doc_item_free(&d->items[i]);
    free(d->items);
    free(d->xml);
    free(d->part);
    free(d->path);
    if (d->zip) zip_close(d->zip);
    memset(d, 0, sizeof(*d));
}
int doc_dirty(const DocFile *d)
{
    for (size_t i = 0; i < d->nitems; ++i) {
        const DocItem *it = &d->items[i];
        if (it->edited || it->deleted || it->is_new) return 1;
    }
    return 0;
}

static void load_styles(DocFile *d, Styles *out);
int doc_open(DocFile *d, const char *path, char *err, size_t errlen)
{
    memset(d, 0, sizeof(*d));
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) { fail(err, errlen, T("ファイルを開けません")); return -1; }
    d->path = strdup(path);
    if (!d->path) { fail(err, errlen, T("メモリが足りません")); return -1; }
    d->mtime = st.st_mtime;
    d->size = st.st_size;
    if (zip_open(path, &d->zip, err, errlen)) { d->zip = NULL; doc_free(d); return -1; }
    Rels root;
    char *part = NULL;
    if (!read_rels(d->zip, "_rels/.rels", &root)) {
        const Rel *r = rel_type(&root, "/officeDocument");
        if (r) part = resolve("", r->target);
        rels_free(&root);
    }
    if (!part || zip_find(d->zip, part) < 0) {
        free(part);
        part = strdup("word/document.xml");
    }
    d->part = part;
    if (!d->part) { fail(err, errlen, T("メモリが足りません")); doc_free(d); return -1; }
    if (read_part(d->zip, d->part, PART_LIMIT, &d->xml, &d->xml_len, err, errlen)) { doc_free(d); return -1; }
    Styles styles;
    load_styles(d, &styles);
    int rc = parse_main(d, &styles, err, errlen);
    styles_free(&styles);
    if (rc) { doc_free(d); return -1; }
    return 0;
}
/* Revision 1 item 38: the document again from other bytes of the main part
 * (an undo or redo), with the same zip, path and backup state. */
int doc_reparse(DocFile *d, unsigned char *xml, size_t len, char *err, size_t errlen)
{
    DocFile t = *d;
    t.items = NULL;
    t.nitems = t.citems = 0;
    t.xml = xml;
    t.xml_len = len;
    t.readonly = NULL;
    t.root_prefix[0] = 0;
    Styles styles;
    load_styles(&t, &styles);
    int rc = parse_main(&t, &styles, err, errlen);
    styles_free(&styles);
    if (rc) {
        for (size_t i = 0; i < t.nitems; ++i) doc_item_free(&t.items[i]);
        free(t.items);
        free(xml);
        return -1;
    }
    for (size_t i = 0; i < d->nitems; ++i) doc_item_free(&d->items[i]);
    free(d->items);
    free(d->xml);
    *d = t;
    return 0;
}
/* styles.xml through the main part's relationships (empty when there is
 * none or it cannot be read: then nothing is a heading). */
static void load_styles(DocFile *d, Styles *out)
{
    Styles styles;
    memset(&styles, 0, sizeof(styles));
    char *rn = rels_name(d->part), *style_part = NULL;
    Rels drels;
    if (rn && !read_rels(d->zip, rn, &drels)) {
        const Rel *r = rel_type(&drels, "/styles");
        if (r) style_part = resolve(d->part, r->target);
        rels_free(&drels);
    }
    free(rn);
    unsigned char *sdata;
    size_t sn;
    if (style_part && zip_find(d->zip, style_part) >= 0 && !read_part(d->zip, style_part, STYLE_LIMIT, &sdata, &sn, NULL, 0)) {
        XML_Parser p = XML_ParserCreateNS(NULL, NS_SEP);
        if (p && sn <= INT32_MAX) {
            XML_SetReturnNSTriplet(p, 1);
            XML_SetUserData(p, &styles);
            XML_SetElementHandler(p, styles_start, styles_end);
            if (XML_Parse(p, (const char *)sdata, (int)sn, 1) != XML_STATUS_OK) styles_free(&styles);
        }
        if (p) XML_ParserFree(p);
        free(sdata);
    }
    free(style_part);
    *out = styles;
}
