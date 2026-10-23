// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 6: opening a pptx and drawing one slide (docs/FDTP_VIEWERS.md).
 * The slide is drawn into an RGB buffer of the fitted size: background,
 * then each shape in tree order (fill and line as rectangles, pictures
 * through the stage 2 decoders, text in the 16-dot Shinonome font at 1,
 * 1.5, 2 or 3 times, tables as rules and text, other frames as grey boxes).
 * Layouts and masters are read for the slide being drawn only, and are
 * consulted for placeholders without a position, their text defaults and
 * the background. */
#include "slide_model.h"
#include "i18n.h"
#include "zipfile.h"
#include "img_decode.h"
#include "font24.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PART_LIMIT (32u << 20)
#define MEDIA_LIMIT (64u << 20)
#define DEFAULT_CX 9144000LL
#define DEFAULT_CY 6858000LL
#define EMU_PT 12700.0
#define GREY 0xc8c8c8u
#define GREY_EDGE 0x808080u
#define RULE 0x404040u

struct SlideDeck {
    ZipArchive *zip;
    SPresentation pres;
    char **parts;          /* slide part names in presentation order */
    int count;
    /* Revision 1 item 41: the edited slides' XML (NULL: as in the file),
     * the file for saving, and the paragraphs' places on the drawn slide. */
    char **edited;
    size_t *edited_len;
    char *path;
    time_t mtime;
    off_t size;
    int backup_done;
    SParaBox *boxes;
    int nboxes, cboxes;
};

static void say(char *err, size_t n, const char *fmt, const char *arg)
{
    if (!err || !n || err[0]) return;
    snprintf(err, n, fmt, arg ? arg : "");
}

/* ---- part names ---- */
static char *resolve(const char *source, const char *target)
{
    size_t sl = strlen(source), tl = strlen(target);
    char *b = malloc(sl + tl + 2);
    if (!b) return NULL;
    b[0] = 0;
    if (target[0] == '/') strcpy(b, target + 1);
    else {
        const char *slash = strrchr(source, '/');
        size_t dir = slash ? (size_t)(slash - source + 1) : 0;
        memcpy(b, source, dir);
        strcpy(b + dir, target);
    }
    char *out = malloc(strlen(b) + 1);
    if (!out) { free(b); return NULL; }
    size_t n = 0;
    for (char *seg = b; seg && *seg;) {
        char *next = strchr(seg, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        if (len == 1 && seg[0] == '.') {
        } else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n) {
                --n;
                while (n && out[n - 1] != '/') --n;
            }
        } else if (len) {
            memcpy(out + n, seg, len);
            n += len;
            if (next) out[n++] = '/';
        }
        seg = next ? next + 1 : NULL;
    }
    out[n] = 0;
    free(b);
    return out;
}
static char *rels_name(const char *part)
{
    const char *slash = strrchr(part, '/');
    size_t dir = slash ? (size_t)(slash - part + 1) : 0;
    char *b = malloc(strlen(part) + 12);
    if (!b) return NULL;
    memcpy(b, part, dir);
    sprintf(b + dir, "_rels/%s.rels", part + dir);
    return b;
}
static char *read_part(ZipArchive *z, const char *name, size_t limit, size_t *len)
{
    long i = name && z ? zip_find(z, name) : -1;
    unsigned char *data = NULL;
    char err[160];
    if (i < 0 || zip_read(z, (size_t)i, limit, &data, len, err, sizeof(err))) return NULL;
    return (char *)data;
}
/* Relationships of part (an absent part has none). */
static void read_rels(ZipArchive *z, const char *part, SRels *out)
{
    memset(out, 0, sizeof(*out));
    char *name = rels_name(part);
    size_t len;
    char *xml = read_part(z, name, PART_LIMIT, &len);
    if (xml) srels_parse(xml, len, out);
    free(xml);
    free(name);
}
static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}
static const SRel *rel_type(const SRels *r, const char *tail)
{
    for (int i = 0; i < r->n; ++i) if (ends_with(r->rels[i].type, tail)) return &r->rels[i];
    return NULL;
}
static const SRel *rel_id(const SRels *r, const char *id)
{
    for (int i = 0; i < r->n; ++i) if (!strcmp(r->rels[i].id, id)) return &r->rels[i];
    return NULL;
}

