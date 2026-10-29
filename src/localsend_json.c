// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "localsend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>

#define JSON_LIMIT (1024U * 1024U)
#define JSON_NODES 8192U
#define JSON_DEPTH 32U

void ls_buf_add(LsBuf *b, const char *fmt, ...) {
    if (b->failed) return;
    va_list ap, cp;
    va_start(ap, fmt); va_copy(cp, ap);
    int needed = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (needed < 0 || (size_t)needed > SIZE_MAX - b->len - 1) {
        b->failed = 1; va_end(ap); return;
    }
    size_t want = b->len + (size_t)needed + 1;
    if (want > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < want) {
            if (cap > SIZE_MAX / 2) { cap = want; break; }
            cap *= 2;
        }
        char *s = realloc(b->s, cap);
        if (!s) { b->failed = 1; va_end(ap); return; }
        b->s = s; b->cap = cap;
    }
    vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
    va_end(ap); b->len += (size_t)needed;
}
void ls_buf_quote(LsBuf *b, const char *s) {
    ls_buf_add(b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': ls_buf_add(b, "\\\""); break;
        case '\\': ls_buf_add(b, "\\\\"); break;
        default:
            if (*p < 32) ls_buf_add(b, "\\u%04x", *p);
            else ls_buf_add(b, "%c", *p);
        }
    }
    ls_buf_add(b, "\"");
}
void ls_buf_free(LsBuf *b) { free(b->s); memset(b, 0, sizeof(*b)); }
void ls_json_free(LsJson *j) {
    while (j) {
        LsJson *next = j->next;
        ls_json_free(j->child); free(j->key); free(j->text); free(j); j = next;
    }
}
typedef struct { const unsigned char *p; unsigned nodes; } Parser;
static void space(Parser *p) {
    while (*p->p == ' ' || *p->p == '\n' || *p->p == '\r' || *p->p == '\t') p->p++;
}
static int hex4(Parser *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned c = *p->p;
        if (!c) return 0;
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return 0;
        *v = (*v << 4) | d; p->p++;
    }
    return 1;
}
static char *string(Parser *p) {
    if (*p->p++ != '"') return NULL;
    LsBuf b = {0};
    while (*p->p && *p->p != '"') {
        unsigned c = *p->p++;
        if (c < 32) goto fail;
        if (c == '\\') {
            c = *p->p++;
            if (!c) goto fail;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned u;
                if (!hex4(p, &u) || !u) goto fail;
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (p->p[0] != '\\' || p->p[1] != 'u') goto fail;
                    p->p += 2;
                    unsigned low;
                    if (!hex4(p, &low) || low < 0xdc00 || low > 0xdfff) goto fail;
                    u = 0x10000 + ((u - 0xd800) << 10) + low - 0xdc00;
                } else if (u >= 0xdc00 && u <= 0xdfff) goto fail;
                if (u < 0x80) ls_buf_add(&b, "%c", u);
                else if (u < 0x800) ls_buf_add(&b, "%c%c", 0xc0 | (u >> 6), 0x80 | (u & 63));
                else if (u < 0x10000) ls_buf_add(&b, "%c%c%c", 0xe0 | (u >> 12), 0x80 | ((u >> 6) & 63), 0x80 | (u & 63));
                else ls_buf_add(&b, "%c%c%c%c", 0xf0 | (u >> 18), 0x80 | ((u >> 12) & 63), 0x80 | ((u >> 6) & 63), 0x80 | (u & 63));
                continue;
            }
            default: goto fail;
            }
        } else if (c >= 0x80) {
            unsigned u, min, count;
            if (c >= 0xc2 && c <= 0xdf) { u = c & 31; count = 1; min = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { u = c & 15; count = 2; min = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { u = c & 7; count = 3; min = 0x10000; }
            else goto fail;
            ls_buf_add(&b, "%c", c);
            for (unsigned i = 0; i < count; i++) {
                unsigned d = *p->p;
                if (d < 0x80 || d > 0xbf) goto fail;
                p->p++; u = (u << 6) | (d & 63); ls_buf_add(&b, "%c", d);
            }
            if (u < min || u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff)) goto fail;
            continue;
        }
        ls_buf_add(&b, "%c", c);
    }
    if (*p->p != '"' || b.failed) goto fail;
    p->p++;
    if (!b.s) return strdup("");
    return b.s;
