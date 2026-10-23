// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "zipfile.h"
#include "i18n.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define ZLIB_CONST
#include <zlib.h>

#define SIG_LOCAL 0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD 0x06054b50u
#define SIG_EOCD64 0x06064b50u
#define SIG_LOCATOR64 0x07064b50u
#define SIG_DESCRIPTOR 0x08074b50u
#define CHUNK 65536
/* A central directory beyond this is not a document; refuse rather than
 * allocate (the Pomera has 994 MB). */
#define CD_LIMIT (256u * 1024 * 1024)

struct ZipArchive {
    char *path;
    int fd;
    uint64_t size;
    ZipEntry *entries;
    size_t count;
    int zip64;
    unsigned char *comment;
    uint16_t comment_len;
};

static void fail(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static int read_full(int fd, void *buf, size_t n, uint64_t at)
{
    unsigned char *p = buf;
    while (n) {
        if (at > (uint64_t)INT64_MAX) return -1;
        ssize_t r = pread(fd, p, n, (off_t)at);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r; at += (uint64_t)r;
    }
    return 0;
}

size_t zip_count(const ZipArchive *z) { return z ? z->count : 0; }
const ZipEntry *zip_entry(const ZipArchive *z, size_t i) { return z && i < z->count ? &z->entries[i] : NULL; }
int zip_is_zip64(const ZipArchive *z) { return z && z->zip64; }
const char *zip_path(const ZipArchive *z) { return z ? z->path : NULL; }
long zip_find(const ZipArchive *z, const char *name)
{
    if (!z || !name) return -1;
    for (size_t i = 0; i < z->count; ++i)
        if (!strcmp(z->entries[i].name, name)) return (long)i;
    return -1;
}
void zip_close(ZipArchive *z)
{
    if (!z) return;
    for (size_t i = 0; i < z->count; ++i) {
        free(z->entries[i].name); free(z->entries[i].extra); free(z->entries[i].comment);
    }
    if (z->fd >= 0) close(z->fd);
    free(z->entries); free(z->comment); free(z->path); free(z);
}

/* ZIP64 extended information: only the saturated fields are present, in
 * this order. */
static int apply_zip64(ZipEntry *e, int disk_saturated)
{
    const unsigned char *p = e->extra, *end = e->extra + e->extra_len;
    while (end - p >= 4) {
        uint16_t id = le16(p), len = le16(p + 2);
        const unsigned char *d = p + 4;
        if ((size_t)(end - d) < len) return -1;
        if (id == 1) {
            const unsigned char *q = d, *qe = d + len;
            uint64_t *field[3] = {&e->uncompressed_size, &e->compressed_size, &e->local_offset};
            for (int k = 0; k < 3; ++k) {
                if (*field[k] != 0xffffffffu) continue;
                if (qe - q < 8) return -1;
                *field[k] = le64(q);
                q += 8;
            }
            if (disk_saturated && qe - q < 4) return -1;
            e->zip64 = 1;
            return 0;
        }
        p = d + len;
    }
    if (e->uncompressed_size == 0xffffffffu || e->compressed_size == 0xffffffffu ||
        e->local_offset == 0xffffffffu) return -1;
    return 0;
}

int zip_open(const char *path, ZipArchive **out, char *err, size_t errlen)
{
    *out = NULL;
    ZipArchive *z = calloc(1, sizeof(*z));
    unsigned char *tail = NULL, *cd = NULL;
    if (!z) { fail(err, errlen, T("メモリが足りません")); return -1; }
    z->fd = open(path, O_RDONLY);
    struct stat st;
    if (z->fd < 0 || fstat(z->fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        fail(err, errlen, T("ファイルを開けません")); goto error;
    }
    z->path = strdup(path);
    if (!z->path) { fail(err, errlen, T("メモリが足りません")); goto error; }
    z->size = (uint64_t)st.st_size;
    if (z->size < 22) { fail(err, errlen, T("zip の形式ではありません")); goto error; }
    size_t tail_len = z->size < 65557 ? (size_t)z->size : 65557;
    uint64_t tail_at = z->size - tail_len;
    tail = malloc(tail_len);
    if (!tail || read_full(z->fd, tail, tail_len, tail_at)) { fail(err, errlen, T("ファイルを読めません")); goto error; }
    long eocd = -1;
    for (long i = (long)tail_len - 22; i >= 0; --i)
        if (le32(tail + i) == SIG_EOCD && (size_t)i + 22 + le16(tail + i + 20) <= tail_len) { eocd = i; break; }
    if (eocd < 0) { fail(err, errlen, T("zip の形式ではありません")); goto error; }
    const unsigned char *e = tail + eocd;
    uint64_t eocd_pos = tail_at + (uint64_t)eocd;
    uint64_t total = le16(e + 10), cd_size = le32(e + 12), cd_offset = le32(e + 16);
    if (le16(e + 4) != 0 || le16(e + 6) != 0) { fail(err, errlen, T("分割された zip には対応していません")); goto error; }
    z->comment_len = le16(e + 20);
    if (z->comment_len) {
        z->comment = malloc(z->comment_len);
        if (!z->comment) { fail(err, errlen, T("メモリが足りません")); goto error; }
        memcpy(z->comment, e + 22, z->comment_len);
    }
    uint64_t cd_end = eocd_pos;
    if (eocd >= 20 && le32(tail + eocd - 20) == SIG_LOCATOR64) {
        const unsigned char *l = tail + eocd - 20;
        uint64_t at = le64(l + 8);
        unsigned char r[56];
        if (at > z->size - 56 || read_full(z->fd, r, 56, at) || le32(r) != SIG_EOCD64) {
            fail(err, errlen, T("zip が壊れています（ZIP64 の終わりの記録）")); goto error;
        }
        total = le64(r + 32); cd_size = le64(r + 40); cd_offset = le64(r + 48);
        cd_end = at;
        z->zip64 = 1;
    } else if (total == 0xffff || cd_size == 0xffffffffu || cd_offset == 0xffffffffu) {
        fail(err, errlen, T("zip が壊れています（ZIP64 の記録がありません）")); goto error;
    }
    /* Data prepended to the archive (self-extractors) shifts every offset. */
    if (cd_size > cd_end || cd_offset > cd_end - cd_size) { fail(err, errlen, T("zip が壊れています（目次の位置）")); goto error; }
    uint64_t base = cd_end - cd_size - cd_offset;
    if (cd_size > CD_LIMIT || total > cd_size / 46 + 1) { fail(err, errlen, T("zip の目次が大きすぎます")); goto error; }
    cd = malloc(cd_size ? (size_t)cd_size : 1);
    if (!cd) { fail(err, errlen, T("メモリが足りません")); goto error; }
    if (cd_size && read_full(z->fd, cd, (size_t)cd_size, cd_offset + base)) { fail(err, errlen, T("ファイルを読めません")); goto error; }
    z->entries = calloc(total ? (size_t)total : 1, sizeof(ZipEntry));
    if (!z->entries) { fail(err, errlen, T("メモリが足りません")); goto error; }
    size_t at = 0;
    for (uint64_t i = 0; i < total; ++i) {
        if (cd_size - at < 46 || le32(cd + at) != SIG_CENTRAL) { fail(err, errlen, T("zip が壊れています（目次）")); goto error; }
        const unsigned char *c = cd + at;
        ZipEntry *x = &z->entries[z->count];
        uint16_t nlen = le16(c + 28), elen = le16(c + 30), clen = le16(c + 32);
        if (cd_size - at - 46 < (uint64_t)nlen + elen + clen) { fail(err, errlen, T("zip が壊れています（目次）")); goto error; }
        x->version_made = le16(c + 4); x->version_needed = le16(c + 6); x->flags = le16(c + 8);
        x->method = le16(c + 10); x->mod_time = le16(c + 12); x->mod_date = le16(c + 14);
        x->crc32 = le32(c + 16); x->compressed_size = le32(c + 20); x->uncompressed_size = le32(c + 24);
        x->internal_attr = le16(c + 36); x->external_attr = le32(c + 38); x->local_offset = le32(c + 42);
        x->name = malloc((size_t)nlen + 1);
        x->extra = elen ? malloc(elen) : NULL;
        x->comment = clen ? malloc(clen) : NULL;
        z->count++;
        if (!x->name || (elen && !x->extra) || (clen && !x->comment)) { fail(err, errlen, T("メモリが足りません")); goto error; }
        memcpy(x->name, c + 46, nlen); x->name[nlen] = 0;
        if (elen) memcpy(x->extra, c + 46 + nlen, elen);
        if (clen) memcpy(x->comment, c + 46 + nlen + elen, clen);
        x->extra_len = elen; x->comment_len = clen;
        if (apply_zip64(x, le16(c + 34) == 0xffff)) { fail(err, errlen, T("zip が壊れています（ZIP64 の項目）")); goto error; }
        if (x->zip64) z->zip64 = 1;
        if (x->local_offset > UINT64_MAX - base) { fail(err, errlen, T("zip が壊れています（目次の位置）")); goto error; }
        x->local_offset += base;
        if (x->local_offset >= z->size) { fail(err, errlen, T("zip が壊れています（中身の位置）")); goto error; }
        at += 46 + (size_t)nlen + elen + clen;
    }
    free(tail); free(cd);
    *out = z;
    return 0;
error:
    free(tail); free(cd); zip_close(z);
    return -1;
}

/* Offset of the entry's data, after its local header. */
static int data_offset(ZipArchive *z, const ZipEntry *e, uint64_t *offset, uint64_t *header_len,
                       char *err, size_t errlen)
{
    unsigned char h[30];
    if (e->local_offset > z->size - 30 || read_full(z->fd, h, 30, e->local_offset) || le32(h) != SIG_LOCAL) {
        fail(err, errlen, T("zip が壊れています（%s の見出し）"), e->name); return -1;
    }
    *header_len = 30 + (uint64_t)le16(h + 26) + le16(h + 28);
    *offset = e->local_offset + *header_len;
    if (*offset > z->size || e->compressed_size > z->size - *offset) {
        fail(err, errlen, T("zip が壊れています（%s が途中で切れています）"), e->name); return -1;
    }
    return 0;
}

int zip_read_stream(ZipArchive *z, size_t index, ZipSink sink, void *ctx, char *err, size_t errlen)
{
    const ZipEntry *e = zip_entry(z, index);
    if (!e) { fail(err, errlen, T("中身がありません")); return -1; }
    if (e->flags & 1) { fail(err, errlen, T("暗号化された zip は読めません")); return -1; }
    if (e->method != ZIP_STORE && e->method != ZIP_DEFLATE) {
        fail(err, errlen, T("対応していない圧縮方式です（%u）"), e->method); return -1;
    }
    uint64_t at, header;
    if (data_offset(z, e, &at, &header, err, errlen)) return -1;
    unsigned char *in = malloc(CHUNK), *out = malloc(CHUNK);
    if (!in || !out) { free(in); free(out); fail(err, errlen, T("メモリが足りません")); return -1; }
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t left = e->compressed_size, produced = 0;
    int rc = 0;
    if (e->method == ZIP_STORE) {
        if (e->compressed_size != e->uncompressed_size) { fail(err, errlen, T("zip が壊れています（%s の大きさ）"), e->name); rc = -1; }
        while (!rc && left) {
            size_t n = left < CHUNK ? (size_t)left : CHUNK;
            if (read_full(z->fd, in, n, at)) { fail(err, errlen, T("ファイルを読めません")); rc = -1; break; }
            crc = crc32(crc, in, (uInt)n);
            if (sink(ctx, in, n)) { rc = -2; break; }
            at += n; left -= n; produced += n;
        }
    } else {
        z_stream s;
        memset(&s, 0, sizeof(s));
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(in); free(out); fail(err, errlen, T("展開を始められません")); return -1; }
        int status = Z_OK, starved = 1;
        while (!rc && status != Z_STREAM_END) {
            /* More input only when the last call did not fill the output. */
            if (!s.avail_in && starved) {
                if (!left) { fail(err, errlen, T("zip が壊れています（%s が途中で切れています）"), e->name); rc = -1; break; }
                size_t n = left < CHUNK ? (size_t)left : CHUNK;
                if (read_full(z->fd, in, n, at)) { fail(err, errlen, T("ファイルを読めません")); rc = -1; break; }
                at += n; left -= n;
                s.next_in = in; s.avail_in = (uInt)n;
            }
            s.next_out = out; s.avail_out = CHUNK;
            status = inflate(&s, Z_NO_FLUSH);
            size_t n = CHUNK - s.avail_out;
            if (status != Z_OK && status != Z_STREAM_END && !(status == Z_BUF_ERROR && !s.avail_in)) {
                fail(err, errlen, T("zip が壊れています（%s を展開できません）"), e->name); rc = -1; break;
            }
            starved = s.avail_out != 0;
            if (n) {
                produced += n;
                /* Never pass on more than the directory declares (zip bombs). */
                if (produced > e->uncompressed_size) { fail(err, errlen, T("zip が壊れています（%s の大きさ）"), e->name); rc = -1; break; }
                crc = crc32(crc, out, (uInt)n);
                if (sink(ctx, out, n)) { rc = -2; break; }
            }
        }
        inflateEnd(&s);
    }
    free(in); free(out);
    if (rc == -2) return -1;
    if (rc) return -1;
    if (produced != e->uncompressed_size || (uint32_t)crc != e->crc32) {
        fail(err, errlen, T("zip が壊れています（%s の CRC が合いません）"), e->name); return -1;
    }
    return 0;
}

typedef struct { unsigned char *data; size_t size, cap, limit; } Buffer;
static int buffer_sink(void *ctx, const unsigned char *data, size_t n)
{
    Buffer *b = ctx;
    if (n > b->limit - b->size) return 1;
    if (b->size + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->size + n + 1) cap *= 2;
        unsigned char *p = realloc(b->data, cap);
        if (!p) return 1;
        b->data = p; b->cap = cap;
    }
    memcpy(b->data + b->size, data, n);
    b->size += n;
    return 0;
}
int zip_read(ZipArchive *z, size_t index, size_t limit, unsigned char **data, size_t *size, char *err, size_t errlen)
{
    *data = NULL; *size = 0;
    const ZipEntry *e = zip_entry(z, index);
    if (!e) { fail(err, errlen, T("中身がありません")); return -1; }
    if (e->uncompressed_size > limit || e->uncompressed_size >= SIZE_MAX) {
        fail(err, errlen, T("%s が大きすぎます"), e->name); return -1;
    }
    Buffer b = {NULL, 0, 0, limit};
    b.cap = (size_t)e->uncompressed_size + 1;
    b.data = malloc(b.cap);
    if (!b.data) { fail(err, errlen, T("メモリが足りません")); return -1; }
    char local[256] = "";
    if (zip_read_stream(z, index, buffer_sink, &b, local, sizeof(local))) {
        free(b.data);
        fail(err, errlen, "%s", local[0] ? local : T("メモリが足りません"));
        return -1;
    }
    b.data[b.size] = 0;
    *data = b.data; *size = b.size;
    return 0;
}

