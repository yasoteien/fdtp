// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Input method placeholder for macOS and for Linux without uim: every key is
 * passed back to the field, which inserts alphanumerics itself. */
#include "ime.h"
#include "i18n.h"
#include <stdlib.h>

struct Ime { int unused; };

Ime *ime_open(void) { return calloc(1, sizeof(Ime)); }
void ime_close(Ime *ime) { free(ime); }
int ime_available(const Ime *ime) { (void)ime; return 0; }
int ime_on(const Ime *ime) { (void)ime; return 0; }
const char *ime_label(const Ime *ime) { (void)ime; return T("切"); }
int ime_key(Ime *ime, const Key *key) { (void)ime; (void)key; return 0; }
const char *ime_commit(Ime *ime) { (void)ime; return ""; }
void ime_commit_done(Ime *ime) { (void)ime; }
int ime_busy(const Ime *ime) { (void)ime; return 0; }
void ime_preedit(const Ime *ime, ImePreedit *out)
{
    (void)ime;
    *out = (ImePreedit){"", 0, 0, 0};
}
void ime_candidates(const Ime *ime, ImeCandidates *out)
{
    (void)ime;
    *out = (ImeCandidates){0};
}
void ime_reset(Ime *ime) { (void)ime; }
