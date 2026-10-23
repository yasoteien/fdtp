# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# LHa archives made by p (docs/FDTP_REVISION1.md §1 13): the level 2 header
# writer and the -lh5- compressor. archive.o calls them; extraction stays
# with lhasa. tests/check_lha.py builds tests/lha_harness.c from LHA_SOURCES.
LHA_SOURCES := lha_write lha_lh5
MODULES += $(LHA_SOURCES)
