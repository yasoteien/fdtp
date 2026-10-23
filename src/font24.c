// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "font24.h"
#include <stddef.h>
#include "font24.inc"

static const Font24Glyph *find_glyph(uint32_t code);

int font24_width(uint32_t code)
{
    const Font24Glyph *glyph = find_glyph(code);
    if (glyph) return glyph->width / 8;
    /* Missing glyph widths use fixed East Asian wide/fullwidth ranges,
       independent of locale. Other missing characters occupy one cell. */
    return (code >= 0x1100 &&
            (code <= 0x115f || code == 0x2329 || code == 0x232a ||
             (code >= 0x2e80 && code <= 0xa4cf && code != 0x303f) ||
             (code >= 0xac00 && code <= 0xd7a3) ||
             (code >= 0xf900 && code <= 0xfaff) ||
             (code >= 0xfe10 && code <= 0xfe19) ||
             (code >= 0xfe30 && code <= 0xfe6f) ||
             (code >= 0xff01 && code <= 0xff60) ||
             (code >= 0xffe0 && code <= 0xffe6) ||
             (code >= 0x1f300 && code <= 0x1faff) ||
             (code >= 0x20000 && code <= 0x3fffd))) ? 2 : 1;
}

static const Font24Glyph *find_glyph(uint32_t code)
{
    size_t lo = 0, hi = sizeof(font24_glyphs) / sizeof(font24_glyphs[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (font24_glyphs[mid].code < code) lo = mid + 1;
        else if (font24_glyphs[mid].code > code) hi = mid;
        else return &font24_glyphs[mid];
    }
    return NULL;
}

const Font24Glyph *font24_lookup(uint32_t code)
{
    const Font24Glyph *glyph = find_glyph(code);
    return glyph ? glyph : find_glyph(font24_width(code) == 2 ? 0x3013 : '?');
}

uint32_t font24_decode(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t code, minimum;
    unsigned count;
    if (!*s) return 0;
    if (*s < 0x80) { *text += 1; return *s; }
    if (*s >= 0xc2 && *s <= 0xdf) { code = *s & 0x1f; count = 2; minimum = 0x80; }
    else if (*s >= 0xe0 && *s <= 0xef) { code = *s & 0x0f; count = 3; minimum = 0x800; }
    else if (*s >= 0xf0 && *s <= 0xf4) { code = *s & 7; count = 4; minimum = 0x10000; }
    else { *text += 1; return '?'; }
    for (unsigned i = 1; i < count; ++i) {
        if ((s[i] & 0xc0) != 0x80) { *text += 1; return '?'; }
        code = (code << 6) | (s[i] & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        *text += 1;
        return '?';
    }
    *text += count;
    return code;
}