/* ---- the deck ---- */
SlideDeck *slide_open(const char *path, char *err, size_t errlen)
{
    char local[256];
    if (!err || !errlen) { err = local; errlen = sizeof(local); }
    err[0] = 0;
    SlideDeck *d = calloc(1, sizeof(*d));
    if (!d) { say(err, errlen, T("メモリが足りません"), NULL); return NULL; }
    struct stat st;
    if (!stat(path, &st)) { d->mtime = st.st_mtime; d->size = st.st_size; }
    if (!(d->path = strdup(path))) { say(err, errlen, T("メモリが足りません"), NULL); free(d); return NULL; }
    char zerr[192] = "";
    if (zip_open(path, &d->zip, zerr, sizeof(zerr))) {
        say(err, errlen, T("pptx を開けません（%s）"), zerr);
        free(d->path);
        free(d);
        return NULL;
    }
    SRels root;
    read_rels(d->zip, "", &root);
    const SRel *doc = rel_type(&root, "/officeDocument");
    char *pres = resolve("", doc ? doc->target : "ppt/presentation.xml");
    srels_free(&root);
    size_t len;
    char *xml = pres ? read_part(d->zip, pres, PART_LIMIT, &len) : NULL;
    if (!xml) say(err, errlen, T("pptx ではありません（%s がありません）"), pres ? pres : "presentation.xml");
    else if (spresentation_parse(xml, len, &d->pres)) say(err, errlen, T("%s が壊れています"), pres);
    free(xml);
    if (err[0]) { free(pres); slide_close(d); return NULL; }
    SRels rels;
    read_rels(d->zip, pres, &rels);
    d->parts = calloc((size_t)d->pres.count + 1, sizeof(*d->parts));
    for (int i = 0; d->parts && i < d->pres.count; ++i) {
        const SRel *r = rel_id(&rels, d->pres.rids[i]);
        if (!r || !ends_with(r->type, "/slide")) continue;
        char *name = resolve(pres, r->target);
        if (name && zip_find(d->zip, name) >= 0) d->parts[d->count++] = name;
        else free(name);
    }
    srels_free(&rels);
    free(pres);
    d->edited = calloc((size_t)d->pres.count + 1, sizeof(*d->edited));
    d->edited_len = calloc((size_t)d->pres.count + 1, sizeof(*d->edited_len));
    if (!d->parts || !d->edited || !d->edited_len) say(err, errlen, T("メモリが足りません"), NULL);
    else if (!d->count) say(err, errlen, T("スライドがありません"), NULL);
    if (err[0]) { slide_close(d); return NULL; }
    if (d->pres.cx <= 0 || d->pres.cy <= 0 || d->pres.cx > 51206400 || d->pres.cy > 51206400) {
        d->pres.cx = DEFAULT_CX;
        d->pres.cy = DEFAULT_CY;
    }
    return d;
}
void slide_close(SlideDeck *d)
{
    if (!d) return;
    for (int i = 0; d->parts && i < d->count; ++i) free(d->parts[i]);
    for (int i = 0; d->edited && i < d->count; ++i) free(d->edited[i]);
    free(d->parts);
    free(d->edited);
    free(d->edited_len);
    free(d->boxes);
    free(d->path);
    spresentation_free(&d->pres);
    zip_close(d->zip);
    free(d);
}
int slide_count(const SlideDeck *d) { return d->count; }
void slide_size(const SlideDeck *d, long long *cx, long long *cy)
{
    *cx = d->pres.cx;
    *cy = d->pres.cy;
}

/* ---- drawing context ---- */
typedef struct {
    unsigned char *rgb;
    int w, h;
    double scale;          /* pixels per EMU */
    STheme theme;
    unsigned char clrmap[4];
    const SPart *layout, *master;
    const SStyle *defaults;
    SlideDeck *deck;
    const char *part;      /* the slide's part name */
    const SRels *rels;
    char *warn;
    size_t warnlen;
} R;

static void put(R *r, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= r->w || y >= r->h) return;
    unsigned char *p = r->rgb + ((size_t)y * r->w + x) * 3;
    p[0] = (unsigned char)(c >> 16);
    p[1] = (unsigned char)(c >> 8);
    p[2] = (unsigned char)c;
}
static void blend(R *r, int x, int y, uint32_t c, int n)
{
    if (x < 0 || y < 0 || x >= r->w || y >= r->h || n <= 0) return;
    unsigned char *p = r->rgb + ((size_t)y * r->w + x) * 3;
    for (int i = 0; i < 3; ++i) {
        int f = (int)(c >> (16 - 8 * i)) & 255;
        p[i] = (unsigned char)((f * n + p[i] * (4 - n)) / 4);
    }
}
static int clampi(double v, int lo, int hi)
{
    if (!(v > lo)) return lo;     /* also NaN */
    if (v > hi) return hi;
    return (int)v;
}
static void fill_rect(R *r, double x0, double y0, double x1, double y1, uint32_t c)
{
    int ax = clampi(floor(x0 + .5), 0, r->w), bx = clampi(floor(x1 + .5), 0, r->w);
    int ay = clampi(floor(y0 + .5), 0, r->h), by = clampi(floor(y1 + .5), 0, r->h);
    for (int y = ay; y < by; ++y)
        for (int x = ax; x < bx; ++x) put(r, x, y, c);
}
/* An outline of thickness t inside the box (at least one pixel). */
static void frame_rect(R *r, double x0, double y0, double x1, double y1, int t, uint32_t c)
{
    int ax = clampi(floor(x0 + .5), -1, r->w + 1), bx = clampi(floor(x1 + .5), -1, r->w + 1);
    int ay = clampi(floor(y0 + .5), -1, r->h + 1), by = clampi(floor(y1 + .5), -1, r->h + 1);
    if (bx <= ax) bx = ax + 1;
    if (by <= ay) by = ay + 1;
    for (int k = 0; k < t; ++k) {
        for (int x = ax; x < bx; ++x) {
            put(r, x, ay + k, c);
            put(r, x, by - 1 - k, c);
        }
        for (int y = ay; y < by; ++y) {
            put(r, ax + k, y, c);
            put(r, bx - 1 - k, y, c);
        }
    }
}
static void line(R *r, double x0, double y0, double x1, double y1, int t, uint32_t c)
{
    double dx = x1 - x0, dy = y1 - y0;
    double len = fmax(fabs(dx), fabs(dy));
    if (!isfinite(len)) return;
    int steps = clampi(ceil(len), 1, 8192);
    for (int i = 0; i <= steps; ++i) {
        int x = (int)floor(x0 + dx * i / steps), y = (int)floor(y0 + dy * i / steps);
        for (int a = 0; a < t; ++a)
            for (int b = 0; b < t; ++b) put(r, x + a - t / 2, y + b - t / 2, c);
    }
}

/* ---- colours ---- */
static const uint32_t office_scheme[SCH_THEME] = { 0x000000, 0xffffff, 0x44546a, 0xe7e6e6, 0x4472c4, 0xed7d31,
                                                    0xa5a5a5, 0xffc000, 0x5b9bd5, 0x70ad47, 0x0563c1, 0x954f72 };
