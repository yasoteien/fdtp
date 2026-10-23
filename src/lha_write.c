// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Creating and adding to LHa archives (see lha_write.h).
 *
 * Level 2 header, as written here (little-endian):
 *    0  2  header size (all of it, extended headers and padding included)
 *    2  5  method: -lh5- compressed, -lh0- stored, -lhd- directory or link
 *    7  4  data size in the archive        11  4  original size
 *   15  4  modification time (UNIX time)   19  1  0x20 (file) / 0x10 (dir)
 *   20  1  level 2                         21  2  CRC16 of the data
 *   23  1  'U' (made on UNIX)              24  2  size of the first extension
 * then extensions: type, data, size of the next one (0 ends the list):
 *   0x00 CRC16 of the whole header (computed with this field 0)
 *   0x01 file name           0x02 directories, each followed by 0xFF
 *   0x50 UNIX mode (type bits included)
 * A header whose size has 0 as its low byte gets one padding byte, so that
 * it never begins with the 0 that ends an archive. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _DARWIN_C_SOURCE
#include "lha_write.h"
#include "i18n.h"
#include "lha_lh5.h"
#include "jis.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CHUNK = 65536, HEADER_MAX = 65535, EARLY_STORE = 512 * 1024 };
enum { KIND_FILE, KIND_DIR, KIND_LINK };
enum { ENTRY_PENDING, ENTRY_REPLACES, ENTRY_DROPPED };
enum { ACT_COPY, ACT_ADD };
enum { PH_IDLE, PH_COPY, PH_COMPRESS, PH_STORE, PH_DONE };

typedef struct {
    char *src, *path;            /* where to read it; the stored path */
    unsigned char *name, *dir;   /* encoded 0x01 and 0x02 contents */
    size_t name_len, dir_len;
    int kind, state;
    uint32_t mode;
    time_t mtime;
    uint64_t size;
} Entry;
typedef struct { off_t offset; uint64_t length; char *path; time_t mtime; } OldMember;
typedef struct { int type; size_t index; } Action;

struct LhaJob {
    LhaOptions opt;
    int add, started;
    char *dest, *temp;
    int fd, old_fd, src_fd;
    struct stat dest_st;
    int dest_exists;
    Entry *entries; size_t entry_count, entry_cap;
    OldMember *old; size_t old_count, old_cap;
    Action *actions; size_t action_count, next;
    /* The member being written. */
    int phase;
    off_t out_pos, member_start, data_start, copy_at;
    uint64_t remaining, in_size;
    uint16_t crc;
    Lh5Encoder *enc;
    const Entry *entry;
    const char *current;
    int write_failed;
    size_t done;
    uint64_t bytes_done, bytes_total;
    LhaStats stats;
    unsigned char chunk[CHUNK];
};

/* ---- small helpers -------------------------------------------------------- */

