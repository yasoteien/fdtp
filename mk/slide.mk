# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Slides (docs/FDTP_VIEWERS.md stage 6): pptx display on expat over
# src/zipfile.c, pictures through the stage 2 decoders. slideview itself is
# already in COMMON (the launcher's entrance); the zip reader, zlib and
# expat come from mk/sheet.mk and the image libraries from mk/imageview.mk,
# so nothing is added to MODULE_LIBS here. slide_edit: editing the text
# (revision 1 item 41), with doc_input and doc_undo from mk/doc.mk.
SLIDE_SOURCES := slide_xml slide_render slide_edit
MODULES += $(SLIDE_SOURCES)
