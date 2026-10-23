// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SLIDE_MODEL_H
#define FDTP_SLIDE_MODEL_H
/* Stage 6 pptx model (docs/FDTP_VIEWERS.md): slide_xml.c reads the parts
 * with expat over zipfile.c, slide_render.c draws one slide into an RGB
 * buffer. Only the shown slide is parsed and drawn; nothing of the other
 * slides (nor any image pixels after drawing) is kept. */
#include <stddef.h>
#include <stdint.h>

/* EMU: 914400 per inch, 12700 per point. */
#define SLIDE_LEVELS 9

/* kind: 0 unset, 1 rgb, 2 scheme, 3 none (noFill). */
enum { SC_UNSET, SC_RGB, SC_SCHEME, SC_NONE };
/* Scheme slots: the theme's clrScheme order, then the mapped names. */
enum { SCH_DK1, SCH_LT1, SCH_DK2, SCH_LT2, SCH_ACC1, SCH_ACC2, SCH_ACC3, SCH_ACC4, SCH_ACC5, SCH_ACC6,
       SCH_HLINK, SCH_FOLHLINK, SCH_THEME, /* mapped through clrMap: */ SCH_BG1 = SCH_THEME, SCH_TX1, SCH_BG2, SCH_TX2, SCH_COUNT };
typedef struct {
    unsigned char kind, scheme;
    uint32_t rgb;
    int lummod, lumoff, shade, tint;   /* 100000 = 100 %; 0 lumoff = none, -1 shade/tint = none */
} SColor;

/* Paragraph properties of one outline level (lstStyle, txStyles, pPr). */
typedef struct {
    int size;                 /* hundredths of a point, 0 unset */
    int marl, indent;         /* EMU */
    signed char algn;         /* -1 unset, 0 left, 1 centre, 2 right */
    signed char bullet_set;   /* -1 unset, 0 none, 1 character */
    char has_marl, has_indent;
    uint32_t bullet;
    SColor color;
} SLevel;
typedef struct { SLevel lvl[SLIDE_LEVELS]; } SStyle;

typedef struct { char *text; int size; SColor color; } SRun;
/* Revision 1 item 41: where an a:p is in the slide's XML, read when the
 * part is parsed with keep_text 2 (byte offsets in the XML given). The
 * new paragraph is [start, head) + the new runs + [tail, end): head is
 * after the start tag and the pPr, tail is the endParaRPr or the end tag. */
typedef struct {
    size_t start, tag_end, end, head, tail;
    int empty;                  /* <a:p/> */
    size_t run, run_tag;        /* the first a:r: its start, the end of its start tag */
    int has_run, run_empty;     /* <a:r/> */
    size_t rpr, rpr_end;        /* its rPr (equal: none) */
    size_t epr, epr_end;        /* endParaRPr (equal: none) */
    char prefix[16];            /* of the a:p ("a", "" for none) */
    char run_name[32], epr_name[32];   /* qualified names */
    int stage;                  /* children so far: 0 none, 1 pPr, 2 runs, 3 endParaRPr */
    const char *refuse;         /* why the text cannot be edited, or NULL */
} SParaSrc;
typedef struct {
    int level; SLevel props; SRun *runs; int nruns, cap; int end_size;
    int gid;                    /* the paragraphs of a part numbered in document order */
    SParaSrc src;               /* keep_text 2 */
} SPara;
typedef struct {
    SPara *paras; int npara, cap;
    SStyle lst;
    signed char anchor;       /* -1 unset, 0 top, 1 centre, 2 bottom */
    signed char wrap;         /* -1 unset, 0 none, 1 square */
    int ins[4];               /* l t r b, -1 unset */
    int fontscale;            /* normAutofit, 100000 = none */
} SText;

typedef struct {
    int row, col, gridspan, rowspan, merged;
    SColor fill;
    SText *text;
} SCell;
typedef struct {
    int ncols, nrows;
    long long *colw, *rowh;
    SCell *cells; int ncells, cap;
} STable;

enum { SH_SP, SH_PIC, SH_TABLE, SH_FRAME, SH_LINE };
typedef struct {
    int kind;
    int has_xfrm;
    double x, y, w, h;        /* EMU on the slide, after the group transforms */
    int flip_h, flip_v;
    int ph;                   /* has a placeholder */
    char ph_type[16];
    int ph_idx;               /* -1 none */
    SColor fill, line;        /* spPr; SC_UNSET when not given */
    int line_w;               /* EMU, -1 unset */
    int fillref, lnref;       /* p:style indices, 0 none */
    SColor style_fill, style_line, style_font;
    SText *text;
    char rid[64];
    int crop[4];              /* l t r b, 1/1000 % */
    int media;                /* 1 video, 2 audio */
    const char *label;        /* SH_FRAME: "[グラフ]" etc. */
    STable *table;
} SShape;