/* ---- writing ---- */

struct ZipWriter {
    FILE *f;
    int method, failed;
    z_stream s;
    uLong crc;
    uint64_t usize, csize;
    unsigned char out[CHUNK];
};
static int put(ZipWriter *w, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, w->f) != n) { w->failed = 1; return -1; }
    return 0;
}
static int deflate_some(ZipWriter *w, int flush)
{
    int status;
    do {
        w->s.next_out = w->out; w->s.avail_out = CHUNK;
        status = deflate(&w->s, flush);
        if (status == Z_STREAM_ERROR) { w->failed = 1; return -1; }
        size_t n = CHUNK - w->s.avail_out;
        if (put(w, w->out, n)) return -1;
        w->csize += n;
    } while (w->s.avail_out == 0 || (flush == Z_FINISH && status != Z_STREAM_END));
    return 0;
}
int zip_writer_write(ZipWriter *w, const void *data, size_t size)
{
    if (w->failed) return -1;
    const unsigned char *p = data;
    while (size) {
        size_t n = size > (1u << 30) ? (1u << 30) : size;
        w->crc = crc32(w->crc, p, (uInt)n);
        w->usize += n;
        if (w->method == ZIP_STORE) {
            if (put(w, p, n)) return -1;
            w->csize += n;
        } else {
            w->s.next_in = p; w->s.avail_in = (uInt)n;
            if (deflate_some(w, Z_NO_FLUSH)) return -1;
        }
        p += n; size -= n;
    }
    return 0;
}

