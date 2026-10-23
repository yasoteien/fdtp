// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 6 pptx parts with expat: presentation.xml, relationships, the
 * theme's colours, and the shape trees of slides, layouts and masters.
 * Expat is used without namespace processing, so elements and attributes
 * are matched by their local names ("p:sp" and "sp" alike). */
#include "slide_model.h"
#include "i18n.h"
#include "expat_compat.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SHAPES 4000
#define MAX_TEXT (4u << 20)        /* bytes of text kept from one part */
#define MAX_DEPTH 256
#define MAX_GROUPS 32
#define MAX_TABLE 1000             /* rows and columns of one table */
#define NAME_LEN 24

void slevel_init(SLevel *l)
{
    memset(l, 0, sizeof(*l));
    l->algn = -1;
    l->bullet_set = -1;
}
void sstyle_init(SStyle *s)
{
    for (int i = 0; i < SLIDE_LEVELS; ++i) slevel_init(&s->lvl[i]);
}
static void stext_init(SText *t)
{
    memset(t, 0, sizeof(*t));
    sstyle_init(&t->lst);
    t->anchor = -1;
    t->wrap = -1;
    for (int i = 0; i < 4; ++i) t->ins[i] = -1;
    t->fontscale = 100000;
}
void stext_free(SText *t)
{
    if (!t) return;
    for (int i = 0; i < t->npara; ++i) {
        for (int j = 0; j < t->paras[i].nruns; ++j) free(t->paras[i].runs[j].text);
        free(t->paras[i].runs);
    }
    free(t->paras);
    free(t);
}
static void stable_free(STable *t)
{
    if (!t) return;
    for (int i = 0; i < t->ncells; ++i) stext_free(t->cells[i].text);
    free(t->cells);
    free(t->colw);
    free(t->rowh);
    free(t);
}
void spart_free(SPart *p)
{
    if (!p) return;
    for (int i = 0; i < p->nshapes; ++i) {
        stext_free(p->shapes[i].text);
        stable_free(p->shapes[i].table);
    }
    free(p->shapes);
    memset(p, 0, sizeof(*p));
}

static const char *local(const char *name)
{
    const char *c = strrchr(name, ':');
    return c ? c + 1 : name;
}
/* Attribute by local name. qualified: only a prefixed one ("r:id", not "id"). */
static const char *attr_q(const char **a, const char *name, int qualified)
{
    for (; a && a[0]; a += 2) {
        const char *ln = local(a[0]);
        if (!strcmp(ln, name) && (!qualified || ln != a[0])) return a[1];
    }
    return NULL;
}
static const char *attr(const char **a, const char *name) { return attr_q(a, name, 0); }
static long long num(const char *s, long long fallback)
{
    if (!s || !*s) return fallback;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s) return fallback;
    return v;
}
/* Values in EMU or 1/1000 % that must stay inside an int. */
static int inum(const char *s, int fallback)
{
    long long v = num(s, fallback);
    if (v > INT_MAX / 2) v = INT_MAX / 2;
    if (v < -INT_MAX / 2) v = -INT_MAX / 2;
    return (int)v;
}
static int truthy(const char *s) { return s && (!strcmp(s, "1") || !strcmp(s, "true")); }
static uint32_t hex_rgb(const char *s)
{
    if (!s) return 0;
    return (uint32_t)strtoul(s, NULL, 16) & 0xffffff;
}
static int scheme_slot(const char *s)
{
    static const char *const names[SCH_COUNT] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4",
                                                  "accent5", "accent6", "hlink", "folHlink", "bg1", "tx1", "bg2", "tx2" };
    if (!s) return -1;
    for (int i = 0; i < SCH_COUNT; ++i) if (!strcmp(s, names[i])) return i;
    return -1;
}
static uint32_t preset_rgb(const char *s)
{
    static const struct { const char *name; uint32_t rgb; } t[] = {
        { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "lime", 0x00ff00 },
        { "blue", 0x0000ff }, { "yellow", 0xffff00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 },
        { "orange", 0xffa500 }, { "navy", 0x000080 }, { "cyan", 0x00ffff }, { "magenta", 0xff00ff }, { "darkGray", 0xa9a9a9 },
        { "lightGray", 0xd3d3d3 }, { "purple", 0x800080 }, { "brown", 0xa52a2a } };
    for (size_t i = 0; s && i < sizeof(t) / sizeof(*t); ++i) if (!strcmp(s, t[i].name)) return t[i].rgb;
    return 0;
}
/* UTF-8 first character of s, or 0. */
static uint32_t first_char(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p || !*p) return 0;
    if (*p < 0x80) return *p;
    int n = *p >= 0xf0 ? 3 : *p >= 0xe0 ? 2 : *p >= 0xc0 ? 1 : 0;
    uint32_t c = *p & (0x3f >> n);
    for (int i = 1; i <= n; ++i) {
        if ((p[i] & 0xc0) != 0x80) return '?';
        c = c << 6 | (p[i] & 0x3f);
    }
    return n ? c : '?';
}

