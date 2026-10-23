// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DOC_INPUT_H
#define FDTP_DOC_INPUT_H
/* The row-23 input line of the docx editor (stage 5). Unlike widgets.c it
 * has no length limit, keeps tabs and line breaks of the paragraph, scrolls
 * sideways for long paragraphs and takes Japanese through ime.h (uim on
 * Linux; on the Mac ime_none, alphanumerics only). */
#include "cells.h"
#include "ime.h"
#include "keys.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int active;
    char label[64];
    uint32_t *text;
    size_t len, cap, cursor, scroll;
    Ime *ime;                   /* borrowed */
    int failed;
} DocInput;
enum { DOC_INPUT_MORE, DOC_INPUT_ACCEPT, DOC_INPUT_CANCEL };

/* 0 ok, -1 out of memory. */
int doc_input_open(DocInput *in, Ime *ime, const char *label, const char *utf8);
int doc_input_key(DocInput *in, const Key *k);
/* Row 23 and, while the input method shows candidates, the rows above. */
void doc_input_draw(DocInput *in, Cells *c);
/* The text as UTF-8 (malloc'd), NULL when out of memory. */
char *doc_input_text(const DocInput *in);
void doc_input_close(DocInput *in);
#endif
