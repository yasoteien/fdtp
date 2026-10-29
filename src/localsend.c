// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "localsend.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 1024
#define MAX_JOBS 32
#define MAX_PEERS 256
#define JSON_LIMIT (1024 * 1024)
#define GROUP "224.0.0.167"
/* An accepted sender that sends nothing this long is given up. */
#define SESSION_IDLE 30.0

typedef struct {
    char *id, *name;
    char token[65], sha[65];
    uint64_t size;
    int state;
} Incoming;
typedef struct {
    char id[65], ip[64], fingerprint[65];
    Incoming *files;
    size_t count, refs;
    int cancelled, active, uploading;
    double touched;   /* the last sign of the sender (prepare, upload data) */
} Session;
typedef struct {
    LsIdentity *id;
    int listener, udp, dirfd, receive, ask, emit, stop, failures;
    char *dir;
    const char *pin;
    pthread_mutex_t lock;
    pthread_cond_t idle;
    int jobs, sockets[MAX_JOBS];
    Session *session;
    LsShare *share;
    LsPeer peers[MAX_PEERS];
    size_t peer_count;
    struct in_addr interfaces[64];
    size_t interface_count;
    int multicast_port;
} Server;
typedef struct { Server *s; int slot, fd; LsPeer peer; int probe; } Job;

static volatile sig_atomic_t interrupted;
static void interrupt_handler(int sig) { (void)sig; interrupted = 1; }
static int loopback(void)
{
    const char *p = getenv("FDTP_LOCALSEND_LOOPBACK");
    return p && !strcmp(p, "1");
}
static void fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}
static double session_idle(void)
{
    /* FDTP_LOCALSEND_IDLE (seconds) shortens it for the tests. */
    const char *v = getenv("FDTP_LOCALSEND_IDLE");
    double d = v && *v ? atof(v) : 0;
    return d > 0 ? d : SESSION_IDLE;
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void session_free(Session *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->count; ++i) { free(s->files[i].id); free(s->files[i].name); }
    free(s->files); free(s);
}
/* Caller holds the server lock. */
static void release_session(Session *s)
{
    if (s && !--s->refs) session_free(s);
}
/* why: NULL when this side refused (no word), else peer, timeout or failed
 * for the cancelled line. */
static void cancel_session(Server *s, const char *why)
{
    if (s->session) {
        if (why) printf("cancelled\t%s\n", why);
        s->session->cancelled = 1;
        release_session(s->session); s->session = NULL;
    }
}
static int hex64(const char *p)
{
    if (!p || strlen(p) != 64) return 0;
    for (; *p; ++p) if (!isxdigit((unsigned char)*p)) return 0;
    return 1;
}
int ls_query(const char *path, const char *key, char *value, size_t cap)
{
    const char *p = strchr(path, '?');
    int found = 0;
    if (!p || !cap) return 0;
    ++p;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == strlen(key) && !memcmp(p, key, strlen(key))) {
            if (found++) return -1;
            size_t i = 0;
            for (const char *v = eq + 1; v < end; ++v) {
                unsigned char c = (unsigned char)*v;
                if (c == '%') {
                    if (end - v < 3 || !isxdigit((unsigned char)v[1]) || !isxdigit((unsigned char)v[2])) return -1;
                    char tmp[3] = {v[1], v[2], 0};
                    c = (unsigned char)strtoul(tmp, NULL, 16); v += 2;
                } else if (c == '+') c = ' ';
                if (!c || c < 32 || c == 127 || i + 1 >= cap) return -1;
                value[i++] = (char)c;
            }
            value[i] = 0;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}
