// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "localsend.h"
#include "zipfile.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* LocalSend v2 sender. Network and JSON primitives live in localsend_transport.c and localsend_json.c.
 * File contents and zip producers always stream in bounded blocks. */
typedef LsSendFile SendFile;
typedef struct {
    char *relative, *name;
    int rootfd, directory;
    struct stat snapshot;
} ArchiveEntry;
typedef struct { ArchiveEntry *entries; size_t count, cap; } ArchiveList;

static void seterr(char *err, size_t n, const char *what)
{
    snprintf(err, n, "%s", what);
}

static char *join_path(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    if (na > SIZE_MAX - nb - 2) return NULL;
    char *s = malloc(na + nb + 2);
    if (s) snprintf(s, na + nb + 2, "%s%s%s", a, na && a[na - 1] != '/' ? "/" : "", b);
    return s;
}

static char *base_name(const char *path)
{
    char *copy = strdup(path);
    if (!copy) return NULL;
    size_t n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = 0;
    char *p = strrchr(copy, '/');
    char *name = strdup(p ? p + 1 : copy);
    free(copy);
    if (name && (!strcmp(name, ".") || !strcmp(name, ".."))) {
        char *resolved = realpath(path, NULL);
        free(name);
        name = NULL;
        if (resolved) {
            const char *last = strrchr(resolved, '/');
            if (last && last[1]) name = strdup(last + 1);
            free(resolved);
        }
    }
    if (name && !*name) { free(name); name = NULL; }
    return name;
}

static int same_file(const struct stat *a, const struct stat *b)
{
    return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}

/* Re-open each directory component without following links, so a directory
 * renamed to a symlink after enumeration cannot redirect a zip producer. */
static int archive_open(const ArchiveEntry *entry)
{
    char *relative = strdup(entry->relative);
    if (!relative) return -1;
    int current = dup(entry->rootfd), result = -1;
    char *save = NULL, *component = strtok_r(relative, "/", &save);
    while (current >= 0 && component) {
        char *next = strtok_r(NULL, "/", &save);
        int fd = openat(current, component, O_RDONLY | O_NOFOLLOW | (next ? O_DIRECTORY : 0));
        close(current);
        current = fd;
        if (!next) break;
        component = next;
    }
    if (current >= 0) {
        struct stat st;
        if (!fstat(current, &st) && same_file(&entry->snapshot, &st)) result = current;
        else close(current);
    }
    free(relative);
    return result;
}

static int archive_produce(void *ctx, ZipWriter *writer)
{
    ArchiveEntry *entry = ctx;
    int fd = archive_open(entry);
    if (fd < 0) return -1;
    unsigned char data[65536];
    uint64_t left = (uint64_t)entry->snapshot.st_size;
    int rc = 0;
    while (left) {
        size_t want = left < sizeof(data) ? (size_t)left : sizeof(data);
        ssize_t n = read(fd, data, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || zip_writer_write(writer, data, (size_t)n)) { rc = -1; break; }
        left -= (uint64_t)n;
    }
    struct stat st;
    if (fstat(fd, &st) || !same_file(&entry->snapshot, &st)) rc = -1;
    close(fd);
    return rc;
}

static void archive_free(ArchiveList *list)
{
    for (size_t i = 0; i < list->count; i++) {
        free(list->entries[i].relative);
        free(list->entries[i].name);
    }
    free(list->entries);
}

static int archive_add(ArchiveList *list, int rootfd, const char *relative,
                       const char *name, const struct stat *st, int directory)
{
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 32;
        if (cap < list->cap || cap > SIZE_MAX / sizeof(*list->entries)) return -1;
        ArchiveEntry *p = realloc(list->entries, cap * sizeof(*p));
        if (!p) return -1;
        list->entries = p;
        list->cap = cap;
    }
    ArchiveEntry e = {0};
    e.relative = strdup(relative);
    e.name = directory ? join_path(name, "") : strdup(name);
    if (!e.relative || !e.name) { free(e.relative); free(e.name); return -1; }
    e.rootfd = rootfd;
    e.directory = directory;
    e.snapshot = *st;
    list->entries[list->count++] = e;
    return 0;
}

