// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Japanese input through libuim (Linux; uim 1.9.6 + Mozc on the Pomera).
 * Used the way uim-fep uses it: uim_init once, then one context per Ime
 * (UTF-8, "ja", the default IM of ~/.uim), every key pressed and released
 * through uim, results from the commit/preedit/candidate callbacks.
 * When uim cannot start the handle behaves like ime_none.c. */
#define _POSIX_C_SOURCE 200809L
#include "ime.h"
#include "i18n.h"
#include <uim/uim.h>
#include <stdlib.h>
#include <string.h>

#define IME_CANDIDATES_MAX 512

typedef struct { char *text; size_t length, cap; } Buffer;

struct Ime {
    uim_context uc;
    Buffer commit, preedit;
    size_t caret, hl_start, hl_end;
    int cand_active, cand_count, cand_index, cand_limit;
    char *cand[IME_CANDIDATES_MAX];
    char label[16];
};

static int uim_state; /* 0 untried, 1 running, -1 failed */

static void stop_uim(void) { uim_quit(); }

static int append(Buffer *b, const char *s, size_t n)
{
    if (b->length + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->length + n + 1) cap *= 2;
        char *p = realloc(b->text, cap);
        if (!p) return -1;
        b->text = p; b->cap = cap;
    }
    memcpy(b->text + b->length, s, n);
    b->length += n;
    b->text[b->length] = 0;
    return 0;
}
static void clear(Buffer *b) { b->length = 0; if (b->text) b->text[0] = 0; }

static void commit_cb(void *ptr, const char *str)
{
    Ime *ime = ptr;
    if (str) append(&ime->commit, str, strlen(str));
}
static void preedit_clear_cb(void *ptr)
{
    Ime *ime = ptr;
    clear(&ime->preedit);
    ime->caret = (size_t)-1; /* no cursor segment: the caret is at the end */
    ime->hl_start = ime->hl_end = 0;
}
static void preedit_pushback_cb(void *ptr, int attr, const char *str)
{
    Ime *ime = ptr;
    size_t n = str ? strlen(str) : 0;
    if (attr & UPreeditAttr_Cursor) ime->caret = ime->preedit.length;
    if ((attr & UPreeditAttr_Reverse) && n) {
        if (ime->hl_start == ime->hl_end) ime->hl_start = ime->preedit.length;
        ime->hl_end = ime->preedit.length + n;
    }
    if (n) append(&ime->preedit, str, n);
}
static void preedit_update_cb(void *ptr) { (void)ptr; }

static void free_candidates(Ime *ime)
{
    for (int i = 0; i < ime->cand_count; ++i) { free(ime->cand[i]); ime->cand[i] = NULL; }
    ime->cand_count = 0;
}
static void cand_activate_cb(void *ptr, int nr, int display_limit)
{
    Ime *ime = ptr;
    free_candidates(ime);
    if (nr > IME_CANDIDATES_MAX) nr = IME_CANDIDATES_MAX;
    ime->cand_limit = display_limit > 0 && display_limit <= IME_PAGE_MAX ? display_limit : IME_PAGE_MAX;
    for (int i = 0; i < nr; ++i) {
        uim_candidate c = uim_get_candidate(ime->uc, i, ime->cand_limit ? i % ime->cand_limit : i);
        const char *s = c ? uim_candidate_get_cand_str(c) : NULL;
        ime->cand[i] = strdup(s ? s : "");
        if (c) uim_candidate_free(c);
        if (!ime->cand[i]) break;
        ime->cand_count = i + 1;
    }
    ime->cand_index = -1;
    ime->cand_active = ime->cand_count > 0;
}
static void cand_select_cb(void *ptr, int index)
{
    Ime *ime = ptr;
    if (index >= 0 && index < ime->cand_count) ime->cand_index = index;
}
static void cand_shift_page_cb(void *ptr, int direction)
{
    Ime *ime = ptr;
    if (!ime->cand_count) return;
    int index = ime->cand_index < 0 ? 0 : ime->cand_index;
    index += direction ? ime->cand_limit : -ime->cand_limit;
    if (index < 0) index = ((ime->cand_count - 1) / ime->cand_limit) * ime->cand_limit;
    if (index >= ime->cand_count) index = 0;
    ime->cand_index = index;
    uim_set_candidate_index(ime->uc, index);
}
static void cand_deactivate_cb(void *ptr)
{
    Ime *ime = ptr;
    ime->cand_active = 0;
    free_candidates(ime);
}