fail:
    ls_buf_free(&b); return NULL;
}
static LsJson *value(Parser *p, unsigned depth) {
    if (depth > JSON_DEPTH || ++p->nodes > JSON_NODES) return NULL;
    space(p);
    LsJson *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    unsigned c = *p->p;
    if (c == '"') { j->kind = 's'; j->text = string(p); if (!j->text) goto fail; }
    else if (c == '{' || c == '[') {
        j->kind = c == '{' ? 'o' : 'a'; unsigned close = c == '{' ? '}' : ']';
        p->p++; space(p); LsJson **tail = &j->child;
        if (*p->p != close) for (;;) {
            char *key = NULL;
            if (j->kind == 'o') {
                if (*p->p != '"' || !(key = string(p))) goto fail;
                for (LsJson *q = j->child; q; q = q->next)
                    if (!strcmp(q->key, key)) { free(key); goto fail; }
                space(p); if (*p->p != ':') { free(key); goto fail; } p->p++;
            }
            LsJson *item = value(p, depth + 1);
            if (!item) { free(key); goto fail; }
            item->key = key; *tail = item; tail = &item->next;
            space(p);
            if (*p->p != ',') break;
            p->p++; space(p);
        }
        if (*p->p != close) goto fail;
        p->p++;
    } else if (c == 't' && !strncmp((const char *)p->p, "true", 4)) { j->kind = 't'; p->p += 4; }
    else if (c == 'f' && !strncmp((const char *)p->p, "false", 5)) { j->kind = 'f'; p->p += 5; }
    else if (c == 'n' && !strncmp((const char *)p->p, "null", 4)) { j->kind = '0'; p->p += 4; }
    else {
        const unsigned char *start = p->p;
        if (*p->p == '-') p->p++;
        if (*p->p == '0') p->p++;
        else if (*p->p >= '1' && *p->p <= '9') { do { p->p++; } while (isdigit(*p->p)); }
        else goto fail;
        if (*p->p == '.') {
            p->p++; if (!isdigit(*p->p)) goto fail;
            do { p->p++; } while (isdigit(*p->p));
        }
        if (*p->p == 'e' || *p->p == 'E') {
            p->p++; if (*p->p == '+' || *p->p == '-') p->p++;
            if (!isdigit(*p->p)) goto fail;
            do { p->p++; } while (isdigit(*p->p));
        }
        j->kind = 'n'; j->text = strndup((const char *)start, (size_t)(p->p - start));
        if (!j->text) goto fail;
    }
    return j;
fail: ls_json_free(j); return NULL;
}
LsJson *ls_json_parse(const char *s) {
    if (!s || strnlen(s, JSON_LIMIT + 1) > JSON_LIMIT) return NULL;
    Parser p = {(const unsigned char *)s, 0};
    LsJson *j = value(&p, 0); space(&p);
    if (*p.p) { ls_json_free(j); return NULL; }
    return j;
}
LsJson *ls_json_get(const LsJson *j, const char *key) {
    if (!j || j->kind != 'o') return NULL;
    for (LsJson *v = j->child; v; v = v->next) if (!strcmp(v->key, key)) return v;
    return NULL;
}
const char *ls_json_str(const LsJson *j, const char *key) {
    LsJson *v = ls_json_get(j, key); return v && v->kind == 's' ? v->text : NULL;
}
int ls_json_u64(const LsJson *j, const char *key, uint64_t *out) {
    LsJson *v = ls_json_get(j, key);
    if (!v || v->kind != 'n' || !v->text || !*v->text) return -1;
    uint64_t n = 0;
    for (const unsigned char *s = (const unsigned char *)v->text; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (*s - '0')) / 10) return -1;
        n = n * 10 + (*s - '0');
    }
    *out = n; return 0;
}