static double hue(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1. / 6) return p + (q - p) * 6 * t;
    if (t < .5) return q;
    if (t < 2. / 3) return p + (q - p) * (2. / 3 - t) * 6;
    return p;
}
static uint32_t lum(uint32_t c, int mod, int off)
{
    double rgb[3] = { (c >> 16 & 255) / 255., (c >> 8 & 255) / 255., (c & 255) / 255. };
    double mx = fmax(rgb[0], fmax(rgb[1], rgb[2])), mn = fmin(rgb[0], fmin(rgb[1], rgb[2]));
    double l = (mx + mn) / 2, s = 0, h = 0, d = mx - mn;
    if (d > 0) {
        s = l > .5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == rgb[0]) h = (rgb[1] - rgb[2]) / d + (rgb[1] < rgb[2] ? 6 : 0);
        else if (mx == rgb[1]) h = (rgb[2] - rgb[0]) / d + 2;
        else h = (rgb[0] - rgb[1]) / d + 4;
        h /= 6;
    }
    l = fmin(1, fmax(0, l * mod / 100000. + off / 100000.));
    double out[3];
    if (s == 0) out[0] = out[1] = out[2] = l;
    else {
        double q = l < .5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        out[0] = hue(p, q, h + 1. / 3);
        out[1] = hue(p, q, h);
        out[2] = hue(p, q, h - 1. / 3);
    }
    uint32_t v = 0;
    for (int i = 0; i < 3; ++i) v = v << 8 | (uint32_t)clampi(out[i] * 255 + .5, 0, 255);
    return v;
}
/* 1 and the colour in *out when k is a colour (not unset or none). */
static int color_of(const R *r, const SColor *k, uint32_t *out)
{
    if (!k || (k->kind != SC_RGB && k->kind != SC_SCHEME)) return 0;
    uint32_t c = k->rgb;
    if (k->kind == SC_SCHEME) {
        int slot = k->scheme;
        if (slot >= SCH_THEME) slot = r->clrmap[slot - SCH_THEME];
        c = r->theme.scheme[slot < SCH_THEME ? slot : 0];
    }
    if (k->lummod != 100000 || k->lumoff) c = lum(c, k->lummod, k->lumoff);
    if (k->shade >= 0 || k->tint >= 0) {
        uint32_t v = 0;
        for (int i = 0; i < 3; ++i) {
            double ch = (c >> (16 - 8 * i) & 255);
            if (k->shade >= 0) ch = ch * k->shade / 100000.;
            if (k->tint >= 0) ch = 255 - (255 - ch) * k->tint / 100000.;
            v = v << 8 | (uint32_t)clampi(ch + .5, 0, 255);
        }
        c = v;
    }
    *out = c;
    return 1;
}

/* ---- placeholders ---- */
enum { PH_NONE, PH_TITLE, PH_BODY, PH_OTHER };
static int ph_class(const SShape *s)
{
    if (!s->ph) return PH_NONE;
    const char *t = s->ph_type;
    if (!strcmp(t, "title") || !strcmp(t, "ctrTitle")) return PH_TITLE;
    if (!strcmp(t, "dt") || !strcmp(t, "ftr") || !strcmp(t, "sldNum") || !strcmp(t, "hdr")) return PH_OTHER;
    return PH_BODY;
}
static int same_kind(const SShape *a, const SShape *b)
{
    int ca = ph_class(a);
    if (ca != ph_class(b)) return 0;
    return ca != PH_OTHER || !strcmp(a->ph_type, b->ph_type);
}
/* The layout's or master's placeholder that s inherits from. */
static const SShape *inherit(const SPart *p, const SShape *s, int by_idx)
{
    if (!p || !s->ph) return NULL;
    if (by_idx && s->ph_idx >= 0)
        for (int i = 0; i < p->nshapes; ++i)
            if (p->shapes[i].ph && p->shapes[i].ph_idx == s->ph_idx) return &p->shapes[i];
    /* A title prefers a title over ctrTitle, and the body class its own type. */
    const SShape *loose = NULL;
    for (int i = 0; i < p->nshapes; ++i) {
        const SShape *c = &p->shapes[i];
        if (!c->ph || !same_kind(c, s)) continue;
        if (!strcmp(c->ph_type, s->ph_type)) return c;
        if (!loose) loose = c;
    }
    return loose;
}

/* Up to five sources of level defaults, nearest first. */
typedef struct {
    const SStyle *src[6];
    int n;
    const SText *bodies[3];  /* the shape's, the layout's and the master's text bodies */
    int nb;
    int own;                 /* index of the first source after the shape's own list style */
    SColor style_font;
} Chain;
static void chain_for(const R *r, const SShape *s, const SText *own, int table, Chain *c)
{
    memset(c, 0, sizeof(*c));
    c->style_font.kind = SC_UNSET;
    if (own) { c->src[c->n++] = &own->lst; c->bodies[c->nb++] = own; }
    c->own = c->n;
    if (s && !table) c->style_font = s->style_font;
    int cls = s && !table ? ph_class(s) : PH_NONE;
    if (cls != PH_NONE) {
        const SShape *l = inherit(r->layout, s, 1);
        const SShape *m = inherit(r->master, l ? l : s, 0);
        if (l && l->text) { c->src[c->n++] = &l->text->lst; c->bodies[c->nb++] = l->text; }
        if (m && m->text) { c->src[c->n++] = &m->text->lst; c->bodies[c->nb++] = m->text; }
        if (r->master) c->src[c->n++] = cls == PH_TITLE ? &r->master->title : cls == PH_BODY ? &r->master->body : &r->master->other;
        c->src[c->n++] = r->defaults;
    } else {
        c->src[c->n++] = r->defaults;
        if (r->master) c->src[c->n++] = &r->master->other;
    }
}
static SLevel resolve_level(const Chain *c, const SLevel *own, int level)
{
    SLevel out = *own;
    for (int i = 0; i < c->n; ++i) {
        const SLevel *l = &c->src[i]->lvl[level];
        if (!out.size) out.size = l->size;
        if (out.algn < 0) out.algn = l->algn;
        if (!out.has_marl && l->has_marl) { out.marl = l->marl; out.has_marl = 1; }
        if (!out.has_indent && l->has_indent) { out.indent = l->indent; out.has_indent = 1; }
        if (out.bullet_set < 0 && l->bullet_set >= 0) { out.bullet_set = l->bullet_set; out.bullet = l->bullet; }
        /* The shape's own list style, then its p:style font colour, then the rest. */
        if (out.color.kind == SC_UNSET && i == c->own && c->style_font.kind != SC_UNSET) out.color = c->style_font;
        if (out.color.kind == SC_UNSET && l->color.kind != SC_UNSET) out.color = l->color;
    }
    if (out.color.kind == SC_UNSET) out.color = c->style_font;
    if (!out.size) out.size = 1800;
    if (out.algn < 0) out.algn = 0;
    return out;
}

