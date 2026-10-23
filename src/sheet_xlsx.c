// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
/* xlsx (OOXML SpreadsheetML) with expat over zipfile.c.
 * Load: relationships -> workbook (sheets, date1904) -> sharedStrings ->
 * styles (number formats: dates and decimals) -> each sheet when shown.
 * Save: the edited sheet XML is streamed twice from the original zip entry.
 * Pass 1 finds the byte ranges of the edited cells, of the rows they live in
 * and of <dimension>; pass 2 copies every other byte unchanged and splices
 * in the new cells. All other zip entries are copied byte for byte, except
 * xl/calcChain.xml when a formula was removed (its entries would otherwise
 * point at cells without formulas, which Excel reports as damage). */
#include "sheet_model.h"
#include <ctype.h>
#include "expat_compat.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PART_LIMIT (64u * 1024 * 1024)

static void fail(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n || err[0]) return;   /* keep the first, most specific message */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- small text buffer ---- */
typedef struct { char *data; size_t len, cap; int failed; } Buf;
static void buf_add(Buf *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) { b->failed = 1; return; }
        b->data = p; b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_fmt(Buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}
static void buf_clear(Buf *b) { b->len = 0; if (b->data) b->data[0] = 0; }
static const char *buf_text(const Buf *b) { return b->data ? b->data : ""; }
/* XML escaping; strings additionally use the OOXML _xHHHH_ escape for
 * characters XML cannot carry and for literal "_xHHHH_" text. */
static void buf_xml(Buf *b, const char *s, int attribute, int ooxml)
{
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '&') buf_str(b, "&amp;");
        else if (c == '<') buf_str(b, "&lt;");
        else if (c == '>') buf_str(b, "&gt;");
        else if (c == '"' && attribute) buf_str(b, "&quot;");
        else if (c < 0x20 && c != '\t' && c != '\n') {
            if (ooxml) buf_fmt(b, "_x%04X_", c);
        } else if (ooxml && c == '_' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
                   isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5]) && p[6] == '_') buf_str(b, "_x005F_");
        else buf_add(b, p, 1);
    }
}
/* OOXML strings: decode _xHHHH_ into UTF-8. */
static void unescape_ooxml(Buf *b)
{
    if (!b->data || !strstr(b->data, "_x")) return;
    char *w = b->data;
    for (const char *p = b->data; *p;) {
        unsigned v;
        if (p[0] == '_' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
            isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5]) && p[6] == '_' && sscanf(p + 2, "%4x", &v) == 1) {
            if (v < 0x80) *w++ = (char)v;
            else if (v < 0x800) { *w++ = (char)(0xc0 | v >> 6); *w++ = (char)(0x80 | (v & 0x3f)); }
            else { *w++ = (char)(0xe0 | v >> 12); *w++ = (char)(0x80 | ((v >> 6) & 0x3f)); *w++ = (char)(0x80 | (v & 0x3f)); }
            p += 7;
        } else *w++ = *p++;
    }
    *w = 0;
    b->len = (size_t)(w - b->data);
}

static const char *local(const char *name)
{
    const char *c = strrchr(name, ':');
    return c ? c + 1 : name;
}
static const char *attr(const char **a, const char *name)
{
    for (; a && a[0]; a += 2) if (!strcmp(local(a[0]), name)) return a[1];
    return NULL;
}
/* Prefix of a qualified name including ':' ("x:" of "x:c"), or "". */
static void prefix_of(const char *name, char *out, size_t n)
{
    const char *c = strrchr(name, ':');
    size_t k = c ? (size_t)(c - name + 1) : 0;
    if (k >= n) k = 0;
    memcpy(out, name, k);
    out[k] = 0;
}
static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* ---- streaming expat over a zip entry ---- */
typedef struct { XML_Parser parser; int stop; XML_XmlDeclHandler decl; } XmlBase;
typedef struct { XML_Parser parser; char *err; size_t errlen; const char *part; } Feed;
static int feed_sink(void *ctx, const unsigned char *data, size_t n)
{
    Feed *f = ctx;
    while (n) {
        int k = n > (1u << 30) ? (1 << 30) : (int)n;
        if (XML_Parse(f->parser, (const char *)data, k, 0) != XML_STATUS_OK) {
            if (XML_GetErrorCode(f->parser) != XML_ERROR_ABORTED)
                fail(f->err, f->errlen, T("%s の XML が壊れています（%llu 行）"), f->part, (unsigned long long)fdtp_xml_line_number(f->parser));
            return 1;
        }
        data += k; n -= (size_t)k;
    }
    return 0;
}
static int parse_part(Book *b, const char *part, XmlBase *ud, XML_StartElementHandler st, XML_EndElementHandler en,
                      XML_CharacterDataHandler ch, char *err, size_t errlen)
{
    long index = zip_find(b->zip, part);
    if (index < 0) { fail(err, errlen, T("%s がありません"), part); return -1; }
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) { fail(err, errlen, T("メモリが足りません")); return -1; }
    ud->parser = p;
    XML_SetUserData(p, ud);
    XML_SetElementHandler(p, st, en);
    if (ch) XML_SetCharacterDataHandler(p, ch);
    if (ud->decl) XML_SetXmlDeclHandler(p, ud->decl);
    Feed f = {p, err, errlen, part};
    int rc = zip_read_stream(b->zip, (size_t)index, feed_sink, &f, err, errlen);
    if (!rc && XML_Parse(p, "", 0, 1) != XML_STATUS_OK) {
        if (XML_GetErrorCode(p) != XML_ERROR_ABORTED)
            fail(err, errlen, T("%s の XML が壊れています（%llu 行）"), part, (unsigned long long)fdtp_xml_line_number(p));
        rc = -1;
    }
    if (rc && ud->stop) rc = -1;
    XML_ParserFree(p);
    if (ud->stop) { fail(err, errlen, T("%s を読めません"), part); return -1; }
    return rc ? -1 : 0;
}
static void stop(XmlBase *ud) { ud->stop = 1; XML_StopParser(ud->parser, XML_FALSE); }
static uint64_t here(XmlBase *ud) { return (uint64_t)fdtp_xml_byte_index(ud->parser); }
static uint64_t here_end(XmlBase *ud) { return (uint64_t)fdtp_xml_byte_index(ud->parser) + (uint64_t)fdtp_xml_byte_count(ud->parser); }