/* The state label. Mozc names its modes (and the iconic labels of the input
 * mode branch of the property list) after the characters it produces. */
static void set_label_from_name(Ime *ime, const char *name)
{
    static const struct { const char *key, *label; } names[] = {
        {"direct", "切"}, {"直接", "切"}, /* i18n:keep */
        {"halfwidth-katakana", "ｱ"}, {"halfkana", "ｱ"}, {"半角カタカナ", "ｱ"}, /* i18n:keep */
        {"hiragana", "あ"}, {"ひらがな", "あ"}, /* i18n:keep */
        {"katakana", "ア"}, {"カタカナ", "ア"}, /* i18n:keep */
        {"fullwidth", "Ａ"}, {"全角英数", "Ａ"}, /* i18n:keep */
        {"alnum", "A"}, {"英数", "A"}, {"latin", "A"}, /* i18n:keep */
    };
    if (!name || !*name) return;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strstr(name, names[i].key)) {
            strncpy(ime->label, names[i].label, sizeof(ime->label) - 1);
            return;
        }
}
static void mode_cb(void *ptr, int mode)
{
    Ime *ime = ptr;
    set_label_from_name(ime, uim_get_mode_name(ime->uc, mode));
}
static void prop_list_cb(void *ptr, const char *str)
{
    /* Lines: "branch\t<indication id>\t<iconic label>\t<label>..." */
    Ime *ime = ptr;
    const char *line = str;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (n > 7 && !strncmp(line, "branch\t", 7)) {
            char copy[256];
            if (n >= sizeof(copy)) n = sizeof(copy) - 1;
            memcpy(copy, line, n); copy[n] = 0;
            char *id = copy + 7, *icon = strchr(id, '\t');
            if (icon) {
                *icon++ = 0;
                char *tab = strchr(icon, '\t'); if (tab) *tab = 0;
                if (strstr(id, "input_mode") || strstr(id, "input-mode")) {
                    if (!strcmp(icon, "-") || !strcmp(icon, "直")) strcpy(ime->label, "切"); /* i18n:keep */
                    else if (strlen(icon) < sizeof(ime->label)) strcpy(ime->label, icon);
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
}

Ime *ime_open(void)
{
    Ime *ime = calloc(1, sizeof(*ime));
    if (!ime) return NULL;
    strcpy(ime->label, "切"); /* i18n:keep */
    ime->caret = (size_t)-1;
    if (!uim_state) {
        uim_state = uim_init() == 0 ? 1 : -1;
        if (uim_state == 1) atexit(stop_uim);
    }
    if (uim_state != 1) return ime;
    const char *engine = uim_get_default_im_name("ja_JP.UTF-8");
    ime->uc = uim_create_context(ime, "UTF-8", "ja", engine, NULL, commit_cb);
    if (!ime->uc) return ime;
    uim_set_preedit_cb(ime->uc, preedit_clear_cb, preedit_pushback_cb, preedit_update_cb);
    uim_set_candidate_selector_cb(ime->uc, cand_activate_cb, cand_select_cb,
                                  cand_shift_page_cb, cand_deactivate_cb);
    uim_set_mode_cb(ime->uc, mode_cb);
    uim_set_prop_list_update_cb(ime->uc, prop_list_cb);
    set_label_from_name(ime, uim_get_mode_name(ime->uc, uim_get_current_mode(ime->uc)));
    uim_prop_list_update(ime->uc);
    return ime;
}
void ime_close(Ime *ime)
{
    if (!ime) return;
    if (ime->uc) uim_release_context(ime->uc);
    free_candidates(ime);
    free(ime->commit.text); free(ime->preedit.text); free(ime);
}
int ime_available(const Ime *ime) { return ime && ime->uc; }
int ime_on(const Ime *ime) { return ime && ime->uc && strcmp(ime->label, "切"); } /* i18n:keep */
const char *ime_label(const Ime *ime)
{
    if (!ime) return T("切");
    if (i18n_lang() == I18N_EN) {
        if (!strcmp(ime->label, "切")) return T("切"); /* i18n:keep */
        if (!strcmp(ime->label, "あ")) return "Hir"; /* i18n:keep */
        if (!strcmp(ime->label, "ア")) return "Kat"; /* i18n:keep */
        if (!strcmp(ime->label, "ｱ")) return "HKat"; /* i18n:keep */
        if (!strcmp(ime->label, "Ａ")) return "Wide"; /* i18n:keep */
        return "A";
    }
    return ime->label;
}

/* FDTP logical key -> uim key and modifier state; 0 when uim has no use. */
static int to_ukey(const Key *k, int *key, int *state)
{
    static const struct { int fd, ukey; } special[] = {
        {KUP, UKey_Up}, {KDOWN, UKey_Down}, {KLEFT, UKey_Left}, {KRIGHT, UKey_Right},
        {KPGUP, UKey_Prior}, {KPGDN, UKey_Next}, {KHOME, UKey_Home}, {KEND, UKey_End},
        {KINS, UKey_Insert}, {KDEL, UKey_Delete}, {KBS, UKey_Backspace},
        {KRET, UKey_Return}, {KTAB, UKey_Tab}, {KESC, UKey_Escape},
    };
    *state = (k->mods & MOD_SHIFT ? UMod_Shift : 0) | (k->mods & MOD_CTRL ? UMod_Control : 0) |
             (k->mods & MOD_ALT ? UMod_Alt : 0);
    if (k->key == KCHAR) {
        if (k->ch == IME_CH_ZENKAKU_HANKAKU) { *key = UKey_Zenkaku_Hankaku; return 1; }
        if (k->ch == 0x00a5) { *key = UKey_Yen; return 1; }
        if (k->ch >= 32 && k->ch < 127) { *key = (int)k->ch; return 1; }
        return 0;
    }
    if (k->key >= KF1 && k->key <= KF12) { *key = UKey_F1 + (k->key - KF1); return 1; }
    for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); ++i)
        if (special[i].fd == k->key) { *key = special[i].ukey; return 1; }
    return 0;
}
int ime_key(Ime *ime, const Key *k)
{
    int key, state;
    if (!ime || !ime->uc || !k || !to_ukey(k, &key, &state)) return 0;
    /* uim_press_key returns 0 when the input method filtered the key. */
    int not_filtered = uim_press_key(ime->uc, key, state);
    uim_release_key(ime->uc, key, state);
    return not_filtered == 0;
}
const char *ime_commit(Ime *ime) { return ime && ime->commit.text ? ime->commit.text : ""; }
void ime_commit_done(Ime *ime) { if (ime) clear(&ime->commit); }
int ime_busy(const Ime *ime) { return ime && (ime->preedit.length || ime->cand_active); }
void ime_preedit(const Ime *ime, ImePreedit *out)
{
    if (!ime || !ime->preedit.length) { *out = (ImePreedit){"", 0, 0, 0}; return; }
    size_t caret = ime->caret > ime->preedit.length ? ime->preedit.length : ime->caret;
    *out = (ImePreedit){ime->preedit.text, caret, ime->hl_start, ime->hl_end};
}
void ime_candidates(const Ime *ime, ImeCandidates *out)
{
    *out = (ImeCandidates){0};
    if (!ime || !ime->cand_active || !ime->cand_count) return;
    int index = ime->cand_index < 0 ? 0 : ime->cand_index;
    int first = index / ime->cand_limit * ime->cand_limit;
    out->active = 1; out->count = ime->cand_count; out->index = ime->cand_index;
    out->first = first;
    for (int i = first; i < ime->cand_count && out->shown < ime->cand_limit; ++i)
        out->items[out->shown++] = ime->cand[i];
}
void ime_reset(Ime *ime)
{
    if (!ime || !ime->uc) return;
    uim_reset_context(ime->uc);
    preedit_clear_cb(ime);
    cand_deactivate_cb(ime);
}
void ime_caret(Ime *ime, int x, int y, int h) { (void)ime; (void)x; (void)y; (void)h; }