/* ---- text ---- */
/* Font factors in halves: 1, 1.5, 2 and 3 times the 16-dot glyph. */
static const int factors[4] = { 2, 3, 4, 6 };
static int factor_for(const R *r, double size_hundredths)
{
    double px = size_hundredths / 100. * EMU_PT * r->scale;
    int best = 2;
    double err = 1e300;
    for (int i = 0; i < 4; ++i) {
        double e = fabs(8. * factors[i] - px);
        if (e < err) { err = e; best = factors[i]; }
    }
    return best;
}
typedef struct { uint32_t code; unsigned char h; uint32_t rgb; } G;
typedef struct { int start, end, width, height, left; signed char algn; } Line;
typedef struct { G *g; int ng, gcap; Line *l; int nl, lcap; int failed; } Layout;
static int glyph_w(const G *g) { return font24_width(g->code) * 4 * g->h; }
static void add_glyph(Layout *t, uint32_t code, int h, uint32_t rgb)
{
    if (t->failed || t->ng >= 200000) return;
    if (t->ng == t->gcap) {
        int cap = t->gcap ? t->gcap * 2 : 256;
        G *n = realloc(t->g, (size_t)cap * sizeof(*n));
        if (!n) { t->failed = 1; return; }
        t->g = n;
        t->gcap = cap;
    }
    t->g[t->ng++] = (G){ code, (unsigned char)h, rgb };
}
static void add_line(Layout *t, int start, int end, int empty_h, int left, int algn)
{
    if (t->failed) return;
    if (t->nl == t->lcap) {
        int cap = t->lcap ? t->lcap * 2 : 32;
        Line *n = realloc(t->l, (size_t)cap * sizeof(*n));
        if (!n) { t->failed = 1; return; }
        t->l = n;
        t->lcap = cap;
    }
    int w = 0, h = 0;
    for (int i = start; i < end; ++i) {
        w += glyph_w(&t->g[i]);
        if (t->g[i].h > h) h = t->g[i].h;
    }
    if (!h) h = empty_h;
    t->l[t->nl++] = (Line){ start, end, w, (8 * h * 6 + 4) / 5, left, (signed char)algn };
}
static int breakable_after(uint32_t a, uint32_t b)
{
    return a == ' ' || font24_width(a) == 2 || font24_width(b) == 2;
}
/* Wraps glyphs [start, end) of one paragraph into lines. */
static void wrap(Layout *t, int start, int end, int avail_first, int avail, int wrap_on, int empty_h,
                 int left_first, int left, int algn)
{
    if (start == end) { add_line(t, start, end, empty_h, left_first, algn); return; }
    int first = 1;
    int i = start;
    while (i < end && !t->failed) {
        int line_start = i, w = 0, last_break = -1;
        int limit = first ? avail_first : avail;
        while (i < end) {
            if (t->g[i].code == '\n') break;
            int gw = glyph_w(&t->g[i]);
            if (wrap_on && w + gw > limit && i > line_start) break;
            w += gw;
            if (i + 1 < end && breakable_after(t->g[i].code, t->g[i + 1].code)) last_break = i + 1;
            ++i;
        }
        int forced = i < end && t->g[i].code == '\n';
        if (!forced && i < end && last_break > line_start && t->g[i].code != ' ' && font24_width(t->g[i].code) != 2)
            i = last_break;
        int stop = i;
        while (stop > line_start && t->g[stop - 1].code == ' ' && !forced) --stop;
        add_line(t, line_start, stop, empty_h, first ? left_first : left, algn);
        first = 0;
        if (forced) ++i;
        else
            while (i < end && t->g[i].code == ' ') ++i;
        if (forced && i == end) add_line(t, end, end, empty_h, left, algn);
    }
}
static void draw_glyph(R *r, uint32_t code, int h, int x0, int y0, uint32_t rgb)
{
    const Font24Glyph *g = font24_lookup(code);
    int gw = g->width * h / 2, gh = 8 * h;
    for (int y = 0; y < gh; ++y) {
        if (y0 + y < 0 || y0 + y >= r->h) continue;
        for (int x = 0; x < gw; ++x) {
            if (x0 + x < 0 || x0 + x >= r->w) continue;
            int n = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    int sx = (2 * x + dx) / h, sy = (2 * y + dy) / h;
                    if (sx < g->width && sy < 16 && ((g->rows[sy] >> (g->width - 1 - sx)) & 1)) ++n;
                }
            blend(r, x0 + x, y0 + y, rgb, n);
        }
    }
}
/* Revision 1 item 41: a drawn line of paragraph gid joins its place
 * (clipped to the page). */