/* ---- part names and relationships ---- */
static char *resolve(const char *source, const char *target)
{
    Buf b = {0};
    if (target[0] == '/') buf_str(&b, target + 1);
    else {
        const char *slash = strrchr(source, '/');
        if (slash) buf_add(&b, source, (size_t)(slash - source + 1));
        buf_str(&b, target);
    }
    if (b.failed) { free(b.data); return NULL; }
    /* Normalise "." and ".." segments. */
    char *out = malloc(b.len + 1);
    if (!out) { free(b.data); return NULL; }
    size_t n = 0;
    for (char *seg = b.data; seg && *seg;) {
        char *next = strchr(seg, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        if (len == 1 && seg[0] == '.') {}
        else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n) { --n; while (n && out[n - 1] != '/') --n; }
        } else if (len) {
            memcpy(out + n, seg, len); n += len;
            if (next) out[n++] = '/';
        }
        seg = next ? next + 1 : NULL;
    }
    out[n] = 0;
    free(b.data);
    return out;
}
static char *rels_of(const char *part)
{
    const char *slash = strrchr(part, '/');
    size_t dir = slash ? (size_t)(slash - part + 1) : 0;
    Buf b = {0};
    buf_add(&b, part, dir);
    buf_str(&b, "_rels/");
    buf_str(&b, part + dir);
    buf_str(&b, ".rels");
    if (b.failed) { free(b.data); return NULL; }
    return b.data;
}
typedef struct { char *id, *type, *target; } Rel;
typedef struct { XmlBase base; Rel *rels; size_t n; int failed; } RelsCtx;
static void rels_start(void *u, const char *name, const char **a)
{
    RelsCtx *c = u;
    if (strcmp(local(name), "Relationship")) return;
    const char *id = attr(a, "Id"), *type = attr(a, "Type"), *target = attr(a, "Target"), *mode = attr(a, "TargetMode");
    if (!id || !type || !target || (mode && !strcmp(mode, "External"))) return;
    Rel *p = realloc(c->rels, (c->n + 1) * sizeof(*p));
    if (!p) { stop(&c->base); return; }
    c->rels = p;
    p[c->n].id = strdup(id); p[c->n].type = strdup(type); p[c->n].target = strdup(target);
    c->n++;
    if (!p[c->n - 1].id || !p[c->n - 1].type || !p[c->n - 1].target) stop(&c->base);
}
static void rels_free(RelsCtx *c)
{
    for (size_t i = 0; i < c->n; ++i) { free(c->rels[i].id); free(c->rels[i].type); free(c->rels[i].target); }
    free(c->rels);
}
static const Rel *rel_type(const RelsCtx *c, const char *tail)
{
    for (size_t i = 0; i < c->n; ++i) if (ends_with(c->rels[i].type, tail)) return &c->rels[i];
    return NULL;
}
static const Rel *rel_id(const RelsCtx *c, const char *id)
{
    for (size_t i = 0; i < c->n; ++i) if (!strcmp(c->rels[i].id, id)) return &c->rels[i];
    return NULL;
}
static void ignore_end(void *u, const char *name) { (void)u; (void)name; }

/* ---- workbook ---- */
typedef struct { char *name, *rid; uint32_t id; } SheetRef;
typedef struct { XmlBase base; SheetRef *sheets; size_t n; int date1904; } WorkbookCtx;
static void workbook_start(void *u, const char *name, const char **a)
{
    WorkbookCtx *c = u;
    const char *ln = local(name);
    if (!strcmp(ln, "workbookPr")) {
        const char *d = attr(a, "date1904");
        c->date1904 = d && (!strcmp(d, "1") || !strcmp(d, "true"));
    } else if (!strcmp(ln, "sheet")) {
        const char *n = attr(a, "name"), *id = attr(a, "sheetId"), *rid = attr(a, "id");
        SheetRef *p = realloc(c->sheets, (c->n + 1) * sizeof(*p));
        if (!p) { stop(&c->base); return; }
        c->sheets = p;
        p[c->n].name = strdup(n ? n : "");
        p[c->n].rid = strdup(rid ? rid : "");
        p[c->n].id = id ? (uint32_t)strtoul(id, NULL, 10) : 0;
        c->n++;
        if (!p[c->n - 1].name || !p[c->n - 1].rid) stop(&c->base);
    }
}

/* ---- shared strings ---- */
typedef struct { XmlBase base; Book *b; Buf text; int in_si, in_t, rph; size_t cap; } SstCtx;
static void sst_start(void *u, const char *name, const char **a)
{
    (void)a;
    SstCtx *c = u;
    const char *ln = local(name);
    if (!strcmp(ln, "si")) { c->in_si = 1; buf_clear(&c->text); }
    else if (!strcmp(ln, "rPh")) c->rph++;
    else if (!strcmp(ln, "t") && c->in_si && !c->rph) c->in_t = 1;
}
static void sst_end(void *u, const char *name)
{
    SstCtx *c = u;
    const char *ln = local(name);
    if (!strcmp(ln, "t")) c->in_t = 0;
    else if (!strcmp(ln, "rPh")) { if (c->rph) c->rph--; }
    else if (!strcmp(ln, "si") && c->in_si) {
        c->in_si = 0;
        Book *b = c->b;
        if (b->nsst == c->cap) {
            size_t cap = c->cap ? c->cap * 2 : 1024;
            uint32_t *p = realloc(b->sst_off, cap * sizeof(*p));
            if (!p) { stop(&c->base); return; }
            b->sst_off = p; c->cap = cap;
        }
        unescape_ooxml(&c->text);
        uint32_t at = arena_add(&b->sst, buf_text(&c->text), c->text.len);
        if (at == SHEET_NONE || c->text.failed) { stop(&c->base); return; }
        b->sst_off[b->nsst++] = at;
    }
}
static void sst_chars(void *u, const char *s, int n)
{
    SstCtx *c = u;
    if (c->in_t) buf_add(&c->text, s, (size_t)n);
}

/* ---- styles: number formats ---- */
typedef struct { uint32_t id; char *code; } NumFmt;
typedef struct { XmlBase base; NumFmt *fmts; size_t nfmts; uint32_t *xf; size_t nxf; int in_xfs; } StyleCtx;
static void style_start(void *u, const char *name, const char **a)
{
    StyleCtx *c = u;
    const char *ln = local(name);
    if (!strcmp(ln, "numFmt")) {
        const char *id = attr(a, "numFmtId"), *code = attr(a, "formatCode");
        if (!id || !code) return;
        NumFmt *p = realloc(c->fmts, (c->nfmts + 1) * sizeof(*p));
        if (!p) { stop(&c->base); return; }
        c->fmts = p;
        p[c->nfmts].id = (uint32_t)strtoul(id, NULL, 10);
        p[c->nfmts].code = strdup(code);
        if (!p[c->nfmts++].code) stop(&c->base);
    } else if (!strcmp(ln, "cellXfs")) c->in_xfs = 1;
    else if (!strcmp(ln, "xf") && c->in_xfs) {
        const char *id = attr(a, "numFmtId");
        uint32_t *p = realloc(c->xf, (c->nxf + 1) * sizeof(*p));
        if (!p) { stop(&c->base); return; }
        c->xf = p;
        p[c->nxf++] = id ? (uint32_t)strtoul(id, NULL, 10) : 0;
    }
}
static void style_end(void *u, const char *name)
{
    StyleCtx *c = u;
    if (!strcmp(local(name), "cellXfs")) c->in_xfs = 0;
}
static CellFormat classify_code(const char *code)
{
    CellFormat f = {FMT_GENERAL, 0, 0};
    if (!strncasecmp(code, "General", 7)) return f;
    int date = 0, time = 0, month = 0, percent = 0, digits = 0, decimals = 0, point = 0, seconds = 0;
    for (const char *p = code; *p && *p != ';'; ++p) {
        char c = *p;
        if (c == '"') {
            p = strchr(p + 1, '"');
            if (!p) break;
            continue;
        }
        if (c == '\\' || c == '_' || c == '*') {
            if (p[1]) ++p;
            continue;
        }
        if (c == '[') {
            const char *q = p + 1;
            if (*q == 'h' || *q == 'H' || *q == 'm' || *q == 'M' || *q == 's' || *q == 'S') time = 1;
            while (*p && *p != ']') ++p;
            if (!*p) break;
            continue;
        }
        c = (char)tolower((unsigned char)c);
        if (c == 'y' || c == 'd' || c == 'e' || c == 'g') date = 1;
        else if (c == 'h') time = 1;
        else if (c == 's') { time = 1; seconds = 1; }
        else if (c == 'm') month = 1;
        else if (c == '%') percent = 1;
        else if (c == '.') point = 1;
        else if (c == '0' || c == '#' || c == '?') { digits = 1; if (point) ++decimals; }
    }
    if (month && !time) date = 1;
    if (date || time) {
        f.kind = date && time ? FMT_DATETIME : date ? FMT_DATE : FMT_TIME;
        f.seconds = (uint8_t)seconds;
        return f;
    }
    if (percent) { f.kind = FMT_PERCENT; f.decimals = (uint8_t)(decimals > 10 ? 10 : decimals); return f; }
    if (digits) { f.kind = FMT_FIXED; f.decimals = (uint8_t)(decimals > 10 ? 10 : decimals); }
    return f;
}
static CellFormat classify(uint32_t id, const StyleCtx *c)
{
    for (size_t i = 0; i < c->nfmts; ++i) if (c->fmts[i].id == id) return classify_code(c->fmts[i].code);
    CellFormat f = {FMT_GENERAL, 0, 0};
    switch (id) {
    case 1: case 3: case 37: case 38: f.kind = FMT_FIXED; break;
    case 2: case 4: case 39: case 40: f.kind = FMT_FIXED; f.decimals = 2; break;
    case 9: f.kind = FMT_PERCENT; break;
    case 10: f.kind = FMT_PERCENT; f.decimals = 2; break;
    case 14: case 15: case 16: case 17: f.kind = FMT_DATE; break;
    case 18: case 20: f.kind = FMT_TIME; break;
    case 19: case 21: case 45: case 46: case 47: f.kind = FMT_TIME; f.seconds = 1; break;
    case 22: f.kind = FMT_DATETIME; break;
    default:
        if ((id >= 27 && id <= 36) || (id >= 50 && id <= 58)) f.kind = FMT_DATE;
        break;
    }
    return f;
}
CellFormat xlsx_format(const Book *b, uint32_t style)
{
    CellFormat f = {FMT_GENERAL, 0, 0};
    if (b->type != BOOK_XLSX || style >= b->nxf) return f;
    return b->xf[style];
}

