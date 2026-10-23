// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "jis.h"
#include <stdlib.h>
#include <string.h>
#include "jis0208.inc"

const char *jis_encoding_name(JisEncoding encoding) {
    switch (encoding) {
    case JIS_UTF8: return "UTF-8";
    case JIS_SHIFT_JIS: return "Shift_JIS";
    case JIS_EUC_JP: return "EUC-JP";
    case JIS_ISO2022JP: return "ISO-2022-JP";
    case JIS_UTF16LE: return "UTF-16LE";
    case JIS_UTF16BE: return "UTF-16BE";
    }
    return "?";
}

size_t jis_decode_one(const unsigned char *s, size_t n, JisEncoding enc,
                      uint32_t *out) {
    uint32_t c;
    size_t count, i;
    if (!n || !s || !out || enc < JIS_UTF8 || enc >= JIS_ENCODING_COUNT) return 0;
    if (enc >= JIS_ISO2022JP) {
        unsigned state = JIS_STATE_INITIAL;
        size_t used = jis_decode_step(s, n, enc, &state, &c);
        if (!used || c == JIS_NO_SCALAR) return 0;
        *out = c; return used;
    }
    c = s[0];
    if (c < 0x80) { *out = c; return 1; }
    if (enc == JIS_UTF8) {
        if (c >= 0xc2 && c <= 0xdf) { count = 2; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { count = 3; c &= 0xf; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 4; c &= 7; }
        else return 0;
        if (n < count) return 0;
        for (i = 1; i < count; ++i) {
            if ((s[i] & 0xc0) != 0x80) return 0;
            c = (c << 6) | (s[i] & 0x3f);
        }
        if ((count == 2 && c < 0x80) || (count == 3 && c < 0x800) ||
            (count == 4 && c < 0x10000) || c > 0x10ffff ||
            (c >= 0xd800 && c <= 0xdfff)) return 0;
        *out = c; return count;
    }
    if (enc == JIS_SHIFT_JIS) {
        unsigned lead;
        if (c >= 0xa1 && c <= 0xdf) { *out = 0xff61 + c - 0xa1; return 1; }
        /* CP932's documented single-byte private-use mappings. */
        if (c == 0x80) { *out = c; return 1; }
        if (c == 0xa0 || c >= 0xfd) {
            *out = c == 0xa0 ? 0xf8f0 : 0xf8f1 + c - 0xfd;
            return 1;
        }
        if (c >= 0x81 && c <= 0x9f) lead = c - 0x81;
        else if (c >= 0xe0 && c <= 0xfc) lead = 31 + c - 0xe0;
        else return 0;
        if (n < 2 || s[1] < 0x40 || s[1] > 0xfc) return 0;
        c = jis_cp932[lead * 189 + s[1] - 0x40];
        if (!c) return 0;
        *out = c; return 2;
    }
    if (enc != JIS_EUC_JP) return 0;
    if (c == 0x8e) {
        if (n < 2 || s[1] < 0xa1 || s[1] > 0xdf) return 0;
        *out = 0xff61 + s[1] - 0xa1; return 2;
    }
    if (c == 0x8f) {
        if (n < 3 || s[1] < 0xa1 || s[1] > 0xfe ||
            s[2] < 0xa1 || s[2] > 0xfe) return 0;
        c = jis0212[(s[1] - 0xa1) * 94 + s[2] - 0xa1];
        if (!c) return 0;
        *out = c; return 3;
    }
    if (c < 0xa1 || c > 0xfe || n < 2 || s[1] < 0xa1 || s[1] > 0xfe) return 0;
    c = jis0208[(c - 0xa1) * 94 + s[1] - 0xa1];
    if (!c) return 0;
    *out = c; return 2;
}

/* JIS X 0208 row/cell (0x21..0x7e each) <-> Shift_JIS bytes. */
static unsigned jis_to_sjis(unsigned j1, unsigned j2) {
    unsigned s1 = ((j1 + 1) >> 1) + (j1 <= 0x5e ? 0x70 : 0xb0);
    unsigned s2 = j2 + (j1 & 1 ? (j2 >= 0x60 ? 0x20 : 0x1f) : 0x7e);
    return s1 << 8 | s2;
}
static unsigned sjis_to_jis(unsigned code) {
    unsigned s1 = code >> 8, s2 = code & 0xff, row;
    if (s1 >= 0x81 && s1 <= 0x9f) row = (s1 - 0x81) * 2 + 0x21;
    else if (s1 >= 0xe0 && s1 <= 0xef) row = (s1 - 0xe0) * 2 + 0x5f;
    else return 0;
    if (s2 < 0x40 || s2 > 0xfc || s2 == 0x7f) return 0;
    if (s2 >= 0x9f) return (row + 1) << 8 | (s2 - 0x7e);
    return row << 8 | (s2 - (s2 >= 0x80 ? 0x20 : 0x1f));
}
static uint32_t cp932_at(unsigned sjis) {
    unsigned s1 = sjis >> 8, s2 = sjis & 0xff, lead;
    if (s1 >= 0x81 && s1 <= 0x9f) lead = s1 - 0x81;
    else if (s1 >= 0xe0 && s1 <= 0xfc) lead = 31 + s1 - 0xe0;
    else return 0;
    if (s2 < 0x40 || s2 > 0xfc) return 0;
    return jis_cp932[lead * 189 + s2 - 0x40];
}

enum { ST_ASCII, ST_ROMAN, ST_KANA, ST_X0208, ST_X0212 };

static size_t iso2022_step(const unsigned char *s, size_t n, unsigned *state,
                           uint32_t *out) {
    unsigned c = s[0];
    if (c == 0x1b) {
        static const struct { const char *seq; unsigned state; } escapes[] = {
            {"(B", ST_ASCII}, {"(J", ST_ROMAN}, {"(I", ST_KANA},
            {"$@", ST_X0208}, {"$B", ST_X0208}, {"$(D", ST_X0212},
            {"$(O", ST_X0208}, {"$(Q", ST_X0208}, {"&@", 99},
        };
        for (size_t i = 0; i < sizeof(escapes) / sizeof(escapes[0]); ++i) {
            size_t len = strlen(escapes[i].seq);
            if (n < len + 1 || memcmp(s + 1, escapes[i].seq, len)) continue;
            if (escapes[i].state != 99) *state = escapes[i].state;
            *out = JIS_NO_SCALAR; return len + 1;
        }
        return 0;
    }
    if (c == 0x0e) { *state = ST_KANA; *out = JIS_NO_SCALAR; return 1; }
    if (c == 0x0f) { *state = ST_ASCII; *out = JIS_NO_SCALAR; return 1; }
    if (c >= 0x80) return 0;
    if (c < 0x21 || c == 0x7f) { *out = c; return 1; }
    switch (*state) {
    case ST_ROMAN:
        *out = c == 0x5c ? 0xa5 : (c == 0x7e ? 0x203e : c); return 1;
    case ST_KANA:
        if (c > 0x5f) return 0;
        *out = 0xff61 + c - 0x21; return 1;
    case ST_X0208: case ST_X0212: {
        unsigned d;
        uint32_t code;
        if (n < 2) return 0;
        d = s[1];
        if (d < 0x21 || d > 0x7e) return 0;
        if (*state == ST_X0212) code = jis0212[(c - 0x21) * 94 + d - 0x21];
        else {
            code = jis0208[(c - 0x21) * 94 + d - 0x21];
            if (!code) code = cp932_at(jis_to_sjis(c, d));
        }
        if (!code) return 0;
        *out = code; return 2;
    }
    default:
        *out = c; return 1;
    }
}

size_t jis_decode_step(const unsigned char *s, size_t n, JisEncoding enc,
                       unsigned *state, uint32_t *out) {
    if (!n || !s || !out || !state || enc < JIS_UTF8 || enc >= JIS_ENCODING_COUNT) return 0;
    if (enc == JIS_ISO2022JP) {
        unsigned next = *state;
        size_t used = iso2022_step(s, n, &next, out);
        if (used) *state = next;
        return used;
    }
    if (enc == JIS_UTF16LE || enc == JIS_UTF16BE) {
        uint32_t unit, low;
        if (n < 2) return 0;
        unit = enc == JIS_UTF16LE ? (uint32_t)(s[0] | s[1] << 8) : (uint32_t)(s[0] << 8 | s[1]);
        if (unit >= 0xdc00 && unit <= 0xdfff) return 0;
        if (unit < 0xd800 || unit > 0xdbff) { *out = unit; return 2; }
        if (n < 4) return 0;
        low = enc == JIS_UTF16LE ? (uint32_t)(s[2] | s[3] << 8) : (uint32_t)(s[2] << 8 | s[3]);
        if (low < 0xdc00 || low > 0xdfff) return 0;
        *out = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
        return 4;
    }
    return jis_decode_one(s, n, enc, out);
}

size_t jis_bom_length(const unsigned char *s, size_t n, JisEncoding enc) {
    if (!s) return 0;
    if (enc == JIS_UTF8 && n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) return 3;
    if (enc == JIS_UTF16LE && n >= 2 && s[0] == 0xff && s[1] == 0xfe) return 2;
    if (enc == JIS_UTF16BE && n >= 2 && s[0] == 0xfe && s[1] == 0xff) return 2;
    return 0;
}

JisEncoding jis_detect_all(const unsigned char *s, size_t n) {
    static const char *const escapes[] = {"$@", "$B", "$(D", "(J", "(I"};
    int escape = 0;
    if (jis_bom_length(s, n, JIS_UTF8)) return JIS_UTF8;
    if (jis_bom_length(s, n, JIS_UTF16LE)) return JIS_UTF16LE;
    if (jis_bom_length(s, n, JIS_UTF16BE)) return JIS_UTF16BE;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] >= 0x80) { escape = 0; break; }
        if (s[i] != 0x1b || escape) continue;
        for (size_t e = 0; e < sizeof(escapes) / sizeof(escapes[0]); ++e) {
            size_t len = strlen(escapes[e]);
            if (n - i - 1 >= len && !memcmp(s + i + 1, escapes[e], len)) escape = 1;
        }
    }
    return escape ? JIS_ISO2022JP : jis_detect(s, n);
}