static void add_box(R *r, int gid, int x0, int y0, int x1, int y1)
{
    SlideDeck *d = r->deck;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->w) x1 = r->w;
    if (y1 > r->h) y1 = r->h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int i = d->nboxes - 1; i >= 0; --i) {
        SParaBox *b = &d->boxes[i];
        if (b->gid != gid) continue;
        if (x0 < b->x0) b->x0 = x0;
        if (y0 < b->y0) b->y0 = y0;
        if (x1 > b->x1) b->x1 = x1;
        if (y1 > b->y1) b->y1 = y1;
        return;
    }
    if (d->nboxes == d->cboxes) {
        int cap = d->cboxes ? d->cboxes * 2 : 64;
        SParaBox *n = realloc(d->boxes, (size_t)cap * sizeof(*n));
        if (!n) return;
        d->boxes = n;
        d->cboxes = cap;
    }
    d->boxes[d->nboxes++] = (SParaBox){ gid, x0, y0, x1, y1 };
}
static int text_visible(const SPara *p)
{
    for (int i = 0; i < p->nruns; ++i)
        for (const char *s = p->runs[i].text; *s; ++s)
            if (*s != ' ' && *s != '\n') return 1;
    return 0;
}
static int inset(const Chain *c, int side, int fallback)
{
    for (int i = 0; i < c->nb; ++i) if (c->bodies[i]->ins[side] >= 0) return c->bodies[i]->ins[side];
    return fallback;
}
static int body_anchor(const Chain *c)
{
    for (int i = 0; i < c->nb; ++i) if (c->bodies[i]->anchor >= 0) return c->bodies[i]->anchor;
    return 0;
}
static int body_wrap(const Chain *c)
{
    for (int i = 0; i < c->nb; ++i) if (c->bodies[i]->wrap >= 0) return c->bodies[i]->wrap;
    return 1;
}
static void number_text(char *buf, size_t n, int v) { snprintf(buf, n, "%d.", v); }
/* Draws a text body inside the box (pixels). force_dark: a table cell
 * without its own fill (table styles are not modelled, so a light text
 * colour meant for a coloured header would vanish on the white page). */
static void draw_text(R *r, const SShape *s, const SText *t, int table, int force_dark,
                      double bx, double by, double bw, double bh)
{
    if (!t || !t->npara) return;
    Chain chain;
    chain_for(r, s, t, table, &chain);
    double li = inset(&chain, 0, 91440) * r->scale, ti = inset(&chain, 1, 45720) * r->scale;
    double ri = inset(&chain, 2, 91440) * r->scale, bi = inset(&chain, 3, 45720) * r->scale;
    int inner_w = (int)fmax(1, bw - li - ri);
    int wrap_on = body_wrap(&chain);
    double fscale = t->fontscale > 0 && t->fontscale <= 100000 ? t->fontscale / 100000. : 1;
    uint32_t fallback = 0;
    SColor tx1 = { SC_SCHEME, SCH_TX1, 0, 100000, 0, -1, -1 };
    color_of(r, &tx1, &fallback);
    Layout lay = { 0 };
    int number = 0, number_level = -1;
    /* The first line of each paragraph, for the places the editor shows. */
    int *first_line = calloc((size_t)t->npara + 1, sizeof(int));
    if (!first_line) return;
    for (int pi = 0; pi < t->npara && !lay.failed; ++pi) {
        const SPara *p = &t->paras[pi];
        first_line[pi] = lay.nl;
        SLevel lv = resolve_level(&chain, &p->props, p->level);
        uint32_t pcolor = fallback;
        if (!force_dark) color_of(r, &lv.color, &pcolor);
        int start = lay.ng;
        int visible = text_visible(p);
        int first_h = factor_for(r, (p->nruns && p->runs[0].size ? p->runs[0].size : lv.size) * fscale);
        if (lv.bullet_set == 1 && lv.bullet == 1 && visible) {
            if (number_level != p->level) { number = 0; number_level = p->level; }
            char buf[16];
            number_text(buf, sizeof(buf), ++number);
            for (const char *q = buf; *q; ++q) add_glyph(&lay, (uint32_t)*q, first_h, pcolor);
            add_glyph(&lay, ' ', first_h, pcolor);
        } else if (lv.bullet_set == 1 && lv.bullet && visible) {
            uint32_t b = lv.bullet;
            if (font24_lookup(b)->code != b) b = 0x30fb;   /* "・" when the font lacks it (U+2022 etc.) */
            add_glyph(&lay, b, first_h, pcolor);
            if (font24_width(b) == 1) add_glyph(&lay, ' ', first_h, pcolor);
        }
        if (!(lv.bullet_set == 1 && lv.bullet == 1)) number_level = -1;
        for (int ri2 = 0; ri2 < p->nruns; ++ri2) {
            const SRun *run = &p->runs[ri2];
            int h = factor_for(r, (run->size ? run->size : lv.size) * fscale);
            uint32_t c = pcolor;
            if (!force_dark) color_of(r, &run->color, &c);
            for (const char *q = run->text; *q;) {
                uint32_t code = font24_decode(&q);
                if (code == '\t') code = ' ';
                if (code == '\v' || code == '\r') code = '\n';
                if (code < 0x20 && code != '\n') continue;
                add_glyph(&lay, code, h, c);
            }
        }
        int empty_h = factor_for(r, (p->end_size ? p->end_size : lv.size) * fscale);
        int left = (int)(lv.marl * r->scale);
        int left_first = (int)((lv.marl + lv.indent) * r->scale);
        if (left_first < 0) left_first = 0;
        wrap(&lay, start, lay.ng, inner_w - left_first, inner_w - left, wrap_on, empty_h, left_first, left, lv.algn);
    }
    first_line[t->npara] = lay.nl;
    if (!lay.failed) {
        int pi = 0;
        int total = 0;
        for (int i = 0; i < lay.nl; ++i) total += lay.l[i].height;
        int anchor = body_anchor(&chain);
        double y = by + ti;
        double room = bh - ti - bi;
        if (anchor == 1) y += (room - total) / 2;
        else if (anchor == 2) y += room - total;
        for (int i = 0; i < lay.nl; ++i) {
            const Line *ln = &lay.l[i];
            int maxh = 0;
            for (int k = ln->start; k < ln->end; ++k) if (lay.g[k].h > maxh) maxh = lay.g[k].h;
            double x = bx + li + ln->left;
            double span = inner_w - ln->left;
            if (ln->algn == 1) x += (span - ln->width) / 2;
            else if (ln->algn == 2) x += span - ln->width;
            int top = (int)floor(y + .5) + (ln->height - 8 * maxh) / 2;
            int cx = (int)floor(x + .5);
            while (pi + 1 < t->npara && first_line[pi + 1] <= i) ++pi;
            int width = ln->width > 0 ? ln->width : 8;
            add_box(r, t->paras[pi].gid, cx, (int)floor(y + .5), cx + width, (int)floor(y + .5) + ln->height);
            for (int k = ln->start; k < ln->end; ++k) {
                const G *g = &lay.g[k];
                if (g->code != ' ') draw_glyph(r, g->code, g->h, cx, top + 8 * (maxh - g->h), g->rgb);
                cx += glyph_w(g);
            }
            y += ln->height;
            if (y > r->h) break;
        }
    }
    free(first_line);
    free(lay.g);
    free(lay.l);
}
/* A grey box with a label, for what cannot be drawn. */
static void placeholder_box(R *r, double x0, double y0, double x1, double y1, const char *label)
{
    fill_rect(r, x0, y0, x1, y1, GREY);
    frame_rect(r, x0, y0, x1, y1, 1, GREY_EDGE);
    int w = 0;
    for (const char *q = label; *q;) w += font24_width(font24_decode(&q)) * 8;
    int x = (int)((x0 + x1) / 2 - w / 2.), y = (int)((y0 + y1) / 2 - 8);
    for (const char *q = label; *q;) {
        uint32_t code = font24_decode(&q);
        draw_glyph(r, code, 2, x, y, 0x000000);
        x += font24_width(code) * 8;
    }
}
static void note(R *r, const char *msg)
{
    if (r->warn && r->warnlen && !r->warn[0]) snprintf(r->warn, r->warnlen, "%s", msg);
}

