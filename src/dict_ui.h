// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DICT_UI_H
#define FDTP_DICT_UI_H
/* The Pomera DM250's stock dictionaries in the reader box (revision 1 §16).
 * The books are read by fdtp-dict, a program of its own beside fdtp (in
 * the Pomera .deb, binary only; not part of this source): fdtp runs it once
 * per lookup and shows what it prints. Without it, or when its gate fails
 * (no stock eMMC mounted at /mnt/pomera-dict), nothing is available.
 * $FDTP_DICT_HELPER replaces the program (the tests' fake one). */
#include "widgets.h"

/* 1 when fdtp-dict is there and at least one book passes its gate. */
int dict_ui_available(void);

/* How text is looked up. EXACT: as it is (a selection, an English word, a
 * typed word). FROM: the longest start that is a word (the text after the
 * cursor). UPTO: the longest end (the text before the cursor). */
enum { DICT_UI_EXACT, DICT_UI_FROM, DICT_UI_UPTO };
/* Opens the reader box on the result (or 「見つかりません」); English text in
 * the English-Japanese book, other text in the Japanese book used last
 * (明鏡国語辞典 at first). An error box when nothing is available. */
void dict_ui_lookup(Widgets *w, const char *text, int how);
/* Shows the last result again (after a cancelled word box); 0 when none. */
int dict_ui_reopen(Widgets *w);

/* Keys while the box is open. → or Tab, ← or Shift+Tab: the next or
 * previous book (the same word); Shift+← →: the previous or next entry;
 * Esc: close; scrolling is the box's own. ASK (RET): the caller asks for another word, first text
 * dict_ui_word(), then calls dict_ui_lookup or dict_ui_reopen. */
enum { DICT_UI_OPEN, DICT_UI_CLOSED, DICT_UI_ASK };
int dict_ui_key(Widgets *w, const Key *k);
const char *dict_ui_word(void);
#endif