static size_t invalid_count(const unsigned char *s, size_t n, JisEncoding enc) {
    size_t pos = 0, invalid = 0;
    uint32_t c;
    while (pos < n) {
        size_t used = jis_decode_one(s + pos, n - pos, enc, &c);
        if (!used) { ++invalid; used = 1; }
        pos += used;
    }
    return invalid;
}

JisEncoding jis_detect(const unsigned char *s, size_t n) {
    if (!invalid_count(s, n, JIS_UTF8)) return JIS_UTF8;
    return invalid_count(s, n, JIS_EUC_JP) < invalid_count(s, n, JIS_SHIFT_JIS)
        ? JIS_EUC_JP : JIS_SHIFT_JIS;
}

static size_t utf8_encode(uint32_t c, unsigned char *out) {
    if (c < 0x80) { out[0] = (unsigned char)c; return 1; }
    if (c < 0x800) {
        out[0] = 0xc0 | (c >> 6); out[1] = 0x80 | (c & 63); return 2;
    }
    if (c < 0x10000) {
        out[0] = 0xe0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 63);
        out[2] = 0x80 | (c & 63); return 3;
    }
    out[0] = 0xf0 | (c >> 18); out[1] = 0x80 | ((c >> 12) & 63);
    out[2] = 0x80 | ((c >> 6) & 63); out[3] = 0x80 | (c & 63); return 4;
}

