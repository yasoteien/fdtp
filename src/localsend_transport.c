// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "localsend.h"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/ec.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>

static void error_text(char *err, size_t n, const char *msg) {
    if (n) snprintf(err, n, "%s", msg);
}
void ls_field(char *dst, size_t n, const char *src) {
    if (!n) return;
    size_t i = 0;
    if (src) for (; src[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i]; dst[i] = c < 32 || c == 127 ? ' ' : (char)c;
    }
    /* A bounded peer/alias field must not cut a UTF-8 code point in half. */
    if (src && src[i] && i + 1 == n) {
        size_t cut = i;
        while (cut && ((unsigned char)src[cut] & 0xc0) == 0x80) --cut;
        i = cut;
    }
    dst[i] = 0;
}
int ls_error(const char *code, const char *message) {
    char c[128], m[2048]; ls_field(c, sizeof(c), code); ls_field(m, sizeof(m), message);
    printf("error\t%s\t%s\n", c, m); fflush(stdout); return 1;
}
int ls_random(char *hex, size_t bytes) {
    if (bytes > INT_MAX || bytes > (SIZE_MAX - 1) / 2) return -1;
    unsigned char *data = malloc(bytes ? bytes : 1);
    if (!data || (bytes && RAND_bytes(data, (int)bytes) != 1)) { free(data); return -1; }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) { hex[2*i] = digits[data[i] >> 4]; hex[2*i+1] = digits[data[i] & 15]; }
    hex[2*bytes] = 0; free(data); return 0;
}
char *ls_urlencode(const char *s) {
    size_t n = strlen(s);
    if (n > (SIZE_MAX - 1) / 3) return NULL;
    char *out = malloc(n * 3 + 1), *q = out;
    if (!out) return NULL;
    static const char h[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p)) *q++ = (char)*p;
        else { *q++ = '%'; *q++ = h[*p >> 4]; *q++ = h[*p & 15]; }
    }
    *q = 0; return out;
}
static int cert_valid(X509 *cert) {
    if (!cert || X509_cmp_current_time(X509_get0_notBefore(cert)) >= 0 ||
        X509_cmp_current_time(X509_get0_notAfter(cert)) <= 0 ||
        X509_NAME_cmp(X509_get_subject_name(cert), X509_get_issuer_name(cert))) return 0;
    EVP_PKEY *key = X509_get_pubkey(cert);
    int ok = key && X509_verify(cert, key) == 1;
    EVP_PKEY_free(key); return ok;
}
static int fingerprint(X509 *cert, char out[65]) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned len = 0;
    if (!cert || !X509_digest(cert, EVP_sha256(), digest, &len) || len != 32) return -1;
    static const char h[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < len; i++) { out[i*2] = h[digest[i] >> 4]; out[i*2+1] = h[digest[i] & 15]; }
    out[64] = 0; return 0;
}
/* LocalSend certificates authenticate themselves, rather than a DNS hostname. */
static int verify_peer(int preverified, X509_STORE_CTX *ctx) {
    (void)preverified;
    if (X509_STORE_CTX_get_error_depth(ctx) != 0) return 1;
    X509 *cert = X509_STORE_CTX_get_current_cert(ctx);
    if (!cert_valid(cert)) return 0;
    SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    const char *expected = ssl ? SSL_get_app_data(ssl) : NULL;
    char actual[65];
    return !expected || !*expected || (!fingerprint(cert, actual) && !strcasecmp(expected, actual));
}
static int private_dir(int parent, const char *name, int private) {
    if (mkdirat(parent, name, 0700) < 0 && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & (private ? 0077 : 0022))) { close(fd); errno = EACCES; return -1; }
    return fd;
}
static int identity_file(int dir, const char *name, int flags) {
    int fd = openat(dir, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1 || (st.st_mode & 0777) != 0600) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}
