// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _XOPEN_SOURCE 700
#include "novel_doc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int npos_cmp(NPos a, NPos b)
{
    if (a.line != b.line) return a.line < b.line ? -1 : 1;
    return a.off < b.off ? -1 : a.off > b.off;
}

static int reserve(NLine *l, size_t need)
{
    if (need + 1 <= l->cap) return 0;
    size_t cap = l->cap ? l->cap : 16;
    while (cap < need + 1) cap = cap * 2;
    char *p = realloc(l->s, cap);
    if (!p) return -1;
    l->s = p; l->cap = cap;
    return 0;
}
static int reserve_lines(NovelDoc *d, size_t need)
{
    if (need <= d->cap) return 0;
    size_t cap = d->cap ? d->cap : 64;
    while (cap < need) cap *= 2;
    NLine *p = realloc(d->lines, cap * sizeof(*p));
    if (!p) return -1;
    d->lines = p; d->cap = cap;
    return 0;
}
static void free_history(NovelDoc *d, size_t from)
{
    for (size_t i = from; i < d->undo_total; ++i) free(d->undo[i].text);
    d->undo_total = from;
    if (d->undo_n > from) d->undo_n = from;
}
void novel_doc_init(NovelDoc *d)
{
    memset(d, 0, sizeof(*d));
    strcpy(d->newline, "\n");
    d->encoding = JIS_UTF8;
}
static void free_lines(NovelDoc *d)
{
    for (size_t i = 0; i < d->count; ++i) free(d->lines[i].s);
    free(d->lines);
    d->lines = NULL; d->count = d->cap = 0;
}
void novel_doc_free(NovelDoc *d)
{
    free_lines(d);
    free_history(d, 0);
    free(d->undo);
    d->undo = NULL; d->undo_cap = 0;
}
static int push_line(NovelDoc *d, const char *s, size_t n)
{
    if (reserve_lines(d, d->count + 1)) return -1;
    NLine l = {0};
    if (reserve(&l, n)) return -1;
    memcpy(l.s, s, n); l.s[n] = 0; l.len = n;
    d->lines[d->count++] = l;
    return 0;
}
int novel_doc_set_text(NovelDoc *d, const char *text, size_t len)
{
    NovelDoc next;
    novel_doc_init(&next);
    next.encoding = d->encoding; next.bom = d->bom; next.lossy = d->lossy;
    int seen = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i < len && text[i] != '\n' && text[i] != '\r') continue;
        if (push_line(&next, text + start, i - start)) { novel_doc_free(&next); return -1; }
        if (i == len) break;
        int crlf = text[i] == '\r' && i + 1 < len && text[i + 1] == '\n';
        if (!seen) { strcpy(next.newline, crlf ? "\r\n" : text[i] == '\r' ? "\r" : "\n"); seen = 1; }
        if (crlf) ++i;
        start = i + 1;
    }
    novel_doc_free(d);
    *d = next;
    d->version = 1;
    return 0;
}
/* Kana and kanji count up, half-width katakana and invalid bytes down; -1
 * when the bytes are not valid in that encoding at all. */
