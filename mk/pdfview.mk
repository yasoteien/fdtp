# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# PDF viewer (docs/FDTP_VIEWERS.md stage 3). pdfview is already in COMMON;
# on Linux it is built against libmupdf from the sysroot, on the Mac it
# stays the notice stub (no MuPDF there).
ifneq ($(UNAME),Darwin)
MODULE_CFLAGS += -DFDTP_WITH_MUPDF
# libmupdf's own dependencies (libgumbo, libmujs, libjbig2dec) are also in
# $ORIGIN/../lib; DT_RUNPATH would only serve fdtp's direct NEEDED, so use
# DT_RPATH, which the loader also applies to the libraries' dependencies.
MODULE_LIBS += -lmupdf -lm -Wl,--disable-new-dtags
# Revision 1 item 39: the work of the PDF menu (pdftools.c asks, this writes).
MODULES += pdftools_job
endif
