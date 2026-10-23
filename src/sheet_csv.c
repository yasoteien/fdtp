// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
/* csv/tsv: read whole, keep the encoding, BOM, newline and quoting so that
 * an unedited field is written back as it was read. */
#include "sheet_model.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CSV_LIMIT (256u * 1024 * 1024)

static void fail(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

typedef struct { char *data; size_t len, cap; int failed; } Text;
static void text_add(Text *t, const char *s, size_t n)
{
    if (t->failed) return;
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 4096;
        while (cap < t->len + n + 1) cap *= 2;
        char *p = realloc(t->data, cap);
        if (!p) { t->failed = 1; return; }
        t->data = p; t->cap = cap;
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = 0;
}

static int write_beside(const char *path, const void *data, size_t n, char *err, size_t errlen);
/* Writes data to a temporary file beside path and renames it over path,
 * keeping path's mode. Shared with the backup copy in sheetview.c. */
int sheet_write_file(const char *path, const void *data, size_t n, char *err, size_t errlen)
{
    /* Through a symbolic link: replace the file it points to. */
    char *real = realpath(path, NULL);
    if (real) path = real;
    int rc = write_beside(path, data, n, err, errlen);
    free(real);
    return rc;
}
static int write_beside(const char *path, const void *data, size_t n, char *err, size_t errlen)
{
    const char *slash = strrchr(path, '/');
    size_t dir = slash ? (size_t)(slash - path + 1) : 0;
    size_t len = strlen(path) + 16;
    char *temp = malloc(len);
    if (!temp) { fail(err, errlen, T("メモリが足りません")); return -1; }
    snprintf(temp, len, "%.*s.%s.XXXXXX", (int)dir, path, path + dir);
    int fd = mkstemp(temp);
    if (fd < 0) { fail(err, errlen, T("一時ファイルを作れません")); free(temp); return -1; }
    const char *p = data;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fail(err, errlen, T("書き込めません")); close(fd); unlink(temp); free(temp); return -1; }
        p += w; left -= (size_t)w;
    }
    struct stat st;
    if (!stat(path, &st)) fchmod(fd, st.st_mode & 07777);
    else { mode_t mask = umask(0); umask(mask); fchmod(fd, 0666 & ~mask); }
    if (fsync(fd) || close(fd)) { fail(err, errlen, T("書き込めません")); unlink(temp); free(temp); return -1; }
    if (rename(temp, path)) { fail(err, errlen, T("置き換えられません（%s）"), strerror(errno)); unlink(temp); free(temp); return -1; }
    free(temp);
    return 0;
}

static int read_file(const char *path, unsigned char **data, size_t *size, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        fail(err, errlen, T("ファイルを開けません")); return -1;
    }
    if ((uint64_t)st.st_size > CSV_LIMIT) { close(fd); fail(err, errlen, T("大きすぎて開けません")); return -1; }
    size_t n = (size_t)st.st_size, got = 0;
    unsigned char *p = malloc(n + 1);
    if (!p) { close(fd); fail(err, errlen, T("メモリが足りません")); return -1; }
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != n) { free(p); fail(err, errlen, T("ファイルを読めません")); return -1; }
    p[n] = 0;
    *data = p; *size = n;
    return 0;
}

static int add_field(Sheet *s, uint32_t row, uint32_t col, const char *text, size_t n, int quoted)
{
    if (!n && !quoted) return 0;
    if (col >= SHEET_MAX_COLS) return -1;
    SheetCell *c = sheet_insert(s, row, col);
    if (!c) return -1;
    if (n) {
        c->text = arena_add(&s->arena, text, n);
        if (c->text == SHEET_NONE) return -1;
        c->kind = SK_STR;
    }
    if (quoted) c->flags |= SF_QUOTED;
    return 0;
}
static int end_row(Sheet *s, uint32_t fields)
{
    if (s->nrows >= SHEET_MAX_ROWS) return -1;
    /* Capacity doubles at powers of two (row_fields has no separate cap). */
    if (s->nrows < 16 ? !s->nrows : !(s->nrows & (s->nrows - 1))) {
        size_t cap = s->nrows < 16 ? 16 : s->nrows * 2;
        uint32_t *p = realloc(s->row_fields, cap * sizeof(*p));
        if (!p) return -1;
        s->row_fields = p;
    }
    s->row_fields[s->nrows++] = fields;
    return 0;
}