static long japanese_score(const unsigned char *bytes, size_t size, JisEncoding enc)
{
    long score = 0;
    for (size_t at = 0; at < size;) {
        uint32_t c;
        size_t used = jis_decode_one(bytes + at, size - at, enc, &c);
        if (!used) return -1;
        if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x4e00 && c <= 0x9fff)) score += 2;
        else if (c >= 0xff61 && c <= 0xff9f) score -= 1;
        at += used;
    }
    return score;
}
int novel_doc_load(NovelDoc *d, const char *path, size_t max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISREG(st.st_mode)) { close(fd); errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL; return -1; }
    if ((unsigned long long)st.st_size > max) { close(fd); errno = EFBIG; return -1; }
    size_t size = (size_t)st.st_size, got = 0;
    unsigned char *bytes = malloc(size ? size : 1);
    if (!bytes) { close(fd); return -1; }
    while (got < size) {
        ssize_t n = read(fd, bytes + got, size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != size) { free(bytes); errno = EIO; return -1; }
    size_t skip = 0;
    int bom = size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
    if (bom) skip = 3;
    JisEncoding enc = bom ? JIS_UTF8 : jis_detect(bytes, size);
    if (enc == JIS_SHIFT_JIS) {
        /* Short EUC-JP text is often valid Shift_JIS too (jis_detect prefers
         * Shift_JIS on ties). Take the reading with more kana and kanji. */
        long sjis = japanese_score(bytes, size, JIS_SHIFT_JIS), euc = japanese_score(bytes, size, JIS_EUC_JP);
        if (euc > sjis) enc = JIS_EUC_JP;
    }
    char *utf8 = NULL; size_t n = 0; int lossy = 0;
    if (jis_to_utf8(bytes + skip, size - skip, enc, 0, &utf8, &n)) {
        lossy = 1;
        if (jis_to_utf8(bytes + skip, size - skip, enc, 1, &utf8, &n)) { free(bytes); errno = ENOMEM; return -1; }
    }
    /* Treat decoded NUL as binary (UTF-16's ordinary zero bytes are fine).
     * Successful decoding can also normalize a legacy mapping, so check
     * whether re-encoding reproduces the original byte stream. */
    if (memchr(utf8, 0, n)) lossy = 1;
    unsigned char *roundtrip = NULL; size_t roundtrip_n = 0;
    if (jis_from_utf8(utf8, n, enc, &roundtrip, &roundtrip_n) ||
        roundtrip_n != size - skip || memcmp(roundtrip, bytes + skip, roundtrip_n))
        lossy = 1;
    free(roundtrip);
    free(bytes);
    d->encoding = enc; d->bom = bom; d->lossy = lossy;
    int rc = novel_doc_set_text(d, utf8, n);
    free(utf8);
    if (rc) { errno = ENOMEM; return -1; }
    d->encoding = enc; d->bom = bom; d->lossy = lossy;
    return 0;
}
char *novel_doc_text(const NovelDoc *d, size_t *len)
{
    size_t nl = strlen(d->newline), total = d->bom ? 3 : 0;
    for (size_t i = 0; i < d->count; ++i) total += d->lines[i].len + (i + 1 < d->count ? nl : 0);
    char *out = malloc(total + 1), *p = out;
    if (!out) return NULL;
    if (d->bom) { memcpy(p, "\xef\xbb\xbf", 3); p += 3; }
    for (size_t i = 0; i < d->count; ++i) {
        memcpy(p, d->lines[i].s, d->lines[i].len); p += d->lines[i].len;
        if (i + 1 < d->count) { memcpy(p, d->newline, nl); p += nl; }
    }
    *p = 0;
    *len = total;
    return out;
}
static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
int novel_doc_save(NovelDoc *d, const char *path, int utf8)
{
    size_t len;
    char *text = novel_doc_text(d, &len);
    if (!text) return -1;
    unsigned char *bytes = (unsigned char *)text; size_t n = len;
    unsigned char *converted = NULL;
    JisEncoding enc = utf8 ? JIS_UTF8 : d->encoding;
    if (enc != JIS_UTF8) {
        if (jis_from_utf8(text, len, enc, &converted, &n)) { free(text); return -2; }
        bytes = converted;
    }
    /* Write a sibling temporary and rename it over the file, so a failed save
     * never truncates the original. Without a writable directory, overwrite. */
    struct stat st; int have = !stat(path, &st);
    /* Replacing through rename would also overwrite a read-only file. */
    if (have && access(path, W_OK)) { free(converted); free(text); return -1; }
    size_t pl = strlen(path);
    char *tmp = malloc(pl + 16);
    int rc = -1, fd = -1;
    if (tmp) {
        const char *slash = strrchr(path, '/');
        size_t dir = slash ? (size_t)(slash - path + 1) : 0;
        memcpy(tmp, path, dir);
        snprintf(tmp + dir, pl + 16 - dir, ".fdtpXXXXXX");
        fd = mkstemp(tmp);
    }
    if (fd >= 0) {
        if (have && fchmod(fd, st.st_mode & 07777)) { /* keep the default mode */ }
        rc = write_all(fd, bytes, n);
        if (!rc) rc = fsync(fd);
        if (close(fd)) rc = -1;
        if (!rc) rc = rename(tmp, path);
        if (rc) { int e = errno; unlink(tmp); errno = e; }
    } else {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            rc = write_all(fd, bytes, n);
            if (close(fd)) rc = -1;
        }
    }
    free(tmp); free(converted); free(text);
    if (rc) return -1;
    if (utf8) { d->encoding = JIS_UTF8; }
    novel_doc_mark_clean(d);
    return 0;
}
NPos novel_doc_end(const NovelDoc *d)
{
    return (NPos){d->count - 1, d->lines[d->count - 1].len};
}
NPos novel_doc_clamp(const NovelDoc *d, NPos p)
{
    if (p.line >= d->count) return novel_doc_end(d);
    if (p.off > d->lines[p.line].len) p.off = d->lines[p.line].len;
    while (p.off && ((unsigned char)d->lines[p.line].s[p.off] & 0xc0) == 0x80) --p.off;
    return p;
}
char *novel_doc_copy(const NovelDoc *d, NPos a, NPos b, size_t *len)
{
    if (npos_cmp(a, b) > 0) { NPos t = a; a = b; b = t; }
    size_t total = 0;
    for (size_t i = a.line; i <= b.line; ++i) {
        size_t from = i == a.line ? a.off : 0, to = i == b.line ? b.off : d->lines[i].len;
        total += to - from + (i < b.line);
    }
    char *out = malloc(total + 1), *p = out;
    if (!out) return NULL;
    for (size_t i = a.line; i <= b.line; ++i) {
        size_t from = i == a.line ? a.off : 0, to = i == b.line ? b.off : d->lines[i].len;
        memcpy(p, d->lines[i].s + from, to - from); p += to - from;
        if (i < b.line) *p++ = '\n';
    }
    *p = 0;
    *len = total;
    return out;
}

