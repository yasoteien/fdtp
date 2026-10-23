// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Word vertical export (novel_docx.h). */
#include "i18n.h"
#include "novel_docx.h"
#include "novel_format.h"
#include "novel_ruby.h"
#include "zipfile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PART "word/document.xml"
#define STYLES "word/styles.xml"
#define XML_LIMIT (64u * 1024 * 1024)

typedef struct { char *p; size_t n, cap; int fail; } Buf;
static void put(Buf *b, const char *s, size_t n)
{
    if (b->fail) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->fail = 1; return; }
        b->p = p; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

/* Characters XML 1.0 can hold; others (control characters, U+FFFE/FFFF)
 * become U+FFFD. A tab is w:tab, handled by the caller. */
static void text_cp(Buf *b, uint32_t c)
{
    char tmp[4];
    if (c == '&') puts_(b, "&amp;");
    else if (c == '<') puts_(b, "&lt;");
    else if (c == '>') puts_(b, "&gt;");
    else if (c == '"') puts_(b, "&quot;");
    else if ((c < 0x20 && c != '\t') || c == 0xfffe || c == 0xffff || (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff)
        put(b, tmp, novel_encode(0xfffd, tmp));
    else put(b, tmp, novel_encode(c, tmp));
}
/* One run of plain text from code points [a, b), tabs as w:tab. */
static void run(Buf *out, const uint32_t *cp, size_t a, size_t b, const char *rpr)
{
    if (a >= b) return;
    puts_(out, "<w:r>");
    if (rpr) puts_(out, rpr);
    int open = 0;
    for (size_t i = a; i < b; ++i) {
        if (cp[i] == '\t') {
            if (open) { puts_(out, "</w:t>"); open = 0; }
            puts_(out, "<w:tab/>");
            continue;
        }
        if (!open) { puts_(out, "<w:t xml:space=\"preserve\">"); open = 1; }
        text_cp(out, cp[i]);
    }
    if (open) puts_(out, "</w:t>");
    puts_(out, "</w:r>");
}
static void ruby_run(Buf *out, const uint32_t *cp, const NRuby *r, int size)
{
    char head[512];
    int small = size / 2 > 0 ? size / 2 : 1;
    snprintf(head, sizeof(head),
             "<w:r><w:ruby><w:rubyPr><w:rubyAlign w:val=\"distributeSpace\"/><w:hps w:val=\"%d\"/>"
             "<w:hpsRaise w:val=\"%d\"/><w:hpsBaseText w:val=\"%d\"/><w:lid w:val=\"ja-JP\"/></w:rubyPr><w:rt>",
             small, size > 3 ? size - 3 : size, size);
    puts_(out, head);
    char rpr[96];
    snprintf(rpr, sizeof(rpr), "<w:rPr><w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/></w:rPr>", small, small);
    run(out, cp, r->ruby, r->ruby_end, rpr);
    puts_(out, "</w:rt><w:rubyBase>");
    run(out, cp, r->base, r->base_end, NULL);
    puts_(out, "</w:rubyBase></w:ruby></w:r>");
}
static int paragraph(Buf *out, const char *s, size_t n, const char *ppr, int ruby, int size)
{
    puts_(out, "<w:p>");
    if (ppr) puts_(out, ppr);
    size_t count = 0;
    for (size_t i = 0; i < n; i = novel_next(s, n, i)) ++count;
    uint32_t *cp = malloc((count + 1) * sizeof(*cp));
    if (!cp) return -1;
    count = 0;
    for (size_t i = 0; i < n;) cp[count++] = novel_decode(s, n, i, &i);
    NRuby *spans = NULL; size_t cap = 0, spans_n = 0;
    if (ruby) {
        spans_n = novel_ruby_parse(cp, count, &spans, &cap);
        if (spans_n == (size_t)-1) { free(cp); return -1; }
    }
    size_t at = 0;
    for (size_t k = 0; k < spans_n; ++k) {
        const NRuby *r = &spans[k];
        run(out, cp, at, r->start, NULL);
        if (r->kind == NR_EMPHASIS) run(out, cp, r->base, r->base_end, "<w:rPr><w:em w:val=\"dot\"/></w:rPr>");
        else if (r->kind == NR_ESCAPE) run(out, cp, r->base, r->base_end, NULL);
        else ruby_run(out, cp, r, size);
        at = r->end;
    }
    run(out, cp, at, count, NULL);
    puts_(out, "</w:p>");
    free(spans); free(cp);
    return out->fail ? -1 : 0;
}
/* title_ppr/title_size: the title line's paragraph properties and size
 * (the built-in template's 12 pt title); NULL/0 = as the body. */
static char *body_xml(const NovelDoc *d, const char *ppr, int ruby, int size,
                      const char *title_ppr, int title_size, size_t *len)
{
    Buf out = {0};
    size_t title = 0, body = novel_header(d, &title), end = d->count;
    if (end > 1 && d->lines[end - 1].len == 0) --end;  /* the line end of the last line */
    int rc = 0;
    if (body && title < d->count)
        rc = paragraph(&out, d->lines[title].s, d->lines[title].len, title_ppr ? title_ppr : ppr, ruby,
                       title_size ? title_size : size);
    for (size_t i = body; !rc && i < end; ++i) rc = paragraph(&out, d->lines[i].s, d->lines[i].len, ppr, ruby, size);
    if (rc || out.fail) { free(out.p); return NULL; }
    if (!out.p) put(&out, "", 0);
    if (out.fail) return NULL;
    *len = out.n;
    return out.p;
}

/* ---- the template ------------------------------------------------------- */
char *novel_docx_body(const NovelDoc *d, const char *ppr, int ruby, int size, size_t *len)
{
    return body_xml(d, ppr, ruby, size, NULL, 0, len);
}
static const char *find(const char *s, const char *end, const char *what)
{
    size_t n = strlen(what);
    for (const char *p = s; p + n <= end; ++p) if (!memcmp(p, what, n)) return p;
    return NULL;
}
/* The start of an element named tag (not a longer name such as w:pPr). */
static const char *element(const char *s, const char *end, const char *tag)
{
    size_t n = strlen(tag);
    for (const char *p = find(s, end, tag); p; p = find(p + 1, end, tag)) {
        char c = p + n < end ? p[n] : 0;
        if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n') return p;
    }
    return NULL;
}
static const char *find_last(const char *s, const char *end, const char *tag)
{
    const char *last = NULL;
    for (const char *p = element(s, end, tag); p; p = element(p + 1, end, tag)) last = p;
    return last;
}
/* The body text size in half-points: the default paragraph style's w:sz,
 * else the document defaults', else 21 (10.5 pt). */
static int sz_in(const char *a, const char *b)
{
    const char *p = a ? element(a, b, "<w:sz") : NULL;
    if (!p) return 0;
    const char *v = find(p, b, "w:val=\"");
    return v ? atoi(v + 7) : 0;
}
static int body_size_xml(const char *s, size_t n)
{
    const char *end = s + n;
    int size = 0;
    for (const char *p = element(s, end, "<w:style"); p && !size; p = element(p + 1, end, "<w:style")) {
        const char *gt = find(p, end, ">"), *close = find(p, end, "</w:style>");
        if (!gt || !close) break;
        if (find(p, gt, "w:type=\"paragraph\"") && find(p, gt, "w:default=\"1\"")) size = sz_in(p, close);
    }
    if (!size) {
        const char *a = find(s, end, "<w:docDefaults"), *b = a ? find(a, end, "</w:docDefaults>") : NULL;
        if (b) size = sz_in(a, b);
    }
    return size > 0 && size < 400 ? size : 21;
}
static int body_size(ZipArchive *z)
{
    long i = zip_find(z, STYLES);
    unsigned char *data = NULL; size_t n = 0;
    if (i < 0 || zip_read(z, (size_t)i, XML_LIMIT, &data, &n, NULL, 0)) return 21;
    int size = body_size_xml((const char *)data, n);
    free(data);
    return size;
}

/* ---- the built-in template ------------------------------------------------
 * Used when the user has no template (revision 1 4.3). Written from scratch
 * (nothing taken from anyone's template): A4 landscape, vertical (sectPr
 * w:textDirection tbRl, the Japanese Word way: 縦書き turns A4 sideways),
 * 30 lines of up to 40 characters of 10.5 pt, margins 30 mm left/right and
 * 25 mm top/bottom; the body font 游明朝 (written as character references so
 * the source stays ASCII; Word substitutes a Mincho where it is missing),
 * Japanese as the East Asian language. The body is spliced in exactly as
 * for a user template. */
#define W_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
#define R_NS "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
#define MINCHO "&#x6E38;&#x660E;&#x671D;"
static const char builtin_types[] = XML_HEAD
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
    "</Types>";
static const char builtin_rels[] = XML_HEAD
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
    "</Relationships>";
static const char builtin_document_rels[] = XML_HEAD
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>"
    "</Relationships>";
static const char builtin_document[] = XML_HEAD
    "<w:document " W_NS " " R_NS "><w:body><w:p/>"
    "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
    "<w:pgMar w:top=\"1418\" w:right=\"1701\" w:bottom=\"1418\" w:left=\"1701\" w:header=\"851\" w:footer=\"851\" w:gutter=\"0\"/>"
    "<w:cols w:space=\"425\"/><w:textDirection w:val=\"tbRl\"/>"
    "<w:docGrid w:type=\"lines\" w:linePitch=\"447\"/></w:sectPr>"
    "</w:body></w:document>";
static const char builtin_styles[] = XML_HEAD
    "<w:styles " W_NS "><w:docDefaults><w:rPrDefault><w:rPr>"
    "<w:rFonts w:ascii=\"" MINCHO "\" w:eastAsia=\"" MINCHO "\" w:hAnsi=\"" MINCHO "\" w:cs=\"Times New Roman\"/>"
    "<w:kern w:val=\"2\"/><w:sz w:val=\"21\"/><w:szCs w:val=\"21\"/>"
    "<w:lang w:val=\"en-US\" w:eastAsia=\"ja-JP\" w:bidi=\"ar-SA\"/></w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr><w:widowControl w:val=\"0\"/><w:jc w:val=\"both\"/></w:pPr></w:pPrDefault></w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/>"
    "<w:rPr><w:sz w:val=\"21\"/><w:szCs w:val=\"21\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"FdtpTitle\"><w:name w:val=\"FDTP Title\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:next w:val=\"Normal\"/><w:qFormat/><w:rPr><w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/></w:rPr></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/>"
    "<w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>"
    "</w:styles>";
static const char builtin_settings[] = XML_HEAD
    "<w:settings " W_NS "><w:defaultTabStop w:val=\"840\"/><w:characterSpacingControl w:val=\"compressPunctuation\"/>"
    "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat>"
    "<w:themeFontLang w:val=\"en-US\" w:eastAsia=\"ja-JP\"/></w:settings>";

/* The new document.xml: s with the content of w:body before its last
 * w:sectPr replaced by d's paragraphs. NULL with err set on failure. */
static char *splice(const NovelDoc *d, const char *s, size_t n, int ruby, int size,
                    const char *title_ppr, int title_size, size_t *len, char *err, size_t errlen)
{
    const char *end = s + n;
    const char *body = element(s, end, "<w:body");
    const char *open_end = body ? find(body, end, ">") : NULL;
    const char *close = body ? find(body, end, "</w:body>") : NULL;
    const char *sect = open_end && close ? find_last(open_end, close, "<w:sectPr") : NULL;
    if (!open_end || !close || open_end[-1] == '/') {
        snprintf(err, errlen, "%s", T("雛形の本文（w:body）が読めません"));
        return NULL;
    }
    const char *tail = sect ? sect : close;
    /* The template's first paragraph: its w:pPr, if it has one. */
    char *ppr = NULL;
    const char *p = element(open_end + 1, tail, "<w:p");
    if (p) {
        const char *gt = find(p, tail, ">");
        if (gt && gt[-1] != '/') {
            const char *a = element(gt + 1, tail, "<w:pPr");
            const char *pend = find(gt + 1, tail, "</w:p>");
            const char *first_child = gt + 1;
            while (first_child < tail && (*first_child == ' ' || *first_child == '\n' || *first_child == '\r' || *first_child == '\t')) ++first_child;
            if (a && a == first_child && pend && a < pend) {
                const char *agt = find(a, pend, ">");
                const char *b = agt && agt[-1] == '/' ? agt + 1 : find(a, pend, "</w:pPr>");
                if (b) {
                    if (agt && agt[-1] != '/') b += strlen("</w:pPr>");
                    ppr = malloc((size_t)(b - a) + 1);
                    if (ppr) { memcpy(ppr, a, (size_t)(b - a)); ppr[b - a] = 0; }
                }
            }
        }
    }
    size_t paragraphs_n = 0;
    char *paragraphs = body_xml(d, ppr, ruby, size, title_ppr, title_size, &paragraphs_n);
    free(ppr);
    Buf xml = {0};
    if (paragraphs) {
        put(&xml, s, (size_t)(open_end + 1 - s));
        put(&xml, paragraphs, paragraphs_n);
        put(&xml, tail, (size_t)(end - tail));
    }
    free(paragraphs);
    if (!paragraphs || xml.fail) {
        free(xml.p);
        snprintf(err, errlen, "%s", T("メモリが足りません"));
        return NULL;
    }
    *len = xml.n;
    return xml.p;
}
static int write_builtin(const NovelDoc *d, const char *dest, int ruby, char *err, size_t errlen)
{
    size_t n = 0;
    char *xml = splice(d, builtin_document, sizeof(builtin_document) - 1, ruby,
                       ruby ? body_size_xml(builtin_styles, sizeof(builtin_styles) - 1) : 21,
                       "<w:pPr><w:pStyle w:val=\"FdtpTitle\"/></w:pPr>", 24, &n, err, errlen);
    if (!xml) return -1;
    ZipChange changes[] = {
        {"[Content_Types].xml", 0, ZIP_DEFLATE, builtin_types, sizeof(builtin_types) - 1, NULL, NULL},
        {"_rels/.rels", 0, ZIP_DEFLATE, builtin_rels, sizeof(builtin_rels) - 1, NULL, NULL},
        {PART, 0, ZIP_DEFLATE, xml, n, NULL, NULL},
        {"word/_rels/document.xml.rels", 0, ZIP_DEFLATE, builtin_document_rels, sizeof(builtin_document_rels) - 1, NULL, NULL},
        {STYLES, 0, ZIP_DEFLATE, builtin_styles, sizeof(builtin_styles) - 1, NULL, NULL},
        {"word/settings.xml", 0, ZIP_DEFLATE, builtin_settings, sizeof(builtin_settings) - 1, NULL, NULL},
    };
    int rc = zip_rewrite(NULL, dest, changes, sizeof(changes) / sizeof(changes[0]), err, errlen);
    free(xml);
    return rc ? -1 : 0;
}
int novel_docx_write(const NovelDoc *d, const char *template_path, const char *dest, int ruby,
                     char *err, size_t errlen)
{
    if (!template_path) return write_builtin(d, dest, ruby, err, errlen);
    ZipArchive *z = NULL;
    if (zip_open(template_path, &z, err, errlen)) return -1;
    long index = zip_find(z, PART);
    unsigned char *data = NULL; size_t n = 0;
    if (index < 0) { snprintf(err, errlen, T("雛形に %s がありません"), PART); zip_close(z); return -1; }
    if (zip_read(z, (size_t)index, XML_LIMIT, &data, &n, err, errlen)) { zip_close(z); return -1; }
    size_t xml_n = 0;
    char *xml = splice(d, (const char *)data, n, ruby, ruby ? body_size(z) : 21, NULL, 0, &xml_n, err, errlen);
    free(data);
    if (!xml) { zip_close(z); return -1; }
    ZipChange change = {PART, 0, ZIP_DEFLATE, xml, xml_n, NULL, NULL};
    int rc = zip_rewrite(z, dest, &change, 1, err, errlen);
    free(xml);
    zip_close(z);
    return rc ? -1 : 0;
}