/* ---- relationships ---- */
typedef struct { XML_Parser p; int stop; SRels *r; } RelsCtx;
static void rels_start(void *u, const char *name, const char **a)
{
    RelsCtx *c = u;
    if (c->stop || strcmp(local(name), "Relationship")) return;
    const char *id = attr(a, "Id"), *type = attr(a, "Type"), *target = attr(a, "Target"), *mode = attr(a, "TargetMode");
    if (!id || !type || !target || (mode && !strcmp(mode, "External")) || c->r->n >= 100000) return;
    SRel *n = realloc(c->r->rels, (size_t)(c->r->n + 1) * sizeof(*n));
    if (!n) { c->stop = 1; XML_StopParser(c->p, XML_FALSE); return; }
    c->r->rels = n;
    SRel *e = &n[c->r->n++];
    e->id = strdup(id); e->type = strdup(type); e->target = strdup(target);
    if (!e->id || !e->type || !e->target) { c->stop = 1; XML_StopParser(c->p, XML_FALSE); }
}
static void no_end(void *u, const char *name) { (void)u; (void)name; }
void srels_free(SRels *r)
{
    if (!r) return;
    for (int i = 0; i < r->n; ++i) { free(r->rels[i].id); free(r->rels[i].type); free(r->rels[i].target); }
    free(r->rels);
    r->rels = NULL;
    r->n = 0;
}
static int run_parser(XML_Parser p, const char *xml, size_t len, const int *stop)
{
    int ok = 1;
    while (ok && len) {
        int k = len > (1u << 30) ? (1 << 30) : (int)len;
        ok = XML_Parse(p, xml, k, 0) == XML_STATUS_OK;
        xml += k; len -= (size_t)k;
    }
    if (ok) ok = XML_Parse(p, "", 0, 1) == XML_STATUS_OK;
    return ok && !*stop ? 0 : -1;
}
int srels_parse(const char *xml, size_t len, SRels *out)
{
    memset(out, 0, sizeof(*out));
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) return -1;
    RelsCtx c = { p, 0, out };
    XML_SetUserData(p, &c);
    XML_SetElementHandler(p, rels_start, no_end);
    int rc = run_parser(p, xml, len, &c.stop);
    XML_ParserFree(p);
    if (rc) srels_free(out);
    return rc;
}

/* ---- theme ---- */
typedef struct { XML_Parser p; int stop, in_scheme, slot; STheme *t; } ThemeCtx;
static void theme_start(void *u, const char *name, const char **a)
{
    ThemeCtx *c = u;
    const char *ln = local(name);
    if (!strcmp(ln, "clrScheme")) { c->in_scheme++; return; }
    if (!c->in_scheme) return;
    int slot = scheme_slot(ln);
    if (slot >= 0 && slot < SCH_THEME) { c->slot = slot; return; }
    if (c->slot < 0) return;
    if (!strcmp(ln, "srgbClr")) c->t->scheme[c->slot] = hex_rgb(attr(a, "val"));
    else if (!strcmp(ln, "sysClr")) c->t->scheme[c->slot] = hex_rgb(attr(a, "lastClr"));
    else return;
    c->t->has_scheme = 1;
}
static void theme_end(void *u, const char *name)
{
    ThemeCtx *c = u;
    const char *ln = local(name);
    if (!strcmp(ln, "clrScheme") && c->in_scheme) c->in_scheme--;
    else if (scheme_slot(ln) >= 0) c->slot = -1;
}
int stheme_parse(const char *xml, size_t len, STheme *out)
{
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) return -1;
    ThemeCtx c = { p, 0, 0, -1, out };
    XML_SetUserData(p, &c);
    XML_SetElementHandler(p, theme_start, theme_end);
    int rc = run_parser(p, xml, len, &c.stop);
    XML_ParserFree(p);
    return rc;
}

/* ---- shape trees, presentation.xml ---- */
typedef struct { double sx, tx, sy, ty; } Xform;
typedef struct {
    XML_Parser p;
    int stop, keep_text;
    SPart *part;
    SPresentation *pres;
    char names[MAX_DEPTH][NAME_LEN];
    int depth, skip;
    Xform grp[MAX_GROUPS];
    int ngrp, gdepth;              /* group nesting beyond MAX_GROUPS keeps the last transform */
    long long goff[2], gext[2], gchoff[2], gchext[2];
    int in_gxfrm;
    SShape *cur; int cur_depth;
    long long off[2], ext[2]; int have_off, have_ext, in_xfrm;
    SText *text;
    SPara *para;
    SRun *run;
    int in_t;
    size_t text_bytes, run_len;
    SStyle *style;
    SLevel *lvl;
    SColor *target;                /* colour container (solidFill, gs, fillRef...) */
    int target_depth;
    SColor *color;                 /* the colour element being read (modifiers) */
    int grad_first;
    STable *table;
    SCell *cell;
    int row, col;
    /* Revision 1 item 41 (keep_text 2): the paragraphs' places. */
    int pcount;                    /* paragraphs made: the next SPara.gid */
    int para_depth;                /* depth of the a:p being read */
    int first_run;                 /* inside the paragraph's first a:r */
    int fallback;                  /* inside mc:Fallback elements */
} Ctx;