typedef struct {
    const ZipEntry *source;     /* NULL for a new entry */
    const char *name;
    uint16_t version_needed, flags, method, mod_time, mod_date;
    uint32_t crc32;
    uint64_t csize, usize, offset;
    int replaced;
} OutEntry;

static void dos_now(uint16_t *t, uint16_t *d)
{
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || tm.tm_year < 80) { *t = 0; *d = (1 << 5) | 1; return; }
    *t = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *d = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}
static int ascii(const char *s)
{
    for (; *s; ++s) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}
static int too_big(uint64_t v) { return v >= 0xffffffffu; }

static int copy_raw(ZipArchive *z, FILE *f, uint64_t at, uint64_t n, char *err, size_t errlen)
{
    unsigned char *buf = malloc(CHUNK);
    if (!buf) { fail(err, errlen, T("メモリが足りません")); return -1; }
    while (n) {
        size_t k = n < CHUNK ? (size_t)n : CHUNK;
        if (read_full(z->fd, buf, k, at)) { free(buf); fail(err, errlen, T("ファイルを読めません")); return -1; }
        if (fwrite(buf, 1, k, f) != k) { free(buf); fail(err, errlen, T("書き込めません")); return -1; }
        at += k; n -= k;
    }
    free(buf);
    return 0;
}