int jis_to_utf8(const unsigned char *s, size_t n, JisEncoding enc,
                int replace, char **out, size_t *out_n) {
    unsigned char *result;
    size_t pos = 0, len = 0;
    unsigned state = JIS_STATE_INITIAL;
    uint32_t c;
    if (!out || !out_n || (!s && n) || enc < JIS_UTF8 || enc >= JIS_ENCODING_COUNT) return -1;
    *out = NULL; *out_n = 0;
    if (n > (SIZE_MAX - 1) / 4) return -1;
    result = malloc(n * 4 + 1);
    if (!result) return -1;
    while (pos < n) {
        size_t used = jis_decode_step(s + pos, n - pos, enc, &state, &c);
        if (!used) {
            if (!replace) { free(result); return -1; }
            c = 0xfffd;
            used = (enc == JIS_UTF16LE || enc == JIS_UTF16BE) && n - pos >= 2 ? 2 : 1;
        }
        if (c != JIS_NO_SCALAR) len += utf8_encode(c, result + len);
        pos += used;
    }
    result[len] = 0; *out = (char *)result; *out_n = len; return 0;
}

static uint32_t legacy_code(uint32_t c, JisEncoding enc) {
    size_t low = 0, high = sizeof(jis_reverse) / sizeof(jis_reverse[0]);
    if (enc == JIS_SHIFT_JIS) {
        if (c == 0x80) return 0x80;
        if (c >= 0xf8f0 && c <= 0xf8f3) return c == 0xf8f0 ? 0xa0 : 0xfd + c - 0xf8f1;
    }
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (jis_reverse[mid].unicode < c) low = mid + 1;
        else high = mid;
    }
    if (low == sizeof(jis_reverse) / sizeof(jis_reverse[0]) || jis_reverse[low].unicode != c) return 0;
    return enc == JIS_SHIFT_JIS ? jis_reverse[low].sjis : jis_reverse[low].euc;
}

