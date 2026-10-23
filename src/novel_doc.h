// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVEL_DOC_H
#define FDTP_NOVEL_DOC_H
/* The novel editor's document: the whole text as an array of lines (UTF-8,
 * without line ends), the file's encoding and line end, and an unlimited
 * undo/redo history grouped by command. Text passed in or out of the editing
 * calls uses '\n' between lines; the file's own line end ("\n", "\r\n" or
 * "\r", the first one found) is applied only when saving. */
#include <stddef.h>
#include "jis.h"

typedef struct { size_t line, off; } NPos;
typedef struct { char *s; size_t len, cap; } NLine;
typedef struct {
    int insert;            /* 1 insert, 0 delete */
    NPos at;               /* start of the inserted/deleted text */
    char *text; size_t len;
    unsigned long group, seq;
} NUndo;
typedef struct {
    NLine *lines; size_t count, cap;
    char newline[3];
    JisEncoding encoding;
    int bom;               /* UTF-8 file began with a BOM; kept on save */
    int lossy;             /* non-roundtripping decode or binary NUL: warn before saving */
    NUndo *undo; size_t undo_n, undo_total, undo_cap;
    unsigned long group, seq, clean;
    unsigned long version; /* bumps on every change, including undo/redo */
} NovelDoc;

int npos_cmp(NPos a, NPos b);
void novel_doc_init(NovelDoc *d);
void novel_doc_free(NovelDoc *d);
/* Replace everything with text (any line ends). Clears the history. */
int novel_doc_set_text(NovelDoc *d, const char *text, size_t len);
/* Read a file: detects UTF-8/Shift_JIS/EUC-JP (jis.c), keeps the encoding,
 * BOM and line end. Returns 0, or -1 with errno (EFBIG over max bytes). */
int novel_doc_load(NovelDoc *d, const char *path, size_t max);
/* Whole text with the file's line ends, UTF-8, plus BOM when it had one. */
char *novel_doc_text(const NovelDoc *d, size_t *len);
/* Save in the encoding it was read in (or UTF-8 when utf8). 0 ok, -1 error
 * (errno), -2 the text has characters the encoding cannot represent. */
int novel_doc_save(NovelDoc *d, const char *path, int utf8);
/* Text between a and b with '\n' between lines. Caller frees. */
char *novel_doc_copy(const NovelDoc *d, NPos a, NPos b, size_t *len);
NPos novel_doc_end(const NovelDoc *d);
NPos novel_doc_clamp(const NovelDoc *d, NPos p);

/* Recorded edits. A new group starts with novel_doc_group(); everything up
 * to the next call is undone/redone together. Return the end position of the
 * inserted text / the position of the deletion; on allocation failure the
 * document is unchanged and *ok (when given) is set to 0. */
void novel_doc_group(NovelDoc *d);
/* Makes the edits recorded since undo_n was `from` one group with the first
 * of them (two commands undone by one C-z). */
void novel_doc_join(NovelDoc *d, size_t from);
NPos novel_doc_insert(NovelDoc *d, NPos at, const char *text, size_t len, int *ok);
NPos novel_doc_delete(NovelDoc *d, NPos a, NPos b, int *ok);
/* Undo/redo one group; *cursor receives where the caret belongs. 0 when
 * there is nothing to undo/redo. */
int novel_doc_undo(NovelDoc *d, NPos *cursor);
int novel_doc_redo(NovelDoc *d, NPos *cursor);
int novel_doc_modified(const NovelDoc *d);
void novel_doc_mark_clean(NovelDoc *d);
void novel_doc_mark_dirty(NovelDoc *d);

/* UTF-8 helpers within a line. */
size_t novel_next(const char *s, size_t len, size_t off);
size_t novel_prev(const char *s, size_t off);
uint32_t novel_decode(const char *s, size_t len, size_t off, size_t *next);
size_t novel_encode(uint32_t cp, char out[4]);
/* Forward search for pattern (no '\n') from p, wrapping once to the start.
 * Returns 1 with the match in *start and *end, 0 when absent. */
int novel_doc_find(const NovelDoc *d, NPos p, const char *pattern, size_t n,
                   NPos *start, NPos *end, int *wrapped);
#endif
