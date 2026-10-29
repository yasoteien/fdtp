// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_FDSCREEN_H
#define FDTP_FDSCREEN_H
#include "cells.h"
#include "dirlist.h"
/* Rows of the file list (6 .. FD_ROWS-3): 17 at 25 rows. A page is
 * FDSCREEN_ROWS * fdscreen_slots(columns) entries. */
#define FDSCREEN_ROWS (FD_ROWS - 8)
/* Display modes 1 2 3 5 (the keys): how many lists side by side, the left
 * column of each and its width. 80 columns: 1 2 3 5 lists of 80 40 26 16.
 * Wider (WIDE_NOTES.md): mode 2 has a list for every 40 columns, 3 for every
 * 26, 5 for every 16, the width shared out; mode 1 stays one list. */
/* The name in the title, "FDTP"; the macOS window's is "FDTM". 4 letters. */
extern const char *fdscreen_program;
int fdscreen_slots(int columns);
int fdscreen_slot_x(int columns, int slot);
int fdscreen_slot_width(int columns);
/* columns: 2 (FD's default: name, size, date, time) or 5 (names only). */
void fdscreen_build(Cells *cells, const DirList *list, size_t cursor, int columns, time_t now);
void fdscreen_entry(Cells *c,const DirEntry *entry,int selected,int columns,int slot,int row);
void fdscreen_number(Cells *c,int end,int row,uint64_t value,int min_col);
#endif
