// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "keys.h"
#include "ime.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Linux input-event-codes numeric values, with the jp106 symbol positions. */
int keys_evdev(int code, int mods, Key *out)
{
    static const struct { int code; char plain, shift; } chars[] = {
        {2,'1','!'}, {3,'2','"'}, {4,'3','#'}, {5,'4','$'}, {6,'5','%'},
        {7,'6','&'}, {8,'7','\''}, {9,'8','('}, {10,'9',')'}, {11,'0','0'},
        {12,'-','='}, {13,'^','~'}, {16,'q','Q'}, {17,'w','W'},
        {18,'e','E'}, {19,'r','R'}, {20,'t','T'}, {21,'y','Y'},
        {22,'u','U'}, {23,'i','I'}, {24,'o','O'}, {25,'p','P'},
        {26,'@','`'}, {27,'[','{'}, {30,'a','A'}, {31,'s','S'},
        {32,'d','D'}, {33,'f','F'}, {34,'g','G'}, {35,'h','H'},
        {36,'j','J'}, {37,'k','K'}, {38,'l','L'}, {39,';','+'},
        {40,':','*'}, {43,']','}'}, {44,'z','Z'}, {45,'x','X'},
        {46,'c','C'}, {47,'v','V'}, {48,'b','B'}, {49,'n','N'},
        {50,'m','M'}, {51,',','<'}, {52,'.','>'}, {53,'/','?'},
        {57,' ',' '}, {89,'\\','_'},
        {55,'*','*'}, {74,'-','-'}, {78,'+','+'}, {98,'/','/'},
        {71,'7','7'}, {72,'8','8'}, {73,'9','9'}, {75,'4','4'},
        {76,'5','5'}, {77,'6','6'}, {79,'1','1'}, {80,'2','2'},
        {81,'3','3'}, {82,'0','0'}, {83,'.','.'}
    };
    static const struct { int code, key; } special[] = {
        {1,KESC},{14,KBS},{15,KTAB},{28,KRET},{96,KRET},
        {102,KHOME},{103,KUP},{104,KPGUP},{105,KLEFT},{106,KRIGHT},
        {107,KEND},{108,KDOWN},{109,KPGDN},{110,KINS},{111,KDEL}
    };
    if (!out) return 0;
    mods &= MOD_SHIFT | MOD_CTRL | MOD_ALT;
    /* jp106 半角/全角: KEY_GRAVE (41) on the JIS layout, KEY_ZENKAKUHANKAKU (85).
     * It switches the input method (ime.h), like Alt+Space. */
    if (code == 41 || code == 85) {
        *out = (Key){KCHAR, IME_CH_ZENKAKU_HANKAKU, mods};
        return 1;
    }
    if (code == 124) {
        *out = (Key){KCHAR, mods & MOD_SHIFT ? '|' : 0x00a5, mods};
        return 1;
    }
    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); ++i) {
        if (chars[i].code == code) {
            *out = (Key){KCHAR, (unsigned char)((mods & MOD_SHIFT) ? chars[i].shift : chars[i].plain), mods};
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); ++i) {
        if (special[i].code == code) {
            *out = (Key){special[i].key, 0, mods};
            return 1;
        }
    }
    if ((code >= 59 && code <= 68) || code == 87 || code == 88) {
        *out = (Key){code >= 87 ? KF11 + code - 87 : KF1 + code - 59, 0, mods};
        return 1;
    }
    return 0;
}

