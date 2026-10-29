// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_LOCALSEND_H
#define FDTP_LOCALSEND_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <openssl/ssl.h>

typedef struct LsJson {
    int kind; /* 'o', 'a', 's', 'n', 't', 'f', '0' */
    char *key, *text;
    struct LsJson *child, *next;
} LsJson;
LsJson *ls_json_parse(const char *s);
void ls_json_free(LsJson *j);
LsJson *ls_json_get(const LsJson *j, const char *key);
const char *ls_json_str(const LsJson *j, const char *key);
int ls_json_u64(const LsJson *j, const char *key, uint64_t *value);
typedef struct { char *s; size_t len, cap; int failed; } LsBuf;
void ls_buf_add(LsBuf *b, const char *fmt, ...);
void ls_buf_quote(LsBuf *b, const char *s);
void ls_buf_free(LsBuf *b);

typedef struct {
    SSL_CTX *server, *client;
    char fingerprint[65], alias[256];
    int port;
    /* The multicast group's port; 0: port (the official apps use one number). */
    int multicast_port;
    int download; /* plain HTTP browser share, independent of receive */
} LsIdentity;
typedef struct { int fd; SSL *ssl; char fingerprint[65]; } LsConn;
typedef struct {
    char method[16], path[4096];
    int status, chunked;
    uint64_t length;
    int japanese; /* Accept-Language starts with ja */
} LsHttp;
int ls_identity_init(LsIdentity *id, const char *alias, int port, char *err, size_t n);
void ls_identity_free(LsIdentity *id);
/* Status routines return 0 on success, -1 on failure; listen returns an fd.
 * accept owns an already accepted socket, including on failure. */
int ls_listen(int port, char *err, size_t n);
int ls_accept(LsIdentity *id, int fd, LsConn *c, char *ip, size_t iplen);
int ls_connect(LsIdentity *id, const char *host, int port, int tls,
               const char *fingerprint, LsConn *c, char *err, size_t n);
ssize_t ls_read(LsConn *c, void *p, size_t n);
int ls_write(LsConn *c, const void *p, size_t n);
void ls_close(LsConn *c);
/* ls_close for accepted connections: drains the peer first (no reset). */
void ls_close_server(LsConn *c);
int ls_http_head(LsConn *c, LsHttp *h);
/* Reads a complete bounded JSON body, including chunked transfer coding. */
char *ls_http_body(LsConn *c, const LsHttp *h, size_t limit);
int ls_http_reply(LsConn *c, int status, const char *json);
int ls_http_request(LsConn *c, const char *host, const char *path,
                    const char *type, uint64_t length);
void ls_info(LsIdentity *id, LsBuf *b, int announce); /* -1: no announce member */
void ls_field(char *dst, size_t n, const char *src); /* safe TSV field */
int ls_error(const char *code, const char *message);
int ls_random(char *hex, size_t bytes);
char *ls_urlencode(const char *s);
int ls_query(const char *path, const char *key, char *value, size_t cap);

typedef struct {
    char fingerprint[65], alias[256], ip[64], protocol[8], type[32], model[256];
    int port;
} LsPeer;
/* Discovery runs its own temporary HTTPS listener; prints only when emit != 0.
 * Returns 1 if wanted was found, 0 otherwise, -1 on error. */
int ls_discover(LsIdentity *id, int seconds, int emit, const char *wanted,
                LsPeer *out, char *err, size_t n);
int ls_send(LsIdentity *id, const char *to, const char *pin, char **paths, int count);
int ls_receive(LsIdentity *id, const char *dir, const char *pin, int ask);
int ls_serve(LsIdentity *id, const char *pin, char **paths, int count);

/* Open only command-line sources once; directories become private ZIPs.
 * Each download uses pread, so simultaneous readers have independent offsets. */
typedef struct {
    char *name, *source, *temporary, *token;
    int fd;
    uint64_t size;
    struct stat snapshot;
    char id[33], sha256[65];
} LsSendFile;
LsSendFile *ls_files_open(char **paths, int count, int hash, char *err, size_t n);
void ls_files_free(LsSendFile *files, int count);
const char *ls_file_type(const char *name);
typedef struct LsShare LsShare;
LsShare *ls_share_new(LsIdentity *id, const char *pin, char **paths, int count, char *err, size_t n);
void ls_share_free(LsShare *share);
void ls_share_handle(LsShare *share, LsConn *c, const LsHttp *h, const char *ip);
int ls_share_stopped(LsShare *share);
#endif