/* One parsed part: a slide, a layout or a master. */
typedef struct {
    SShape *shapes; int nshapes, cap;
    SColor bg;                /* SC_UNSET when none */
    SStyle title, body, other;/* master txStyles */
    int has_clrmap;
    unsigned char clrmap[4];  /* bg1 tx1 bg2 tx2 -> SCH_DK1.. */
    int truncated;            /* too many shapes: the rest left out */
} SPart;

typedef struct SlideDeck SlideDeck;
/* Opens a pptx: presentation.xml (slide order and size, defaultTextStyle).
 * Returns NULL with a Japanese message in err. */
SlideDeck *slide_open(const char *path, char *err, size_t errlen);
void slide_close(SlideDeck *d);
int slide_count(const SlideDeck *d);
void slide_size(const SlideDeck *d, long long *cx, long long *cy);
/* Draws slide index (0-based) into rgb (w*h*3, the slide fitted to w x h).
 * Returns 0, or -1 with err set (rgb then holds a white page). warn gets
 * a non-fatal note such as an unreadable image (may stay empty). */
int slide_render(SlideDeck *d, int index, unsigned char *rgb, int w, int h, char *err, size_t errlen,
                 char *warn, size_t warnlen);

/* Revision 1 item 41: editing the text of the slides. An edited slide's
 * XML is kept in the deck (drawn from there) until slide_save() writes
 * it into the pptx. */
/* Where each paragraph of the last drawn slide went, in its pixels (gid as
 * SPara.gid; paragraphs not drawn have none). */
typedef struct { int gid, x0, y0, x1, y1; } SParaBox;
const SParaBox *slide_boxes(const SlideDeck *d, int *n);
/* A copy of slide index's XML as it is now (edited, or from the file).
 * 0, or -1 with err set. */
int slide_get_xml(SlideDeck *d, int index, char **xml, size_t *len, char *err, size_t errlen);
/* The slide's XML from now on (taken). */
void slide_set_xml(SlideDeck *d, int index, char *xml, size_t len);
/* Writes the edited slides into the pptx: only their parts change, the
 * other entries are copied byte for byte (zip_rewrite). NAME.bak first,
 * once per opening, when backup. 0, or -1 with err set (the edits are
 * kept then). */
int slide_save(SlideDeck *d, int backup, int *backed, char *err, size_t errlen);

/* slide_edit.c */
/* The paragraphs of a slide that the view lets the user choose, in
 * document order: those of the shapes' text and of the table cells (not
 * the merged-over ones). part is the slide parsed with keep_text 2 (kept
 * by the caller while list is used); *list is malloc'd. 0, or -1. */
int slide_edit_list(const char *xml, size_t len, SPart *part, SPara ***list, int *n);
/* The paragraph's text as the input line edits it: UTF-8, '\n' for a:br. */
char *slide_edit_text(const SPara *p);
/* xml with paragraph p's text replaced: one run with the first run's
 * format (its start tag and rPr; the endParaRPr's when it had none), a:br
 * for '\n'. malloc'd, NULL with err set. */
char *slide_edit_rewrite(const char *xml, size_t len, const SPara *p, const char *text, size_t *out, char *err, size_t errlen);
/* NAME.bak, through a temporary file beside it. */
int slide_backup(const char *path, char *err, size_t errlen);

/* slide_xml.c internals shared with slide_render.c */
typedef struct {
    uint32_t scheme[SCH_THEME];
    int has_scheme;
} STheme;
void spart_free(SPart *p);
/* Reads one part (slide, layout or master); keep_text 0 drops the runs. */
int spart_parse(const char *xml, size_t len, int keep_text, SPart *out);
int stheme_parse(const char *xml, size_t len, STheme *out);
/* presentation.xml: rids in order, sldSz and defaultTextStyle. */
typedef struct { char **rids; int count; long long cx, cy; SStyle defaults; } SPresentation;
int spresentation_parse(const char *xml, size_t len, SPresentation *out);
void spresentation_free(SPresentation *p);
typedef struct { char *id, *type, *target; } SRel;
typedef struct { SRel *rels; int n; } SRels;
int srels_parse(const char *xml, size_t len, SRels *out);
void srels_free(SRels *r);
void slevel_init(SLevel *l);
void sstyle_init(SStyle *s);
void stext_free(SText *t);
#endif