/* Strict UTF-8 decoding: never advance over a malformed sequence. */
static int decode(const char *p, uint32_t *ch, size_t *length)
{
    const unsigned char *s = (const unsigned char *)p;
    uint32_t value;
    size_t n;
    if (!s[0]) return 0;
    if (s[0] < 0x80) { *ch = s[0]; *length = 1; return 1; }
    if (s[0] >= 0xc2 && s[0] <= 0xdf) { n = 2; value = s[0] & 31; }
    else if (s[0] >= 0xe0 && s[0] <= 0xef) { n = 3; value = s[0] & 15; }
    else if (s[0] >= 0xf0 && s[0] <= 0xf4) { n = 4; value = s[0] & 7; }
    else return 0;
    for (size_t i = 1; i < n; ++i) {
        if (!s[i] || (s[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (s[i] & 63);
    }
    if ((n == 3 && value < 0x800) || (n == 4 && value < 0x10000) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    *ch = value;
    *length = n;
    return 1;
}

int keys_parse(const char *token, Key *out)
{
    static const struct { const char *name; int key; } names[] = {
        {"UP",KUP},{"DOWN",KDOWN},{"LEFT",KLEFT},{"RIGHT",KRIGHT},
        {"PGUP",KPGUP},{"PGDN",KPGDN},{"HOME",KHOME},{"END",KEND},
        {"INS",KINS},{"DEL",KDEL},{"BS",KBS},{"RET",KRET},
        {"TAB",KTAB},{"ESC",KESC}
    };
    int mods = 0;
    if (!token || !out) return 0;
    while ((token[0] == 'C' || token[0] == 'M' || token[0] == 'S') && token[1] == '-') {
        int flag = token[0] == 'C' ? MOD_CTRL : token[0] == 'M' ? MOD_ALT : MOD_SHIFT;
        if (mods & flag) return 0;
        mods |= flag;
        token += 2;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(token, names[i].name)) {
            *out = (Key){names[i].key, 0, mods};
            return 1;
        }
    }
    if (token[0] == 'F' && token[1] >= '1' && token[1] <= '9') {
        char *end;
        long f = strtol(token + 1, &end, 10);
        if (!*end && f >= 1 && f <= 12) { *out = (Key){KF1 + (int)f - 1, 0, mods}; return 1; }
    }
    if (!strcmp(token, "SPC")) { *out = (Key){KCHAR, ' ', mods}; return 1; }
    uint32_t ch;
    size_t length;
    if (!decode(token, &ch, &length) || token[length]) return 0;
    if (ch >= 'A' && ch <= 'Z') mods |= MOD_SHIFT;
    if ((mods & MOD_SHIFT) && ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    *out = (Key){KCHAR, ch, mods};
    return 1;
}

int keys_equal(const Key *a, const Key *b)
{
    if (!a || !b || a->key != b->key) return 0;
    int mask = a->key == KCHAR ? MOD_CTRL | MOD_ALT : MOD_SHIFT | MOD_CTRL | MOD_ALT;
    return (a->key != KCHAR || a->ch == b->ch) && (a->mods & mask) == (b->mods & mask);
}

void keys_script_init(KeyScript *s, const char *text)
{
    memset(s, 0, sizeof(*s));
    s->text = text ? text : "";
}
static ScriptEvent failure(KeyScript *s, const char *message)
{
    snprintf(s->error, sizeof(s->error), "%s (byte %zu)", message, s->pos);
    return (ScriptEvent){.kind = SCRIPT_ERROR};
}
ScriptEvent keys_script_next(KeyScript *s)
{
    ScriptEvent event = {.kind = SCRIPT_END};
    if (s->error[0]) return (ScriptEvent){.kind = SCRIPT_ERROR};
    for (;;) {
        if (s->quoted) {
            const char *p = s->text + s->pos;
            if (!*p) return failure(s, "Unterminated quoted string");
            if (*p == '"') {
                s->pos++;
                s->quoted = 0;
                if (s->text[s->pos] && !isspace((unsigned char)s->text[s->pos]) && s->text[s->pos] != '#')
                    return failure(s, "Expected whitespace after quoted string");
                continue;
            }
            uint32_t ch;
            size_t length;
            if (*p == '\\') {
                s->pos++;
                char escaped = s->text[s->pos];
                if (!escaped) return failure(s, "Incomplete escape");
                switch (escaped) {
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                case 'n': case 'r': ch = '\n'; break;
                case 't': ch = '\t'; break;
                default: return failure(s, "Unknown escape");
                }
                s->pos++;
            } else {
                if (!decode(p, &ch, &length)) return failure(s, "Invalid UTF-8");
                s->pos += length;
            }
            event.kind = SCRIPT_KEY;
            event.key = (Key){ch == '\n' || ch == '\r' ? KRET : ch == '\t' ? KTAB : KCHAR, 0, 0};
            if (event.key.key == KCHAR) {
                event.key.ch = ch;
                if (ch >= 'A' && ch <= 'Z') event.key.mods = MOD_SHIFT;
            }
            return event;
        }
        while (isspace((unsigned char)s->text[s->pos])) s->pos++;
        if (s->text[s->pos] == '#') {
            while (s->text[s->pos] && s->text[s->pos] != '\n') s->pos++;
            continue;
        }
        if (!s->text[s->pos]) return event;
        if (s->text[s->pos] == '"') { s->quoted = 1; s->pos++; continue; }
        size_t start = s->pos;
        while (s->text[s->pos] && !isspace((unsigned char)s->text[s->pos])) s->pos++;
        size_t length = s->pos - start;
        char token[128];
        if (length >= sizeof(token)) return failure(s, "Token too long");
        memcpy(token, s->text + start, length);
        token[length] = 0;
        if (!strcmp(token, "SNAP")) return (ScriptEvent){.kind = SCRIPT_SNAP};
        if (!strcmp(token, "HOLD-SHIFT") || !strcmp(token, "RELEASE-SHIFT")) {
            if (token[0] == 'H') s->mods |= MOD_SHIFT; else s->mods &= ~MOD_SHIFT;
            return (ScriptEvent){.kind = SCRIPT_MODS, .mods = s->mods};
        }
        if (!strncmp(token, "WAIT:", 5)) {
            char *end;
            errno = 0;
            double value = strtod(token + 5, &end);
            if (!token[5] || *end || errno || !isfinite(value) || value < 0)
                return failure(s, "Invalid WAIT duration");
            return (ScriptEvent){.kind = SCRIPT_WAIT, .seconds = value};
        }
        if (!strncmp(token, "DISPLAY:", 8)) {
            char *end;
            long w = strtol(token + 8, &end, 10), h = 0;
            if (end != token + 8 && (*end == 'x' || *end == 'X')) {
                const char *r = end + 1;
                h = strtol(r, &end, 10);
                if (end == r) h = 0;
            }
            if (*end || w < 1 || h < 1 || w > 16384 || h > 16384) return failure(s, "Invalid DISPLAY size");
            return (ScriptEvent){.kind = SCRIPT_DISPLAY, .width = (int)w, .height = (int)h};
        }
        if (!keys_parse(token, &event.key)) return failure(s, "Unknown key token");
        event.kind = SCRIPT_KEY;
        return event;
    }
}