/* ---- pictures ---- */
static int load_media(R *r, const char *rid, int mw, int mh, Img *im)
{
    const SRel *rel = rel_id(r->rels, rid);
    if (!rel) return -1;
    char *name = resolve(r->part, rel->target);
    size_t len = 0;
    char *data = name ? read_part(r->deck->zip, name, MEDIA_LIMIT, &len) : NULL;
    free(name);
    if (!data) return -1;
    /* The stage 2 decoders read files: the entry goes to a private file in
     * $TMPDIR (or /tmp) that is removed as soon as it is decoded. */
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char path[4096];
    snprintf(path, sizeof(path), "%s/fdtp-slide-XXXXXX", dir);
    int fd = mkstemp(path);
    int rc = -1;
    if (fd >= 0) {
        size_t done = 0;
        while (done < len) {
            ssize_t k = write(fd, data + done, len - done);
            if (k <= 0) break;
            done += (size_t)k;
        }
        int ok = done == len;
        if (close(fd)) ok = 0;
        char err[192] = "";
        if (ok) rc = img_load(path, mw, mh, im, err, sizeof(err));
        unlink(path);
    }
    free(data);
    return rc;
}
static void draw_picture(R *r, const SShape *s, double x0, double y0, double x1, double y1)
{
    double crop[4];
    for (int i = 0; i < 4; ++i) crop[i] = fmin(.99, fmax(0, s->crop[i] / 100000.));
    /* Crops that leave nothing (or less than 1 %) are scaled back. */
    for (int i = 0; i < 2; ++i) {
        double sum = crop[i] + crop[i + 2];
        if (sum > .99) { crop[i] *= .99 / sum; crop[i + 2] *= .99 / sum; }
    }
    double fx = 1 - crop[0] - crop[2], fy = 1 - crop[1] - crop[3];
    double bw = x1 - x0, bh = y1 - y0;
    if (bw < 1 || bh < 1) return;
    /* The image is decoded at the size it is shown (its whole, before the crop). */
    int mw = clampi(ceil(bw / fx), 1, 2048), mh = clampi(ceil(bh / fy), 1, 2048);
    Img im = { 0 };
    if (!s->rid[0] || load_media(r, s->rid, mw, mh, &im)) {
        placeholder_box(r, x0, y0, x1, y1, T("[画像]"));
        note(r, T("表示できない画像があります"));
        return;
    }
    int ax = clampi(floor(x0 + .5), 0, r->w), bx = clampi(floor(x1 + .5), 0, r->w);
    int ay = clampi(floor(y0 + .5), 0, r->h), by = clampi(floor(y1 + .5), 0, r->h);
    for (int y = ay; y < by; ++y) {
        double v = (y + .5 - y0) / bh;
        if (s->flip_v) v = 1 - v;
        int iy = clampi((crop[1] + v * fy) * im.height, 0, im.height - 1);
        for (int x = ax; x < bx; ++x) {
            double u = (x + .5 - x0) / bw;
            if (s->flip_h) u = 1 - u;
            int ix = clampi((crop[0] + u * fx) * im.width, 0, im.width - 1);
            const unsigned char *p = im.pixels + ((size_t)iy * im.width + ix) * 3;
            put(r, x, y, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
        }
    }
    img_free(&im);
}

/* ---- tables ---- */
static void draw_table(R *r, const SShape *s, double x0, double y0, double x1, double y1)
{
    const STable *t = s->table;
    if (!t || !t->ncols || !t->nrows) { placeholder_box(r, x0, y0, x1, y1, T("[表]")); return; }
    double *cx = malloc(sizeof(double) * ((size_t)t->ncols + 1)), *cy = malloc(sizeof(double) * ((size_t)t->nrows + 1));
    if (!cx || !cy) { free(cx); free(cy); return; }
    double sumw = 0, sumh = 0;
    for (int i = 0; i < t->ncols; ++i) sumw += t->colw[i] > 0 ? (double)t->colw[i] : 0;
    for (int i = 0; i < t->nrows; ++i) sumh += t->rowh[i] > 0 ? (double)t->rowh[i] : 0;
    cx[0] = x0;
    for (int i = 0; i < t->ncols; ++i)
        cx[i + 1] = cx[i] + (sumw > 0 ? fmax(0, (double)t->colw[i]) / sumw : 1. / t->ncols) * (x1 - x0);
    cy[0] = y0;
    for (int i = 0; i < t->nrows; ++i)
        cy[i + 1] = cy[i] + (sumh > 0 ? fmax(0, (double)t->rowh[i]) / sumh : 1. / t->nrows) * (y1 - y0);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < t->ncells; ++i) {
            const SCell *c = &t->cells[i];
            if (c->merged || c->row >= t->nrows || c->col >= t->ncols) continue;
            int ce = c->col + (c->gridspan > 1 ? c->gridspan : 1), re = c->row + (c->rowspan > 1 ? c->rowspan : 1);
            if (ce > t->ncols) ce = t->ncols;
            if (re > t->nrows) re = t->nrows;
            double a = cx[c->col], b = cx[ce], top = cy[c->row], bot = cy[re];
            uint32_t fill;
            int filled = color_of(r, &c->fill, &fill);
            if (pass == 0) {
                if (filled) fill_rect(r, a, top, b, bot, fill);
                draw_text(r, s, c->text, 1, !filled, a, top, b - a, bot - top);
            } else frame_rect(r, a, top, b + 1, bot + 1, 1, RULE);
        }
    free(cx);
    free(cy);
}

