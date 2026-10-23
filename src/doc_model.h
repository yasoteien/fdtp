// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DOC_MODEL_H
#define FDTP_DOC_MODEL_H
/* Stage 5 (docs/FDTP_VIEWERS.md): the docx model shared by docview.c and
 * doc_parse.c / doc_layout.c / doc_save.c.
 *
 * The main document part is read whole and kept: every body-level paragraph
 * (w:p) and table (w:tbl) becomes an item with its byte range in that buffer.
 * Saving splices only the changed items (edited, added, deleted paragraphs)
 * into the original bytes; everything else, including the namespace
 * declarations on the root, is copied as it is. */
#include "zipfile.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* Display text: one code point and colour per character. Control codes:
 * '\t' tab, '\n' line break, '\f' page break. */
typedef struct {
    uint32_t *code;
    unsigned char *color;
    size_t n, cap;
    int failed;
} DocText;
void doc_text_add(DocText *t, uint32_t code, int color);
void doc_text_utf8(DocText *t, const char *s, size_t n, int color);
void doc_text_free(DocText *t);

typedef struct { uint64_t start, end; } DocRange;

typedef struct {
    DocText text;
    int span;       /* w:gridSpan, at least 1 */
    int vcont;      /* w:vMerge continuing the cell above */
} DocCell;
typedef struct { DocCell *cells; size_t n, cap; } DocRow;
typedef struct {
    DocRow *rows;
    size_t n, cap;
} DocTable;

enum { DOC_PARA, DOC_TABLE };

/* A direct child of an original paragraph that matters for rewriting. */
enum { DOC_CHILD_TEXT_RUN, DOC_CHILD_SPECIAL_RUN, DOC_CHILD_MARK, DOC_CHILD_PPR };
typedef struct {
    int kind;
    DocRange range;
    uint64_t tag_end;   /* end of the start tag */
    int empty;          /* <w:r/> */
    DocRange rpr;       /* w:rPr of a run, start == end when absent */
    int rpr_change;     /* the rPr holds w:rPrChange */
    int images;         /* [画像] shown for this child */
} DocChild;

typedef struct {
    int kind;
    int is_new, edited, deleted;
    int heading;
    int sep_before, sep_after;  /* page break before / section break after */
    int has_sect;               /* the pPr holds a w:sectPr */
    int images_before, images_after; /* image runs outside the text runs */
    DocText disp;               /* paragraph display text (DOC_PARA) */
    char *edit;                 /* text of the text runs, UTF-8 with \t \n \f */
    const char *refuse;         /* why the text cannot be edited, or NULL */
    const char *refuse_delete;  /* why it cannot be deleted, or NULL */
    char prefix[32];            /* prefix of the WordprocessingML namespace here */
    /* Original bytes (not for is_new). */
    DocRange whole;
    uint64_t tag_end, end_tag;
    int empty_tag;
    DocChild *children;
    size_t nchildren, cchildren;
    int has_ppr;
    DocRange ppr, ppr_rpr;      /* ppr_rpr.start == end when absent */
    int ppr_rpr_clean;          /* the paragraph mark's rPr can be a run's rPr */
    DocRange drops[8];          /* inside the pPr, left out when copied by o */
    int ndrops;
    /* For is_new: the copied pPr and rPr (may be NULL). */
    char *new_ppr, *new_rpr;
    DocTable *table;            /* DOC_TABLE */
} DocItem;

typedef struct {
    char *path;                 /* the .docx */
    char *part;                 /* zip name of the main document part */
    ZipArchive *zip;
    unsigned char *xml;         /* the part, whole */
    size_t xml_len;
    DocItem *items;
    size_t nitems, citems;
    char root_prefix[32];
    time_t mtime;
    off_t size;
    int backup_done;
    const char *readonly;       /* why saving is refused, or NULL */
} DocFile;

/* doc_parse.c */
int doc_open(DocFile *d, const char *path, char *err, size_t errlen);
void doc_free(DocFile *d);
void doc_item_free(DocItem *it);
int doc_dirty(const DocFile *d);
/* Revision 1 item 38: reads the items again from xml (taken: freed on
 * failure, when d is left as it was). For undo and redo, which replace the
 * main part with the bytes doc_build() gave for an earlier state. */
int doc_reparse(DocFile *d, unsigned char *xml, size_t len, char *err, size_t errlen);

/* doc_save.c. Edits refuse with a Japanese reason in err. */
int doc_set_text(DocFile *d, size_t item, const char *text, char *err, size_t errlen);
/* Inserts an empty paragraph after item (after the last item when item is
 * out of range), formatted like the nearest paragraph before it. Returns
 * the new index or -1. */
long doc_insert_after(DocFile *d, size_t item, char *err, size_t errlen);
int doc_delete(DocFile *d, size_t item, char *err, size_t errlen);
/* 0 when doc_delete() would delete item, else -1 with the reason. */
int doc_can_delete(const DocFile *d, size_t item, char *err, size_t errlen);
/* Writes the new main part into the zip (the other entries copied byte for
 * byte), NAME.bak first when backup and not yet made, then reloads d. */
int doc_save(DocFile *d, int backup, int *backed, char *err, size_t errlen);
/* The bytes the save would write for the main part (malloc'd). */
unsigned char *doc_build(const DocFile *d, size_t *len, char *err, size_t errlen);

/* doc_layout.c: the screen lines, 80 columns. */
enum { DOC_LINE_TEXT, DOC_LINE_SEP, DOC_LINE_TABLE };
typedef struct {
    size_t item;
    int kind;
    size_t off, len;            /* TEXT: glyphs of the item's disp; TABLE: tlines[off] */
} DocLine;
typedef struct {
    DocLine *lines;
    size_t n, cap;
    DocText *tlines;
    size_t ntl, ctl;
    int failed;
} DocLayout;
int doc_layout(const DocFile *d, DocLayout *out, int width);
void doc_layout_free(DocLayout *l);
#endif
