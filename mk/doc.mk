# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Documents (docs/FDTP_VIEWERS.md stage 5): docx view and paragraph editing
# on expat with src/zipfile.c. zipfile, zlib and expat come from mk/sheet.mk;
# adding zipfile again would link zipfile.o twice. tests/check_doc.py reads
# DOC_SOURCES. doc_undo (revision 1 items 38 and 41) is the undo history the
# docx, xlsx and pptx views share; the sheet and slide tests link it too.
DOC_SOURCES := docview doc_parse doc_layout doc_save doc_input doc_undo
MODULES += $(DOC_SOURCES)
