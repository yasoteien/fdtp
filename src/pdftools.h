// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_PDFTOOLS_H
#define FDTP_PDFTOOLS_H
#include "app.h"
#include "fileops.h"
/* Revision 1 item 39: the PDF menu (Alt+p), done on the Pomera only with the
 * bundled MuPDF. first picks the entry to open straight away: the menu
 * itself, or split / merge (Alt+n and Alt+h on PDFs, item 37). page is the
 * PDF viewer's page (0-based) to start the page fields with, -1 from the
 * filer. Returns 0 once a screen or box is up, -1 after a notice. */
typedef enum { PDFTOOLS_MENU, PDFTOOLS_SPLIT, PDFTOOLS_MERGE } PdfToolsEntry;
int pdftools_open(App *app, const FileSelection *selection, PdfToolsEntry first, int page);
/* 1 when name ends in .pdf (any case). */
int pdftools_is_pdf(const char *name);
#endif