static char *safe_name(const char *name)
{
    /* Leave room for a collision suffix within NAME_MAX. */
    char clean[221]; size_t n = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        unsigned char c = *p++;
        if (c < 32 || c == 127 || c == '/' || c == '\\') continue;
        if (c == '.' && *p == '.') { ++p; continue; }
        if (c < 0x80) { if (n < 220) clean[n++] = (char)c; }
        else {
            if (c == 0xc2 && *p >= 0x80 && *p <= 0x9f) { ++p; continue; }
            /* A broken sequence (a stray byte, or cut at the end) is dropped. */
            size_t width = c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c < 0xf5 ? 4 : 0, k = 1;
            while (k < width && (p[k - 1] & 0xc0) == 0x80) ++k;
            if (!width || k < width) continue;
            if (n + width <= 220) { clean[n++] = (char)c; memcpy(clean + n, p, width - 1); n += width - 1; }
            p += width - 1;
        }
    }
    clean[n] = 0;
    if (!n || !strcmp(clean, ".") || !strcmp(clean, "..")) strcpy(clean, "file");
    return strdup(clean);
}
static int peer_parse(const LsJson *j, const char *ip, const char *fp, LsPeer *p)
{
    const char *alias = ls_json_str(j, "alias"), *protocol = ls_json_str(j, "protocol");
    const char *fingerprint = fp && *fp ? fp : ls_json_str(j, "fingerprint");
    uint64_t port;
    if (!j || j->kind != 'o' || !alias || !fingerprint || !*fingerprint ||
        ls_json_u64(j, "port", &port) || !port || port > 65535 ||
        !protocol || (strcmp(protocol, "http") && strcmp(protocol, "https"))) return -1;
    memset(p, 0, sizeof(*p));
    ls_field(p->alias, sizeof(p->alias), alias);
    ls_field(p->fingerprint, sizeof(p->fingerprint), fingerprint);
    ls_field(p->ip, sizeof(p->ip), ip);
    ls_field(p->protocol, sizeof(p->protocol), protocol);
    const char *type = ls_json_str(j, "deviceType"), *model = ls_json_str(j, "deviceModel");
    ls_field(p->type, sizeof(p->type), type ? type : "desktop");
    ls_field(p->model, sizeof(p->model), model ? model : "");
    p->port = (int)port;
    if (!strcmp(protocol, "https") && !hex64(p->fingerprint)) return -1;
    return 0;
}
static void peer_add(Server *s, const LsPeer *p)
{
    if (!strcasecmp(p->fingerprint, s->id->fingerprint)) return;
    pthread_mutex_lock(&s->lock);
    size_t i;
    for (i = 0; i < s->peer_count; ++i)
        if (!strcasecmp(s->peers[i].fingerprint, p->fingerprint)) break;
    int fresh = i == s->peer_count;
    if (i < MAX_PEERS) {
        s->peers[i] = *p;
        if (fresh) ++s->peer_count;
        if (s->emit && fresh)
            printf("peer\t%s\t%s\t%s\t%d\t%s\t%s\t%s\n", p->fingerprint,
                   p->alias, p->ip, p->port, p->protocol, p->type, p->model);
    }
    pthread_mutex_unlock(&s->lock);
}
static int pin_check(Server *s, const char *path)
{
    char pin[64] = "";
    if (!s->pin) return 0;   /* --no-pin: the tests with the official CLI, which cannot send one */
    int present = ls_query(path, "pin", pin, sizeof(pin));
    if (present == 1 && !strcmp(pin, s->pin)) return 0;
    pthread_mutex_lock(&s->lock);
    if (present) {
        puts("pin\twrong");
        ++s->failures;
    } else puts("pin\trequired");
    pthread_mutex_unlock(&s->lock);
    return -1;
}
static int ask_decision(Server *s, LsConn *c, const char *alias, Session *session)
{
    uint64_t total = 0;
    for (size_t i = 0; i < session->count; ++i) {
        if (UINT64_MAX - total < session->files[i].size) return 0;
        total += session->files[i].size;
    }
    char clean[256]; ls_field(clean, sizeof(clean), alias);
    /* An answer typed after an earlier request was given up is not this one's. */
    for (char drop[64]; ; ) {
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0 || read(STDIN_FILENO, drop, sizeof(drop)) <= 0) break;
    }
    printf("request\t%s\t%zu\t%" PRIu64 "\n", clean, session->count, total);
    char line[16]; size_t used = 0;
    while (!interrupted) {
        pthread_mutex_lock(&s->lock);
        int stop = s->stop || session->cancelled;
        pthread_mutex_unlock(&s->lock);
        if (stop) return 0;
        /* The sender waits for the answer without a word: anything from
         * it (its close_notify, the end of the stream) means it left. */
        struct pollfd p[2] = {{STDIN_FILENO, POLLIN, 0}, {c->fd, POLLIN, 0}};
        if (poll(p, 2, 100) <= 0) continue;
        if (p[1].revents) return -1;
        if (!(p[0].revents & POLLIN)) continue;
        char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) return 0;
        if (ch == '\n') { line[used] = 0; return !strcmp(line, "accept"); }
        if (ch != '\r') {
            if (used + 1 >= sizeof(line)) return 0;
            line[used++] = ch;
        }
    }
    return 0;
}
static Session *parse_session(const LsJson *json, const char *ip, const char *fp)
{
    LsJson *files = ls_json_get(json, "files"), *info = ls_json_get(json, "info");
    if (!files || files->kind != 'o' || !files->child || !info || info->kind != 'o' ||
        !ls_json_str(info, "alias")) return NULL;
    size_t count = 0;
    for (LsJson *f = files->child; f; f = f->next) if (++count > MAX_FILES) return NULL;
    Session *ss = calloc(1, sizeof(*ss));
    if (!ss) return NULL;
    ss->refs = 1; ss->count = count; ss->touched = now();
    ss->files = calloc(count, sizeof(*ss->files));
    if (!ss->files) { free(ss); return NULL; }
    ls_field(ss->ip, sizeof(ss->ip), ip);
    ls_field(ss->fingerprint, sizeof(ss->fingerprint), fp);
    if (ls_random(ss->id, 32)) { session_free(ss); return NULL; }
    size_t i = 0; uint64_t total = 0;
    for (LsJson *f = files->child; f; f = f->next, ++i) {
        Incoming *file = &ss->files[i];
        const char *name = ls_json_str(f, "fileName"), *id = ls_json_str(f, "id");
        LsJson *sha = ls_json_get(f, "sha256");
        if (f->kind != 'o' || !f->key || !*f->key || strlen(f->key) > 512 ||
            !id || strcmp(id, f->key) || !name || strlen(name) > 4096 ||
            ls_json_u64(f, "size", &file->size) || file->size > INT64_MAX ||
            total > UINT64_MAX - file->size ||
            (sha && sha->kind != '0' && (sha->kind != 's' || !hex64(sha->text)))) {
            session_free(ss); return NULL;
        }
        total += file->size;
        file->id = strdup(f->key); file->name = safe_name(name);
        if (sha && sha->kind == 's') memcpy(file->sha, sha->text, 65);
        if (!file->id || !file->name || ls_random(file->token, 32)) { session_free(ss); return NULL; }
    }
    return ss;
}
static void prepare(Server *s, LsConn *c, const LsHttp *h, const char *ip)
{
    char *body = ls_http_body(c, h, JSON_LIMIT);
    if (pin_check(s, h->path)) {
        free(body);
        ls_http_reply(c, 401, NULL);
        pthread_mutex_lock(&s->lock);
        if (s->failures >= 5) s->stop = 1;
        pthread_mutex_unlock(&s->lock);
        return;
    }
    pthread_mutex_lock(&s->lock);
    int busy = s->session != NULL;
    pthread_mutex_unlock(&s->lock);
    if (busy) { free(body); ls_http_reply(c, 409, NULL); return; }
    LsJson *j = body ? ls_json_parse(body) : NULL;
    free(body);
    Session *ss = j ? parse_session(j, ip, c->fingerprint) : NULL;
    if (!ss) { ls_json_free(j); ls_http_reply(c, 400, NULL); return; }
    pthread_mutex_lock(&s->lock);
    if (s->session || s->stop) {
        pthread_mutex_unlock(&s->lock); session_free(ss); ls_json_free(j);
        ls_http_reply(c, 409, NULL); return;
    }
    s->session = ss; ++ss->refs;
    pthread_mutex_unlock(&s->lock);
    int accepted = !s->ask ? 1 : ask_decision(s, c, ls_json_str(ls_json_get(j, "info"), "alias"), ss);
    ls_json_free(j);
    LsBuf b = {0};
    pthread_mutex_lock(&s->lock);
    if (accepted <= 0 || ss->cancelled) {
        if (s->session == ss) cancel_session(s, accepted < 0 ? "peer" : NULL);
        release_session(ss); pthread_mutex_unlock(&s->lock);
        ls_http_reply(c, 403, NULL); return;
    }
    ss->active = 1; ss->touched = now();
    ls_buf_add(&b, "{\"sessionId\":"); ls_buf_quote(&b, ss->id);
    ls_buf_add(&b, ",\"files\":{");
    for (size_t i = 0; i < ss->count; ++i) {
        if (i) ls_buf_add(&b, ",");
        ls_buf_quote(&b, ss->files[i].id); ls_buf_add(&b, ":"); ls_buf_quote(&b, ss->files[i].token);
    }
    ls_buf_add(&b, "}}");
    pthread_mutex_unlock(&s->lock);
    int result = b.failed ? -1 : ls_http_reply(c, 200, b.s);
    ls_buf_free(&b);
    pthread_mutex_lock(&s->lock);
    if (result && s->session == ss) cancel_session(s, "peer");
    release_session(ss); pthread_mutex_unlock(&s->lock);
}
static int alive(Server *s, Session *ss)
{
    pthread_mutex_lock(&s->lock);
    int ok = !s->stop && !ss->cancelled && !interrupted;
    pthread_mutex_unlock(&s->lock);
    return ok;
}
static int read_exact(LsConn *c, void *p, size_t n)
{
    unsigned char *b = p;
    while (n) {
        ssize_t r = ls_read(c, b, n);
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}
static int chunk_length(LsConn *c, uint64_t *length)
{
    char line[128]; size_t n = 0;
    for (;;) {
        if (n + 1 >= sizeof(line) || read_exact(c, line + n, 1)) return -1;
        unsigned char ch = (unsigned char)line[n++];
        if (ch == '\n') break;
        if (!ch || (ch < 32 && ch != '\r') || ch == 127) return -1;
    }
    if (n < 3 || line[n - 2] != '\r') return -1;
    line[n - 2] = 0;
    char *semi = strchr(line, ';');
    if (semi) *semi = 0;
    if (!*line) return -1;
    for (char *p = line; *p; ++p) if (!isxdigit((unsigned char)*p)) return -1;
    char *end; errno = 0;
    *length = strtoull(line, &end, 16);
    return errno || *end ? -1 : 0;
}
static int write_file(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int save_body(Server *s, Session *ss, Incoming *f, LsConn *c, const LsHttp *h, int fd)
{
    if (!h->chunked && h->length > f->size) return 413;
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    if (!hash || EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1) { EVP_MD_CTX_free(hash); return 500; }
    unsigned char buf[65536]; uint64_t done = 0, left = h->length;
    int result = 200;
    printf("file\t%s\t%" PRIu64 "\n", f->name, f->size);
    do {
        if (h->chunked) {
            if (chunk_length(c, &left)) { result = 400; break; }
            if (!left) {
                char crlf[2];
                if (read_exact(c, crlf, 2) || memcmp(crlf, "\r\n", 2)) result = 400;
                break;
            }
            if (left > f->size - done) { result = 413; break; }
        }
        while (left) {
            if (!alive(s, ss)) { result = 400; break; }
            size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
            ssize_t r = ls_read(c, buf, want);
            if (r <= 0) { result = 400; break; }
            if (write_file(fd, buf, (size_t)r) || EVP_DigestUpdate(hash, buf, (size_t)r) != 1) {
                result = 500; break;
            }
            left -= (uint64_t)r; done += (uint64_t)r;
            pthread_mutex_lock(&s->lock); ss->touched = now(); pthread_mutex_unlock(&s->lock);
            printf("progress\t%s\t%" PRIu64 "\t%" PRIu64 "\n", f->name, done, f->size);
        }
        if (result != 200) break;
        if (h->chunked) {
            char crlf[2];
            if (read_exact(c, crlf, 2) || memcmp(crlf, "\r\n", 2)) { result = 400; break; }
        }
    } while (h->chunked);
    if (result == 200 && done != f->size) result = 400;
    if (result == 200 && f->sha[0]) {
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int size = 0; char hex[65];
        if (EVP_DigestFinal_ex(hash, digest, &size) != 1 || size != 32) result = 500;
        else {
            for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcasecmp(hex, f->sha)) result = 422;
        }
    }
    EVP_MD_CTX_free(hash);
    return result;
}
static int publish(Server *s, const char *temp, const char *name, char *out, size_t cap)
{
    const char *ext = strrchr(name, '.');
    if (ext == name) ext = NULL;
    for (unsigned i = 1; i < 100000; ++i) {
        int n;
        if (i == 1) n = snprintf(out, cap, "%s", name);
        else if (ext) n = snprintf(out, cap, "%.*s (%u)%s", (int)(ext - name), name, i, ext);
        else n = snprintf(out, cap, "%s (%u)", name, i);
        if (n < 0 || (size_t)n >= cap) return -1;
        if (!linkat(s->dirfd, temp, s->dirfd, out, 0)) return 0;
        if (errno == EEXIST) continue;
        /* FAT (the Pomera's internal memory) has no hard links. The caller
         * holds s->lock, so nothing of ours takes the name in between. */
        struct stat st;
        if (!fstatat(s->dirfd, out, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (errno != ENOENT) return -1;
        if (!renameat(s->dirfd, temp, s->dirfd, out)) return 0;
        return -1;
    }
    return -1;
}
static void reject_upload(LsConn *c, const LsHttp *h, int status)
{
    char *body = ls_http_body(c, h, JSON_LIMIT);
    free(body);
    ls_http_reply(c, status, NULL);
}
static void upload(Server *s, LsConn *c, const LsHttp *h, const char *ip)
{
    char sid[128], fid[1024], token[128];
    if (ls_query(h->path, "sessionId", sid, sizeof(sid)) != 1 ||
        ls_query(h->path, "fileId", fid, sizeof(fid)) != 1 || ls_query(h->path, "token", token, sizeof(token)) != 1) {
        reject_upload(c, h, 400); return;
    }
    pthread_mutex_lock(&s->lock);
    Session *ss = s->session; Incoming *f = NULL;
    int status = 403;
    if (ss && !strcmp(ss->id, sid) && !strcmp(ss->ip, ip) &&
        !strcasecmp(ss->fingerprint, c->fingerprint)) {
        for (size_t i = 0; i < ss->count; ++i)
            if (!strcmp(ss->files[i].id, fid)) { f = &ss->files[i]; break; }
        if (f && !strcmp(f->token, token)) {
            if (f->state) status = 409;
            else { status = 200; f->state = 1; ++ss->refs; ++ss->uploading; ss->touched = now(); }
        }
    }
    pthread_mutex_unlock(&s->lock);
    if (status != 200) { reject_upload(c, h, status); return; }
    char random[33], temp[80] = "", saved[256];
    int fd = -1;
    if (ls_random(random, 16)) status = 500;
    else {
        snprintf(temp, sizeof(temp), ".fdtp-localsend-%s.part", random);
        fd = openat(s->dirfd, temp, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0666);   /* the umask decides, as for any new file */
        status = fd < 0 ? 500 : save_body(s, ss, f, c, h, fd);
    }
    if (fd >= 0 && close(fd)) status = 500;
    pthread_mutex_lock(&s->lock);
    --ss->uploading; ss->touched = now();
    if (ss->cancelled || s->stop || interrupted) status = 400;
    if (status == 200 && publish(s, temp, f->name, saved, sizeof(saved))) status = 500;
    if (*temp) unlinkat(s->dirfd, temp, 0);
    if (status == 200) {
        f->state = 2;
        char path[PATH_MAX], field[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", s->dir, saved); ls_field(field, sizeof(field), path);
        printf("saved\t%s\n", field);
        size_t complete = 0;
        for (size_t i = 0; i < ss->count; ++i) if (ss->files[i].state == 2) ++complete;
        if (complete == ss->count && s->session == ss) {
            s->session = NULL; release_session(ss); puts("done");
        }
    } else if (s->session == ss) cancel_session(s, "failed");
    release_session(ss);
    pthread_mutex_unlock(&s->lock);
    ls_http_reply(c, status, NULL);
}
static void handle(Server *s, LsConn *c, const char *ip)
{
    LsHttp h;
    if (ls_http_head(c, &h)) {
        if (getenv("FDTP_LOCALSEND_DEBUG")) fprintf(stderr, "debug\t%s\tbad request head\n", ip);
        ls_http_reply(c, 400, NULL); return;
    }
    if (getenv("FDTP_LOCALSEND_DEBUG"))
        fprintf(stderr, "debug\t%s\t%s %.*s\tlength %lld%s\n", ip, h.method,
                (int)strcspn(h.path, "?"), h.path, (long long)h.length, h.chunked ? " chunked" : "");
    if (s->share) { ls_share_handle(s->share, c, &h, ip); return; }
    char route[4096]; snprintf(route, sizeof(route), "%s", h.path);
    char *q = strchr(route, '?'); if (q) *q = 0;
    const char *base = "/api/localsend/v2/";
    if (strncmp(route, base, strlen(base))) { ls_http_reply(c, 404, NULL); return; }
    const char *op = route + strlen(base);
    if ((!strcmp(h.method, "GET") && !strcmp(op, "info")) ||
        (!strcmp(h.method, "POST") && !strcmp(op, "register"))) {
        if (!strcmp(op, "register")) {
            char *body = ls_http_body(c, &h, JSON_LIMIT);
            LsJson *j = body ? ls_json_parse(body) : NULL; free(body);
            LsPeer peer;
            if (!j || peer_parse(j, ip, c->fingerprint, &peer)) {
                ls_json_free(j); ls_http_reply(c, 400, NULL); return;
            }
            /* A client without a certificate can query register, but cannot prove its identity. */
            const char *claimed = ls_json_str(j, "fingerprint");
            if (*c->fingerprint && claimed && !strcasecmp(claimed, c->fingerprint)) peer_add(s, &peer);
            ls_json_free(j);
        }
        LsBuf b = {0}; ls_info(s->id, &b, -1);
        ls_http_reply(c, b.failed ? 500 : 200, b.failed ? NULL : b.s); ls_buf_free(&b); return;
    }
    if (strcmp(h.method, "POST")) { ls_http_reply(c, 405, NULL); return; }
    if (!s->receive) { ls_http_reply(c, 403, NULL); return; }
    if (!strcmp(op, "prepare-upload")) prepare(s, c, &h, ip);
    else if (!strcmp(op, "upload")) upload(s, c, &h, ip);
    else if (!strcmp(op, "cancel")) {
        char sid[128]; int present = ls_query(h.path, "sessionId", sid, sizeof(sid));
        pthread_mutex_lock(&s->lock);
        Session *ss = s->session;
        int ok = ss && !strcmp(ss->ip, ip) && !strcasecmp(ss->fingerprint, c->fingerprint) &&
                 ((present == 0 && !ss->active) || (present == 1 && !strcmp(ss->id, sid)));
        if (ok) cancel_session(s, "peer");
        pthread_mutex_unlock(&s->lock);
        ls_http_reply(c, ok ? 200 : 403, NULL);
    } else ls_http_reply(c, 404, NULL);
}
static void multicast_send(Server *s, int announce)
{
    if (s->udp < 0) return;
    LsBuf b = {0}; ls_info(s->id, &b, announce);
    if (!b.failed) {
        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET; dest.sin_port = htons((uint16_t)s->multicast_port);
        inet_pton(AF_INET, GROUP, &dest.sin_addr);
        pthread_mutex_lock(&s->lock);
        for (size_t i = 0; i < s->interface_count; ++i) {
            if (!setsockopt(s->udp, IPPROTO_IP, IP_MULTICAST_IF, &s->interfaces[i], sizeof(struct in_addr)))
                (void)sendto(s->udp, b.s, b.len, 0, (struct sockaddr *)&dest, sizeof(dest));
        }
        pthread_mutex_unlock(&s->lock);
    }
    ls_buf_free(&b);
}
static int multicast_open(Server *s)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    unsigned char ttl = 1, enabled = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &enabled, sizeof(enabled));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)s->multicast_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) { close(fd); return -1; }
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces)) { close(fd); return -1; }
    for (struct ifaddrs *i = interfaces; i && s->interface_count < 64; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
        struct in_addr ip = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        int is_loop = (ntohl(ip.s_addr) >> 24) == 127;
        if (loopback() ? !is_loop : !(i->ifa_flags & IFF_MULTICAST)) continue;
        size_t k;
        for (k = 0; k < s->interface_count; ++k) if (s->interfaces[k].s_addr == ip.s_addr) break;
        if (k != s->interface_count) continue;
        struct ip_mreq membership;
        inet_pton(AF_INET, GROUP, &membership.imr_multiaddr);
        membership.imr_interface = ip;
        if (!setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)))
            s->interfaces[s->interface_count++] = ip;
    }
    freeifaddrs(interfaces);
    if (!s->interface_count) { close(fd); return -1; }
    return fd;
}
static void probe_peer(Server *s, LsPeer *peer, int slot)
{
    LsConn c; char err[256]; LsHttp h;
    int tls = !strcmp(peer->protocol, "https");
    if (ls_connect(s->id, peer->ip, peer->port, tls, tls ? peer->fingerprint : NULL,
                   &c, err, sizeof(err))) { multicast_send(s, 0); return; }
    pthread_mutex_lock(&s->lock);
    s->sockets[slot] = c.fd;
    if (s->stop) shutdown(c.fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->lock);
    LsBuf b = {0}; ls_info(s->id, &b, -1);
    char *body = NULL;
    if (!b.failed && !ls_http_request(&c, peer->ip, "/api/localsend/v2/register", "application/json", b.len) &&
        !ls_write(&c, b.s, b.len) && !ls_http_head(&c, &h) && h.status == 200)
        body = ls_http_body(&c, &h, JSON_LIMIT);
    LsJson *j = body ? ls_json_parse(body) : NULL;
    if (j && j->kind == 'o' && ls_json_str(j, "alias")) {
        ls_field(peer->alias, sizeof(peer->alias), ls_json_str(j, "alias"));
        const char *model = ls_json_str(j, "deviceModel"), *type = ls_json_str(j, "deviceType");
        ls_field(peer->type, sizeof(peer->type), type ? type : "desktop");
        ls_field(peer->model, sizeof(peer->model), model ? model : "");
        if (tls) memcpy(peer->fingerprint, c.fingerprint, sizeof(peer->fingerprint));
        peer_add(s, peer);
    } else multicast_send(s, 0);
    ls_json_free(j); free(body); ls_buf_free(&b);
    pthread_mutex_lock(&s->lock); s->sockets[slot] = -1; pthread_mutex_unlock(&s->lock);
    ls_close(&c);
}
static void *worker(void *arg)
{
    Job *job = arg; Server *s = job->s;
    if (job->probe) probe_peer(s, &job->peer, job->slot);
    else {
        LsConn c; char ip[64];
        if (!ls_accept(s->id, job->fd, &c, ip, sizeof(ip))) {
            handle(s, &c, ip);
            pthread_mutex_lock(&s->lock); s->sockets[job->slot] = -1; pthread_mutex_unlock(&s->lock);
            ls_close_server(&c);
        }
    }
    pthread_mutex_lock(&s->lock);
    s->sockets[job->slot] = -2; --s->jobs;
    pthread_cond_broadcast(&s->idle);
    pthread_mutex_unlock(&s->lock);
    free(job); return NULL;
}
static void launch(Server *s, int fd, const LsPeer *peer)
{
    Job *job = calloc(1, sizeof(*job));
    if (!job) { if (fd >= 0) close(fd); return; }
    pthread_mutex_lock(&s->lock);
    int slot;
    for (slot = 0; slot < MAX_JOBS; ++slot) if (s->sockets[slot] == -2) break;
    if (slot == MAX_JOBS || s->stop) {
        pthread_mutex_unlock(&s->lock); free(job); if (fd >= 0) close(fd); return;
    }
    job->s = s; job->fd = fd; job->slot = slot;
    if (peer) { job->probe = 1; job->peer = *peer; }
    s->sockets[slot] = fd; ++s->jobs;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, job)) {
        s->sockets[slot] = -2; --s->jobs; free(job); if (fd >= 0) close(fd);
    } else pthread_detach(thread);
    pthread_mutex_unlock(&s->lock);
}
static void multicast_read(Server *s)
{
    char buf[65536]; struct sockaddr_in from; socklen_t len = sizeof(from);
    ssize_t n = recvfrom(s->udp, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &len);
    if (n <= 0 || (loopback() && (ntohl(from.sin_addr.s_addr) >> 24) != 127)) return;
    buf[n] = 0;
    if (memchr(buf, 0, (size_t)n)) return;
    LsJson *j = ls_json_parse(buf); LsPeer peer; char ip[64];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    if (j && !peer_parse(j, ip, NULL, &peer) && strcasecmp(peer.fingerprint, s->id->fingerprint)) {
        LsJson *announce = ls_json_get(j, "announce");
        if (announce && announce->kind == 't') launch(s, -1, &peer);
        else if (announce && announce->kind == 'f') peer_add(s, &peer);
    }
    ls_json_free(j);
}
static void server_init(Server *s, LsIdentity *id)
{
    memset(s, 0, sizeof(*s)); s->id = id;
    s->listener = s->udp = s->dirfd = -1;
    s->multicast_port = id->multicast_port ? id->multicast_port : id->port;
    for (int i = 0; i < MAX_JOBS; ++i) s->sockets[i] = -2;
    pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->idle, NULL);
}
static void server_quiesce(Server *s)
{
    pthread_mutex_lock(&s->lock);
    s->stop = 1; cancel_session(s, NULL);
    for (int i = 0; i < MAX_JOBS; ++i)
        if (s->sockets[i] >= 0) shutdown(s->sockets[i], SHUT_RDWR);
    while (s->jobs) pthread_cond_wait(&s->idle, &s->lock);
    pthread_mutex_unlock(&s->lock);
}
static void server_close(Server *s)
{
    server_quiesce(s);
    if (s->listener >= 0) close(s->listener);
    if (s->udp >= 0) close(s->udp);
    if (s->dirfd >= 0) close(s->dirfd);
    ls_share_free(s->share);
    free(s->dir);
    pthread_cond_destroy(&s->idle); pthread_mutex_destroy(&s->lock);
}
static int server_run(Server *s, double duration)
{
    double start = now(), times[] = {0.1, 0.6, 2.6};
    size_t announcement = 0;
    while (!interrupted) {
        pthread_mutex_lock(&s->lock); int stop = s->stop; pthread_mutex_unlock(&s->lock);
        double elapsed = now() - start;
        if (stop || (s->share && ls_share_stopped(s->share)) || (duration > 0 && elapsed >= duration)) break;
        if (announcement < 3 && elapsed >= times[announcement]) { multicast_send(s, 1); ++announcement; }
        /* An accepted sender that went away (an app closed after the PIN)
         * would hold the receiver busy for good: give it up. */
        pthread_mutex_lock(&s->lock);
        if (s->session && s->session->active && !s->session->uploading && now() - s->session->touched > session_idle())
            cancel_session(s, "timeout");
        pthread_mutex_unlock(&s->lock);
        struct pollfd fds[2] = {{s->listener, POLLIN, 0}, {s->udp, POLLIN, 0}};
        int r = poll(fds, 2, 50);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (fds[0].revents & POLLIN) {
            int fd = accept(s->listener, NULL, NULL);
            if (fd >= 0) launch(s, fd, NULL);
        }
        if (fds[1].revents & POLLIN) multicast_read(s);
    }
    return 0;
}
static void install_signals(void)
{
    interrupted = 0;
    struct sigaction a; memset(&a, 0, sizeof(a)); a.sa_handler = interrupt_handler;
    sigemptyset(&a.sa_mask);
    sigaction(SIGINT, &a, NULL); sigaction(SIGTERM, &a, NULL);
}
int ls_receive(LsIdentity *id, const char *dir, const char *pin, int ask)
{
    Server s; server_init(&s, id); s.receive = 1; s.ask = ask; s.pin = pin;
    s.dir = realpath(dir, NULL);
    if (s.dir) s.dirfd = open(s.dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (!s.dir || s.dirfd < 0) { server_close(&s); return ls_error("io", "cannot open receive directory"); }
    char err[512];
    s.listener = ls_listen(id->port, err, sizeof(err));
    if (s.listener < 0) {
        int busy = errno == EADDRINUSE;
        server_close(&s); return ls_error(busy ? "port_busy" : "network", err);
    }
    s.udp = multicast_open(&s);
    install_signals();
    printf("ready\t%d\thttps://127.0.0.1:%d\n", id->port, id->port);
    int result = server_run(&s, 0);
    server_close(&s);
    return result ? ls_error("network", "server poll failed") : 0;
}
int ls_serve(LsIdentity *id, const char *pin, char **paths, int count)
{
    Server s; server_init(&s, id);
    char err[512] = "";
    s.listener = ls_listen(id->port, err, sizeof(err));
    if (s.listener < 0) {
        int busy = errno == EADDRINUSE;
        server_close(&s); return ls_error(busy ? "port_busy" : "network", err);
    }
    s.share = ls_share_new(id, pin, paths, count, err, sizeof(err));
    if (!s.share) { server_close(&s); return ls_error("io", err); }
    s.udp = multicast_open(&s);
    /* URLs must also work when multicast is unavailable. List each active
     * IPv4 once, keeping loopback out of the ordinary LAN-facing display. */
    struct in_addr ips[64]; size_t count_ips = 0;
    struct ifaddrs *all = NULL;
    if (!getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && count_ips < 64; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP)) continue;
            struct in_addr ip = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
            int is_loop = (ntohl(ip.s_addr) >> 24) == 127;
            if (loopback() ? !is_loop : is_loop) continue;
            size_t i;
            for (i = 0; i < count_ips; ++i) if (ips[i].s_addr == ip.s_addr) break;
            if (i == count_ips) ips[count_ips++] = ip;
        }
        freeifaddrs(all);
    }
    if (!count_ips) { ips[0].s_addr = htonl(INADDR_LOOPBACK); count_ips = 1; }
    install_signals();
    for (size_t i = 0; i < count_ips; ++i) {
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ips[i], ip, sizeof(ip));
        if (!i) printf("ready\t%d\thttp://%s:%d\n", id->port, ip, id->port);
        else printf("url\thttp://%s:%d\n", ip, id->port);
    }
    int result = server_run(&s, 0);
    server_close(&s);
    return result ? ls_error("network", "server poll failed") : 0;
}
int ls_discover(LsIdentity *id, int seconds, int emit, const char *wanted,
                LsPeer *out, char *err, size_t n)
{
    Server s; server_init(&s, id); s.emit = emit;
    s.listener = ls_listen(0, err, n);
    if (s.listener < 0) { server_close(&s); return -1; }
    struct sockaddr_in addr; socklen_t len = sizeof(addr);
    if (getsockname(s.listener, (struct sockaddr *)&addr, &len)) {
        fail(err, n, "cannot determine discovery port"); server_close(&s); return -1;
    }
    int old_port = id->port; id->port = ntohs(addr.sin_port);
    s.udp = multicast_open(&s);
    install_signals();
    int result = server_run(&s, seconds);
    server_quiesce(&s);
    pthread_mutex_lock(&s.lock);
    if (wanted) {
        result = 0;
        for (size_t i = 0; i < s.peer_count; ++i)
            if (!strcasecmp(s.peers[i].fingerprint, wanted)) {
                if (out) *out = s.peers[i];
                result = 1;
                break;
            }
    }
    pthread_mutex_unlock(&s.lock);
    server_close(&s); id->port = old_port;
    if (result < 0) fail(err, n, "discovery poll failed");
    return result;
}