/* ISO-2022-JP: the set and two bytes (one for single-byte sets) for c. */
static int iso2022_code(uint32_t c, unsigned *set, unsigned *code) {
    uint32_t euc, sjis;
    if (c == 0x1b || c == 0x0e || c == 0x0f) return 0; /* would read back as a shift */
    if (c < 0x80) { *set = ST_ASCII; *code = c; return 1; }
    if (c == 0xa5 || c == 0x203e) { *set = ST_ROMAN; *code = c == 0xa5 ? 0x5c : 0x7e; return 1; }
    if (c >= 0xff61 && c <= 0xff9f) { *set = ST_KANA; *code = c - 0xff61 + 0x21; return 1; }
    euc = legacy_code(c, JIS_EUC_JP);
    if (euc >= 0xa1a1 && euc <= 0xfefe) { *set = ST_X0208; *code = euc - 0x8080; return 1; }
    if (euc >= 0x8fa1a1 && euc <= 0x8ffefe) { *set = ST_X0212; *code = (euc & 0xffff) - 0x8080; return 1; }
    sjis = legacy_code(c, JIS_SHIFT_JIS);
    if (sjis > 0xff && (*code = sjis_to_jis(sjis))) { *set = ST_X0208; return 1; }
    /* CP932 prefers IBM rows (F0..FC) that 7-bit JIS cannot hold; the NEC
     * selected copy (rows 89-92) of the same character can. */
    for (unsigned lead = 0; lead < 47; ++lead)
        for (unsigned trail = 0x40; trail <= 0xfc; ++trail)
            if (jis_cp932[lead * 189 + trail - 0x40] == c) {
                unsigned s1 = lead < 31 ? 0x81 + lead : 0xe0 + lead - 31;
                if ((*code = sjis_to_jis(s1 << 8 | trail))) { *set = ST_X0208; return 1; }
            }
    return 0;
}

int jis_from_utf8(const char *s, size_t n, JisEncoding enc,
                  unsigned char **out, size_t *out_n) {
    static const char *const designate[] = {"\x1b(B", "\x1b(J", "\x1b(I", "\x1b$B", "\x1b$(D"};
    unsigned char *result;
    size_t pos = 0, len = 0;
    unsigned state = ST_ASCII;
    uint32_t c;
    if (!out || !out_n || (!s && n) || enc < JIS_UTF8 || enc >= JIS_ENCODING_COUNT) return -1;
    *out = NULL; *out_n = 0;
    if (n > (SIZE_MAX - 8) / 4) return -1;
    /* Worst case ISO-2022-JP: one designation (<= 4 bytes) per input byte. */
    result = malloc(n * 4 + 8);
    if (!result) return -1;
    while (pos < n) {
        uint32_t code;
        size_t used = jis_decode_one((const unsigned char *)s + pos, n - pos, JIS_UTF8, &c);
        if (!used) { free(result); return -1; }
        pos += used;
        if (enc == JIS_UTF8) { len += utf8_encode(c, result + len); continue; }
        if (enc == JIS_UTF16LE || enc == JIS_UTF16BE) {
            uint32_t units[2] = {c, 0};
            int count = 1;
            if (c >= 0x10000) {
                units[0] = 0xd800 + ((c - 0x10000) >> 10);
                units[1] = 0xdc00 + ((c - 0x10000) & 0x3ff); count = 2;
            }
            for (int i = 0; i < count; ++i) {
                result[len++] = (unsigned char)(enc == JIS_UTF16LE ? units[i] : units[i] >> 8);
                result[len++] = (unsigned char)(enc == JIS_UTF16LE ? units[i] >> 8 : units[i]);
            }
            continue;
        }
        if (enc == JIS_ISO2022JP) {
            unsigned set, value;
            if (!iso2022_code(c, &set, &value)) { free(result); return -1; }
            if (set != state) {
                size_t dl = strlen(designate[set]);
                memcpy(result + len, designate[set], dl); len += dl; state = set;
            }
            if (set >= ST_X0208) result[len++] = (unsigned char)(value >> 8);
            result[len++] = (unsigned char)value;
            continue;
        }
        if (c < 0x80) { result[len++] = (unsigned char)c; continue; }
        code = legacy_code(c, enc);
        if (!code) { free(result); return -1; }
        if (code > 0xffff) result[len++] = (unsigned char)(code >> 16);
        if (code > 0xff) result[len++] = (unsigned char)(code >> 8);
        result[len++] = (unsigned char)code;
    }
    if (state != ST_ASCII) { memcpy(result + len, "\x1b(B", 3); len += 3; }
    result[len] = 0; *out = result; *out_n = len; return 0;
}