/* ---- open ---- */
int xlsx_open(Book *b, const char *path, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    b->type = BOOK_XLSX;
    if (zip_open(path, &b->zip, err, errlen)) return -1;
    RelsCtx pkg = {{0}, NULL, 0, 0}, wbrels = {{0}, NULL, 0, 0};
    WorkbookCtx wb = {{0}, NULL, 0, 0};
    char *workbook = NULL, *rels = NULL;
    int rc = -1;
    if (zip_find(b->zip, "_rels/.rels") >= 0 &&
        parse_part(b, "_rels/.rels", &pkg.base, rels_start, ignore_end, NULL, err, errlen)) goto done;
    const Rel *office = rel_type(&pkg, "/officeDocument");
    workbook = office ? resolve("", office->target) : strdup("xl/workbook.xml");
    if (!workbook) { fail(err, errlen, T("メモリが足りません")); goto done; }
    if (zip_find(b->zip, workbook) < 0) { fail(err, errlen, T("xlsx ではありません（ブックがありません）")); goto done; }
    if (parse_part(b, workbook, &wb.base, workbook_start, ignore_end, NULL, err, errlen)) goto done;
    b->date1904 = wb.date1904;
    rels = rels_of(workbook);
    if (!rels) { fail(err, errlen, T("メモリが足りません")); goto done; }
    if (zip_find(b->zip, rels) >= 0 && parse_part(b, rels, &wbrels.base, rels_start, ignore_end, NULL, err, errlen)) goto done;
    b->workbook_rels = rels; rels = NULL;
    if (!wb.n) { fail(err, errlen, T("シートがありません")); goto done; }
    b->sheets = calloc(wb.n, sizeof(Sheet));
    if (!b->sheets) { fail(err, errlen, T("メモリが足りません")); goto done; }
    b->nsheets = (int)wb.n;
    for (size_t i = 0; i < wb.n; ++i) {
        Sheet *s = &b->sheets[i];
        s->name = strdup(wb.sheets[i].name);
        s->sheet_id = wb.sheets[i].id;
        const Rel *r = rel_id(&wbrels, wb.sheets[i].rid);
        if (r && ends_with(r->type, "/worksheet")) s->part = resolve(workbook, r->target);
        if (!s->name) { fail(err, errlen, T("メモリが足りません")); goto done; }
    }
    const Rel *sst = rel_type(&wbrels, "/sharedStrings");
    char *sst_part = sst ? resolve(workbook, sst->target) : NULL;
    if (sst_part && zip_find(b->zip, sst_part) >= 0) {
        SstCtx c = {{0}, b, {0}, 0, 0, 0, 0};
        int bad = parse_part(b, sst_part, &c.base, sst_start, sst_end, sst_chars, err, errlen);
        free(c.text.data);
        if (bad) { free(sst_part); goto done; }
    }
    free(sst_part);
    const Rel *sty = rel_type(&wbrels, "/styles");
    char *style_part = sty ? resolve(workbook, sty->target) : NULL;
    if (style_part && zip_find(b->zip, style_part) >= 0) {
        StyleCtx c = {{0}, NULL, 0, NULL, 0, 0};
        int bad = parse_part(b, style_part, &c.base, style_start, style_end, NULL, err, errlen);
        if (!bad) {
            b->xf = calloc(c.nxf ? c.nxf : 1, sizeof(CellFormat));
            if (b->xf) { b->nxf = c.nxf; for (size_t i = 0; i < c.nxf; ++i) b->xf[i] = classify(c.xf[i], &c); }
        }
        for (size_t i = 0; i < c.nfmts; ++i) free(c.fmts[i].code);
        free(c.fmts); free(c.xf);
        if (bad) { free(style_part); goto done; }
    }
    free(style_part);
    const Rel *chain = rel_type(&wbrels, "/calcChain");
    if (chain) b->calc_chain = resolve(workbook, chain->target);
    rc = 0;
done:
    for (size_t i = 0; i < wb.n; ++i) { free(wb.sheets[i].name); free(wb.sheets[i].rid); }
    free(wb.sheets); free(workbook); free(rels);
    rels_free(&pkg); rels_free(&wbrels);
    return rc;
}

