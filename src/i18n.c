// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "i18n.h"
#include <stddef.h>
#include <string.h>
#include "i18n_en.inc"

static int language = I18N_JA;

void i18n_set(int lang)
{
    language = lang == I18N_EN ? I18N_EN : I18N_JA;
}

int i18n_lang(void)
{
    return language;
}

const char *T(const char *ja)
{
    if (language != I18N_EN || !ja) return ja;
    size_t lo = 0, hi = i18n_en_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = strcmp(ja, i18n_en[mid].ja);
        if (order == 0) return i18n_en[mid].en;
        if (order < 0) hi = mid;
        else lo = mid + 1;
    }
    return ja;
}