uint16_t lha_crc16(uint16_t crc, const void *data, size_t size)
{
    static uint16_t table[256];
    static int ready;
    if (!ready) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0xA001u : c >> 1;
            table[i] = (uint16_t)c;
        }
        ready = 1;
    }
    const unsigned char *p = data;
    while (size--) crc = (uint16_t)((crc >> 8) ^ table[(crc ^ *p++) & 0xff]);
    return crc;
}
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static unsigned get16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static char *join(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s%s%s", a, *a && a[strlen(a) - 1] != '/' ? "/" : "", b);
    return p;
}
static int read_full(int fd, void *buf, size_t n, off_t at)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, at + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static int write_out(LhaJob *j, const void *data, size_t n)
{
    const char *p = data;
    while (n) {
        ssize_t w = write(j->fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { j->write_failed = 1; return -1; }
        p += w; n -= (size_t)w; j->out_pos += w;
    }
    return 0;
}
static int lh5_sink(void *ctx, const unsigned char *data, size_t n) { return write_out(ctx, data, n); }

/* name in the archive's encoding: Shift_JIS (strict) or the bytes as they are. */
static unsigned char *encode(const LhaJob *j, const char *s, size_t n, size_t *out_len)
{
    if (j->opt.utf8_names) {
        unsigned char *p = malloc(n + 1);
        if (p) { memcpy(p, s, n); p[n] = 0; *out_len = n; }
        return p;
    }
    unsigned char *out = NULL;
    if (jis_from_utf8(s, n, JIS_SHIFT_JIS, &out, out_len) < 0) return NULL;
    return out;
}

/* ---- the entries ---------------------------------------------------------- */

/* The path to store: path, or "path|target" for a link. NULL after err. */
static char *path_copy_with_link(const char *src, const char *path, int kind, char *err, size_t errlen)
{
    if (kind != KIND_LINK) { char *p = strdup(path); if (!p) snprintf(err, errlen, "%s", T("メモリが足りません")); return p; }
    char target[4096];
    ssize_t t = readlink(src, target, sizeof(target) - 1);
    if (t < 0) { snprintf(err, errlen, T("リンクを読めません: %.80s"), path); return NULL; }
    target[t] = 0;
    size_t n = strlen(path) + (size_t)t + 2;
    char *full = malloc(n);
    if (!full) { snprintf(err, errlen, "%s", T("メモリが足りません")); return NULL; }
    snprintf(full, n, "%s|%s", path, target);
    return full;
}
static int add_entry(LhaJob *j, const char *src, const char *path, const struct stat *st, int kind, char *err, size_t errlen)
{
    if (j->entry_count == j->entry_cap) {
        size_t cap = j->entry_cap ? j->entry_cap * 2 : 64;
        Entry *e = realloc(j->entries, cap * sizeof(*e));
        if (!e) goto memory;
        j->entries = e; j->entry_cap = cap;
    }
    Entry *e = &j->entries[j->entry_count];
    memset(e, 0, sizeof(*e));
    e->kind = kind; e->mode = (uint32_t)st->st_mode & 0xffff; e->mtime = st->st_mtime;
    e->size = kind == KIND_FILE ? (uint64_t)st->st_size : 0;
    if (e->size >= 0xffffffffu) { snprintf(err, errlen, T("4 GB 以上のファイルは LHa に入れられません: %.80s"), path); return -1; }
    if (!(e->src = strdup(src)) || !(e->path = strdup(path))) goto memory_entry;
    /* Directories: every part goes in the 0x02 header; otherwise the part
     * after the last '/' is the name. A link is stored as LHa for UNIX does,
     * as the path "name|target" split at its last '/' (which may be inside
     * the target: readers join the two before looking for '|'). */
    char *stored = path_copy_with_link(src, path, kind, err, errlen);
    if (!stored) goto fail_entry;
    const char *slash = kind == KIND_DIR ? stored + strlen(stored) : strrchr(stored, '/');
    size_t dir_bytes = slash ? (size_t)(slash - stored) : 0;
    if (dir_bytes) {
        /* Encode part by part so that no encoded byte is taken for '/'. */
        unsigned char *buf = NULL;
        size_t at = 0;
        for (const char *p = stored; p < stored + dir_bytes;) {
            const char *q = memchr(p, '/', (size_t)(stored + dir_bytes - p));
            if (!q) q = stored + dir_bytes;
            size_t n = 0;
            unsigned char *enc = encode(j, p, (size_t)(q - p), &n);
            if (!enc) { free(buf); free(stored); goto unnamed; }
            unsigned char *grown = realloc(buf, at + n + 1);
            if (!grown) { free(enc); free(buf); free(stored); goto memory_entry; }
            buf = grown;
            memcpy(buf + at, enc, n); at += n; buf[at++] = 0xff;
            free(enc);
            p = q + 1;
        }
        e->dir = buf; e->dir_len = at;
    }
    if (kind != KIND_DIR) {
        const char *base = slash ? slash + 1 : stored;
        e->name = encode(j, base, strlen(base), &e->name_len);
        if (!e->name) { free(stored); goto unnamed; }
    }
    free(stored);
    if (e->name_len + e->dir_len > HEADER_MAX - 64) { snprintf(err, errlen, T("名前が長すぎます: %.80s"), path); goto fail_entry; }
    ++j->entry_count;
    return 0;
unnamed:
    snprintf(err, errlen, T("Shift_JIS にできない名前です（名前を UTF-8 にしてください）: %.80s"), path);
    goto fail_entry;
memory_entry:
    snprintf(err, errlen, "%s", T("メモリが足りません"));
fail_entry:
    free(e->src); free(e->path); free(e->name); free(e->dir);
    return -1;
memory:
    snprintf(err, errlen, "%s", T("メモリが足りません"));
    return -1;
}
static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int walk(LhaJob *j, const char *src, const char *path, char *err, size_t errlen)
{
    struct stat st;
    if (lstat(src, &st) < 0) { snprintf(err, errlen, T("ありません: %.80s"), path); return -1; }
    if (j->dest_exists && st.st_dev == j->dest_st.st_dev && st.st_ino == j->dest_st.st_ino) return 0;
    if (S_ISREG(st.st_mode)) return add_entry(j, src, path, &st, KIND_FILE, err, errlen);
    if (S_ISLNK(st.st_mode)) return add_entry(j, src, path, &st, KIND_LINK, err, errlen);
    if (!S_ISDIR(st.st_mode)) { ++j->stats.skipped; return 0; }
    if (add_entry(j, src, path, &st, KIND_DIR, err, errlen) < 0) return -1;
    DIR *d = opendir(src);
    if (!d) { snprintf(err, errlen, T("ディレクトリを読めません: %.80s"), path); return -1; }
    char **names = NULL; size_t n = 0, cap = 0; int result = 0;
    for (struct dirent *de; (de = readdir(d));) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **grown = realloc(names, cap * sizeof(*names));
            if (!grown) { result = -1; break; }
            names = grown;
        }
        if (!(names[n] = strdup(de->d_name))) { result = -1; break; }
        ++n;
    }
    closedir(d);
    if (result < 0) snprintf(err, errlen, "%s", T("メモリが足りません"));
    else qsort(names, n, sizeof(*names), compare_names);
    for (size_t i = 0; i < n && !result; ++i) {
        char *s = join(src, names[i]), *p = join(path, names[i]);
        if (!s || !p) { snprintf(err, errlen, "%s", T("メモリが足りません")); result = -1; }
        else result = walk(j, s, p, err, errlen);
        free(s); free(p);
    }
    for (size_t i = 0; i < n; ++i) free(names[i]);
    free(names);
    return result;
}