/* ---- sheet load ---- */
typedef struct {
    XmlBase base;
    Book *b;
    Sheet *s;
    int in_data, in_c, in_v, in_f, in_is, in_t, rph, depth, data_depth;
    long row, col;
    uint32_t style;
    char t[16], ft[16];
    int has_f, has_si, has_v, too_big;
    uint32_t si;
    Buf v, f, is, ref;
} LoadCtx;
static int push_range(SheetRange **list, size_t *n, const char *ref)
{
    SheetRange r;
    if (!ref || sheet_parse_range(ref, &r)) return 0;
    SheetRange *p = realloc(*list, (*n + 1) * sizeof(*p));
    if (!p) return -1;
    *list = p;
    p[(*n)++] = r;
    return 0;
}
static void load_start(void *u, const char *name, const char **a)
{
    LoadCtx *c = u;
    const char *ln = local(name);
    c->depth++;
    if (!strcmp(ln, "sheetData")) { c->in_data = 1; c->data_depth = c->depth; c->row = -1; return; }
    if (!strcmp(ln, "mergeCell")) {
        if (push_range(&c->s->merges, &c->s->nmerges, attr(a, "ref"))) stop(&c->base);
        return;
    }
    if (!c->in_data) return;
    if (!strcmp(ln, "row") && c->depth == c->data_depth + 1) {
        const char *r = attr(a, "r");
        c->row = r ? strtol(r, NULL, 10) - 1 : c->row + 1;
        c->col = -1;
        return;
    }
    if (!strcmp(ln, "c") && c->depth == c->data_depth + 2) {
        const char *r = attr(a, "r"), *s = attr(a, "s"), *t = attr(a, "t");
        uint32_t row, col;
        if (r && sheet_parse_address(r, &row, &col) == strlen(r)) { c->row = row; c->col = col; }
        else c->col++;
        c->in_c = 1;
        c->style = s ? (uint32_t)strtoul(s, NULL, 10) : 0;
        snprintf(c->t, sizeof(c->t), "%s", t ? t : "");
        c->has_f = c->has_si = c->has_v = 0;
        c->ft[0] = 0;
        buf_clear(&c->v); buf_clear(&c->f); buf_clear(&c->is); buf_clear(&c->ref);
        return;
    }
    if (!c->in_c) return;
    if (!strcmp(ln, "f")) {
        const char *t = attr(a, "t"), *si = attr(a, "si"), *ref = attr(a, "ref");
        c->in_f = 1; c->has_f = 1;
        snprintf(c->ft, sizeof(c->ft), "%s", t ? t : "");
        if (si) { c->has_si = 1; c->si = (uint32_t)strtoul(si, NULL, 10); }
        if (ref) buf_str(&c->ref, ref);
    } else if (!strcmp(ln, "v")) { c->in_v = 1; c->has_v = 1; }
    else if (!strcmp(ln, "is")) c->in_is = 1;
    else if (!strcmp(ln, "rPh")) c->rph++;
    else if (!strcmp(ln, "t") && c->in_is && !c->rph) c->in_t = 1;
}
static void finish_cell(LoadCtx *c)
{
    Sheet *s = c->s;
    if (c->row < 0 || c->col < 0 || c->row >= (long)SHEET_MAX_ROWS || c->col >= (long)SHEET_MAX_COLS) return;
    int has_value = c->has_v || c->in_is || c->is.len || !strcmp(c->t, "inlineStr");
    if (!has_value && !c->has_f && !c->style) return;
    SheetCell *x = sheet_insert(s, (uint32_t)c->row, (uint32_t)c->col);
    if (!x) { c->too_big = 1; stop(&c->base); return; }
    uint32_t keep_text = SHEET_NONE;
    x->style = c->style;
    if (!strcmp(c->t, "s")) {
        if (c->has_v) { x->kind = SK_SST; x->text = (uint32_t)strtoul(buf_text(&c->v), NULL, 10); }
    } else if (!strcmp(c->t, "inlineStr")) {
        unescape_ooxml(&c->is);
        keep_text = arena_add(&s->arena, buf_text(&c->is), c->is.len); x->kind = SK_STR;
    } else if (!strcmp(c->t, "str") || !strcmp(c->t, "d")) {
        if (c->has_v) { unescape_ooxml(&c->v); keep_text = arena_add(&s->arena, buf_text(&c->v), c->v.len); x->kind = SK_STR; }
    } else if (!strcmp(c->t, "b")) {
        if (c->has_v) { x->kind = SK_BOOL; x->num = strcmp(buf_text(&c->v), "0") && strcmp(buf_text(&c->v), "false") ? 1 : 0; }
    } else if (!strcmp(c->t, "e")) {
        if (c->has_v) { keep_text = arena_add(&s->arena, buf_text(&c->v), c->v.len); x->kind = SK_ERR; }
    } else if (c->has_v && c->v.len) {
        x->kind = SK_NUM; x->num = strtod(buf_text(&c->v), NULL);
    }
    if ((x->kind == SK_STR || x->kind == SK_ERR) && keep_text == SHEET_NONE) { c->too_big = 1; stop(&c->base); return; }
    if (x->kind == SK_STR || x->kind == SK_ERR) x->text = keep_text;
    if (c->has_f) {
        x->flags |= SF_ORIG_FORMULA;
        if (!strcmp(c->ft, "array") || !strcmp(c->ft, "dataTable")) {
            x->flags |= SF_ARRAY;
            if (push_range(&s->arrays, &s->narrays, c->ref.len ? buf_text(&c->ref) : NULL)) { stop(&c->base); return; }
            if (!c->ref.len) {
                SheetRange one = {(uint32_t)c->row, (uint32_t)c->col, (uint32_t)c->row, (uint32_t)c->col};
                SheetRange *p = realloc(s->arrays, (s->narrays + 1) * sizeof(*p));
                if (!p) { stop(&c->base); return; }
                s->arrays = p; p[s->narrays++] = one;
            }
        }
        if (!strcmp(c->ft, "shared") && c->has_si) { x->flags |= SF_SHARED; x->aux = c->si; }
        if (c->f.len && strcmp(c->ft, "dataTable")) {
            x->formula = arena_add(&s->arena, buf_text(&c->f), c->f.len);
            if (x->formula == SHEET_NONE) { c->too_big = 1; stop(&c->base); return; }
            x->flags |= SF_FORMULA;
            if ((x->flags & SF_SHARED) && c->ref.len) {
                SheetShared *p = realloc(s->shared, (s->nshared + 1) * sizeof(*p));
                if (!p) { stop(&c->base); return; }
                s->shared = p;
                p[s->nshared++] = (SheetShared){c->si, (uint32_t)c->row, (uint32_t)c->col, x->formula};
            }
        }
    }
}
static void load_end(void *u, const char *name)
{
    LoadCtx *c = u;
    const char *ln = local(name);
    if (c->in_c) {
        if (!strcmp(ln, "c") && c->depth == c->data_depth + 2) { finish_cell(c); c->in_c = 0; c->in_is = 0; c->rph = 0; }
        else if (!strcmp(ln, "f")) c->in_f = 0;
        else if (!strcmp(ln, "v")) c->in_v = 0;
        else if (!strcmp(ln, "is")) c->in_is = 0;
        else if (!strcmp(ln, "rPh")) { if (c->rph) c->rph--; }
        else if (!strcmp(ln, "t")) c->in_t = 0;
    }
    if (!strcmp(ln, "sheetData") && c->depth == c->data_depth) c->in_data = 0;
    c->depth--;
}
static void load_chars(void *u, const char *s, int n)
{
    LoadCtx *c = u;
    if (c->in_v) buf_add(&c->v, s, (size_t)n);
    else if (c->in_f) buf_add(&c->f, s, (size_t)n);
    else if (c->in_t) buf_add(&c->is, s, (size_t)n);
}
static const SheetShared *shared_master(const Sheet *s, uint32_t si)
{
    for (size_t i = 0; i < s->nshared; ++i) if (s->shared[i].si == si) return &s->shared[i];
    return NULL;
}
int xlsx_load_sheet(Book *b, int index, char *err, size_t errlen)
{
    Sheet *s = &b->sheets[index];
    if (err && errlen) err[0] = 0;
    if (s->loaded) return 0;
    if (!s->part) { fail(err, errlen, T("このシートは表示できません（グラフなど）")); return -1; }
    LoadCtx c;
    memset(&c, 0, sizeof(c));
    c.b = b; c.s = s;
    int rc = parse_part(b, s->part, &c.base, load_start, load_end, load_chars, err, errlen);
    int too_big = c.too_big || c.v.failed || c.f.failed || c.is.failed;
    free(c.v.data); free(c.f.data); free(c.is.data); free(c.ref.data);
    if (rc || too_big) {
        sheet_free(s);
        if (too_big) {
            if (err) err[0] = 0;
            fail(err, errlen, T("大きすぎて開けません"));
        }
        return -1;
    }
    /* Shared-formula members get their own text, moved from the master. */
    for (size_t i = 0; i < s->count; ++i) {
        SheetCell *x = &s->cells[i];
        if ((x->flags & SF_SHARED) && !(x->flags & SF_FORMULA)) {
            const SheetShared *m = shared_master(s, x->aux);
            if (!m) continue;
            char *t = formula_shift(arena_str(&s->arena, m->text), (long)x->row - (long)m->row, (long)x->col - (long)m->col);
            if (!t) continue;
            x->formula = arena_add(&s->arena, t, strlen(t));
            free(t);
            if (x->formula == SHEET_NONE) { sheet_free(s); fail(err, errlen, T("大きすぎて開けません")); return -1; }
            x->flags |= SF_FORMULA;
        }
    }
    for (size_t i = 0; i < s->count; ++i) {
        SheetCell *x = &s->cells[i];
        if ((x->flags & SF_FORMULA) && !(x->flags & SF_ARRAY) && formula_check(arena_str(&s->arena, x->formula)) == FORMULA_OK)
            x->flags |= SF_CALC;
    }
    s->loaded = 1;
    return 0;
}
void xlsx_unload_sheet(Book *b, int index)
{
    Sheet *s = &b->sheets[index];
    if (s->loaded && !s->dirty) sheet_free(s);
}