/* Raw edits without history. */
static int raw_insert(NovelDoc *d, NPos at, const char *text, size_t len, NPos *end)
{
    size_t breaks = 0;
    for (size_t i = 0; i < len; ++i) breaks += text[i] == '\n';
    NLine *line = &d->lines[at.line];
    if (!breaks) {
        if (reserve(line, line->len + len)) return -1;
        memmove(line->s + at.off + len, line->s + at.off, line->len - at.off + 1);
        memcpy(line->s + at.off, text, len);
        line->len += len;
        *end = (NPos){at.line, at.off + len};
        return 0;
    }
    if (reserve_lines(d, d->count + breaks)) return -1;
    /* Build the new lines first so a failure leaves the document intact. */
    NLine *made = calloc(breaks, sizeof(*made));
    if (!made) return -1;
    line = &d->lines[at.line];
    size_t tail = line->len - at.off, seg = 0, start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i < len && text[i] != '\n') continue;
        if (seg) {
            NLine *l = &made[seg - 1];
            size_t n = i - start, extra = seg == breaks ? tail : 0;
            if (reserve(l, n + extra)) {
                for (size_t k = 0; k < breaks; ++k) free(made[k].s);
                free(made); return -1;
            }
            memcpy(l->s, text + start, n);
            if (extra) memcpy(l->s + n, line->s + at.off, extra);
            l->len = n + extra; l->s[l->len] = 0;
        }
        ++seg; start = i + 1;
    }
    size_t first = 0;
    while (first < len && text[first] != '\n') ++first;
    if (reserve(line, at.off + first)) {
        for (size_t k = 0; k < breaks; ++k) free(made[k].s);
        free(made); return -1;
    }
    memcpy(line->s + at.off, text, first);
    line->len = at.off + first; line->s[line->len] = 0;
    memmove(d->lines + at.line + 1 + breaks, d->lines + at.line + 1, (d->count - at.line - 1) * sizeof(NLine));
    memcpy(d->lines + at.line + 1, made, breaks * sizeof(NLine));
    d->count += breaks;
    free(made);
    *end = (NPos){at.line + breaks, d->lines[at.line + breaks].len - tail};
    return 0;
}
static int raw_delete(NovelDoc *d, NPos a, NPos b)
{
    NLine *first = &d->lines[a.line];
    if (a.line == b.line) {
        memmove(first->s + a.off, first->s + b.off, first->len - b.off + 1);
        first->len -= b.off - a.off;
        return 0;
    }
    NLine *last = &d->lines[b.line];
    size_t keep = last->len - b.off;
    if (reserve(first, a.off + keep)) return -1;
    memcpy(first->s + a.off, last->s + b.off, keep);
    first->len = a.off + keep; first->s[first->len] = 0;
    for (size_t i = a.line + 1; i <= b.line; ++i) free(d->lines[i].s);
    memmove(d->lines + a.line + 1, d->lines + b.line + 1, (d->count - b.line - 1) * sizeof(NLine));
    d->count -= b.line - a.line;
    return 0;
}
static int record(NovelDoc *d, int insert, NPos at, const char *text, size_t len)
{
    free_history(d, d->undo_n);
    if (d->undo_n == d->undo_cap) {
        size_t cap = d->undo_cap ? d->undo_cap * 2 : 64;
        NUndo *p = realloc(d->undo, cap * sizeof(*p));
        if (!p) return -1;
        d->undo = p; d->undo_cap = cap;
    }
    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, text, len); copy[len] = 0;
    if (!d->group) d->group = 1;
    d->undo[d->undo_n] = (NUndo){insert, at, copy, len, d->group, ++d->seq};
    d->undo_total = ++d->undo_n;
    return 0;
}
void novel_doc_group(NovelDoc *d) { ++d->group; }
void novel_doc_join(NovelDoc *d, size_t from)
{
    for (size_t i = from + 1; i < d->undo_n; ++i) d->undo[i].group = d->undo[from].group;
}
NPos novel_doc_insert(NovelDoc *d, NPos at, const char *text, size_t len, int *ok)
{
    NPos end = at;
    if (ok) *ok = 1;
    if (!len) return at;
    if (record(d, 1, at, text, len)) { if (ok) *ok = 0; return at; }
    if (raw_insert(d, at, text, len, &end)) {
        free(d->undo[--d->undo_n].text); d->undo_total = d->undo_n;
        if (ok) *ok = 0;
        return at;
    }
    ++d->version;
    return end;
}
NPos novel_doc_delete(NovelDoc *d, NPos a, NPos b, int *ok)
{
    if (ok) *ok = 1;
    if (npos_cmp(a, b) > 0) { NPos t = a; a = b; b = t; }
    if (!npos_cmp(a, b)) return a;
    size_t len;
    char *text = novel_doc_copy(d, a, b, &len);
    if (!text || record(d, 0, a, text, len)) { free(text); if (ok) *ok = 0; return a; }
    free(text);
    if (raw_delete(d, a, b)) {
        free(d->undo[--d->undo_n].text); d->undo_total = d->undo_n;
        if (ok) *ok = 0;
        return a;
    }
    ++d->version;
    return a;
}
static NPos end_of(NPos at, const char *text, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') { ++at.line; at.off = 0; }
        else ++at.off;
    }
    return at;
}
int novel_doc_undo(NovelDoc *d, NPos *cursor)
{
    if (!d->undo_n) return 0;
    unsigned long group = d->undo[d->undo_n - 1].group;
    while (d->undo_n && d->undo[d->undo_n - 1].group == group) {
        NUndo *u = &d->undo[d->undo_n - 1];
        NPos end;
        if (u->insert) {
            if (raw_delete(d, u->at, end_of(u->at, u->text, u->len))) break;
            *cursor = u->at;
        } else {
            if (raw_insert(d, u->at, u->text, u->len, &end)) break;
            *cursor = end;
        }
        --d->undo_n;
    }
    ++d->version;
    novel_doc_group(d);
    return 1;
}
int novel_doc_redo(NovelDoc *d, NPos *cursor)
{
    if (d->undo_n == d->undo_total) return 0;
    unsigned long group = d->undo[d->undo_n].group;
    while (d->undo_n < d->undo_total && d->undo[d->undo_n].group == group) {
        NUndo *u = &d->undo[d->undo_n];
        NPos end;
        if (u->insert) {
            if (raw_insert(d, u->at, u->text, u->len, &end)) break;
            *cursor = end;
        } else {
            if (raw_delete(d, u->at, end_of(u->at, u->text, u->len))) break;
            *cursor = u->at;
        }
        ++d->undo_n;
    }
    ++d->version;
    novel_doc_group(d);
    return 1;
}
static unsigned long state(const NovelDoc *d) { return d->undo_n ? d->undo[d->undo_n - 1].seq : 0; }
int novel_doc_modified(const NovelDoc *d) { return state(d) != d->clean; }
void novel_doc_mark_clean(NovelDoc *d) { d->clean = state(d); }
void novel_doc_mark_dirty(NovelDoc *d) { d->clean = (unsigned long)-1; }