/* ---- shapes ---- */
static int shape_box(const R *r, const SShape *s, double *x, double *y, double *w, double *h)
{
    const SShape *src = s;
    if (!s->has_xfrm) {
        const SShape *l = inherit(r->layout, s, 1);
        if (l && l->has_xfrm) src = l;
        else {
            const SShape *m = inherit(r->master, l ? l : s, 0);
            if (!m || !m->has_xfrm) return 0;
            src = m;
        }
    }
    *x = src->x * r->scale;
    *y = src->y * r->scale;
    *w = src->w * r->scale;
    *h = src->h * r->scale;
    return isfinite(*x) && isfinite(*y) && isfinite(*w) && isfinite(*h);
}
static void draw_shape(R *r, const SShape *s)
{
    double x, y, w, h;
    if (!shape_box(r, s, &x, &y, &w, &h)) return;
    if (fabs(x) > 1e6 || fabs(y) > 1e6 || w > 1e6 || h > 1e6) return;
    uint32_t fill = 0, ln = 0;
    int has_fill = color_of(r, &s->fill, &fill);
    if (!has_fill && s->fill.kind == SC_UNSET && s->fillref > 0) has_fill = color_of(r, &s->style_fill, &fill);
    int has_line = color_of(r, &s->line, &ln);
    if (!has_line && s->line.kind == SC_UNSET && s->lnref > 0) has_line = color_of(r, &s->style_line, &ln);
    int t = clampi(floor((s->line_w >= 0 ? s->line_w : 12700) * r->scale + .5), 1, 64);
    if (s->kind == SH_LINE) {
        if (!has_line) return;
        double ax = x, bx = x + w, ay = y, by = y + h;
        if (s->flip_h) { ax = x + w; bx = x; }
        if (s->flip_v) { ay = y + h; by = y; }
        line(r, ax, ay, bx, by, t, ln);
        return;
    }
    if (s->kind == SH_FRAME) { placeholder_box(r, x, y, x + w, y + h, s->label ? T(s->label) : T("[図]")); return; }
    if (s->kind == SH_TABLE) { draw_table(r, s, x, y, x + w, y + h); return; }
    if (s->kind == SH_PIC) {
        if (s->media) placeholder_box(r, x, y, x + w, y + h, s->media == 2 ? T("[音声]") : T("[動画]"));
        else draw_picture(r, s, x, y, x + w, y + h);
        if (has_line && s->line.kind != SC_UNSET) frame_rect(r, x, y, x + w, y + h, t, ln);
        return;
    }
    if (has_fill) fill_rect(r, x, y, x + w, y + h, fill);
    if (s->rid[0]) draw_picture(r, s, x, y, x + w, y + h);   /* a picture fill */
    if (has_line) frame_rect(r, x, y, x + w, y + h, t, ln);
    draw_text(r, s, s->text, 0, 0, x, y, w, h);
}

static int load_part(ZipArchive *z, const char *name, int keep_text, SPart *out)
{
    size_t len;
    char *xml = read_part(z, name, PART_LIMIT, &len);
    int rc = xml ? spart_parse(xml, len, keep_text, out) : -1;
    free(xml);
    return rc;
}
/* The part a relationship of type tail points at, or NULL. */
static char *related(ZipArchive *z, const char *part, const char *tail, SRels *keep)
{
    SRels rels;
    read_rels(z, part, &rels);
    const SRel *rel = rel_type(&rels, tail);
    char *name = rel ? resolve(part, rel->target) : NULL;
    if (keep) *keep = rels;
    else srels_free(&rels);
    return name;
}