/* ---- save: pass 1 ---- */
typedef struct {
    SheetCell *cell;
    int exists;
    uint64_t start, end, insert_at;
    char *attrs;            /* original attributes other than r s t cm vm */
    char *fxml;             /* the original <f> element, re-serialised */
} Dirty;
typedef struct {
    uint32_t row;
    size_t first, last;     /* Dirty[first..last) */
    int exists, empty, inserts;
    uint64_t start, tag_end, end_tag, end, insert_at;
    char *name, *attrs;     /* qualified name and attributes except spans */
    int had_spans;
} RowPlan;
typedef struct {
    XmlBase base;
    Dirty *d;
    size_t nd;
    RowPlan *rows;
    size_t nrows, tp, cp;
    long cur;               /* RowPlan index of the current row or -1 */
    long target;            /* Dirty index being captured or -1 */
    int depth, data_depth, in_data, seen_data, capture_f;
    long last_row, last_col;
    int inserted_before_row, inserted_before_cell;
    uint64_t dim_start, dim_end, data_start, data_tag_end, data_end_tag, data_end;
    int have_dim, data_empty;
    char dim_name[64], data_name[64], prefix[32];
    SheetRange dim;
    int have_dim_range;
    char *data_attrs;
    Buf f;
    char err[160];
} SaveCtx;
static char *serialize_attrs(const char **a, const char *const *skip)
{
    Buf b = {0};
    buf_add(&b, "", 0);
    for (; a && a[0]; a += 2) {
        int drop = 0;
        for (const char *const *k = skip; k && *k; ++k) if (!strcmp(a[0], *k)) drop = 1;
        if (drop) continue;
        buf_str(&b, " "); buf_str(&b, a[0]); buf_str(&b, "=\""); buf_xml(&b, a[1], 1, 0); buf_str(&b, "\"");
    }
    if (b.failed) { free(b.data); return NULL; }
    return b.data;
}
static void save_fail(SaveCtx *c, const char *message)
{
    if (!c->err[0]) snprintf(c->err, sizeof(c->err), "%s", message);
    stop(&c->base);
}
static void save_start(void *u, const char *name, const char **a)
{
    SaveCtx *c = u;
    const char *ln = local(name);
    c->depth++;
    if (c->capture_f) { save_fail(c, T("数式の中に知らない要素があります")); return; }
    if (!strcmp(ln, "dimension") && !c->in_data && !c->have_dim) {
        c->have_dim = 1; c->dim_start = here(&c->base);
        snprintf(c->dim_name, sizeof(c->dim_name), "%s", name);
        const char *ref = attr(a, "ref");
        c->have_dim_range = ref && !sheet_parse_range(ref, &c->dim);
        return;
    }
    if (!strcmp(ln, "sheetData") && !c->seen_data) {
        c->in_data = 1; c->seen_data = 1; c->data_depth = c->depth;
        c->data_start = here(&c->base); c->data_tag_end = here_end(&c->base);
        snprintf(c->data_name, sizeof(c->data_name), "%s", name);
        prefix_of(name, c->prefix, sizeof(c->prefix));
        static const char *const none[] = {NULL};
        c->data_attrs = serialize_attrs(a, none);
        if (!c->data_attrs) save_fail(c, T("メモリが足りません"));
        return;
    }
    if (!c->in_data) return;
    if (!strcmp(ln, "row") && c->depth == c->data_depth + 1) {
        const char *r = attr(a, "r");
        long row = r ? strtol(r, NULL, 10) - 1 : c->last_row + 1;
        if (row < 0 || row <= c->last_row) { save_fail(c, T("行の順が正しくないため保存できません")); return; }
        uint64_t at = here(&c->base);
        int inserted = 0;
        while (c->tp < c->nrows && c->rows[c->tp].row < (uint32_t)row) {
            c->rows[c->tp].exists = 0; c->rows[c->tp].insert_at = at; c->tp++; inserted = 1;
        }
        if (inserted && !r) { save_fail(c, T("行番号の無い行があるため保存できません")); return; }
        c->cur = -1;
        if (c->tp < c->nrows && c->rows[c->tp].row == (uint32_t)row) {
            RowPlan *p = &c->rows[c->tp];
            p->exists = 1; p->start = at; p->tag_end = here_end(&c->base);
            p->name = strdup(name);
            static const char *const skip[] = {"spans", NULL};
            p->attrs = serialize_attrs(a, skip);
            p->had_spans = attr(a, "spans") != NULL;
            if (!p->name || !p->attrs) { save_fail(c, T("メモリが足りません")); return; }
            c->cur = (long)c->tp;
            c->cp = p->first;
            c->tp++;
        }
        c->last_row = row; c->last_col = -1;
        return;
    }
    if (c->cur < 0) return;
    RowPlan *p = &c->rows[c->cur];
    if (!strcmp(ln, "c") && c->depth == c->data_depth + 2) {
        const char *r = attr(a, "r");
        uint32_t row, col;
        long column;
        if (r) {
            if (sheet_parse_address(r, &row, &col) != strlen(r) || row != p->row) { save_fail(c, T("セルの番地が正しくないため保存できません")); return; }
            column = col;
        } else column = c->last_col + 1;
        if (column <= c->last_col) { save_fail(c, T("セルの順が正しくないため保存できません")); return; }
        uint64_t at = here(&c->base);
        int inserted = 0;
        while (c->cp < p->last && c->d[c->cp].cell->col < (uint32_t)column) {
            c->d[c->cp].exists = 0; c->d[c->cp].insert_at = at; c->cp++; inserted = 1; p->inserts = 1;
        }
        if (inserted && !r) { save_fail(c, T("番地の無いセルがあるため保存できません")); return; }
        if (c->cp < p->last && c->d[c->cp].cell->col == (uint32_t)column) {
            Dirty *d = &c->d[c->cp];
            d->exists = 1; d->start = at;
            static const char *const skip[] = {"r", "s", "t", "cm", "vm", NULL};
            d->attrs = serialize_attrs(a, skip);
            if (!d->attrs) { save_fail(c, T("メモリが足りません")); return; }
            c->target = (long)c->cp;
            c->cp++;
        }
        c->last_col = column;
        return;
    }
    if (c->target >= 0 && !strcmp(ln, "f") && c->depth == c->data_depth + 3) {
        c->capture_f = 1;
        buf_clear(&c->f);
        buf_str(&c->f, "<"); buf_str(&c->f, name);
        static const char *const none[] = {NULL};
        char *at = serialize_attrs(a, none);
        if (!at) { save_fail(c, T("メモリが足りません")); return; }
        buf_str(&c->f, at);
        free(at);
        buf_str(&c->f, ">");
    }
}
static void save_end(void *u, const char *name)
{
    SaveCtx *c = u;
    const char *ln = local(name);
    if (c->capture_f && !strcmp(ln, "f")) {
        c->capture_f = 0;
        buf_str(&c->f, "</"); buf_str(&c->f, name); buf_str(&c->f, ">");
        if (c->f.failed) { save_fail(c, T("メモリが足りません")); return; }
        c->d[c->target].fxml = strdup(buf_text(&c->f));
        if (!c->d[c->target].fxml) { save_fail(c, T("メモリが足りません")); return; }
    } else if (!strcmp(ln, "dimension") && c->have_dim && !c->dim_end && !c->in_data) {
        c->dim_end = here_end(&c->base);
    } else if (c->target >= 0 && !strcmp(ln, "c") && c->depth == c->data_depth + 2) {
        c->d[c->target].end = here_end(&c->base);
        c->target = -1;
    } else if (c->cur >= 0 && !strcmp(ln, "row") && c->depth == c->data_depth + 1) {
        RowPlan *p = &c->rows[c->cur];
        if (!fdtp_xml_byte_count(c->base.parser)) { p->empty = 1; p->end = here(&c->base); }
        else { p->end_tag = here(&c->base); p->end = here_end(&c->base); }
        for (; c->cp < p->last; ++c->cp) {
            c->d[c->cp].exists = 0; c->d[c->cp].insert_at = p->end_tag; p->inserts = 1;
        }
        c->cur = -1;
    } else if (c->in_data && !strcmp(ln, "sheetData") && c->depth == c->data_depth) {
        if (!fdtp_xml_byte_count(c->base.parser)) { c->data_empty = 1; c->data_end = here(&c->base); }
        else { c->data_end_tag = here(&c->base); c->data_end = here_end(&c->base); }
        for (; c->tp < c->nrows; ++c->tp) { c->rows[c->tp].exists = 0; c->rows[c->tp].insert_at = c->data_end_tag; }
        c->in_data = 0;
    }
    c->depth--;
}
/* The spliced bytes are UTF-8, so the XML must be UTF-8 too. */
static void save_decl(void *u, const char *version, const char *encoding, int standalone)
{
    (void)version; (void)standalone;
    SaveCtx *c = u;
    if (encoding && strcasecmp(encoding, "UTF-8")) save_fail(c, T("この xlsx の文字コード（UTF-8 以外）には対応していません"));
}
static void save_chars(void *u, const char *s, int n)
{
    SaveCtx *c = u;
    if (!c->capture_f) return;
    /* Escape the formula text again as it is re-serialised. */
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) { save_fail(c, T("メモリが足りません")); return; }
    memcpy(tmp, s, (size_t)n); tmp[n] = 0;
    buf_xml(&c->f, tmp, 0, 0);
    free(tmp);
}