static int copy_entry(ZipArchive *z, const ZipEntry *e, FILE *f, OutEntry *o, char *err, size_t errlen)
{
    uint64_t data, header;
    if (data_offset(z, e, &data, &header, err, errlen)) return -1;
    uint64_t length = header + e->compressed_size;
    if (e->flags & 8) {
        /* Data descriptor: optional signature, crc, sizes (32-bit: no ZIP64). */
        unsigned char d[4];
        uint64_t at = data + e->compressed_size;
        if (at > z->size - 12 || read_full(z->fd, d, 4, at)) { fail(err, errlen, T("zip が壊れています（%s の記述子）"), e->name); return -1; }
        uint64_t extra = le32(d) == SIG_DESCRIPTOR ? 16 : 12;
        if (at > z->size - extra) { fail(err, errlen, T("zip が壊れています（%s の記述子）"), e->name); return -1; }
        length += extra;
    }
    off_t pos = ftello(f);
    if (pos < 0) { fail(err, errlen, T("書き込めません")); return -1; }
    o->offset = (uint64_t)pos;
    return copy_raw(z, f, e->local_offset, length, err, errlen);
}

static int write_new(FILE *f, OutEntry *o, const ZipChange *c, char *err, size_t errlen)
{
    unsigned char h[30];
    size_t nlen = strlen(o->name);
    if (nlen > 0xffff) { fail(err, errlen, T("名前が長すぎます")); return -1; }
    off_t pos = ftello(f);
    if (pos < 0) { fail(err, errlen, T("書き込めません")); return -1; }
    o->offset = (uint64_t)pos;
    o->method = c->method == ZIP_STORE ? ZIP_STORE : ZIP_DEFLATE;
    o->version_needed = o->method == ZIP_STORE ? 10 : 20;
    o->flags = ascii(o->name) ? 0 : 0x800;
    if (o->source) o->flags = (uint16_t)((o->source->flags & 0x800) | o->flags);
    dos_now(&o->mod_time, &o->mod_date);
    memset(h, 0, sizeof(h));
    put32(h, SIG_LOCAL); put16(h + 4, o->version_needed); put16(h + 6, o->flags);
    put16(h + 8, o->method); put16(h + 10, o->mod_time); put16(h + 12, o->mod_date);
    put16(h + 26, (uint32_t)nlen);
    if (fwrite(h, 1, 30, f) != 30 || fwrite(o->name, 1, nlen, f) != nlen) { fail(err, errlen, T("書き込めません")); return -1; }
    ZipWriter *w = calloc(1, sizeof(*w));
    if (!w) { fail(err, errlen, T("メモリが足りません")); return -1; }
    w->f = f; w->method = o->method; w->crc = crc32(0L, Z_NULL, 0);
    if (w->method == ZIP_DEFLATE && deflateInit2(&w->s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(w); fail(err, errlen, T("圧縮を始められません")); return -1;
    }
    int rc = 0;
    if (c->produce) rc = c->produce(c->ctx, w);
    else rc = zip_writer_write(w, c->data, c->size);
    if (!rc && w->method == ZIP_DEFLATE) { w->s.avail_in = 0; w->s.next_in = NULL; rc = deflate_some(w, Z_FINISH); }
    if (w->method == ZIP_DEFLATE) deflateEnd(&w->s);
    if (rc || w->failed) {
        fail(err, errlen, T("%s の書き込みに失敗しました"), o->name);
        free(w); return -1;
    }
    o->crc32 = (uint32_t)w->crc; o->csize = w->csize; o->usize = w->usize;
    free(w);
    if (too_big(o->csize) || too_big(o->usize)) { fail(err, errlen, T("%s が大きすぎます（ZIP64 の書き込みには対応していません）"), o->name); return -1; }
    off_t end = ftello(f);
    unsigned char sizes[12];
    put32(sizes, o->crc32); put32(sizes + 4, (uint32_t)o->csize); put32(sizes + 8, (uint32_t)o->usize);
    if (end < 0 || fseeko(f, (off_t)o->offset + 14, SEEK_SET) || fwrite(sizes, 1, 12, f) != 12 || fseeko(f, end, SEEK_SET)) {
        fail(err, errlen, T("書き込めません")); return -1;
    }
    o->replaced = 1;
    return 0;
}

static int write_central(FILE *f, const OutEntry *o)
{
    unsigned char h[46];
    const ZipEntry *s = o->source;
    const char *name = s ? s->name : o->name;
    size_t nlen = strlen(name);
    memset(h, 0, sizeof(h));
    put32(h, SIG_CENTRAL);
    if (!o->replaced && s) {
        put16(h + 4, s->version_made); put16(h + 6, s->version_needed); put16(h + 8, s->flags);
        put16(h + 10, s->method); put16(h + 12, s->mod_time); put16(h + 14, s->mod_date);
        put32(h + 16, s->crc32); put32(h + 20, (uint32_t)s->compressed_size); put32(h + 24, (uint32_t)s->uncompressed_size);
        put16(h + 30, s->extra_len); put16(h + 32, s->comment_len);
        put16(h + 36, s->internal_attr); put32(h + 38, s->external_attr);
    } else {
        put16(h + 4, s ? s->version_made : (3u << 8 | 20)); put16(h + 6, o->version_needed); put16(h + 8, o->flags);
        put16(h + 10, o->method); put16(h + 12, o->mod_time); put16(h + 14, o->mod_date);
        put32(h + 16, o->crc32); put32(h + 20, (uint32_t)o->csize); put32(h + 24, (uint32_t)o->usize);
        put16(h + 32, s ? s->comment_len : 0);
        put16(h + 36, s ? s->internal_attr : 0); put32(h + 38, s ? s->external_attr : (uint32_t)0100644 << 16);
    }
    put16(h + 28, (uint32_t)nlen);
    put32(h + 42, (uint32_t)o->offset);
    if (fwrite(h, 1, 46, f) != 46 || fwrite(name, 1, nlen, f) != nlen) return -1;
    if (!o->replaced && s && s->extra_len && fwrite(s->extra, 1, s->extra_len, f) != s->extra_len) return -1;
    if (s && s->comment_len && fwrite(s->comment, 1, s->comment_len, f) != s->comment_len) return -1;
    return 0;
}

/* Writes go beside the file a symbolic link points to, so that the rename
 * replaces the file and keeps the link. */
static char *real_target(const char *dest)
{
    char *real = realpath(dest, NULL);
    return real ? real : strdup(dest);
}
static char *temp_beside(const char *dest)
{
    const char *slash = strrchr(dest, '/');
    size_t dir = slash ? (size_t)(slash - dest + 1) : 0;
    const char *base = dest + dir;
    size_t n = dir + strlen(base) + 16;
    char *t = malloc(n);
    if (t) snprintf(t, n, "%.*s.%s.XXXXXX", (int)dir, dest, base);
    return t;
}

int zip_rewrite(ZipArchive *src, const char *dest, const ZipChange *changes, size_t count, char *err, size_t errlen)
{
    if (src && src->zip64) { fail(err, errlen, T("ZIP64 の zip の書き込みには対応していません")); return -1; }
    char *target = real_target(dest);
    if (!target) { fail(err, errlen, T("メモリが足りません")); return -1; }
    dest = target;
    size_t total = (src ? src->count : 0) + count;
    OutEntry *out = calloc(total ? total : 1, sizeof(*out));
    unsigned char *used = calloc(count ? count : 1, 1);
    char *temp = temp_beside(dest);
    FILE *f = NULL;
    int fd = -1, rc = -1;
    size_t n = 0;
    if (!out || !used || !temp) { fail(err, errlen, T("メモリが足りません")); goto done; }
    fd = mkstemp(temp);
    if (fd < 0) { fail(err, errlen, T("一時ファイルを作れません")); free(temp); temp = NULL; goto done; }
    f = fdopen(fd, "wb");
    if (!f) { fail(err, errlen, T("一時ファイルを作れません")); goto done; }
    fd = -1;
    for (size_t i = 0; src && i < src->count; ++i) {
        const ZipEntry *e = &src->entries[i];
        const ZipChange *c = NULL;
        for (size_t k = 0; k < count; ++k)
            if (!used[k] && changes[k].name && !strcmp(changes[k].name, e->name)) { c = &changes[k]; used[k] = 1; break; }
        if (c && c->remove) continue;
        OutEntry *o = &out[n];
        o->source = e; o->name = e->name;
        if (c) { if (write_new(f, o, c, err, errlen)) goto done; }
        else if (copy_entry(src, e, f, o, err, errlen)) goto done;
        if (too_big(o->offset)) { fail(err, errlen, T("大きすぎます（ZIP64 の書き込みには対応していません）")); goto done; }
        ++n;
    }
    for (size_t k = 0; k < count; ++k) {
        if (used[k] || changes[k].remove || !changes[k].name) continue;
        OutEntry *o = &out[n];
        o->name = changes[k].name;
        if (write_new(f, o, &changes[k], err, errlen)) goto done;
        if (too_big(o->offset)) { fail(err, errlen, T("大きすぎます（ZIP64 の書き込みには対応していません）")); goto done; }
        ++n;
    }
    off_t cd_start = ftello(f);
    if (cd_start < 0) { fail(err, errlen, T("書き込めません")); goto done; }
    for (size_t i = 0; i < n; ++i)
        if (write_central(f, &out[i])) { fail(err, errlen, T("書き込めません")); goto done; }
    off_t cd_end = ftello(f);
    if (cd_end < 0 || n > 0xfffe || too_big((uint64_t)cd_end)) { fail(err, errlen, T("大きすぎます（ZIP64 の書き込みには対応していません）")); goto done; }
    unsigned char eo[22];
    memset(eo, 0, sizeof(eo));
    put32(eo, SIG_EOCD); put16(eo + 8, (uint32_t)n); put16(eo + 10, (uint32_t)n);
    put32(eo + 12, (uint32_t)(cd_end - cd_start)); put32(eo + 16, (uint32_t)cd_start);
    uint16_t clen = src ? src->comment_len : 0;
    put16(eo + 20, clen);
    if (fwrite(eo, 1, 22, f) != 22 || (clen && fwrite(src->comment, 1, clen, f) != clen)) { fail(err, errlen, T("書き込めません")); goto done; }
    if (fflush(f) || fsync(fileno(f))) { fail(err, errlen, T("書き込めません")); goto done; }
    struct stat st;
    if (!stat(dest, &st)) fchmod(fileno(f), st.st_mode & 07777);
    else { mode_t mask = umask(0); umask(mask); fchmod(fileno(f), 0666 & ~mask); }
    if (fclose(f)) { f = NULL; fail(err, errlen, T("書き込めません")); goto done; }
    f = NULL;
    if (rename(temp, dest)) { fail(err, errlen, T("置き換えられません（%s）"), strerror(errno)); goto done; }
    free(temp); temp = NULL;
    rc = 0;
done:
    if (f) fclose(f);
    if (fd >= 0) close(fd);
    if (temp) { unlink(temp); free(temp); }
    free(out); free(used); free(target);
    return rc;
}