static int make_identity(X509 **cert, EVP_PKEY **key) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!ctx) return -1;
    int ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) > 0 && EVP_PKEY_keygen(ctx, key) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !(*cert = X509_new())) return -1;
    unsigned char serial[16];
    if (RAND_bytes(serial, sizeof(serial)) != 1) return -1;
    serial[0] &= 0x7f; serial[0] |= 1;
    BIGNUM *bn = BN_bin2bn(serial, sizeof(serial), NULL);
    if (!bn) return -1;
    ASN1_INTEGER *asn = BN_to_ASN1_INTEGER(bn, NULL); BN_free(bn);
    if (!asn) return -1;
    ok = X509_set_serialNumber(*cert, asn); ASN1_INTEGER_free(asn);
    X509_NAME *name = X509_get_subject_name(*cert);
    return ok && X509_set_version(*cert, 2) && X509_gmtime_adj(X509_getm_notBefore(*cert), -300) &&
        X509_gmtime_adj(X509_getm_notAfter(*cert), 3650L * 24 * 3600) && X509_set_pubkey(*cert, *key) &&
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"LocalSend User", -1, -1, 0) &&
        X509_set_issuer_name(*cert, name) && X509_sign(*cert, *key, EVP_sha256()) > 0 ? 0 : -1;
}
int ls_identity_init(LsIdentity *id, const char *alias, int port, char *err, size_t n) {
    memset(id, 0, sizeof(*id)); id->port = port;
    if (alias) ls_field(id->alias, sizeof(id->alias), alias);
    else {
        char host[256] = {0};
        if (gethostname(host, sizeof(host)-1)) strcpy(host, "FDTP");
        size_t len = strlen(host);
        while (len >= 6 && !strcmp(host + len - 6, ".local")) { len -= 6; host[len] = 0; }
        ls_field(id->alias, sizeof(id->alias), host);
    }
    int home = -1, base = -1, dir = -1, lock = -1, cfd = -1, kfd = -1;
    X509 *cert = NULL; EVP_PKEY *key = NULL; FILE *cf = NULL, *kf = NULL;
    const char *msg = "cannot open private LocalSend identity directory";
    const char *h = getenv("HOME");
    if (!h || !*h || (home = open(h, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
        (base = private_dir(home, ".fdtp", 0)) < 0 || (dir = private_dir(base, "localsend", 1)) < 0) goto fail;
    lock = identity_file(dir, ".lock", O_RDWR | O_CREAT);
    if (lock < 0 || flock(lock, LOCK_EX)) goto fail;
    msg = "cannot read LocalSend certificate or key (regular owned 0600 files required)";
    errno = 0; cfd = identity_file(dir, "cert.pem", O_RDONLY); int cerr = errno;
    errno = 0; kfd = identity_file(dir, "key.pem", O_RDONLY); int kerr = errno;
    if (cfd < 0 && kfd < 0 && cerr == ENOENT && kerr == ENOENT) {
        msg = "cannot generate LocalSend identity";
        if (make_identity(&cert, &key)) goto fail;
        kfd = identity_file(dir, "key.pem", O_WRONLY | O_CREAT | O_EXCL);
        if (kfd < 0) goto fail;
        cfd = identity_file(dir, "cert.pem", O_WRONLY | O_CREAT | O_EXCL);
        if (cfd < 0) { unlinkat(dir, "key.pem", 0); goto fail; }
        kf = fdopen(kfd, "w"); if (!kf) goto fail; kfd = -1;
        cf = fdopen(cfd, "w"); if (!cf) goto fail; cfd = -1;
        int written = PEM_write_PrivateKey(kf, key, NULL, NULL, 0, NULL, NULL) && PEM_write_X509(cf, cert);
        if (fflush(kf) || fflush(cf)) written = 0;
        if (fsync(fileno(kf)) || fsync(fileno(cf))) written = 0;
        if (fclose(kf)) written = 0;
        kf = NULL;
        if (fclose(cf)) written = 0;
        cf = NULL;
        if (!written) { unlinkat(dir, "cert.pem", 0); unlinkat(dir, "key.pem", 0); goto fail; }
    } else {
        if (cfd < 0 || kfd < 0) goto fail;
        cf = fdopen(cfd, "r"); if (!cf) goto fail; cfd = -1;
        kf = fdopen(kfd, "r"); if (!kf) goto fail; kfd = -1;
        cert = PEM_read_X509(cf, NULL, NULL, NULL); key = PEM_read_PrivateKey(kf, NULL, NULL, NULL);
        fclose(cf); cf = NULL; fclose(kf); kf = NULL;
    }
    msg = "LocalSend identity is invalid, expired, or certificate/key mismatch";
    if (!cert_valid(cert) || !key || X509_check_private_key(cert, key) != 1 || fingerprint(cert, id->fingerprint)) goto fail;
    id->server = SSL_CTX_new(TLS_server_method()); id->client = SSL_CTX_new(TLS_client_method());
    if (!id->server || !id->client) goto fail;
    SSL_CTX *contexts[2] = {id->server, id->client};
    for (int i = 0; i < 2; i++) {
        if (!SSL_CTX_set_min_proto_version(contexts[i], TLS1_2_VERSION) ||
            !SSL_CTX_use_certificate(contexts[i], cert) || !SSL_CTX_use_PrivateKey(contexts[i], key) ||
            !SSL_CTX_check_private_key(contexts[i])) goto fail;
        SSL_CTX_set_verify(contexts[i], SSL_VERIFY_PEER, verify_peer);
        SSL_CTX_set_options(contexts[i], SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    }
    /* A server that asks for client certificates must name its sessions, or
     * OpenSSL refuses a resumed one: the official apps resume the TLS session
     * of prepare-upload for each upload. The peer's certificate stays in the
     * session, so the fingerprint checks still see it. */
    static const unsigned char session_context[] = "fdtp-localsend";
    if (!SSL_CTX_set_session_id_context(id->server, session_context, sizeof(session_context) - 1)) goto fail;
    X509_free(cert); EVP_PKEY_free(key);
    close(lock); close(dir); close(base); close(home); return 0;
fail:
    if (cf) fclose(cf);
    if (kf) fclose(kf);
    if (cfd >= 0) close(cfd);
    if (kfd >= 0) close(kfd);
    if (lock >= 0) close(lock);
    if (dir >= 0) close(dir);
    if (base >= 0) close(base);
    if (home >= 0) close(home);
    X509_free(cert); EVP_PKEY_free(key); ls_identity_free(id); error_text(err, n, msg); return -1;
}
void ls_identity_free(LsIdentity *id) { SSL_CTX_free(id->server); SSL_CTX_free(id->client); id->server = id->client = NULL; }
static void timeouts(int fd, int seconds) {
    struct timeval tv = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}
int ls_listen(int port, char *err, size_t n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { error_text(err, n, strerror(errno)); return -1; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(getenv("FDTP_LOCALSEND_LOOPBACK") && !strcmp(getenv("FDTP_LOCALSEND_LOOPBACK"), "1") ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 16)) {
        int e = errno; error_text(err, n, strerror(e)); close(fd); errno = e; return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC); return fd;
}
int ls_accept(LsIdentity *id, int fd, LsConn *c, char *ip, size_t iplen) {
    memset(c, 0, sizeof(*c)); c->fd = -1;
    struct sockaddr_storage a; socklen_t len = sizeof(a);
    int accepted = fd;
    if (accepted < 0) return -1;
    if (getpeername(accepted, (struct sockaddr *)&a, &len)) { close(accepted); return -1; }
    c->fd = accepted; fcntl(accepted, F_SETFD, FD_CLOEXEC); timeouts(accepted, 3);
    if (ip && iplen && getnameinfo((struct sockaddr *)&a, len, ip, (socklen_t)iplen, NULL, 0, NI_NUMERICHOST)) *ip = 0;
    if (id->download) { timeouts(accepted, 15); return 0; }
    c->ssl = SSL_new(id->server);
    if (!c->ssl || !SSL_set_fd(c->ssl, accepted) || SSL_accept(c->ssl) != 1) {
        if (getenv("FDTP_LOCALSEND_DEBUG")) {
            char e[256]; ERR_error_string_n(ERR_peek_last_error(), e, sizeof(e));
            fprintf(stderr, "debug\t%s\tTLS accept failed: %s (errno %d)\n", ip ? ip : "", e, errno);
        }
        ls_close(c); return -1;
    }
    X509 *cert = SSL_get1_peer_certificate(c->ssl);
    int ok = !cert || (cert_valid(cert) && !fingerprint(cert, c->fingerprint));
    X509_free(cert);
    if (!ok) {
        if (getenv("FDTP_LOCALSEND_DEBUG")) fprintf(stderr, "debug\t%s\tclient certificate refused\n", ip ? ip : "");
        ls_close(c); return -1;
    }
    timeouts(accepted, 15); return 0;
}
int ls_connect(LsIdentity *id, const char *host, int port, int tls, const char *expected, LsConn *c, char *err, size_t n) {
    memset(c, 0, sizeof(*c)); c->fd = -1;
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char service[16]; snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &addresses)) { error_text(err, n, "cannot resolve peer address"); return -1; }
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) { close(fd); continue; }
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            do { rc = poll(&p, 1, 750); } while (rc < 0 && errno == EINTR);
            if (rc > 0) { int e = 0; socklen_t size = sizeof(e); rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &size); if (e) rc = -1; }
            else rc = -1;
        }
        if (rc < 0 || fcntl(fd, F_SETFL, flags)) { close(fd); continue; }
        c->fd = fd; break;
    }
    freeaddrinfo(addresses);
    if (c->fd < 0) { error_text(err, n, "cannot connect to peer"); return -1; }
    timeouts(c->fd, 1);
    if (tls) {
        c->ssl = SSL_new(id->client);
        if (c->ssl) SSL_set_app_data(c->ssl, (void *)expected);
        if (!c->ssl || !SSL_set_fd(c->ssl, c->fd) || SSL_connect(c->ssl) != 1) { error_text(err, n, "TLS handshake or certificate verification failed"); ls_close(c); return -1; }
        SSL_set_app_data(c->ssl, NULL);
        X509 *cert = SSL_get1_peer_certificate(c->ssl);
        int valid = cert_valid(cert) && !fingerprint(cert, c->fingerprint);
        X509_free(cert);
        if (!valid || (expected && *expected && (strlen(expected) != 64 || strcasecmp(expected, c->fingerprint)))) {
            error_text(err, n, "peer certificate fingerprint mismatch or invalid certificate"); ls_close(c); return -1;
        }
    } else if (expected && *expected) {
        error_text(err, n, "cannot verify fingerprint over plain HTTP"); ls_close(c); return -1;
    }
    timeouts(c->fd, 15); return 0;
}
ssize_t ls_read(LsConn *c, void *p, size_t n) {
    if (!n) return 0;
    if (n > INT_MAX) n = INT_MAX;
    if (c->ssl) {
        int got = SSL_read(c->ssl, p, (int)n);
        if (got > 0) return got;
        return SSL_get_error(c->ssl, got) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }
    ssize_t got;
    do { got = recv(c->fd, p, n, 0); } while (got < 0 && errno == EINTR);
    return got;
}
int ls_write(LsConn *c, const void *p, size_t n) {
    const unsigned char *s = p;
    while (n) {
        size_t amount = n > INT_MAX ? INT_MAX : n;
        ssize_t got;
        if (c->ssl) got = SSL_write(c->ssl, s, (int)amount);
        else {
#ifdef MSG_NOSIGNAL
            got = send(c->fd, s, amount, MSG_NOSIGNAL);
#else
            got = send(c->fd, s, amount, 0);
#endif
            if (got < 0 && errno == EINTR) continue;
        }
        if (got <= 0) return -1;
        s += got; n -= (size_t)got;
    }
    return 0;
}
void ls_close_server(LsConn *c) {
    /* Closing with unread input makes the kernel send a reset, which can
     * drop the reply just sent (a refused upload's 413). Send close_notify,
     * stop writing, then read and drop what the peer still sends: a peer
     * that closes ends it at once, others after a second. */
    if (c->ssl && SSL_is_init_finished(c->ssl)) (void)SSL_shutdown(c->ssl);
    SSL_free(c->ssl); c->ssl = NULL;
    if (c->fd >= 0) {
        char drop[4096]; size_t total = 0;
        shutdown(c->fd, SHUT_WR); timeouts(c->fd, 1);
        for (ssize_t got; total < (4u << 20) && (got = recv(c->fd, drop, sizeof(drop), 0)) > 0; total += (size_t)got) {}
        close(c->fd);
    }
    c->fd = -1;
}
void ls_close(LsConn *c) {
    /* Send close_notify once; waiting for the peer's alert would block. */
    if (c->ssl && SSL_is_init_finished(c->ssl)) (void)SSL_shutdown(c->ssl);
    SSL_free(c->ssl); c->ssl = NULL;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int line_read(LsConn *c, char *line, size_t cap, size_t *budget) {
    size_t n = 0;
    for (;;) {
        unsigned char ch;
        if (!*budget || ls_read(c, &ch, 1) != 1) return -1;
        (*budget)--;
        if (ch == '\r') {
            if (!*budget || ls_read(c, &ch, 1) != 1 || ch != '\n') return -1;
            (*budget)--; line[n] = 0; return 0;
        }
        if (!ch || ch == '\n' || (ch < 32 && ch != '\t') || ch == 127 || n + 1 >= cap) return -1;
        line[n++] = (char)ch;
    }
}
static int token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
static int decimal(const char *s, uint64_t *v) {
    if (!*s) return -1;
    *v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || *v > (UINT64_MAX - (unsigned)(*s - '0')) / 10) return -1;
        *v = *v * 10 + (unsigned)(*s - '0');
    }
    return 0;
}
static char *header_value(char *line) {
    char *colon = strchr(line, ':');
    if (!colon || colon == line) return NULL;
    for (char *p = line; p < colon; p++) if (!token((unsigned char)*p)) return NULL;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t') colon++;
    size_t len = strlen(colon);
    while (len && (colon[len-1] == ' ' || colon[len-1] == '\t')) colon[--len] = 0;
    return colon;
}
int ls_http_head(LsConn *c, LsHttp *h) {
    memset(h, 0, sizeof(*h));
    char line[8192]; size_t budget = 32768;
    if (line_read(c, line, sizeof(line), &budget)) return -1;
    if (!strncmp(line, "HTTP/1.1 ", 9)) {
        if (strlen(line) < 12 || line[9] < '1' || line[9] > '5' || !isdigit((unsigned char)line[10]) ||
            !isdigit((unsigned char)line[11]) || (line[12] && line[12] != ' ')) return -1;
        h->status = (line[9]-'0') * 100 + (line[10]-'0') * 10 + line[11]-'0';
    } else {
        char *a = strchr(line, ' '), *b = a ? strchr(a + 1, ' ') : NULL;
        if (!a || !b || strcmp(b + 1, "HTTP/1.1") || a == line || (size_t)(a-line) >= sizeof(h->method) || b == a+1 || (size_t)(b-a-1) >= sizeof(h->path)) return -1;
        for (char *p = line; p < a; p++) if (!token((unsigned char)*p)) return -1;
        for (char *p = a+1; p < b; p++) if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127) return -1;
        memcpy(h->method, line, (size_t)(a-line)); memcpy(h->path, a+1, (size_t)(b-a-1));
        if (h->path[0] != '/') return -1;
    }
    int length_seen = 0, transfer_seen = 0;
    unsigned fields = 0;
    for (;;) {
        if (line_read(c, line, sizeof(line), &budget)) return -1;
        if (!*line) break;
        if (++fields > 128) return -1;
        char *v = header_value(line); if (!v) return -1;
        if (!strcasecmp(line, "Content-Length")) {
            if (length_seen++ || decimal(v, &h->length)) return -1;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            if (transfer_seen++ || strcasecmp(v, "chunked")) return -1;
            h->chunked = 1;
        } else if (!strcasecmp(line, "Accept-Language")) {
            h->japanese = !strncasecmp(v, "ja", 2) &&
                (!v[2] || v[2] == '-' || v[2] == ',' || v[2] == ';' || v[2] == ' ');
        }
    }
    if (length_seen && transfer_seen) return -1;
    return 0;
}
static int read_exact(LsConn *c, char *s, size_t len) {
    while (len) {
        ssize_t got = ls_read(c, s, len); if (got <= 0) return -1;
        s += got; len -= (size_t)got;
    }
    return 0;
}
char *ls_http_body(LsConn *c, const LsHttp *h, size_t limit) {
    if (limit == SIZE_MAX || (!h->chunked && h->length > limit)) return NULL;
    size_t cap = h->chunked ? (limit < 4096 ? limit : 4096) : (size_t)h->length;
    char *body = malloc(cap+1);
    if (!body) return NULL;
    size_t used = 0;
    if (!h->chunked) {
        used = (size_t)h->length;
        if (read_exact(c, body, used)) goto fail;
    } else {
        size_t budget = 32768;
        for (;;) {
            char line[4096];
            if (line_read(c, line, sizeof(line), &budget)) goto fail;
            char *p = line; uint64_t amount = 0; unsigned digits = 0;
            while (isxdigit((unsigned char)*p)) {
                unsigned d = *p <= '9' ? (unsigned)(*p-'0') : (unsigned)(tolower((unsigned char)*p)-'a'+10);
                if (amount > (UINT64_MAX-d)/16) goto fail;
                amount = amount*16+d; p++; digits++;
            }
            if (!digits || (*p && *p != ';') || amount > limit-used) goto fail;
            if (!amount) {
                unsigned fields = 0;
                for (;;) {
                    if (line_read(c, line, sizeof(line), &budget)) goto fail;
                    if (!*line) break;
                    if (++fields > 128 || !header_value(line) || !strcasecmp(line, "Content-Length") || !strcasecmp(line, "Transfer-Encoding")) goto fail;
                }
                break;
            }
            size_t want = used+(size_t)amount;
            if (want > cap) {
                size_t next = cap ? cap : 1;
                while (next < want) { if (next > limit/2) { next = limit; break; } next *= 2; }
                char *larger = realloc(body, next+1); if (!larger) goto fail;
                body = larger; cap = next;
            }
            if (read_exact(c, body+used, (size_t)amount)) goto fail;
            used = want;
            char crlf[2]; if (read_exact(c, crlf, 2) || crlf[0] != '\r' || crlf[1] != '\n') goto fail;
        }
    }
    if (memchr(body, 0, used)) goto fail;
    body[used] = 0; return body;
fail: free(body); return NULL;
}
static int http_safe(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if ((unsigned char)*s <= 32 || (unsigned char)*s >= 127) return 0;
    return 1;
}
int ls_http_request(LsConn *c, const char *host, const char *path, const char *type, uint64_t length) {
    if (!http_safe(host) || !http_safe(path) || !http_safe(type) || *path != '/') return -1;
    LsBuf b = {0};
    ls_buf_add(&b, "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n", path, host, type, (unsigned long long)length);
    int rc = b.failed ? -1 : ls_write(c, b.s, b.len); ls_buf_free(&b); return rc;
}
int ls_http_reply(LsConn *c, int status, const char *json) {
    const char *reason;
    switch (status) {
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 401: reason = "Unauthorized"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 409: reason = "Conflict"; break;
    case 413: reason = "Content Too Large"; break;
    case 422: reason = "Unprocessable Content"; break;
    case 500: reason = "Internal Server Error"; break;
    default: reason = "Response"; break;
    }
    if (status < 100 || status > 599) return -1;
    if (getenv("FDTP_LOCALSEND_DEBUG")) fprintf(stderr, "debug\treply %d\n", status);
    if (!json) json = "";
    LsBuf b = {0};
    ls_buf_add(&b, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, strlen(json));
    int rc = b.failed ? -1 : ls_write(c, b.s, b.len);
    if (!rc) rc = ls_write(c, json, strlen(json));
    ls_buf_free(&b); return rc;
}
void ls_info(LsIdentity *id, LsBuf *b, int announce) {
    ls_buf_add(b, "{\"alias\":"); ls_buf_quote(b, id->alias);
    ls_buf_add(b, ",\"version\":\"2.2\",\"deviceModel\":\"FDTP\",\"deviceType\":\"headless\",\"fingerprint\":");
    ls_buf_quote(b, id->fingerprint);
    ls_buf_add(b, ",\"port\":%d,\"protocol\":\"%s\",\"download\":%s", id->port,
               id->download ? "http" : "https", id->download ? "true" : "false");
    if (announce >= 0) ls_buf_add(b, ",\"announce\":%s", announce ? "true" : "false");
    ls_buf_add(b, "}");
}
