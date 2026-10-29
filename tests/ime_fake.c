// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Test input method for tests/check_novel.py (macOS, no uim): implements
 * ime.h with a tiny romaji table so the editor's preedit, candidate list and
 * commit paths can be exercised headless. M-SPC toggles; while on, letters
 * build a hiragana preedit, SPC opens/advances candidates, RET commits,
 * BS deletes, ESC cancels. "nihon" offers 日本 / 二本 / にほん. */
#include "ime.h"
#include <stdlib.h>
#include <string.h>

struct Ime {
    int on, active, index;
    char romaji[64], preedit[256], commit[256];
    const char *cand[3];
    int count;
};
static const struct { const char *r, *k; } table[] = {
    {"nn", "ん"}, {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"sa", "さ"}, {"shi", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"chi", "ち"}, {"tsu", "つ"}, {"te", "て"}, {"to", "と"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"fu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"}, {"ra", "ら"}, {"ri", "り"},
    {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"}, {"wa", "わ"}, {"wo", "を"},
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
};
static void convert(Ime *ime)
{
    const char *r = ime->romaji;
    char *out = ime->preedit;
    *out = 0;
    while (*r) {
        size_t i, n = 0;
        for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
            n = strlen(table[i].r);
            if (!strncmp(r, table[i].r, n)) break;
        }
        if (i < sizeof(table) / sizeof(table[0])) { strcat(out, table[i].k); r += n; }
        else if (*r == 'n' && !r[1]) { strcat(out, "ん"); ++r; }
        else { size_t len = strlen(out); out[len] = *r++; out[len + 1] = 0; }
    }
}
Ime *ime_open(void) { return calloc(1, sizeof(Ime)); }
void ime_close(Ime *ime) { free(ime); }
int ime_available(const Ime *ime) { (void)ime; return 1; }
int ime_on(const Ime *ime) { return ime->on; }
const char *ime_label(const Ime *ime) { return ime->on ? "あ" : "切"; }
static void reset(Ime *ime) { ime->romaji[0] = ime->preedit[0] = 0; ime->active = 0; ime->count = 0; }
static void commit(Ime *ime, const char *s)
{
    if (strlen(ime->commit) + strlen(s) < sizeof(ime->commit)) strcat(ime->commit, s);
    reset(ime);
}
int ime_key(Ime *ime, const Key *k)
{
    if ((k->key == KCHAR && k->ch == ' ' && (k->mods & MOD_ALT)) ||
        (k->key == KCHAR && k->ch == IME_CH_ZENKAKU_HANKAKU)) {
        if (ime->preedit[0]) commit(ime, ime->preedit);
        ime->on = !ime->on;
        return 1;
    }
    if (!ime->on) return 0;
    int busy = ime->preedit[0] != 0;
    if (k->key == KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) && k->ch >= 'a' && k->ch <= 'z') {
        if (ime->active) commit(ime, ime->cand[ime->index]);
        size_t n = strlen(ime->romaji);
        if (n + 1 < sizeof(ime->romaji)) { ime->romaji[n] = (char)k->ch; ime->romaji[n + 1] = 0; }
        convert(ime);
        return 1;
    }
    if (!busy) return 0;
    if (k->key == KCHAR && k->ch == ' ' && !k->mods) {
        if (!ime->active) {
            ime->active = 1; ime->index = 0;
            if (!strcmp(ime->preedit, "にほん")) {
                ime->cand[0] = "日本"; ime->cand[1] = "二本"; ime->cand[2] = "にほん"; ime->count = 3;
            } else { ime->cand[0] = ime->preedit; ime->count = 1; }
        } else ime->index = (ime->index + 1) % ime->count;
        return 1;
    }
    if (k->key == KRET) { commit(ime, ime->active ? ime->cand[ime->index] : ime->preedit); return 1; }
    if (k->key == KESC) { reset(ime); return 1; }
    if (k->key == KBS) {
        if (ime->active) { ime->active = 0; return 1; }
        size_t n = strlen(ime->romaji);
        if (n) ime->romaji[n - 1] = 0;
        convert(ime);
        return 1;
    }
    return 1;
}
const char *ime_commit(Ime *ime) { return ime->commit; }
void ime_commit_done(Ime *ime) { ime->commit[0] = 0; }
int ime_busy(const Ime *ime) { return ime->preedit[0] || ime->active; }
void ime_preedit(const Ime *ime, ImePreedit *out)
{
    const char *text = ime->active ? ime->cand[ime->index] : ime->preedit;
    size_t n = strlen(text);
    *out = (ImePreedit){text, n, ime->active ? 0 : n, n};
}
void ime_candidates(const Ime *ime, ImeCandidates *out)
{
    *out = (ImeCandidates){0};
    if (!ime->active) return;
    out->active = 1; out->count = ime->count; out->index = ime->index; out->first = 0;
    for (int i = 0; i < ime->count; ++i) out->items[out->shown++] = ime->cand[i];
}
void ime_reset(Ime *ime) { reset(ime); }
void ime_caret(Ime *ime, int x, int y, int h) { (void)ime; (void)x; (void)y; (void)h; }
