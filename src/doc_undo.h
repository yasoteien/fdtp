// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DOC_UNDO_H
#define FDTP_DOC_UNDO_H
/* Revision 1 items 38 and 41: Ctrl+z (取り消し) and Ctrl+y (やり直し) of the
 * docx, xlsx and pptx views. A history of steps, one per change (a docx
 * paragraph, an xlsx cell with every cell its edit changed, a pptx
 * paragraph), at most UNDO_LEVELS of them, for as long as the file is open.
 * A new change drops the steps that could be redone. The states are
 * numbered, and the number of the state the file holds is kept: undoing or
 * redoing back to it makes the view clean again (「変更あり」 goes). Saving
 * keeps the history. */
#include <stddef.h>

typedef struct {
    void *data;             /* the change, owned by the history */
    long before, after;     /* the view's place (paragraph) before and after, -1 none */
    int saves;              /* the view's count of saves when the step was made */
} UndoStep;
typedef struct {
    UndoStep *steps;
    size_t n, cap, pos;     /* steps[0..pos) done, steps[pos..n) can be redone */
    unsigned long base;     /* number of the state before steps[0] */
    unsigned long saved;    /* number of the state in the file, UNDO_NOWHERE when gone */
    void (*free_data)(void *);
} UndoHist;
#define UNDO_NOWHERE ((unsigned long)-1)

void undo_init(UndoHist *h, void (*free_data)(void *));
void undo_free(UndoHist *h);
/* Records a change just made (the history takes step->data). levels is
 * UNDO_LEVELS (<= 0: the default 100). 0, or -1 when out of memory: data
 * is freed and the history is emptied (undo_lost). */
int undo_push(UndoHist *h, int levels, const UndoStep *step);
/* The step Ctrl+z / Ctrl+y would apply, NULL when there is none. */
UndoStep *undo_next_undo(UndoHist *h);
UndoStep *undo_next_redo(UndoHist *h);
/* After that step was applied. */
void undo_done_undo(UndoHist *h);
void undo_done_redo(UndoHist *h);
/* The file now holds the present state. */
void undo_saved(UndoHist *h);
/* The present state is not the file's. */
int undo_dirty(const UndoHist *h);
/* The steps cannot be applied any more (the present state is kept). */
void undo_clear(UndoHist *h);
/* The state changed without a step (out of memory): no way back. */
void undo_lost(UndoHist *h);

/* One byte splice: before[at .. at+old_len) became after[at .. at+new_len)
 * (the docx main part, a pptx slide). */
typedef struct {
    size_t at, old_len, new_len;
    unsigned char *old_bytes, *new_bytes;
} UndoSplice;
/* The difference of a (before) and b (after); NULL with *same set when they
 * are equal, NULL without it when out of memory. */
UndoSplice *undo_splice_make(const unsigned char *a, size_t an, const unsigned char *b, size_t bn, int *same);
/* cur with the splice undone (backward) or redone (malloc'd, *out its
 * length). NULL when cur does not hold the bytes the splice expects, or
 * when out of memory. */
unsigned char *undo_splice_apply(const UndoSplice *s, int backward, const unsigned char *cur, size_t n, size_t *out);
void undo_splice_free(void *s);
#endif
