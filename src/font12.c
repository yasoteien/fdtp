// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "font12.h"
#include <stddef.h>
#include "font12.inc"

const Font12Glyph *font12_find(uint32_t code)
{
    size_t lo = 0, hi = sizeof(font12_glyphs) / sizeof(font12_glyphs[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (font12_glyphs[mid].code < code) lo = mid + 1;
        else if (font12_glyphs[mid].code > code) hi = mid;
        else return &font12_glyphs[mid];
    }
    return NULL;
}