int slide_render(SlideDeck *d, int index, unsigned char *rgb, int w, int h, char *err, size_t errlen,
                 char *warn, size_t warnlen)
{
    if (err && errlen) err[0] = 0;
    if (warn && warnlen) warn[0] = 0;
    memset(rgb, 255, (size_t)w * h * 3);
    d->nboxes = 0;
    if (index < 0 || index >= d->count) { say(err, errlen, T("スライドがありません"), NULL); return -1; }
    R r;
    memset(&r, 0, sizeof(r));
    r.rgb = rgb;
    r.w = w;
    r.h = h;
    r.scale = fmin((double)w / d->pres.cx, (double)h / d->pres.cy);
    r.deck = d;
    r.part = d->parts[index];
    r.defaults = &d->pres.defaults;
    r.warn = warn;
    r.warnlen = warnlen;
    memcpy(r.theme.scheme, office_scheme, sizeof(office_scheme));
    r.clrmap[0] = SCH_LT1; r.clrmap[1] = SCH_DK1; r.clrmap[2] = SCH_LT2; r.clrmap[3] = SCH_DK2;
    SPart slide, layout, master;
    memset(&layout, 0, sizeof(layout));
    memset(&master, 0, sizeof(master));
    int bad = d->edited[index] ? spart_parse(d->edited[index], d->edited_len[index], 1, &slide) : load_part(d->zip, r.part, 1, &slide);
    if (bad) {
        char num[16];
        snprintf(num, sizeof(num), "%d", index + 1);
        say(err, errlen, T("スライド %s を読めません"), num);
        return -1;
    }
    SRels rels;
    char *layout_name = related(d->zip, r.part, "/slideLayout", &rels);
    r.rels = &rels;
    char *master_name = NULL, *theme_name = NULL;
    if (layout_name && !load_part(d->zip, layout_name, 0, &layout)) {
        r.layout = &layout;
        master_name = related(d->zip, layout_name, "/slideMaster", NULL);
    }
    if (master_name && !load_part(d->zip, master_name, 0, &master)) {
        r.master = &master;
        theme_name = related(d->zip, master_name, "/theme", NULL);
        if (master.has_clrmap) memcpy(r.clrmap, master.clrmap, sizeof(r.clrmap));
    }
    if (theme_name) {
        size_t len;
        char *xml = read_part(d->zip, theme_name, PART_LIMIT, &len);
        STheme th = { { 0 }, 0 };
        if (xml && !stheme_parse(xml, len, &th) && th.has_scheme) {
            for (int i = 0; i < SCH_THEME; ++i) r.theme.scheme[i] = th.scheme[i];
        }
        free(xml);
    }
    uint32_t bg = 0xffffff;
    if (!color_of(&r, &slide.bg, &bg) && !color_of(&r, &layout.bg, &bg)) color_of(&r, &master.bg, &bg);
    fill_rect(&r, 0, 0, w, h, bg);
    for (int i = 0; i < slide.nshapes; ++i) draw_shape(&r, &slide.shapes[i]);
    if (slide.truncated) note(&r, T("図形が多すぎるため一部を省きました"));
    spart_free(&slide);
    spart_free(&layout);
    spart_free(&master);
    srels_free(&rels);
    free(layout_name);
    free(master_name);
    free(theme_name);
    return 0;
}

/* ---- revision 1 item 41: edited slides ---- */
const SParaBox *slide_boxes(const SlideDeck *d, int *n)
{
    *n = d->nboxes;
    return d->boxes;
}
int slide_get_xml(SlideDeck *d, int index, char **xml, size_t *len, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    *xml = NULL;
    *len = 0;
    if (index < 0 || index >= d->count) { say(err, errlen, T("スライドがありません"), NULL); return -1; }
    if (d->edited[index]) {
        size_t n = d->edited_len[index];
        if (!(*xml = malloc(n + 1))) { say(err, errlen, T("メモリが足りません"), NULL); return -1; }
        memcpy(*xml, d->edited[index], n);
        (*xml)[n] = 0;
        *len = n;
        return 0;
    }
    if (!(*xml = read_part(d->zip, d->parts[index], PART_LIMIT, len))) {
        char num[16];
        snprintf(num, sizeof(num), "%d", index + 1);
        say(err, errlen, T("スライド %s を読めません"), num);
        return -1;
    }
    return 0;
}
void slide_set_xml(SlideDeck *d, int index, char *xml, size_t len)
{
    if (index < 0 || index >= d->count) { free(xml); return; }
    free(d->edited[index]);
    d->edited[index] = xml;
    d->edited_len[index] = len;
}
int slide_save(SlideDeck *d, int backup, int *backed, char *err, size_t errlen)
{
    char local_err[256];
    if (!err || !errlen) { err = local_err; errlen = sizeof(local_err); }
    err[0] = 0;
    if (backed) *backed = 0;
    struct stat st;
    if (!d->zip) { say(err, errlen, T("pptx を読み直せていないため保存しません"), NULL); return -1; }
    if (stat(d->path, &st) || st.st_mtime != d->mtime || st.st_size != d->size) {
        say(err, errlen, T("ファイルが外で書き換えられたため保存しません"), NULL);
        return -1;
    }
    ZipChange *changes = calloc((size_t)d->count + 1, sizeof(*changes));
    if (!changes) { say(err, errlen, T("メモリが足りません"), NULL); return -1; }
    size_t n = 0;
    for (int i = 0; i < d->count; ++i) {
        if (!d->edited[i]) continue;
        /* Edited and undone: as the file has it, so not written again. */
        size_t len = 0;
        char *now = read_part(d->zip, d->parts[i], PART_LIMIT, &len);
        int same = now && len == d->edited_len[i] && !memcmp(now, d->edited[i], len);
        free(now);
        if (same) continue;
        long at = zip_find(d->zip, d->parts[i]);
        int method = at >= 0 && zip_entry(d->zip, (size_t)at)->method == ZIP_STORE ? ZIP_STORE : ZIP_DEFLATE;
        changes[n++] = (ZipChange){ d->parts[i], 0, method, d->edited[i], d->edited_len[i], NULL, NULL };
    }
    if (!n) { free(changes); return 0; }
    if (backup && !d->backup_done) {
        if (slide_backup(d->path, err, errlen)) { free(changes); return -1; }
        d->backup_done = 1;
        if (backed) *backed = 1;
    }
    char zerr[256] = "";
    int rc = zip_rewrite(d->zip, d->path, changes, n, zerr, sizeof(zerr));
    free(changes);
    if (rc) { say(err, errlen, "%s", zerr); return -1; }
    /* The file holds the edits now: read it again for the next save. */
    zip_close(d->zip);
    d->zip = NULL;
    for (int i = 0; i < d->count; ++i) { free(d->edited[i]); d->edited[i] = NULL; d->edited_len[i] = 0; }
    if (!stat(d->path, &st)) { d->mtime = st.st_mtime; d->size = st.st_size; }
    if (zip_open(d->path, &d->zip, zerr, sizeof(zerr))) {
        d->zip = NULL;
        say(err, errlen, T("保存しましたが、読み直せません（%s）"), zerr);
        return -1;
    }
    return 0;
}
