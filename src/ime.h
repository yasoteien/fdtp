// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_IME_H
#define FDTP_IME_H
/* Japanese input for any FDTP text field (the novel editor, the row-23 input
 * line, sheet cell editing). Two implementations share this contract:
 *   ime_uim.c  Linux: libuim (uim 1.9.6 + Mozc, ~/.uim default-im-name),
 *              used like uim-fep: uim_init, one uim context per Ime handle
 *              (UTF-8, "ja"), keys through uim_press_key/uim_release_key,
 *              commit/preedit/candidate callbacks.
 *   ime_none.c macOS, or Linux when uim cannot start: alphanumerics only,
 *              the state is always 切 (off) and every key is passed back.
 *
 * Usage, per key the field receives:
 *     if (!ime_key(ime, key)) handle the key yourself (insert k->ch, move...);
 *     const char *t = ime_commit(ime); if (*t) { insert t; ime_commit_done(ime); }
 * Always check ime_commit() after ime_key(), whatever it returned: a key can
 * both commit the preedit and be passed on.
 * While ime_busy() the field should draw ime_preedit() at its caret
 * (underlined, the reverse range as the segment being converted) and, when
 * ime_candidates() is active, the candidate list below the caret.
 * The Alt+Space and 全角/半角 keys toggle input on/off inside uim (the
 * generic-on-key of ~/.uim); fields never interpret them. */
#include <stddef.h>
#include "keys.h"

/* keys.h has no 全角/半角 key yet. Until it gains one, keys_evdev should map
 * evdev 41 on the jp106 layout (KEY_GRAVE = 半角/全角) to
 * Key{KCHAR, IME_CH_ZENKAKU_HANKAKU, mods}; ime_uim passes it to uim as
 * UKey_Zenkaku_Hankaku and fields must not insert it (Private Use). */
#define IME_CH_ZENKAKU_HANKAKU 0xF8F0u

typedef struct Ime Ime;

typedef struct {
    const char *text;   /* UTF-8 preedit, "" when none; valid until next ime_* call */
    size_t caret;       /* byte offset of the preedit caret in text */
    size_t hl_start, hl_end; /* reversed segment (being converted), bytes; equal = none */
} ImePreedit;

#define IME_PAGE_MAX 16
typedef struct {
    int active;         /* candidate list shown */
    int count;          /* total candidates */
    int index;          /* selected, 0..count-1 (or -1 before the first select) */
    int first;          /* index of items[0] */
    int shown;          /* items on this page (<= IME_PAGE_MAX) */
    const char *items[IME_PAGE_MAX]; /* UTF-8, valid until next ime_* call */
} ImeCandidates;

/* Never NULL unless out of memory; falls back to the "none" behaviour when
 * the input method cannot start (ime_available() is then 0). */
Ime *ime_open(void);
void ime_close(Ime *ime);
/* 1 when Japanese input can be turned on at all. */
int ime_available(const Ime *ime);
/* 1 while input is on (not direct/alphanumeric pass-through). */
int ime_on(const Ime *ime);
/* Short UTF-8 label for the status band: "あ" "ア" "ｱ" "Ａ" "A" ... or "切". */
const char *ime_label(const Ime *ime);
/* Feed a key press (the release is generated internally). Returns 1 when the
 * input method consumed it, 0 when the field must handle it. */
int ime_key(Ime *ime, const Key *key);
/* Text committed since the last ime_commit_done(), "" when none. */
const char *ime_commit(Ime *ime);
void ime_commit_done(Ime *ime);
/* 1 while there is a preedit or a candidate list. */
int ime_busy(const Ime *ime);
void ime_preedit(const Ime *ime, ImePreedit *out);
void ime_candidates(const Ime *ime, ImeCandidates *out);
/* Drop the preedit/candidates without committing (e.g. the field closes). */
void ime_reset(Ime *ime);
#endif