/* ---- the existing archive ------------------------------------------------- */

/* Stored name to the form of Entry.path: UTF-8 (a Shift_JIS name is
 * converted), '\' and 0xFF as '/', no "." parts, no '/' at either end. */
static char *member_path(const unsigned char *raw, size_t n)
{
    char *utf8 = NULL; size_t len = 0;
    unsigned char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, raw, n); copy[n] = 0;
    for (size_t i = 0; i < n; ++i) if (copy[i] == 0xff) copy[i] = '/';
    JisEncoding e = jis_detect(copy, n);
    if (e == JIS_UTF8) { utf8 = (char *)copy; copy = NULL; }
    else if (jis_to_utf8(copy, n, e, 1, &utf8, &len) < 0) utf8 = NULL;
    free(copy);
    if (!utf8) return NULL;
    char *out = malloc(strlen(utf8) + 1);
    if (!out) { free(utf8); return NULL; }
    size_t o = 0;
    for (char *p = utf8; *p; ++p) if (*p == '\\') *p = '/';
    char *save = NULL;
    for (char *part = strtok_r(utf8, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (o) out[o++] = '/';
        size_t k = strlen(part); memcpy(out + o, part, k); o += k;
    }
    out[o] = 0;
    free(utf8);
    return out;
}
static time_t dos_to_time(uint32_t v)
{
    struct tm tm = {0};
    tm.tm_isdst = -1;
    tm.tm_year = (int)((v >> 25) & 0x7f) + 80; tm.tm_mon = (int)((v >> 21) & 0xf) - 1; tm.tm_mday = (int)((v >> 16) & 0x1f);
    tm.tm_hour = (int)((v >> 11) & 0x1f); tm.tm_min = (int)((v >> 5) & 0x3f); tm.tm_sec = (int)(v & 0x1f) * 2;
    return tm.tm_mon < 0 ? 0 : mktime(&tm);
}
static int read_old(LhaJob *j, char *err, size_t errlen)
{
    struct stat st;
    if ((j->old_fd = open(j->dest, O_RDONLY)) < 0 || fstat(j->old_fd, &st) < 0) { snprintf(err, errlen, "%s", T("書庫を開けません")); return -1; }
    uint64_t size = (uint64_t)st.st_size, off = 0;
    unsigned char *h = malloc(HEADER_MAX + 4);
    if (!h) { snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
    int result = -1;
    while (off < size) {
        unsigned char first;
        if (read_full(j->old_fd, &first, 1, (off_t)off) < 0) goto bad;
        if (!first) break;
        if (off + 22 > size || read_full(j->old_fd, h, 22, (off_t)off) < 0) goto bad;
        if (h[2] != '-' || h[3] != 'l' || h[6] != '-') goto bad;
        int level = h[20];
        uint64_t packed = get32(h + 7), hsize, length;
        unsigned char name[1024], dir[1024]; size_t name_n = 0, dir_n = 0;
        time_t mtime = 0;
        if (level == 0 || level == 1) {
            hsize = (uint64_t)h[0] + 2;
            if (hsize < 24 || off + hsize > size || read_full(j->old_fd, h, (size_t)hsize, (off_t)off) < 0) goto bad;
            name_n = h[21];
            if (22 + name_n > hsize) goto bad;
            memcpy(name, h + 22, name_n);
            mtime = dos_to_time(get32(h + 15));
            length = hsize + packed;   /* level 1: the data size counts the extensions */
            if (level == 1) {
                uint64_t pos = off + hsize, ext = 0;
                unsigned next = get16(h + hsize - 2);
                while (next) {
                    if (next < 3 || pos + next > size || read_full(j->old_fd, h, next, (off_t)pos) < 0) goto bad;
                    size_t len = next - 3u;
                    if (h[0] == 1 && len < sizeof(name)) { memcpy(name, h + 1, len); name_n = len; }
                    if (h[0] == 2 && len < sizeof(dir)) { memcpy(dir, h + 1, len); dir_n = len; }
                    if (h[0] == 0x54 && len >= 4) mtime = (time_t)get32(h + 1);
                    pos += next; ext += next; next = get16(h + next - 2);
                }
                if (ext > packed) goto bad;
            }
        } else if (level == 2) {
            hsize = get16(h);
            if (hsize < 26 || off + hsize > size || read_full(j->old_fd, h, (size_t)hsize, (off_t)off) < 0) goto bad;
            mtime = (time_t)get32(h + 15);
            size_t pos = 26; unsigned next = get16(h + 24);
            while (next) {
                if (next < 3 || pos + next > hsize) goto bad;
                size_t len = next - 3u;
                if (h[pos] == 1 && len < sizeof(name)) { memcpy(name, h + pos + 1, len); name_n = len; }
                if (h[pos] == 2 && len < sizeof(dir)) { memcpy(dir, h + pos + 1, len); dir_n = len; }
                unsigned following = get16(h + pos + next - 2);
                pos += next; next = following;
            }
            length = hsize + packed;
        } else { snprintf(err, errlen, T("この LHa の形式（レベル %d）には追加できません"), level); goto done; }
        if (off + length > size) goto bad;
        unsigned char full[2100]; size_t n = 0;
        memcpy(full, dir, dir_n); n = dir_n;
        if (n && full[n - 1] != 0xff && full[n - 1] != '/' && full[n - 1] != '\\' && name_n) full[n++] = '/';
        memcpy(full + n, name, name_n); n += name_n;
        /* A link is "path|target" (the '|' may be in either part). */
        if (!memcmp(h + 2, "-lhd-", 5)) { unsigned char *bar = memchr(full, '|', n); if (bar) n = (size_t)(bar - full); }
        if (j->old_count == j->old_cap) {
            size_t cap = j->old_cap ? j->old_cap * 2 : 64;
            OldMember *m = realloc(j->old, cap * sizeof(*m));
            if (!m) goto memory;
            j->old = m; j->old_cap = cap;
        }
        OldMember *m = &j->old[j->old_count];
        if (!(m->path = member_path(full, n))) goto memory;
        m->offset = (off_t)off; m->length = length; m->mtime = mtime;
        ++j->old_count;
        off += length;
    }
    result = 0;
    goto done;
memory:
    snprintf(err, errlen, "%s", T("メモリが足りません"));
    goto done;
bad:
    snprintf(err, errlen, "%s", T("書庫を読めません（LHa が壊れています）"));
done:
    free(h);
    return result;
}

/* ---- planning ------------------------------------------------------------- */

static const LhaJob *sort_job;
static int compare_entries(const void *a, const void *b)
{
    return strcmp(sort_job->entries[*(const size_t *)a].path, sort_job->entries[*(const size_t *)b].path);
}
static int push_action(LhaJob *j, int type, size_t index)
{
    j->actions[j->action_count++] = (Action){type, index};
    j->bytes_total += type == ACT_COPY ? j->old[index].length : j->entries[index].size;
    return 0;
}
static int plan(LhaJob *j, char *err, size_t errlen)
{
    if (!(j->actions = malloc((j->entry_count + j->old_count + 1) * sizeof(*j->actions)))) goto memory;
    size_t *order = NULL;
    if (j->old_count) {
        if (!(order = malloc((j->entry_count + 1) * sizeof(*order)))) goto memory;
        for (size_t i = 0; i < j->entry_count; ++i) order[i] = i;
        sort_job = j;
        qsort(order, j->entry_count, sizeof(*order), compare_entries);
    }
    for (size_t i = 0; i < j->old_count; ++i) {
        const OldMember *m = &j->old[i];
        size_t lo = 0, hi = j->entry_count; Entry *e = NULL;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            int c = strcmp(j->entries[order[mid]].path, m->path);
            if (!c) { e = &j->entries[order[mid]]; break; }
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        if (!e) { push_action(j, ACT_COPY, i); continue; }
        if (e->state == ENTRY_REPLACES) continue;       /* an older duplicate goes */
        if (e->state == ENTRY_DROPPED) { push_action(j, ACT_COPY, i); continue; }
        if (j->opt.newer_only && !(e->mtime > m->mtime)) { e->state = ENTRY_DROPPED; ++j->stats.older; push_action(j, ACT_COPY, i); continue; }
        e->state = ENTRY_REPLACES;
        push_action(j, ACT_ADD, (size_t)(e - j->entries));
    }
    free(order);
    for (size_t i = 0; i < j->entry_count; ++i) if (j->entries[i].state == ENTRY_PENDING) push_action(j, ACT_ADD, i);
    return 0;
memory:
    snprintf(err, errlen, "%s", T("メモリが足りません"));
    return -1;
}

/* ---- writing -------------------------------------------------------------- */

typedef struct { unsigned type; const unsigned char *data; size_t len; } Ext;
static size_t build_header(const Entry *e, const char *method, uint32_t packed, uint32_t orig, uint16_t crc, unsigned char *h)
{
    memset(h, 0, 26);
    memcpy(h + 2, method, 5);
    put32(h + 7, packed); put32(h + 11, orig);
    put32(h + 15, e->mtime > 0 && (uint64_t)e->mtime <= 0xffffffffu ? (uint32_t)e->mtime : 0);
    h[19] = e->kind == KIND_DIR ? 0x10 : 0x20;
    h[20] = 2;
    put16(h + 21, crc);
    h[23] = 'U';
    size_t pos = 24;
    Ext ext[4];
    unsigned char mode[2];
    int n = 0;
    put16(mode, e->mode);
    ext[n++] = (Ext){0x00, NULL, 2};
    if (e->name) ext[n++] = (Ext){0x01, e->name, e->name_len};
    if (e->dir) ext[n++] = (Ext){0x02, e->dir, e->dir_len};
    ext[n++] = (Ext){0x50, mode, 2};
    put16(h + pos, (unsigned)(ext[0].len + 3)); pos += 2;
    size_t crc_at = 0;
    for (int i = 0; i < n; ++i) {
        h[pos++] = (unsigned char)ext[i].type;
        if (ext[i].data) memcpy(h + pos, ext[i].data, ext[i].len);
        else { memset(h + pos, 0, ext[i].len); crc_at = pos; }
        pos += ext[i].len;
        put16(h + pos, i + 1 < n ? (unsigned)(ext[i + 1].len + 3) : 0); pos += 2;
    }
    if (!(pos & 0xff)) h[pos++] = 0;
    put16(h, (unsigned)pos);
    put16(h + crc_at, lha_crc16(0, h, pos));
    return pos;
}
static int write_header(LhaJob *j, const Entry *e, const char *method, uint32_t packed, uint32_t orig, uint16_t crc, off_t at)
{
    unsigned char h[HEADER_MAX + 1];
    size_t n = build_header(e, method, packed, orig, crc, h);
    if (at < 0) return write_out(j, h, n);
    for (size_t done = 0; done < n;) {
        ssize_t w = pwrite(j->fd, h + done, n - done, at + (off_t)done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { j->write_failed = 1; return -1; }
        done += (size_t)w;
    }
    return 0;
}
static void member_done(LhaJob *j)
{
    ++j->done; j->phase = PH_IDLE; j->current = "";
    if (j->src_fd >= 0) { close(j->src_fd); j->src_fd = -1; }
}
static int fail_io(LhaJob *j, char *err, size_t errlen, const char *what)
{
    if (j->write_failed) snprintf(err, errlen, T("書き込めません: %s"), strerror(errno ? errno : EIO));
    else snprintf(err, errlen, "%s: %.80s", what, j->current ? j->current : "");
    return -1;
}
/* Switch the member to -lh0-: rewrite its data from the start of the file. */
static int begin_store(LhaJob *j)
{
    if (lseek(j->fd, j->data_start, SEEK_SET) < 0 || lseek(j->src_fd, 0, SEEK_SET) < 0) return -1;
    j->out_pos = j->data_start;
    j->bytes_total += j->in_size;
    j->in_size = 0; j->crc = 0; j->phase = PH_STORE;
    return 0;
}
static int finish(LhaJob *j, char *err, size_t errlen)
{
    unsigned char zero = 0;
    if (write_out(j, &zero, 1) < 0 || ftruncate(j->fd, j->out_pos) < 0 || fsync(j->fd) < 0) return fail_io(j, err, errlen, "");
    j->stats.out_bytes = (uint64_t)j->out_pos;
    if (close(j->fd) < 0) { j->fd = -1; return fail_io(j, err, errlen, ""); }
    j->fd = -1;
    if (rename(j->temp, j->dest) < 0) { snprintf(err, errlen, T("書庫を置けません: %s"), strerror(errno)); return -1; }
    free(j->temp); j->temp = NULL;
    j->phase = PH_DONE;
    return 0;
}

int lha_job_step(LhaJob *j, size_t budget, char *err, size_t errlen)
{
    if (j->phase == PH_DONE) return 0;
    if (!j->started) { snprintf(err, errlen, "%s", T("書庫の作成が始まっていません")); return -1; }
    size_t spent = 0;
    if (!budget) budget = 1;
    while (spent < budget) {
        if (j->phase == PH_IDLE) {
            if (j->next == j->action_count) {
                if (finish(j, err, errlen) < 0) return -1;
                return 0;
            }
            const Action *a = &j->actions[j->next++];
            j->member_start = j->out_pos;
            if (a->type == ACT_COPY) {
                const OldMember *m = &j->old[a->index];
                j->current = m->path; j->copy_at = m->offset; j->remaining = m->length; j->phase = PH_COPY;
                continue;
            }
            const Entry *e = j->entry = &j->entries[a->index];
            j->current = e->path;
            if (e->kind != KIND_FILE) {
                if (write_header(j, e, "-lhd-", 0, 0, 0, -1) < 0) return fail_io(j, err, errlen, "");
                ++j->stats.added;
                member_done(j);
                continue;
            }
            if ((j->src_fd = open(e->src, O_RDONLY)) < 0) return fail_io(j, err, errlen, T("読めません"));
            if (write_header(j, e, "-lh5-", 0, 0, 0, -1) < 0) return fail_io(j, err, errlen, "");
            j->data_start = j->out_pos; j->in_size = 0; j->crc = 0;
            /* One encoder for the whole job, started afresh for each file. */
            if (j->enc) lh5_reset(j->enc);
            else if (!(j->enc = lh5_create(j->opt.level, lh5_sink, j))) { snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
            j->phase = PH_COMPRESS;
            continue;
        }
        if (j->phase == PH_COPY) {
            size_t n = j->remaining < CHUNK ? (size_t)j->remaining : CHUNK;
            if (n && read_full(j->old_fd, j->chunk, n, j->copy_at) < 0) return fail_io(j, err, errlen, T("書庫を読めません"));
            if (n && write_out(j, j->chunk, n) < 0) return fail_io(j, err, errlen, "");
            j->copy_at += (off_t)n; j->remaining -= n; spent += n ? n : 1; j->bytes_done += n;
            if (!j->remaining) { ++j->stats.kept; member_done(j); }
            continue;
        }
        /* PH_COMPRESS or PH_STORE: the next piece of the file. */
        ssize_t got = read(j->src_fd, j->chunk, CHUNK);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return fail_io(j, err, errlen, T("読めません"));
        spent += got > 0 ? (size_t)got : 1;
        if (got > 0) {
            j->crc = lha_crc16(j->crc, j->chunk, (size_t)got);
            j->in_size += (uint64_t)got; j->bytes_done += (uint64_t)got;
            if (j->in_size >= 0xffffffffu) { snprintf(err, errlen, T("4 GB 以上のファイルは LHa に入れられません: %.80s"), j->current); return -1; }
            if (j->phase == PH_STORE) { if (write_out(j, j->chunk, (size_t)got) < 0) return fail_io(j, err, errlen, ""); continue; }
            if (lh5_write(j->enc, j->chunk, (size_t)got) < 0) return fail_io(j, err, errlen, "");
            /* Already larger than the input: storing will be smaller. */
            if (j->in_size >= EARLY_STORE && lh5_output_size(j->enc) > j->in_size && begin_store(j) < 0) return fail_io(j, err, errlen, "");
            continue;
        }
        const Entry *e = j->entry;
        if (j->phase == PH_COMPRESS) {
            if (lh5_finish(j->enc) < 0) return fail_io(j, err, errlen, "");
            uint64_t packed = lh5_output_size(j->enc);
            if (packed >= j->in_size) { if (begin_store(j) < 0) return fail_io(j, err, errlen, ""); continue; }
            if (write_header(j, e, "-lh5-", (uint32_t)packed, (uint32_t)j->in_size, j->crc, j->member_start) < 0) return fail_io(j, err, errlen, "");
        } else {
            if (write_header(j, e, "-lh0-", (uint32_t)j->in_size, (uint32_t)j->in_size, j->crc, j->member_start) < 0) return fail_io(j, err, errlen, "");
            ++j->stats.stored;
        }
        j->stats.in_bytes += j->in_size;
        ++j->stats.added;
        member_done(j);
    }
    return 1;
}

/* ---- the job -------------------------------------------------------------- */

LhaJob *lha_job_create(const char *dest, int add, const LhaOptions *options, char *err, size_t errlen)
{
    LhaJob *j = calloc(1, sizeof(*j));
    if (!j || !(j->dest = strdup(dest))) { free(j); snprintf(err, errlen, "%s", T("メモリが足りません")); return NULL; }
    j->opt = *options;
    if (j->opt.level < 1) j->opt.level = 1;
    if (j->opt.level > 9) j->opt.level = 9;
    j->add = add; j->fd = j->old_fd = j->src_fd = -1; j->current = "";
    j->dest_exists = !stat(dest, &j->dest_st);
    if (add && !j->dest_exists) { lha_job_free(j); snprintf(err, errlen, "%s", T("書庫がありません")); return NULL; }
    return j;
}
int lha_job_add(LhaJob *j, const char *dir, const char *name, char *err, size_t errlen)
{
    while (name[0] == '.' && name[1] == '/') name += 2;
    char *src = name[0] == '/' ? strdup(name) : join(dir, name);
    if (!src) { snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
    char *path = strdup(name);
    if (!path) { free(src); snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    int r = walk(j, src, path, err, errlen);
    free(src); free(path);
    return r;
}
int lha_job_start(LhaJob *j, char *err, size_t errlen)
{
    if (j->add && read_old(j, err, errlen) < 0) return -1;
    if (plan(j, err, errlen) < 0) return -1;
    size_t n = strlen(j->dest) + 16;
    if (!(j->temp = malloc(n))) { snprintf(err, errlen, "%s", T("メモリが足りません")); return -1; }
    snprintf(j->temp, n, "%s.tmpXXXXXX", j->dest);
    if ((j->fd = mkstemp(j->temp)) < 0) { free(j->temp); j->temp = NULL; snprintf(err, errlen, T("書庫を作れません: %s"), strerror(errno)); return -1; }
    mode_t mask = umask(0); umask(mask);
    fchmod(j->fd, j->dest_exists ? (j->dest_st.st_mode & 07777) : (0666 & ~mask));
    j->started = 1; j->phase = PH_IDLE;
    return 0;
}
void lha_job_free(LhaJob *j)
{
    if (!j) return;
    if (j->fd >= 0) close(j->fd);
    if (j->temp) unlink(j->temp);
    if (j->old_fd >= 0) close(j->old_fd);
    if (j->src_fd >= 0) close(j->src_fd);
    lh5_free(j->enc);
    for (size_t i = 0; i < j->entry_count; ++i) {
        Entry *e = &j->entries[i];
        free(e->src); free(e->path); free(e->name); free(e->dir);
    }
    for (size_t i = 0; i < j->old_count; ++i) free(j->old[i].path);
    free(j->entries); free(j->old); free(j->actions); free(j->temp); free(j->dest); free(j);
}

size_t lha_job_done(const LhaJob *j) { return j->done; }
size_t lha_job_total(const LhaJob *j) { return j->started ? j->action_count : j->entry_count; }
uint64_t lha_job_bytes_done(const LhaJob *j) { return j->bytes_done; }
uint64_t lha_job_bytes_total(const LhaJob *j) { return j->bytes_total; }
const char *lha_job_current(const LhaJob *j) { return j->current ? j->current : ""; }
void lha_job_stats(const LhaJob *j, LhaStats *s) { *s = j->stats; }