/* ---- save: generating cells ---- */
static void cell_xml(Buf *o, const Book *b, const Sheet *s, const Dirty *d, const char *pf)
{
    const SheetCell *c = d->cell;
    char ref[16];
    sheet_address(c->row, c->col, ref, sizeof(ref));
    int formula = (c->flags & SF_FORMULA) != 0;
    if (!formula && c->kind == SK_EMPTY) {
        if (c->style) buf_fmt(o, "<%sc r=\"%s\" s=\"%u\"/>", pf, ref, c->style);
        return;
    }
    buf_fmt(o, "<%sc r=\"%s\"", pf, ref);
    if (c->style) buf_fmt(o, " s=\"%u\"", c->style);
    if (d->attrs) buf_str(o, d->attrs);
    const char *t = NULL;
    switch (c->kind) {
    case SK_STR: case SK_SST: t = formula ? "str" : "inlineStr"; break;
    case SK_BOOL: t = "b"; break;
    case SK_ERR: t = "e"; break;
    default: break;
    }
    if (t) buf_fmt(o, " t=\"%s\"", t);
    buf_str(o, ">");
    if (formula) {
        if ((c->flags & (SF_EDITED | SF_REWRITE)) || !d->fxml) {
            buf_fmt(o, "<%sf>", pf); buf_xml(o, arena_str(&s->arena, c->formula), 0, 0); buf_fmt(o, "</%sf>", pf);
        } else buf_str(o, d->fxml);
    }
    char number[64];
    switch (c->kind) {
    case SK_NUM:
        sheet_number_text(c->num, number, sizeof(number));
        buf_fmt(o, "<%sv>%s</%sv>", pf, number, pf);
        break;
    case SK_BOOL: buf_fmt(o, "<%sv>%d</%sv>", pf, c->num != 0, pf); break;
    case SK_ERR: buf_fmt(o, "<%sv>", pf); buf_xml(o, cell_string(b, s, c), 0, 0); buf_fmt(o, "</%sv>", pf); break;
    case SK_STR: case SK_SST: {
        const char *text = cell_string(b, s, c);
        if (formula) { buf_fmt(o, "<%sv>", pf); buf_xml(o, text, 0, 1); buf_fmt(o, "</%sv>", pf); break; }
        size_t n = strlen(text);
        int preserve = n && (isspace((unsigned char)text[0]) || isspace((unsigned char)text[n - 1]) || strchr(text, '\n') || strchr(text, '\t'));
        buf_fmt(o, "<%sis><%st%s>", pf, pf, preserve ? " xml:space=\"preserve\"" : "");
        buf_xml(o, text, 0, 1);
        buf_fmt(o, "</%st></%sis>", pf, pf);
        break;
    }
    default: break;
    }
    buf_fmt(o, "</%sc>", pf);
}
static char *row_start(const RowPlan *p, const char *pf, int empty_element)
{
    Buf b = {0};
    if (p->name) { buf_str(&b, "<"); buf_str(&b, p->name); buf_str(&b, p->attrs ? p->attrs : ""); }
    else buf_fmt(&b, "<%srow r=\"%u\"", pf, p->row + 1);
    buf_str(&b, empty_element ? "/>" : ">");
    if (b.failed) { free(b.data); return NULL; }
    return b.data;
}