static void halt(Ctx *c)
{
    if (!c->stop) { c->stop = 1; XML_StopParser(c->p, XML_FALSE); }
}
static const char *parent(const Ctx *c, int up)
{
    int i = c->depth - 2 - up;
    return i >= 0 && i < MAX_DEPTH ? c->names[i] : "";
}
static Xform *xform(Ctx *c) { return &c->grp[c->ngrp ? c->ngrp - 1 : 0]; }
static void color_init(SColor *k, int kind)
{
    memset(k, 0, sizeof(*k));
    k->kind = (unsigned char)kind;
    k->lummod = 100000;
    k->shade = k->tint = -1;
}
static void *grow(void *p, int *cap, int need, size_t size)
{
    if (need <= *cap) return p;
    int n = *cap ? *cap * 2 : 8;
    while (n < need) n *= 2;
    void *q = realloc(p, (size_t)n * size);
    if (q) *cap = n;
    return q;
}
static SText *new_text(Ctx *c)
{
    SText *t = malloc(sizeof(*t));
    if (!t) { halt(c); return NULL; }
    stext_init(t);
    return t;
}
static SPara *new_para(Ctx *c, SText *t)
{
    SPara *p = grow(t->paras, &t->cap, t->npara + 1, sizeof(*p));
    if (!p) { halt(c); return NULL; }
    t->paras = p;
    SPara *e = &p[t->npara++];
    memset(e, 0, sizeof(*e));
    slevel_init(&e->props);
    e->gid = c->pcount++;
    return e;
}
static SRun *new_run(Ctx *c, SPara *p, const char *text)
{
    SRun *r = grow(p->runs, &p->cap, p->nruns + 1, sizeof(*r));
    if (!r) { halt(c); return NULL; }
    p->runs = r;
    SRun *e = &r[p->nruns++];
    memset(e, 0, sizeof(*e));
    color_init(&e->color, SC_UNSET);
    e->text = strdup(text);
    if (!e->text) { p->nruns--; halt(c); return NULL; }
    return e;
}
static SShape *new_shape(Ctx *c, int kind)
{
    SPart *pt = c->part;
    if (!pt) return NULL;
    if (pt->nshapes >= MAX_SHAPES) { pt->truncated = 1; return NULL; }
    SShape *s = grow(pt->shapes, &pt->cap, pt->nshapes + 1, sizeof(*s));
    if (!s) { halt(c); return NULL; }
    pt->shapes = s;
    SShape *e = &s[pt->nshapes++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->ph_idx = -1;
    e->line_w = -1;
    color_init(&e->fill, SC_UNSET);
    color_init(&e->line, SC_UNSET);
    color_init(&e->style_fill, SC_UNSET);
    color_init(&e->style_line, SC_UNSET);
    color_init(&e->style_font, SC_UNSET);
    if (kind == SH_FRAME) e->label = N_("[図]");
    return e;
}
/* Where a fill container's colour goes, from its parent element. */
static SColor *fill_target(Ctx *c, const char *up)
{
    if (!strcmp(up, "spPr") && c->cur) return &c->cur->fill;
    if (!strcmp(up, "ln") && !strcmp(parent(c, 1), "spPr") && c->cur) return &c->cur->line;
    if (!strcmp(up, "rPr") && c->run) return &c->run->color;
    if (!strcmp(up, "defRPr") && c->lvl) return &c->lvl->color;
    if (!strcmp(up, "bgPr") && c->part) return &c->part->bg;
    if (!strcmp(up, "tcPr") && c->cell) return &c->cell->fill;
    return NULL;
}
static void level_attrs(SLevel *l, const char **a)
{
    const char *v;
    if ((v = attr(a, "marL"))) { l->marl = inum(v, 0); l->has_marl = 1; }
    if ((v = attr(a, "indent"))) { l->indent = inum(v, 0); l->has_indent = 1; }
    if ((v = attr(a, "algn"))) l->algn = (signed char)(!strcmp(v, "ctr") ? 1 : !strcmp(v, "r") ? 2 : 0);
}
static void text_chars(void *u, const char *s, int n)
{
    Ctx *c = u;
    if (!c->in_t || !c->run || c->skip || n <= 0) return;
    if (c->text_bytes + (size_t)n > MAX_TEXT) return;
    size_t old = c->run_len;
    char *p = realloc(c->run->text, old + (size_t)n + 1);
    if (!p) { halt(c); return; }
    memcpy(p + old, s, (size_t)n);
    p[old + (size_t)n] = 0;
    c->run->text = p;
    c->run_len += (size_t)n;
    c->text_bytes += (size_t)n;
}
static void group_push(Ctx *c)
{
    c->gdepth++;
    if (c->ngrp >= MAX_GROUPS) return;
    c->grp[c->ngrp] = c->ngrp ? c->grp[c->ngrp - 1] : (Xform){ 1, 0, 1, 0 };
    c->ngrp++;
}
static void group_pop(Ctx *c)
{
    if (c->gdepth > c->ngrp) { c->gdepth--; return; }
    if (c->gdepth) c->gdepth--;
    if (c->ngrp) c->ngrp--;
}
/* A group's xfrm maps its children's chOff/chExt box onto off/ext, which
 * is in the parent group's space. */
static void group_xfrm_done(Ctx *c)
{
    if (!c->ngrp || c->gdepth > c->ngrp) return;
    Xform parent_x = c->ngrp > 1 ? c->grp[c->ngrp - 2] : (Xform){ 1, 0, 1, 0 };
    Xform *g = &c->grp[c->ngrp - 1];
    double s[2], t[2];
    for (int i = 0; i < 2; ++i) {
        s[i] = c->gchext[i] > 0 && c->gext[i] > 0 ? (double)c->gext[i] / (double)c->gchext[i] : 1;
        t[i] = (double)c->goff[i] - (double)c->gchoff[i] * s[i];
    }
    g->sx = parent_x.sx * s[0]; g->tx = parent_x.tx + parent_x.sx * t[0];
    g->sy = parent_x.sy * s[1]; g->ty = parent_x.ty + parent_x.sy * t[1];
}
/* ---- revision 1 item 41: where a paragraph's parts are (keep_text 2) ---- */
static const char R_FIELD[] = N_("フィールドを含むため、この段落は直せません");
static const char R_LINK[] = N_("リンクを含むため、この段落は直せません");
static const char R_ODD[] = N_("知らない要素を含むため、この段落は直せません");
static const char R_ALT[] = N_("互換用の内容の中にあるため、この段落は直せません");
static size_t here(Ctx *c) { return (size_t)fdtp_xml_byte_index(c->p); }
static size_t here_end(Ctx *c) { return here(c) + (size_t)fdtp_xml_byte_count(c->p); }
static void src_refuse(SParaSrc *s, const char *why) { if (!s->refuse) s->refuse = why; }
static void copy_name(SParaSrc *s, char *out, size_t n, const char *name)
{
    if (strlen(name) >= n) { src_refuse(s, R_ODD); out[0] = 0; return; }
    strcpy(out, name);
}
static void src_begin(Ctx *c, const char *name)
{
    SParaSrc *s = &c->para->src;
    s->start = here(c);
    s->tag_end = s->head = s->tail = s->end = here_end(c);
    const char *colon = strchr(name, ':');
    size_t k = colon ? (size_t)(colon - name) : 0;
    if (k >= sizeof(s->prefix)) src_refuse(s, R_ODD);
    else { memcpy(s->prefix, name, k); s->prefix[k] = 0; }
    c->para_depth = c->depth;
    c->first_run = 0;
    if (c->fallback) src_refuse(s, R_ALT);
}
/* An element inside the paragraph starts (its depth is c->depth). */
static void src_start(Ctx *c, const char *name, const char *ln, const char *up)
{
    SParaSrc *s = &c->para->src;
    int d = c->depth - c->para_depth;
    if (!strcmp(ln, "hlinkClick") || !strcmp(ln, "hlinkMouseOver")) src_refuse(s, R_LINK);
    if (d == 1) {
        if (!strcmp(ln, "pPr")) {
            if (s->stage) src_refuse(s, R_ODD);
            s->stage = 1;
            s->head = here_end(c);
        } else if (!strcmp(ln, "r") || !strcmp(ln, "br") || !strcmp(ln, "fld")) {
            if (s->stage == 3) src_refuse(s, R_ODD);
            s->stage = 2;
            if (ln[0] == 'f') src_refuse(s, R_FIELD);
            if (ln[0] == 'r' && !s->has_run) {
                s->has_run = 1;
                s->run = here(c);
                s->run_tag = here_end(c);
                copy_name(s, s->run_name, sizeof(s->run_name), name);
                c->first_run = 1;
            }
        } else if (!strcmp(ln, "endParaRPr")) {
            if (s->stage == 3) src_refuse(s, R_ODD);
            s->stage = 3;
            s->epr = here(c);
            s->epr_end = here_end(c);
            copy_name(s, s->epr_name, sizeof(s->epr_name), name);
        } else src_refuse(s, R_ODD);
        if (s->stage == 3 && strcmp(ln, "endParaRPr")) src_refuse(s, R_ODD);
    } else if (d == 2) {
        if (!strcmp(up, "r")) {
            if (!strcmp(ln, "rPr")) {
                if (c->first_run) { s->rpr = here(c); s->rpr_end = here_end(c); }
            } else if (strcmp(ln, "t")) src_refuse(s, R_ODD);
        } else if (!strcmp(up, "br") && strcmp(ln, "rPr")) src_refuse(s, R_ODD);
    }
}
/* An element inside the paragraph, or the paragraph itself (d 0), ends. */
static void src_end(Ctx *c, const char *ln, int d)
{
    SParaSrc *s = &c->para->src;
    int full = fdtp_xml_byte_count(c->p) != 0;   /* 0: the end of an empty element */
    d -= c->para_depth;
    if (d == 0) {
        s->empty = !full;
        s->end = full ? here_end(c) : s->tag_end;
        s->tail = s->stage == 3 ? s->epr : full ? here(c) : s->tag_end;
    } else if (d == 1) {
        if (!strcmp(ln, "pPr") && full) s->head = here_end(c);
        else if (!strcmp(ln, "endParaRPr") && full) s->epr_end = here_end(c);
        else if (!strcmp(ln, "r") && c->first_run) { s->run_empty = !full; c->first_run = 0; }
    } else if (d == 2 && c->first_run && !strcmp(ln, "rPr") && full) s->rpr_end = here_end(c);
}

static void tree_start(void *u, const char *name, const char **a)
{
    Ctx *c = u;
    const char *ln = local(name);
    if (c->depth < MAX_DEPTH) {
        snprintf(c->names[c->depth], NAME_LEN, "%s", ln);
    }
    c->depth++;
    if (c->stop || c->skip) return;
    const char *up = parent(c, 0);
    const char *v;
    if (!strcmp(ln, "Fallback")) c->fallback++;
    if (c->keep_text == 2 && c->para) src_start(c, name, ln, up);
    /* Markup compatibility: keep the fallback, which every reader understands. */
    if (!strcmp(ln, "Choice")) { c->skip = c->depth; return; }
    if (c->pres) {
        if (!strcmp(ln, "sldId") && (v = attr_q(a, "id", 1))) {
            SPresentation *p = c->pres;
            if (p->count >= 100000) return;
            char **n = realloc(p->rids, (size_t)(p->count + 1) * sizeof(*n));
            if (!n) { halt(c); return; }
            p->rids = n;
            if (!(n[p->count] = strdup(v))) { halt(c); return; }
            p->count++;
            return;
        }
        if (!strcmp(ln, "sldSz")) { c->pres->cx = num(attr(a, "cx"), 0); c->pres->cy = num(attr(a, "cy"), 0); return; }
        if (!strcmp(ln, "defaultTextStyle")) { c->style = &c->pres->defaults; return; }
    }
    if (!strcmp(ln, "spTree") || !strcmp(ln, "grpSp")) {
        if (!c->cur) group_push(c);
        return;
    }
    if (!strcmp(ln, "xfrm") && !strcmp(up, "grpSpPr") && !c->cur) {
        c->in_gxfrm = 1;
        memset(c->goff, 0, sizeof(c->goff)); memset(c->gext, 0, sizeof(c->gext));
        memset(c->gchoff, 0, sizeof(c->gchoff)); memset(c->gchext, 0, sizeof(c->gchext));
        return;
    }
    if (c->in_gxfrm) {
        long long *dst = !strcmp(ln, "off") ? c->goff : !strcmp(ln, "ext") ? c->gext : !strcmp(ln, "chOff") ? c->gchoff : !strcmp(ln, "chExt") ? c->gchext : NULL;
        if (dst) {
            int is_ext = dst == c->gext || dst == c->gchext;
            dst[0] = num(attr(a, is_ext ? "cx" : "x"), 0);
            dst[1] = num(attr(a, is_ext ? "cy" : "y"), 0);
        }
        return;
    }
    if (!c->cur && (!strcmp(ln, "sp") || !strcmp(ln, "pic") || !strcmp(ln, "graphicFrame") || !strcmp(ln, "cxnSp"))) {
        int kind = !strcmp(ln, "sp") ? SH_SP : !strcmp(ln, "pic") ? SH_PIC : !strcmp(ln, "cxnSp") ? SH_LINE : SH_FRAME;
        c->cur = new_shape(c, kind);
        c->cur_depth = c->depth;
        c->have_off = c->have_ext = c->in_xfrm = 0;
        if (!c->cur) c->skip = c->depth;   /* over the limit: leave it out */
        return;
    }
    SShape *s = c->cur;
    if (s) {
        if (!strcmp(ln, "ph")) {
            s->ph = 1;
            const char *t = attr(a, "type");
            snprintf(s->ph_type, sizeof(s->ph_type), "%s", t ? t : "");
            s->ph_idx = (v = attr(a, "idx")) ? inum(v, -1) : -1;
            return;
        }
        if (!strcmp(ln, "videoFile") || !strcmp(ln, "quickTimeFile")) { s->media = 1; return; }
        if (!strcmp(ln, "audioFile") || !strcmp(ln, "wavAudioFile") || !strcmp(ln, "audioCd")) { s->media = 2; return; }
        if (!strcmp(ln, "media") && !s->media) { s->media = 1; return; }
        if (!strcmp(ln, "xfrm") && (!strcmp(up, "spPr") || !strcmp(up, "graphicFrame"))) {
            c->in_xfrm = 1;
            s->flip_h = truthy(attr(a, "flipH"));
            s->flip_v = truthy(attr(a, "flipV"));
            return;
        }
        if (c->in_xfrm && !strcmp(up, "xfrm")) {
            if (!strcmp(ln, "off")) { c->off[0] = num(attr(a, "x"), 0); c->off[1] = num(attr(a, "y"), 0); c->have_off = 1; }
            else if (!strcmp(ln, "ext")) { c->ext[0] = num(attr(a, "cx"), 0); c->ext[1] = num(attr(a, "cy"), 0); c->have_ext = 1; }
            return;
        }
        if (!strcmp(ln, "blip")) {
            if ((v = attr_q(a, "embed", 1)) || (v = attr(a, "embed"))) {
                snprintf(s->rid, sizeof(s->rid), "%s", v);
            }
            return;
        }
        if (!strcmp(ln, "srcRect")) {
            static const char *const k[4] = { "l", "t", "r", "b" };
            for (int i = 0; i < 4; ++i) s->crop[i] = inum(attr(a, k[i]), 0);
            return;
        }
        if (!strcmp(ln, "noFill")) {
            if (!strcmp(up, "spPr")) color_init(&s->fill, SC_NONE);
            else if (!strcmp(up, "ln") && !strcmp(parent(c, 1), "spPr")) color_init(&s->line, SC_NONE);
            return;
        }
        if (!strcmp(ln, "ln") && !strcmp(up, "spPr")) { s->line_w = (v = attr(a, "w")) ? inum(v, -1) : -1; return; }
        if (!strcmp(up, "style") && (!strcmp(ln, "fillRef") || !strcmp(ln, "lnRef") || !strcmp(ln, "fontRef"))) {
            if (ln[0] == 'f' && ln[1] == 'i') { s->fillref = inum(attr(a, "idx"), 0); c->target = &s->style_fill; }
            else if (ln[0] == 'l') { s->lnref = inum(attr(a, "idx"), 0); c->target = &s->style_line; }
            else c->target = &s->style_font;
            c->target_depth = c->depth;
            return;
        }
        if (!strcmp(ln, "graphicData")) {
            const char *uri = attr(a, "uri");
            if (!uri) uri = "";
            if (strstr(uri, "/table")) {
                s->kind = SH_TABLE;
                if (!s->table && !(s->table = calloc(1, sizeof(STable)))) halt(c);
                c->table = s->table;
                c->row = 0;
            } else if (strstr(uri, "chart")) s->label = N_("[グラフ]");
            else if (strstr(uri, "diagram")) s->label = N_("[図表]");
            else if (strstr(uri, "ole")) s->label = N_("[オブジェクト]");
            return;
        }
        if (c->table) {
            STable *t = c->table;
            if (!strcmp(ln, "gridCol")) {
                if (t->ncols >= MAX_TABLE) return;
                long long *n = realloc(t->colw, (size_t)(t->ncols + 1) * sizeof(*n));
                if (!n) { halt(c); return; }
                t->colw = n;
                n[t->ncols++] = num(attr(a, "w"), 0);
                return;
            }
            if (!strcmp(ln, "tr")) {
                if (t->nrows >= MAX_TABLE) { c->skip = c->depth; return; }
                long long *n = realloc(t->rowh, (size_t)(t->nrows + 1) * sizeof(*n));
                if (!n) { halt(c); return; }
                t->rowh = n;
                n[t->nrows++] = num(attr(a, "h"), 0);
                c->row = t->nrows - 1;
                c->col = 0;
                return;
            }
            if (!strcmp(ln, "tc")) {
                if (t->ncells >= 100000) { c->skip = c->depth; return; }
                SCell *n = grow(t->cells, &t->cap, t->ncells + 1, sizeof(*n));
                if (!n) { halt(c); return; }
                t->cells = n;
                SCell *e = &n[t->ncells++];
                memset(e, 0, sizeof(*e));
                e->row = c->row;
                e->col = c->col++;
                e->gridspan = inum(attr(a, "gridSpan"), 1);
                e->rowspan = inum(attr(a, "rowSpan"), 1);
                e->merged = truthy(attr(a, "hMerge")) || truthy(attr(a, "vMerge"));
                color_init(&e->fill, SC_UNSET);
                c->cell = e;
                return;
            }
        }
    }
    if (!strcmp(ln, "txBody") && (s || c->cell)) {
        SText **slot = c->cell ? &c->cell->text : &s->text;
        if (!*slot) *slot = new_text(c);
        c->text = *slot;
        return;
    }
    if (c->text && !strcmp(up, "txBody")) {
        if (!strcmp(ln, "bodyPr")) {
            if ((v = attr(a, "anchor"))) c->text->anchor = (signed char)(!strcmp(v, "ctr") ? 1 : !strcmp(v, "b") ? 2 : 0);
            if ((v = attr(a, "wrap"))) c->text->wrap = (signed char)strcmp(v, "none") != 0;
            static const char *const k[4] = { "lIns", "tIns", "rIns", "bIns" };
            for (int i = 0; i < 4; ++i) if ((v = attr(a, k[i]))) c->text->ins[i] = inum(v, 0);
            return;
        }
        if (!strcmp(ln, "lstStyle")) { c->style = &c->text->lst; return; }
        if (!strcmp(ln, "p")) {
            c->para = c->keep_text ? new_para(c, c->text) : NULL;
            if (c->para && c->keep_text == 2) src_begin(c, name);
            return;
        }
    }
    if (c->text && !strcmp(ln, "normAutofit")) {
        if ((v = attr(a, "fontScale"))) c->text->fontscale = inum(v, 100000);
        return;
    }
    if (c->part && (!strcmp(ln, "titleStyle") || !strcmp(ln, "bodyStyle") || !strcmp(ln, "otherStyle")) && !strcmp(up, "txStyles")) {
        c->style = ln[0] == 't' ? &c->part->title : ln[0] == 'b' ? &c->part->body : &c->part->other;
        return;
    }
    if (c->style && !strncmp(ln, "lvl", 3) && strlen(ln) == 7 && !strcmp(ln + 4, "pPr") && ln[3] >= '1' && ln[3] <= '9') {
        c->lvl = &c->style->lvl[ln[3] - '1'];
        level_attrs(c->lvl, a);
        return;
    }
    if (c->para) {
        if (!strcmp(ln, "pPr")) {
            int l = inum(attr(a, "lvl"), 0);
            c->para->level = l < 0 ? 0 : l >= SLIDE_LEVELS ? SLIDE_LEVELS - 1 : l;
            c->lvl = &c->para->props;
            level_attrs(c->lvl, a);
            return;
        }
        if (!strcmp(ln, "r") || !strcmp(ln, "fld")) { c->run = new_run(c, c->para, ""); c->run_len = 0; return; }
        if (!strcmp(ln, "br")) { new_run(c, c->para, "\n"); c->run = NULL; return; }
        if (!strcmp(ln, "t") && c->run) { c->in_t = 1; return; }
        if (!strcmp(ln, "rPr") && c->run) {
            if ((v = attr(a, "sz"))) c->run->size = inum(v, 0);
            return;
        }
        if (!strcmp(ln, "endParaRPr")) { c->para->end_size = inum(attr(a, "sz"), 0); return; }
    }
    if (c->lvl) {
        if (!strcmp(ln, "buNone")) { c->lvl->bullet_set = 0; return; }
        if (!strcmp(ln, "buChar")) { c->lvl->bullet_set = 1; c->lvl->bullet = first_char(attr(a, "char")); return; }
        if (!strcmp(ln, "buAutoNum")) { c->lvl->bullet_set = 1; c->lvl->bullet = 1; return; }
        if (!strcmp(ln, "defRPr")) { if ((v = attr(a, "sz"))) c->lvl->size = inum(v, 0); return; }
    }
    if (c->part && !strcmp(ln, "clrMap")) {
        static const char *const k[4] = { "bg1", "tx1", "bg2", "tx2" };
        static const unsigned char dflt[4] = { SCH_LT1, SCH_DK1, SCH_LT2, SCH_DK2 };
        for (int i = 0; i < 4; ++i) {
            int slot = scheme_slot(attr(a, k[i]));
            c->part->clrmap[i] = slot >= 0 && slot < SCH_THEME ? (unsigned char)slot : dflt[i];
        }
        c->part->has_clrmap = 1;
        return;
    }
    if (c->part && !strcmp(ln, "bgRef")) { c->target = &c->part->bg; c->target_depth = c->depth; return; }
    if (!strcmp(ln, "solidFill")) {
        c->target = fill_target(c, up);
        c->target_depth = c->depth;
        c->grad_first = 0;
        return;
    }
    if (!strcmp(ln, "gradFill")) {
        c->target = fill_target(c, up);
        c->target_depth = c->depth;
        c->grad_first = 1;
        return;
    }
    if (c->target && c->depth == c->target_depth + (strcmp(up, "gs") ? 1 : 3)) {
        if (!strcmp(up, "gs")) {
            if (!c->grad_first) return;     /* a gradient is drawn in its first stop's colour */
            c->grad_first = 0;
        }
        SColor *k = c->target;
        if (!strcmp(ln, "srgbClr")) { color_init(k, SC_RGB); k->rgb = hex_rgb(attr(a, "val")); }
        else if (!strcmp(ln, "sysClr")) { color_init(k, SC_RGB); k->rgb = hex_rgb(attr(a, "lastClr")); }
        else if (!strcmp(ln, "prstClr")) { color_init(k, SC_RGB); k->rgb = preset_rgb(attr(a, "val")); }
        else if (!strcmp(ln, "schemeClr")) {
            int slot = scheme_slot(attr(a, "val"));
            if (slot < 0) return;          /* phClr: the style's own colour, not modelled */
            color_init(k, SC_SCHEME);
            k->scheme = (unsigned char)slot;
        } else return;
        c->color = k;
        return;
    }
    int in_color = !strcmp(up, "srgbClr") || !strcmp(up, "schemeClr") || !strcmp(up, "sysClr") || !strcmp(up, "prstClr");
    if (c->color && in_color) {
        int val = inum(attr(a, "val"), 0);
        if (!strcmp(ln, "lumMod")) c->color->lummod = val;
        else if (!strcmp(ln, "lumOff")) c->color->lumoff = val;
        else if (!strcmp(ln, "shade")) c->color->shade = val;
        else if (!strcmp(ln, "tint")) c->color->tint = val;
    }
}
static void tree_end(void *u, const char *name)
{
    Ctx *c = u;
    const char *ln = local(name);
    int d = c->depth;
    if (c->depth) c->depth--;
    if (c->stop) return;
    if (c->skip) {
        if (d == c->skip) {
            c->skip = 0;
            if (c->cur && d == c->cur_depth) c->cur = NULL;
        }
        return;
    }
    if (!strcmp(ln, "Fallback") && c->fallback) c->fallback--;
    if (c->keep_text == 2 && c->para && d >= c->para_depth) src_end(c, ln, d);
    if (c->target && d == c->target_depth) { c->target = NULL; c->color = NULL; }
    if (c->color && (!strcmp(ln, "srgbClr") || !strcmp(ln, "schemeClr") || !strcmp(ln, "sysClr") || !strcmp(ln, "prstClr")))
        c->color = NULL;
    if (!strcmp(ln, "t")) { c->in_t = 0; return; }
    if (!strcmp(ln, "r") || !strcmp(ln, "fld")) { c->run = NULL; return; }
    if (!strcmp(ln, "p") && c->para) { c->para = NULL; c->run = NULL; c->lvl = NULL; return; }
    if (!strcmp(ln, "pPr") || !strncmp(ln, "lvl", 3)) { c->lvl = NULL; return; }
    if (!strcmp(ln, "lstStyle") || !strcmp(ln, "titleStyle") || !strcmp(ln, "bodyStyle") || !strcmp(ln, "otherStyle") ||
        !strcmp(ln, "defaultTextStyle")) { c->style = NULL; return; }
    if (!strcmp(ln, "txBody")) { c->text = NULL; c->para = NULL; return; }
    if (!strcmp(ln, "tc")) { c->cell = NULL; return; }
    if (!strcmp(ln, "tbl")) { c->table = NULL; return; }
    if (c->in_gxfrm && !strcmp(ln, "xfrm")) { c->in_gxfrm = 0; group_xfrm_done(c); return; }
    if (c->cur && c->in_xfrm && !strcmp(ln, "xfrm")) {
        c->in_xfrm = 0;
        if (c->have_off && c->have_ext) {
            Xform *g = xform(c);
            SShape *s = c->cur;
            s->x = g->tx + g->sx * (double)c->off[0];
            s->y = g->ty + g->sy * (double)c->off[1];
            s->w = g->sx * (double)c->ext[0];
            s->h = g->sy * (double)c->ext[1];
            s->has_xfrm = s->w >= 0 && s->h >= 0;
        }
        return;
    }
    if (c->cur && d == c->cur_depth) {
        c->cur = NULL;
        c->table = NULL;
        c->cell = NULL;
        c->text = NULL;
        return;
    }
    if (!c->cur && (!strcmp(ln, "grpSp") || !strcmp(ln, "spTree"))) group_pop(c);
}
static int parse_tree(const char *xml, size_t len, Ctx *c)
{
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) return -1;
    c->p = p;
    XML_SetUserData(p, c);
    XML_SetElementHandler(p, tree_start, tree_end);
    XML_SetCharacterDataHandler(p, text_chars);
    int rc = run_parser(p, xml, len, &c->stop);
    XML_ParserFree(p);
    return rc;
}
int spart_parse(const char *xml, size_t len, int keep_text, SPart *out)
{
    memset(out, 0, sizeof(*out));
    color_init(&out->bg, SC_UNSET);
    sstyle_init(&out->title);
    sstyle_init(&out->body);
    sstyle_init(&out->other);
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->keep_text = keep_text;
    c->part = out;
    int rc = parse_tree(xml, len, c);
    free(c);
    if (rc) spart_free(out);
    return rc;
}
void spresentation_free(SPresentation *p)
{
    if (!p) return;
    for (int i = 0; i < p->count; ++i) free(p->rids[i]);
    free(p->rids);
    p->rids = NULL;
    p->count = 0;
}
int spresentation_parse(const char *xml, size_t len, SPresentation *out)
{
    memset(out, 0, sizeof(*out));
    sstyle_init(&out->defaults);
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->pres = out;
    int rc = parse_tree(xml, len, c);
    free(c);
    if (rc) spresentation_free(out);
    return rc;
}