size_t novel_next(const char *s, size_t len, size_t off)
{
    if (off >= len) return len;
    ++off;
    while (off < len && ((unsigned char)s[off] & 0xc0) == 0x80) ++off;
    return off;
}
size_t novel_prev(const char *s, size_t off)
{
    if (!off) return 0;
    --off;
    while (off && ((unsigned char)s[off] & 0xc0) == 0x80) --off;
    return off;
}
uint32_t novel_decode(const char *s, size_t len, size_t off, size_t *next)
{
    const unsigned char *p = (const unsigned char *)s + off;
    size_t n = len - off;
    uint32_t c = p[0];
    size_t count = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (count > n) count = 1;
    if (count == 2) c &= 0x1f;
    else if (count == 3) c &= 0x0f;
    else if (count == 4) c &= 0x07;
    for (size_t i = 1; i < count; ++i) {
        if ((p[i] & 0xc0) != 0x80) { count = 1; c = 0xfffd; break; }
        c = (c << 6) | (p[i] & 0x3f);
    }
    if (count == 1 && p[0] >= 0x80) c = 0xfffd;
    if (next) *next = off + count;
    return c;
}
size_t novel_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63));
    return 4;
}
static const char *find_in(const char *s, size_t len, size_t from, const char *pat, size_t n)
{
    if (n > len || from > len - n) return NULL;
    for (size_t i = from; i + n <= len; ++i) {
        const char *hit = memchr(s + i, pat[0], len - n + 1 - i);
        if (!hit) return NULL;
        i = (size_t)(hit - s);
        if (!memcmp(hit, pat, n)) return hit;
    }
    return NULL;
}
int novel_doc_find(const NovelDoc *d, NPos p, const char *pattern, size_t n,
                   NPos *start, NPos *end, int *wrapped)
{
    if (!n) return 0;
    if (wrapped) *wrapped = 0;
    /* p.line from p.off, the following lines, then (wrapped) the lines from
     * the top, and finally p.line again for matches starting before p.off. */
    for (size_t k = 0; k <= d->count; ++k) {
        size_t line = (p.line + k) % d->count, from = 0;
        const NLine *l = &d->lines[line];
        size_t limit = l->len;
        if (k == 0) from = p.off;
        if (k == d->count) { limit = p.off + n - 1; if (limit > l->len) limit = l->len; }
        const char *hit = find_in(l->s, limit, from, pattern, n);
        if (hit) {
            *start = (NPos){line, (size_t)(hit - l->s)};
            *end = (NPos){line, start->off + n};
            if (wrapped) *wrapped = k && line <= p.line;
            return 1;
        }
    }
    return 0;
}