typedef struct { uint64_t start, end; char *text; size_t len, seq; } Splice;
typedef struct { Splice *s; size_t n, cap; int failed; } Splices;
static void splice(Splices *l, uint64_t start, uint64_t end, Buf *text)
{
    if (l->failed || text->failed) { l->failed = 1; free(text->data); return; }
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        Splice *p = realloc(l->s, cap * sizeof(*p));
        if (!p) { l->failed = 1; free(text->data); return; }
        l->s = p; l->cap = cap;
    }
    l->s[l->n] = (Splice){start, end, text->data, text->len, l->n};
    l->n++;
    memset(text, 0, sizeof(*text));
}
static int splice_order(const void *x, const void *y)
{
    const Splice *a = x, *b = y;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

typedef struct {
    ZipArchive *zip;
    size_t index;
    Splices *list;
    size_t k;
    uint64_t pos;
    ZipWriter *w;
    int failed;
} SpliceRun;
static int emit(SpliceRun *r, const void *p, size_t n)
{
    if (n && zip_writer_write(r->w, p, n)) { r->failed = 1; return -1; }
    return 0;
}
static int splice_sink(void *ctx, const unsigned char *data, size_t n)
{
    SpliceRun *r = ctx;
    size_t i = 0;
    while (i < n) {
        uint64_t abs = r->pos + i;
        if (r->k < r->list->n && r->list->s[r->k].start <= abs) {
            Splice *s = &r->list->s[r->k];
            if (s->text) {
                if (emit(r, s->text, s->len)) return 1;
                free(s->text);
                s->text = NULL;
            }
            if (abs < s->end) {
                uint64_t skip = s->end - abs;
                if (skip > n - i) skip = n - i;
                i += (size_t)skip;
                continue;
            }
            r->k++;
            continue;
        }
        uint64_t next = r->k < r->list->n ? r->list->s[r->k].start : UINT64_MAX;
        size_t run = next - abs < (uint64_t)(n - i) ? (size_t)(next - abs) : n - i;
        if (emit(r, data + i, run)) return 1;
        i += run;
    }
    r->pos += n;
    return 0;
}
static int splice_produce(void *ctx, ZipWriter *w)
{
    SpliceRun *r = ctx;
    r->w = w; r->k = 0; r->pos = 0;
    if (zip_read_stream(r->zip, r->index, splice_sink, r, NULL, 0)) return -1;
    for (; r->k < r->list->n; ++r->k) {
        Splice *s = &r->list->s[r->k];
        if (s->text && emit(r, s->text, s->len)) return -1;
    }
    return r->failed ? -1 : 0;
}
static void splices_free(Splices *l)
{
    for (size_t i = 0; i < l->n; ++i) free(l->s[i].text);
    free(l->s);
    memset(l, 0, sizeof(*l));
}

typedef struct { uint32_t sheet_id, row, col; } Removed;
typedef struct { Removed *r; size_t n; } RemovedList;

/* Builds the splices for one edited sheet. */
static int plan_sheet(Book *b, Sheet *s, Splices *out, RemovedList *removed, char *err, size_t errlen)
{
    SaveCtx c;
    memset(&c, 0, sizeof(c));
    c.cur = -1; c.target = -1; c.last_row = -1; c.last_col = -1;
    for (size_t i = 0; i < s->count; ++i)
        if (s->cells[i].flags & (SF_EDITED | SF_RECALC | SF_REWRITE)) c.nd++;
    c.d = calloc(c.nd ? c.nd : 1, sizeof(Dirty));
    c.rows = calloc(c.nd ? c.nd : 1, sizeof(RowPlan));
    int rc = -1;
    if (!c.d || !c.rows) { fail(err, errlen, T("メモリが足りません")); goto done; }
    size_t k = 0;
    for (size_t i = 0; i < s->count; ++i) {
        SheetCell *x = &s->cells[i];
        if (!(x->flags & (SF_EDITED | SF_RECALC | SF_REWRITE))) continue;
        c.d[k].cell = x;
        if (!c.nrows || c.rows[c.nrows - 1].row != x->row) {
            c.rows[c.nrows].row = x->row; c.rows[c.nrows].first = k; c.nrows++;
        }
        c.rows[c.nrows - 1].last = k + 1;
        k++;
        if ((x->flags & SF_ORIG_FORMULA) && !(x->flags & SF_FORMULA)) {
            Removed *p = realloc(removed->r, (removed->n + 1) * sizeof(*p));
            if (!p) { fail(err, errlen, T("メモリが足りません")); goto done; }
            removed->r = p;
            p[removed->n++] = (Removed){s->sheet_id, x->row, x->col};
        }
    }
    c.base.decl = save_decl;
    if (parse_part(b, s->part, &c.base, save_start, save_end, save_chars, err, errlen)) {
        if (c.err[0]) {
            if (err) err[0] = 0;
            fail(err, errlen, "%s", c.err);
        }
        goto done;
    }
    if (!c.seen_data) { fail(err, errlen, T("シートの XML に sheetData がありません")); goto done; }
    const char *pf = c.prefix;
    /* <dimension>: the original range extended over the new cells. */
    if (c.have_dim && c.have_dim_range && c.dim_end > c.dim_start) {
        SheetRange r = c.dim;
        for (size_t i = 0; i < c.nd; ++i) {
            const SheetCell *x = c.d[i].cell;
            if (x->kind == SK_EMPTY && !(x->flags & SF_FORMULA) && !x->style) continue;
            if (x->row < r.r1) r.r1 = x->row;
            if (x->row > r.r2) r.r2 = x->row;
            if (x->col < r.c1) r.c1 = x->col;
            if (x->col > r.c2) r.c2 = x->col;
        }
        if (memcmp(&r, &c.dim, sizeof(r))) {
            char a1[16], a2[16];
            sheet_address(r.r1, r.c1, a1, sizeof(a1));
            sheet_address(r.r2, r.c2, a2, sizeof(a2));
            Buf t = {0};
            if (r.r1 == r.r2 && r.c1 == r.c2) buf_fmt(&t, "<%s ref=\"%s\"/>", c.dim_name, a1);
            else buf_fmt(&t, "<%s ref=\"%s:%s\"/>", c.dim_name, a1, a2);
            splice(out, c.dim_start, c.dim_end, &t);
        }
    }
    Buf whole = {0};       /* rows for an empty <sheetData/> */
    for (size_t i = 0; i < c.nrows; ++i) {
        RowPlan *p = &c.rows[i];
        if (p->exists && !p->empty) {
            if (p->inserts && p->had_spans) {
                Buf t = {0};
                char *tag = row_start(p, pf, 0);
                if (!tag) { fail(err, errlen, T("メモリが足りません")); goto done; }
                buf_str(&t, tag); free(tag);
                splice(out, p->start, p->tag_end, &t);
            }
            for (size_t j = p->first; j < p->last; ++j) {
                Dirty *d = &c.d[j];
                Buf t = {0};
                cell_xml(&t, b, s, d, pf);
                if (d->exists) splice(out, d->start, d->end, &t);
                else if (t.len) splice(out, d->insert_at, d->insert_at, &t);
                else free(t.data);
            }
            continue;
        }
        Buf cells = {0};
        for (size_t j = p->first; j < p->last; ++j) cell_xml(&cells, b, s, &c.d[j], pf);
        if (!cells.len && !p->exists) { free(cells.data); continue; }
        Buf t = {0};
        char *tag = row_start(p, pf, !cells.len);
        if (!tag) { free(cells.data); fail(err, errlen, T("メモリが足りません")); goto done; }
        buf_str(&t, tag); free(tag);
        if (cells.len) {
            buf_add(&t, cells.data, cells.len);
            if (p->name) { buf_str(&t, "</"); buf_str(&t, p->name); buf_str(&t, ">"); }
            else buf_fmt(&t, "</%srow>", pf);
        }
        if (cells.failed) t.failed = 1;
        free(cells.data);
        if (p->exists) splice(out, p->start, p->end, &t);
        else if (c.data_empty) {
            buf_add(&whole, t.data, t.len);
            if (t.failed) whole.failed = 1;
            free(t.data);
        }
        else splice(out, p->insert_at, p->insert_at, &t);
    }
    if (c.data_empty && whole.len) {
        Buf t = {0};
        buf_fmt(&t, "<%s%s>", c.data_name, c.data_attrs ? c.data_attrs : "");
        buf_add(&t, whole.data, whole.len);
        buf_fmt(&t, "</%s>", c.data_name);
        if (whole.failed) t.failed = 1;
        splice(out, c.data_start, c.data_end, &t);
    }
    free(whole.data);
    if (out->failed) { fail(err, errlen, T("メモリが足りません")); goto done; }
    qsort(out->s, out->n, sizeof(*out->s), splice_order);
    for (size_t i = 1; i < out->n; ++i)
        if (out->s[i].start < out->s[i - 1].end) { fail(err, errlen, T("書き換える場所が重なっています")); goto done; }
    rc = 0;
done:
    for (size_t i = 0; i < c.nd && c.d; ++i) { free(c.d[i].attrs); free(c.d[i].fxml); }
    for (size_t i = 0; i < c.nrows && c.rows; ++i) { free(c.rows[i].name); free(c.rows[i].attrs); }
    free(c.d); free(c.rows); free(c.data_attrs); free(c.f.data);
    return rc;
}

/* ---- calcChain ---- */
typedef struct {
    XmlBase base;
    const RemovedList *removed;
    Buf out;
    const char *src;
    uint64_t copied;
    long effective;
    int prev_removed, any_kept, action;   /* action: 0 copied, 1 removed, 2 rewritten */
} ChainCtx;
static void chain_start(void *u, const char *name, const char **a)
{
    ChainCtx *c = u;
    if (strcmp(local(name), "c")) return;
    const char *r = attr(a, "r"), *i = attr(a, "i");
    /* i (the sheet id) is inherited from the previous <c> when absent. */
    if (i) c->effective = strtol(i, NULL, 10);
    uint32_t row = 0, col = 0;
    int remove = 0;
    if (r && sheet_parse_address(r, &row, &col)) {
        for (size_t k = 0; k < c->removed->n; ++k)
            if (c->removed->r[k].sheet_id == (uint32_t)c->effective && c->removed->r[k].row == row && c->removed->r[k].col == col) remove = 1;
    }
    uint64_t start = here(&c->base), end = here_end(&c->base);
    buf_add(&c->out, c->src + c->copied, (size_t)(start - c->copied));
    if (remove) { c->prev_removed = 1; c->action = 1; }
    else if (c->prev_removed && !i) {
        /* Its previous element is gone: state the inherited id. */
        buf_str(&c->out, "<"); buf_str(&c->out, name);
        static const char *const none[] = {NULL};
        char *at = serialize_attrs(a, none);
        if (!at) { stop(&c->base); return; }
        buf_str(&c->out, at); free(at);
        buf_fmt(&c->out, " i=\"%ld\"/>", c->effective);
        c->prev_removed = 0; c->any_kept = 1; c->action = 2;
    } else {
        buf_add(&c->out, c->src + start, (size_t)(end - start));
        c->prev_removed = 0; c->any_kept = 1; c->action = 0;
    }
    c->copied = end;
}
static void chain_end(void *u, const char *name)
{
    ChainCtx *c = u;
    if (strcmp(local(name), "c")) return;
    /* The end tag of a removed or rewritten <c></c> is dropped too. */
    if (fdtp_xml_byte_count(c->base.parser) && c->action) c->copied = here_end(&c->base);
}
/* Removes elements whose local name is `element` and whose `key` attribute
 * ends with `value` (relationships and content types). */
typedef struct { XmlBase base; const char *element, *key, *value; Buf out; const char *src; uint64_t copied; int skipping, depth, at; } DropCtx;
static void drop_start(void *u, const char *name, const char **a)
{
    DropCtx *c = u;
    c->depth++;
    if (c->skipping) return;
    const char *v = attr(a, c->key);
    if (!strcmp(local(name), c->element) && v && ends_with(v, c->value)) {
        uint64_t start = here(&c->base);
        buf_add(&c->out, c->src + c->copied, (size_t)(start - c->copied));
        c->copied = start;
        c->skipping = 1; c->at = c->depth;
    }
}
static void drop_end(void *u, const char *name)
{
    (void)name;
    DropCtx *c = u;
    if (c->skipping && c->depth == c->at) { c->copied = here_end(&c->base); c->skipping = 0; }
    c->depth--;
}
static int parse_memory(const char *data, size_t n, XmlBase *ud, XML_StartElementHandler st, XML_EndElementHandler en)
{
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) return -1;
    ud->parser = p;
    XML_SetUserData(p, ud);
    XML_SetElementHandler(p, st, en);
    int ok = XML_Parse(p, data, (int)n, 1) == XML_STATUS_OK && !ud->stop;
    XML_ParserFree(p);
    return ok ? 0 : -1;
}
static int drop_elements(Book *b, const char *part, const char *element, const char *key, const char *value,
                         char **out, size_t *outlen, char *err, size_t errlen)
{
    long index = zip_find(b->zip, part);
    unsigned char *data;
    size_t n;
    if (index < 0) { *out = NULL; *outlen = 0; return 1; }
    if (zip_read(b->zip, (size_t)index, PART_LIMIT, &data, &n, err, errlen)) return -1;
    DropCtx c;
    memset(&c, 0, sizeof(c));
    c.element = element; c.key = key; c.value = value; c.src = (const char *)data;
    if (parse_memory((const char *)data, n, &c.base, drop_start, drop_end)) {
        free(data); free(c.out.data); fail(err, errlen, T("%s の XML が壊れています"), part); return -1;
    }
    buf_add(&c.out, c.src + c.copied, n - (size_t)c.copied);
    free(data);
    if (c.out.failed) { free(c.out.data); fail(err, errlen, T("メモリが足りません")); return -1; }
    *out = c.out.data; *outlen = c.out.len;
    return 0;
}

