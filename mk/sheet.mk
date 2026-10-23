# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Tables (docs/FDTP_VIEWERS.md stage 4): csv, tsv and xlsx on zlib and expat
# with the zip reader/writer src/zipfile.c (also for the later Office viewers
# and the archive browser). Mac: the SDK's zlib and expat; Linux: pkg-config.
# sheet_undo: one cell's edit as an undo step (revision 1 item 38).
SHEET_SOURCES := sheetview sheet_model sheet_csv sheet_xlsx sheet_formula sheet_undo zipfile
MODULES += $(SHEET_SOURCES)
ifeq ($(shell uname),Darwin)
SHEET_CFLAGS :=
SHEET_LIBS := -lz -lexpat
else
SHEET_CFLAGS := $(shell $(or $(PKG_CONFIG),pkg-config) --cflags zlib expat)
SHEET_LIBS := $(shell $(or $(PKG_CONFIG),pkg-config) --libs zlib expat) -lm
endif
MODULE_CFLAGS += $(SHEET_CFLAGS)
MODULE_LIBS += $(SHEET_LIBS)
