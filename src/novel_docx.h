// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVEL_DOCX_H
#define FDTP_NOVEL_DOCX_H
/* Word vertical export of the novel editor (docs/FDTP_REVISION1.md 4.3).
 * The template (a .docx, A4 tbRl) is copied with zip_rewrite: every entry
 * byte for byte except word/document.xml, in which only the content of
 * w:body before its last w:sectPr is replaced (namespaces, the section and
 * everything else as in the template). One line is one paragraph (an empty
 * line an empty paragraph), with the template's first paragraph's w:pPr.
 * With the Kakuyomu export header only the title (first paragraph) and the
 * body go in. ruby 0: the ruby notation stays as text; ruby 1: ｜親《ルビ》,
 * 漢字《ルビ》 and 漢字（かな） become w:ruby (ruby half the body size, from
 * styles.xml, 21 half-points when absent) and 《《傍点》》 w:em dot.
 * template_path NULL: the built-in default template (novel_docx.c; A4
 * landscape, tbRl, 游明朝 10.5 pt) is written as a new archive instead. */
#include <stddef.h>
#include "novel_doc.h"

/* 0, or -1 with a Japanese message in err. template_path may be NULL. */
int novel_docx_write(const NovelDoc *d, const char *template_path, const char *dest, int ruby,
                     char *err, size_t errlen);
/* The paragraphs alone (for tests): the w:p elements for d. Caller frees. */
char *novel_docx_body(const NovelDoc *d, const char *ppr, int ruby, int size, size_t *len);
#endif