int xlsx_save(Book *b, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    int nsheets = b->nsheets, rc = -1;
    Splices *lists = calloc((size_t)nsheets, sizeof(Splices));
    SpliceRun *runs = calloc((size_t)nsheets, sizeof(SpliceRun));
    ZipChange *changes = calloc((size_t)nsheets + 3, sizeof(ZipChange));
    RemovedList removed = {NULL, 0};
    char *chain = NULL, *rels = NULL, *types = NULL;
    size_t nchanges = 0;
    if (!lists || !runs || !changes) { fail(err, errlen, T("メモリが足りません")); goto done; }
    if (zip_is_zip64(b->zip)) { fail(err, errlen, T("ZIP64 の xlsx の保存には対応していません")); goto done; }
    for (int i = 0; i < nsheets; ++i) {
        Sheet *s = &b->sheets[i];
        if (!s->loaded || !s->dirty) continue;
        if (plan_sheet(b, s, &lists[i], &removed, err, errlen)) goto done;
        runs[i] = (SpliceRun){b->zip, (size_t)zip_find(b->zip, s->part), &lists[i], 0, 0, NULL, 0};
        changes[nchanges++] = (ZipChange){s->part, 0, ZIP_DEFLATE, NULL, 0, splice_produce, &runs[i]};
    }
    if (removed.n && b->calc_chain && zip_find(b->zip, b->calc_chain) >= 0) {
        unsigned char *data;
        size_t n;
        if (zip_read(b->zip, (size_t)zip_find(b->zip, b->calc_chain), PART_LIMIT, &data, &n, err, errlen)) goto done;
        ChainCtx c;
        memset(&c, 0, sizeof(c));
        c.removed = &removed; c.src = (const char *)data;
        if (parse_memory((const char *)data, n, &c.base, chain_start, chain_end)) {
            free(data); free(c.out.data); fail(err, errlen, T("calcChain の XML が壊れています")); goto done;
        }
        buf_add(&c.out, c.src + c.copied, n - (size_t)c.copied);
        free(data);
        if (c.out.failed) { free(c.out.data); fail(err, errlen, T("メモリが足りません")); goto done; }
        if (c.any_kept) {
            chain = c.out.data;
            changes[nchanges++] = (ZipChange){b->calc_chain, 0, ZIP_DEFLATE, chain, c.out.len, NULL, NULL};
        } else {
            /* An empty calcChain is invalid: remove the part, its
             * relationship and its content type. */
            free(c.out.data);
            changes[nchanges++] = (ZipChange){b->calc_chain, 1, 0, NULL, 0, NULL, NULL};
            size_t rn = 0, tn = 0;
            int r1 = drop_elements(b, b->workbook_rels, "Relationship", "Type", "/calcChain", &rels, &rn, err, errlen);
            if (r1 < 0) goto done;
            if (!r1) changes[nchanges++] = (ZipChange){b->workbook_rels, 0, ZIP_DEFLATE, rels, rn, NULL, NULL};
            char name[512];
            snprintf(name, sizeof(name), "/%s", b->calc_chain);
            int r2 = drop_elements(b, "[Content_Types].xml", "Override", "PartName", name, &types, &tn, err, errlen);
            if (r2 < 0) goto done;
            if (!r2) changes[nchanges++] = (ZipChange){"[Content_Types].xml", 0, ZIP_DEFLATE, types, tn, NULL, NULL};
        }
    }
    if (!nchanges) { rc = 0; goto done; }
    char local_err[256] = "";
    if (zip_rewrite(b->zip, b->path, changes, nchanges, local_err, sizeof(local_err))) {
        fail(err, errlen, "%s", local_err);
        goto done;
    }
    /* The file now holds the edits: reopen it for the next save. */
    zip_close(b->zip);
    b->zip = NULL;
    for (int i = 0; i < nsheets; ++i) {
        Sheet *s = &b->sheets[i];
        if (!s->loaded || !s->dirty) continue;
        for (size_t k = 0; k < s->count; ++k) {
            SheetCell *x = &s->cells[k];
            x->flags &= (uint16_t)~(SF_EDITED | SF_RECALC | SF_REWRITE);
            if (x->flags & SF_FORMULA) x->flags |= SF_ORIG_FORMULA; else x->flags &= (uint16_t)~SF_ORIG_FORMULA;
        }
        s->dirty = 0;
    }
    char again[200] = "";
    if (zip_open(b->path, &b->zip, again, sizeof(again))) {
        fail(err, errlen, T("保存しましたが、読み直せません（%s）"), again);
        goto done;
    }
    rc = 0;
done:
    for (int i = 0; lists && i < nsheets; ++i) splices_free(&lists[i]);
    free(lists); free(runs); free(changes); free(removed.r); free(chain); free(rels); free(types);
    return rc;
}