int csv_open(Book *b, const char *path, char delim, char *err, size_t errlen)
{
    unsigned char *raw = NULL;
    size_t size = 0;
    if (read_file(path, &raw, &size, err, errlen)) return -1;
    if (size >= 2 && ((raw[0] == 0xff && raw[1] == 0xfe) || (raw[0] == 0xfe && raw[1] == 0xff))) {
        free(raw); fail(err, errlen, T("UTF-16 の表には対応していません")); return -1;
    }
    b->type = BOOK_CSV;
    b->delim = delim;
    size_t skip = 0;
    if (size >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) { b->bom = 1; skip = 3; b->encoding = JIS_UTF8; }
    else b->encoding = jis_detect(raw, size);
    char *text = NULL;
    size_t len = 0;
    if (jis_to_utf8(raw + skip, size - skip, b->encoding, 0, &text, &len)) {
        if (jis_to_utf8(raw + skip, size - skip, b->encoding, 1, &text, &len)) {
            free(raw); fail(err, errlen, T("メモリが足りません")); return -1;
        }
        b->readonly = 1;
        snprintf(b->readonly_reason, sizeof(b->readonly_reason), "%s", T("文字コードを正しく読めない所があるため保存できません"));
    }
    free(raw);
    /* The first newline decides how rows are written back. */
    snprintf(b->newline, sizeof(b->newline), "%s", b->encoding == JIS_UTF8 ? "\n" : "\r\n");
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\r') { snprintf(b->newline, sizeof(b->newline), "%s", i + 1 < len && text[i + 1] == '\n' ? "\r\n" : "\r"); break; }
        if (text[i] == '\n') { snprintf(b->newline, sizeof(b->newline), "\n"); break; }
    }
    b->final_newline = len && (text[len - 1] == '\n' || text[len - 1] == '\r');
    b->sheets = calloc(1, sizeof(Sheet));
    if (!b->sheets) { free(text); fail(err, errlen, T("メモリが足りません")); return -1; }
    b->nsheets = 1;
    Sheet *s = &b->sheets[0];
    s->name = strdup("");
    s->loaded = 1;
    Text field = {NULL, 0, 0, 0};
    text_add(&field, "", 0);
    uint32_t row = 0, col = 0;
    size_t i = 0;
    int failed = field.failed;
    while (i < len && !failed) {
        /* One field. */
        int quoted = 0;
        field.len = 0;
        if (text[i] == '"') {
            quoted = 1;
            ++i;
            for (;;) {
                if (i >= len) break;
                if (text[i] == '"') {
                    if (i + 1 < len && text[i + 1] == '"') { text_add(&field, "\"", 1); i += 2; continue; }
                    ++i;
                    break;
                }
                size_t j = i;
                while (j < len && text[j] != '"') ++j;
                text_add(&field, text + i, j - i);
                i = j;
            }
        }
        size_t j = i;
        while (j < len && text[j] != delim && text[j] != '\n' && text[j] != '\r') ++j;
        text_add(&field, text + i, j - i);
        i = j;
        if (field.failed) { failed = 1; break; }
        int newline = i >= len || text[i] == '\n' || text[i] == '\r';
        if (!(newline && col == 0 && !quoted && !field.len)) {
            if (add_field(s, row, col, field.data, field.len, quoted)) { failed = 1; break; }
            ++col;
        }
        if (i < len && text[i] == delim) {
            ++i;
            if (i >= len || text[i] == '\n' || text[i] == '\r') {
                /* A trailing delimiter ends with an empty field. */
                ++col;
            } else continue;
        }
        if (i < len && text[i] == '\r') { ++i; if (i < len && text[i] == '\n') ++i; }
        else if (i < len && text[i] == '\n') ++i;
        if (end_row(s, col)) { failed = 1; break; }
        ++row; col = 0;
    }
    free(field.data);
    free(text);
    if (failed) { fail(err, errlen, T("大きすぎて開けません")); return -1; }
    return 0;
}

int csv_save(Book *b, char *err, size_t errlen)
{
    Sheet *s = &b->sheets[0];
    Text out = {NULL, 0, 0, 0};
    if (b->bom) text_add(&out, "\xef\xbb\xbf", 3);
    text_add(&out, "", 0);
    size_t rows = s->nrows;
    for (size_t i = 0; i < s->count; ++i) {
        SheetCell *c = &s->cells[i];
        if ((c->kind != SK_EMPTY || (c->flags & SF_QUOTED)) && (size_t)c->row + 1 > rows) rows = (size_t)c->row + 1;
    }
    size_t k = 0;
    for (size_t r = 0; r < rows && !out.failed; ++r) {
        size_t first = k;
        uint32_t fields = r < s->nrows ? s->row_fields[r] : 0;
        while (k < s->count && s->cells[k].row == r) {
            SheetCell *c = &s->cells[k];
            if ((c->kind != SK_EMPTY || (c->flags & SF_QUOTED)) && c->col + 1 > fields) fields = c->col + 1;
            ++k;
        }
        size_t at = first;
        for (uint32_t col = 0; col < fields; ++col) {
            if (col) text_add(&out, &b->delim, 1);
            while (at < k && s->cells[at].col < col) ++at;
            SheetCell *c = at < k && s->cells[at].col == col ? &s->cells[at] : NULL;
            const char *t = c ? cell_string(b, s, c) : "";
            int quote = c && (c->flags & SF_QUOTED);
            if (strchr(t, b->delim) || strchr(t, '"') || strchr(t, '\n') || strchr(t, '\r')) quote = 1;
            if (!quote) { text_add(&out, t, strlen(t)); continue; }
            text_add(&out, "\"", 1);
            for (const char *p = t; *p;) {
                const char *q = strchr(p, '"');
                size_t span = q ? (size_t)(q - p) : strlen(p);
                text_add(&out, p, span);
                if (!q) break;
                text_add(&out, "\"\"", 2);
                p = q + 1;
            }
            text_add(&out, "\"", 1);
        }
        if (r + 1 < rows || b->final_newline) text_add(&out, b->newline, strlen(b->newline));
    }
    if (out.failed) { free(out.data); fail(err, errlen, T("メモリが足りません")); return -1; }
    unsigned char *bytes = (unsigned char *)out.data;
    size_t n = out.len;
    unsigned char *converted = NULL;
    if (b->encoding != JIS_UTF8) {
        if (jis_from_utf8(out.data, out.len, b->encoding, &converted, &n)) {
            free(out.data);
            fail(err, errlen, T("%s で書けない文字があります"), jis_encoding_name(b->encoding));
            return -1;
        }
        bytes = converted;
    }
    int rc = sheet_write_file(b->path, bytes, n, err, errlen);
    free(converted);
    free(out.data);
    return rc;
}