static int archive_walk(ArchiveList *list, int rootfd, int dirfd, const char *relative,
                        const char *name, unsigned depth, char *err, size_t n)
{
    if (depth > 256) { seterr(err, n, "directory nesting is too deep"); return -1; }
    struct stat st;
    if (fstat(dirfd, &st) || archive_add(list, rootfd, relative, name, &st, 1)) {
        seterr(err, n, "cannot prepare ZIP directory"); return -1;
    }
    int fd = dup(dirfd);
    DIR *dir = fd < 0 ? NULL : fdopendir(fd);
    if (!dir) { if (fd >= 0) close(fd); seterr(err, n, "cannot read directory"); return -1; }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) { if (errno) rc = -1; break; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *rel = join_path(relative, de->d_name), *member = join_path(name, de->d_name);
        if (!rel || !member || fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) rc = -1;
        else if (S_ISDIR(st.st_mode)) {
            int child = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (child < 0) rc = -1;
            else { rc = archive_walk(list, rootfd, child, rel, member, depth + 1, err, n); close(child); }
        } else if (S_ISREG(st.st_mode) && st.st_size >= 0) {
            rc = archive_add(list, rootfd, rel, member, &st, 0);
        } else {
            seterr(err, n, "symbolic links and special files are not supported");
            rc = -1;
        }
        free(rel); free(member);
        if (rc) break;
    }
    closedir(dir);
    if (rc && !*err) seterr(err, n, "cannot prepare ZIP contents");
    return rc;
}

static char *temporary_archive(char *err, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home) { seterr(err, n, "HOME is not set"); return NULL; }
    char *base = join_path(home, ".fdtp"), *dir = base ? join_path(base, "localsend") : NULL;
    char *path = dir ? join_path(dir, "send-XXXXXX") : NULL;
    if (!base || !dir || !path) { seterr(err, n, "out of memory"); goto bad; }
    if ((mkdir(base, 0700) && errno != EEXIST) || (mkdir(dir, 0700) && errno != EEXIST)) {
        seterr(err, n, "cannot create temporary ZIP directory"); goto bad;
    }
    int fd = mkstemp(path);
    if (fd < 0) { seterr(err, n, "cannot create temporary ZIP"); goto bad; }
    close(fd);
    free(base); free(dir);
    return path;
bad:
    free(base); free(dir); free(path);
    return NULL;
}

static int make_archive(const char *path, SendFile *file, char *err, size_t n)
{
    int rootfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (rootfd < 0) { seterr(err, n, "cannot open directory"); return -1; }
    ArchiveList list = {0};
    ZipChange *changes = NULL;
    int rc = -1;
    if (archive_walk(&list, rootfd, rootfd, "", file->name, 0, err, n)) goto done;
    changes = calloc(list.count, sizeof(*changes));
    if (!changes) { seterr(err, n, "out of memory"); goto done; }
    for (size_t i = 0; i < list.count; i++) {
        changes[i].name = list.entries[i].name;
        changes[i].method = list.entries[i].directory ? ZIP_STORE : ZIP_DEFLATE;
        changes[i].produce = list.entries[i].directory ? NULL : archive_produce;
        changes[i].ctx = &list.entries[i];
    }
    file->temporary = temporary_archive(err, n);
    if (!file->temporary) goto done;
    if (zip_rewrite(NULL, file->temporary, changes, list.count, err, n)) {
        seterr(err, n, "cannot create directory ZIP");
        goto done;
    }
    size_t len = strlen(file->name);
    char *name = realloc(file->name, len + 5);
    if (!name) { seterr(err, n, "out of memory"); goto done; }
    file->name = name;
    memcpy(name + len, ".zip", 5);
    rc = 0;
done:
    free(changes); archive_free(&list); close(rootfd);
    return rc;
}

static int hash_file(SendFile *file, char *err, size_t n)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char data[65536], digest[EVP_MAX_MD_SIZE];
    unsigned digest_len = 0;
    int rc = -1;
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto done;
    uint64_t left = file->size;
    while (left) {
        size_t want = left < sizeof(data) ? (size_t)left : sizeof(data);
        ssize_t got = read(file->fd, data, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0 || EVP_DigestUpdate(ctx, data, (size_t)got) != 1) goto done;
        left -= (uint64_t)got;
    }
    struct stat st;
    if (fstat(file->fd, &st) || !same_file(&file->snapshot, &st) ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 32 ||
        lseek(file->fd, 0, SEEK_SET) < 0) goto done;
    for (unsigned i = 0; i < 32; i++) snprintf(file->sha256 + i * 2, 3, "%02x", digest[i]);
    rc = 0;
done:
    EVP_MD_CTX_free(ctx);
    if (rc) seterr(err, n, "source file changed or cannot be read");
    return rc;
}

const char *ls_file_type(const char *name)
{
    /* Common extensions use mime_guess 2.0.5's first MIME, matching the
     * official CLI. Unlisted extensions intentionally use octet-stream. */
    static const struct { const char *ext, *mime; } types[] = {
        {"txt", "text/plain"}, {"md", "text/x-markdown"}, {"csv", "text/csv"},
        {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"},
        {"json", "application/json"}, {"xml", "text/xml"}, {"pdf", "application/pdf"},
        {"zip", "application/zip"}, {"gz", "application/gzip"}, {"tar", "application/x-tar"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"},
        {"heic", "image/heic"}, {"bmp", "image/bmp"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
        {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"flac", "audio/flac"},
        {"ogg", "audio/ogg"}, {"m4a", "audio/m4a"}, {"mp4", "video/mp4"},
        {"mov", "video/quicktime"}, {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"}
    };
    const char *ext = strrchr(name, '.');
    if (ext) for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(ext + 1, types[i].ext)) return types[i].mime;
    return "application/octet-stream";
}

static int valid_fingerprint(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (size_t i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int resolve_peer(LsIdentity *id, const char *to, LsPeer *peer, char *err, size_t n)
{
    memset(peer, 0, sizeof(*peer));
    if (valid_fingerprint(to)) {
        int found = ls_discover(id, 3, 0, to, peer, err, n);
        if (found > 0) return 0;
        if (!found) seterr(err, n, "no peer found with the requested fingerprint");
        return -1;
    }
    char *host = strdup(to);
    if (!host) { seterr(err, n, "out of memory"); return -1; }
    int rc = -1;
    peer->port = id->port;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon++ = 0;
        char *end = NULL;
        errno = 0;
        long port = strtol(colon, &end, 10);
        if (!*colon || errno || *end || port < 1 || port > 65535) goto bad;
        peer->port = (int)port;
    }
    struct in_addr addr;
    if (inet_pton(AF_INET, host, &addr) != 1 || peer->port < 1 || peer->port > 65535) goto bad;
    snprintf(peer->ip, sizeof(peer->ip), "%s", host);
    snprintf(peer->protocol, sizeof(peer->protocol), "https");
    LsConn conn = {.fd = -1};
    LsBuf info = {0};
    ls_info(id, &info, -1);
    if (info.failed) { seterr(err, n, "out of memory"); ls_buf_free(&info); goto done; }
    if (ls_connect(id, peer->ip, peer->port, 1, NULL, &conn, err, n)) { ls_buf_free(&info); goto done; }
    /* The certificate is authoritative even if register's JSON claims another fingerprint. */
    snprintf(peer->fingerprint, sizeof(peer->fingerprint), "%s", conn.fingerprint);
    if (!valid_fingerprint(peer->fingerprint)) { seterr(err, n, "cannot verify peer certificate"); goto registered; }
    LsHttp response;
    if (ls_http_request(&conn, peer->ip, "/api/localsend/v2/register", "application/json", info.len) ||
        ls_write(&conn, info.s, info.len) || ls_http_head(&conn, &response)) {
        seterr(err, n, "cannot obtain peer information"); goto registered;
    }
    if (response.status != 200) { seterr(err, n, "peer refused registration"); goto registered; }
    char *body = ls_http_body(&conn, &response, 65536);
    LsJson *json = body ? ls_json_parse(body) : NULL;
    if (!json || json->kind != 'o') seterr(err, n, "invalid peer information");
    else rc = 0;
    ls_json_free(json); free(body);
registered:
    ls_close(&conn); ls_buf_free(&info);
    goto done;
bad:
    seterr(err, n, "destination must be a fingerprint or IPv4 address[:port]");
done:
    free(host);
    return rc;
}

static int peer_connect(LsIdentity *id, const LsPeer *peer, LsConn *conn, char *err, size_t n)
{
    int tls = !strcmp(peer->protocol, "https");
    if ((!tls && strcmp(peer->protocol, "http")) || (tls && !valid_fingerprint(peer->fingerprint))) {
        seterr(err, n, "invalid peer protocol or certificate"); return -1;
    }
    return ls_connect(id, peer->ip, peer->port, tls, tls ? peer->fingerprint : NULL, conn, err, n);
}

static void cancel_session(LsIdentity *id, const LsPeer *peer, const char *session)
{
    if (!session) return;
    char *encoded = ls_urlencode(session);
    LsBuf path = {0};
    if (!encoded) return;
    ls_buf_add(&path, "/api/localsend/v2/cancel?sessionId=%s", encoded);
    LsConn conn = {.fd = -1};
    char err[256];
    if (!path.failed && path.len < sizeof(((LsHttp *)0)->path) &&
        !peer_connect(id, peer, &conn, err, sizeof(err))) {
        if (!ls_http_request(&conn, peer->ip, path.s, "application/octet-stream", 0)) {
            LsHttp response;
            (void)ls_http_head(&conn, &response);
        }
        ls_close(&conn);
    }
    free(encoded); ls_buf_free(&path);
}

static LsJson *unique_field(const LsJson *object, const char *key)
{
    LsJson *found = NULL;
    if (!object || object->kind != 'o') return NULL;
    for (LsJson *j = object->child; j; j = j->next) {
        if (j->key && !strcmp(j->key, key)) {
            if (found) return NULL;
            found = j;
        }
    }
    return found;
}

static int accept_tokens(LsJson *json, SendFile *files, int count, char **session, char *err, size_t n)
{
    LsJson *sid = unique_field(json, "sessionId"), *map = unique_field(json, "files");
    if (!sid || sid->kind != 's' || !sid->text || !*sid->text || strlen(sid->text) > 512) goto bad;
    *session = strdup(sid->text);
    if (!*session) { seterr(err, n, "out of memory"); return -1; }
    if (!map || map->kind != 'o') goto bad;
    for (LsJson *token = map->child; token; token = token->next) {
        if (!token->key || token->kind != 's' || !token->text || !*token->text || strlen(token->text) > 512) goto bad;
        int i;
        for (i = 0; i < count; i++) if (!strcmp(files[i].id, token->key)) break;
        if (i == count || files[i].token) goto bad;
        files[i].token = strdup(token->text);
        if (!files[i].token) { seterr(err, n, "out of memory"); return -1; }
    }
    return 0;
bad:
    seterr(err, n, "invalid prepare-upload session or file token mapping");
    return -1;
}

static int upload_file(LsIdentity *id, const LsPeer *peer, const char *session, SendFile *file,
                       char *err, size_t n, const char **code)
{
    char *sid = ls_urlencode(session), *fid = ls_urlencode(file->id), *token = ls_urlencode(file->token);
    LsBuf path = {0};
    LsConn conn = {.fd = -1};
    int rc = -1;
    if (!sid || !fid || !token) { seterr(err, n, "out of memory"); *code = "io"; goto done; }
    ls_buf_add(&path, "/api/localsend/v2/upload?sessionId=%s&fileId=%s&token=%s", sid, fid, token);
    if (path.failed || path.len >= sizeof(((LsHttp *)0)->path)) { seterr(err, n, "upload parameters are too long"); *code = "protocol"; goto done; }
    struct stat st;
    if (fstat(file->fd, &st) || !same_file(&file->snapshot, &st) || lseek(file->fd, 0, SEEK_SET) < 0) {
        seterr(err, n, "source file changed or cannot be read"); *code = "io"; goto done;
    }
    if (peer_connect(id, peer, &conn, err, n)) goto done;
    if (ls_http_request(&conn, peer->ip, path.s, "application/octet-stream", file->size)) {
        seterr(err, n, "cannot start upload"); goto done;
    }
    char field[1024];
    ls_field(field, sizeof(field), file->name);
    printf("file\t%s\t%" PRIu64 "\n", field, file->size);
    printf("progress\t%s\t0\t%" PRIu64 "\n", field, file->size);
    fflush(stdout);
    unsigned char data[65536];
    uint64_t sent = 0, last_report = 0;
    while (sent < file->size) {
        uint64_t left = file->size - sent;
        size_t want = left < sizeof(data) ? (size_t)left : sizeof(data);
        ssize_t got = read(file->fd, data, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { seterr(err, n, "cannot read source file"); *code = "io"; goto done; }
        if (ls_write(&conn, data, (size_t)got)) { seterr(err, n, "file upload was interrupted"); goto done; }
        sent += (uint64_t)got;
        if (sent == file->size || sent - last_report >= 1048576) {
            printf("progress\t%s\t%" PRIu64 "\t%" PRIu64 "\n", field, sent, file->size);
            fflush(stdout);
            last_report = sent;
        }
    }
    LsHttp response;
    if (ls_http_head(&conn, &response)) { seterr(err, n, "cannot obtain upload response"); goto done; }
    if (response.status != 200) {
        snprintf(err, n, "peer rejected file upload (HTTP %d)", response.status);
        *code = response.status == 403 || response.status == 409 ? "refused" : "protocol";
        goto done;
    }
    rc = 0;
done:
    ls_close(&conn); free(sid); free(fid); free(token); ls_buf_free(&path);
    return rc;
}

void ls_files_free(LsSendFile *files, int count)
{
    if (!files) return;
    for (int i = 0; i < count; i++) {
        if (files[i].fd >= 0) close(files[i].fd);
        if (files[i].temporary) unlink(files[i].temporary);
        free(files[i].temporary); free(files[i].name); free(files[i].source); free(files[i].token);
    }
    free(files);
}

LsSendFile *ls_files_open(char **paths, int count, int hash, char *err, size_t n)
{
    if (!paths || count <= 0) { seterr(err, n, "no source files specified"); return NULL; }
    SendFile *files = calloc((size_t)count, sizeof(*files));
    if (!files) { seterr(err, n, "out of memory"); return NULL; }
    for (int i = 0; i < count; i++) files[i].fd = -1;
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (!paths[i] || !*paths[i]) { seterr(err, n, "cannot inspect source file"); goto fail; }
        files[i].source = strdup(paths[i]);
        if (!files[i].source) { seterr(err, n, "out of memory"); goto fail; }
        size_t source_len = strlen(files[i].source);
        while (source_len > 1 && files[i].source[source_len - 1] == '/') files[i].source[--source_len] = 0;
        if (lstat(files[i].source, &st)) { seterr(err, n, "cannot inspect source file"); goto fail; }
        files[i].name = base_name(files[i].source);
        if (!files[i].name) { seterr(err, n, "cannot determine source file name"); goto fail; }
        if (S_ISDIR(st.st_mode)) {
            if (make_archive(files[i].source, &files[i], err, n)) goto fail;
        } else if (!S_ISREG(st.st_mode)) {
            seterr(err, n, "symbolic links and special files are not supported"); goto fail;
        }
        files[i].fd = open(files[i].temporary ? files[i].temporary : files[i].source, O_RDONLY | O_NOFOLLOW);
        if (files[i].fd < 0 || fstat(files[i].fd, &files[i].snapshot) ||
            !S_ISREG(files[i].snapshot.st_mode) || files[i].snapshot.st_size < 0) {
            seterr(err, n, "cannot open source file"); goto fail;
        }
        files[i].size = (uint64_t)files[i].snapshot.st_size;
        if (ls_random(files[i].id, 16) || (hash && hash_file(&files[i], err, n))) {
            if (!*err) seterr(err, n, "cannot generate file ID");
            goto fail;
        }
    }
    return files;
fail:
    ls_files_free(files, count);
    return NULL;
}

int ls_send(LsIdentity *id, const char *to, const char *pin, char **paths, int count)
{
    char err[512] = "";
    const char *code = "io";
    SendFile *files = NULL;
    LsPeer peer;
    LsBuf body = {0}, request_path = {0};
    LsConn conn = {.fd = -1};
    char *encoded_pin = NULL, *session = NULL, *response_body = NULL;
    LsJson *response_json = NULL;
    int rc = -1;
    if (!id || !to || !*to || !paths || count <= 0) return ls_error("protocol", "no source files specified");
    files = ls_files_open(paths, count, 1, err, sizeof(err));
    if (!files) goto done;
    code = "network";
    if (resolve_peer(id, to, &peer, err, sizeof(err))) goto done;
    ls_buf_add(&body, "{\"info\":");
    ls_info(id, &body, -1);
    ls_buf_add(&body, ",\"files\":{");
    for (int i = 0; i < count; i++) {
        ls_buf_add(&body, "%s\"%s\":{\"id\":\"%s\",\"fileName\":", i ? "," : "", files[i].id, files[i].id);
        ls_buf_quote(&body, files[i].name);
        ls_buf_add(&body, ",\"size\":%" PRIu64 ",\"fileType\":", files[i].size);
        ls_buf_quote(&body, ls_file_type(files[i].name));
        ls_buf_add(&body, ",\"sha256\":\"%s\"}", files[i].sha256);
    }
    ls_buf_add(&body, "}}");
    ls_buf_add(&request_path, "/api/localsend/v2/prepare-upload");
    if (pin) {
        encoded_pin = ls_urlencode(pin);
        if (!encoded_pin) { code = "io"; seterr(err, sizeof(err), "out of memory"); goto done; }
        ls_buf_add(&request_path, "?pin=%s", encoded_pin);
    }
    if (body.failed || request_path.failed || body.len > 1048576 || request_path.len >= sizeof(((LsHttp *)0)->path)) {
        code = "protocol"; seterr(err, sizeof(err), "transfer metadata is too large"); goto done;
    }
    if (peer_connect(id, &peer, &conn, err, sizeof(err))) goto done;
    if (ls_http_request(&conn, peer.ip, request_path.s, "application/json", body.len) ||
        ls_write(&conn, body.s, body.len)) { seterr(err, sizeof(err), "cannot send prepare-upload request"); goto done; }
    LsHttp response;
    if (ls_http_head(&conn, &response)) { seterr(err, sizeof(err), "cannot obtain prepare-upload response"); goto done; }
    if (response.status == 401) {
        printf("pin\t%s\n", pin ? "wrong" : "required"); fflush(stdout);
        code = "pin"; seterr(err, sizeof(err), pin ? "incorrect PIN" : "peer PIN is required"); goto done;
    }
    if (response.status == 204) { rc = 0; goto done; }
    if (response.status != 200) {
        code = response.status == 403 || response.status == 409 || response.status == 429 ? "refused" : "protocol";
        snprintf(err, sizeof(err), "peer rejected transfer (HTTP %d)", response.status); goto done;
    }
    response_body = ls_http_body(&conn, &response, 1048576);
    ls_close(&conn);
    response_json = response_body ? ls_json_parse(response_body) : NULL;
    code = "protocol";
    if (!response_json || accept_tokens(response_json, files, count, &session, err, sizeof(err))) {
        if (!*err) seterr(err, sizeof(err), "invalid prepare-upload response");
        goto done;
    }
    for (int i = 0; i < count; i++) {
        if (!files[i].token) continue; /* A receiver may accept only a subset. */
        code = "network";
        if (upload_file(id, &peer, session, &files[i], err, sizeof(err), &code)) goto done;
    }
    rc = 0;
done:
    ls_close(&conn);
    if (rc && session) cancel_session(id, &peer, session);
    ls_files_free(files, count); free(session); free(encoded_pin); free(response_body);
    ls_json_free(response_json); ls_buf_free(&body); ls_buf_free(&request_path);
    if (rc) return ls_error(code, *err ? err : "transfer failed");
    puts("done"); fflush(stdout);
    return 0;
}
