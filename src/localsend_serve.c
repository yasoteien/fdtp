// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "localsend.h"
#include "localsend_art.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Protocol: protocol.git README.md section 5; localsend.git
 * packages/core/src/http/server/web.rs (not its HTML assets).
 * Session IDs are random bearer keys, issued only after a correct PIN.
 * Like the official implementation, sessions are bound to the client's IP,
 * and re-authentication replaces that IP's previous session. */
#define SHARE_CLIENTS 256
typedef struct { char id[65], ip[64]; } DownloadSession;
struct LsShare {
    LsIdentity *identity;
    const char *pin;
    LsSendFile *files;
    int count, failures;
    size_t clients;
    DownloadSession sessions[SHARE_CLIENTS];
    pthread_mutex_t lock;
};

LsShare *ls_share_new(LsIdentity *id, const char *pin, char **paths, int count, char *err, size_t n)
{
    LsShare *s = calloc(1, sizeof(*s));
    if (!s) { snprintf(err, n, "out of memory"); return NULL; }
    s->identity = id; s->pin = pin; s->count = count;
    s->files = ls_files_open(paths, count, 0, err, n);
    if (!s->files) { free(s); return NULL; }
    pthread_mutex_init(&s->lock, NULL);
    return s;
}
void ls_share_free(LsShare *s)
{
    if (!s) return;
    ls_files_free(s->files, s->count);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
int ls_share_stopped(LsShare *s)
{
    pthread_mutex_lock(&s->lock);
    int stopped = s->failures >= 5;
    pthread_mutex_unlock(&s->lock);
    return stopped;
}

/* The lock covers checking the five-attempt limit as well as issuance. */
/* What a phone's Japanese keyboard types: full-width digits (０-９, EF BC 90-99)
 * become ASCII; spaces (ASCII, a form's +, the full-width one) go. */
static void normalize_pin(char *pin)
{
    unsigned char *r = (unsigned char *)pin, *w = r;
    while (*r) {
        if (r[0] == 0xEF && r[1] == 0xBC && r[2] >= 0x90 && r[2] <= 0x99) { *w++ = (unsigned char)('0' + r[2] - 0x90); r += 3; }
        else if (r[0] == 0xE3 && r[1] == 0x80 && r[2] == 0x80) r += 3;
        else if (*r == ' ' || *r == '+') ++r;
        else *w++ = *r++;
    }
    *w = 0;
}
static int authorize(LsShare *s, const char *path, const char *ip, char sid[65])
{
    char supplied[128] = "", pin[64] = "";
    int has_session = ls_query(path, "sessionId", supplied, sizeof(supplied));
    int has_pin = ls_query(path, "pin", pin, sizeof(pin));
    if (has_pin == 1) normalize_pin(pin);
    int status = 401;
    pthread_mutex_lock(&s->lock);
    if (s->failures >= 5) goto out;
    if (has_session == 1) for (size_t i = 0; i < s->clients; ++i) {
        if (!strcmp(s->sessions[i].ip, ip) && !strcmp(s->sessions[i].id, supplied)) {
            memcpy(sid, s->sessions[i].id, 65); status = 200; goto out;
        }
    }
    if (has_pin != 1 || strcmp(pin, s->pin)) {
        if (has_pin) { ++s->failures; printf("pin\twrong\t%s\n", ip); }
        goto out;
    }
    size_t i;
    for (i = 0; i < s->clients; ++i) if (!strcmp(s->sessions[i].ip, ip)) break;
    if (i == SHARE_CLIENTS) { status = 429; goto out; }
    if (ls_random(sid, 32)) { status = 500; goto out; }
    if (i == s->clients) ++s->clients;
    memcpy(s->sessions[i].id, sid, 65);
    snprintf(s->sessions[i].ip, sizeof(s->sessions[i].ip), "%s", ip);
    status = 200;
out:
    pthread_mutex_unlock(&s->lock);
    return status;
}
static void html_escape(LsBuf *b, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        switch (*p) {
        case '&': ls_buf_add(b, "&amp;"); break;
        case '<': ls_buf_add(b, "&lt;"); break;
        case '>': ls_buf_add(b, "&gt;"); break;
        case '"': ls_buf_add(b, "&quot;"); break;
        case '\'': ls_buf_add(b, "&#39;"); break;
        default:
            if (*p < 32 || *p == 127) ls_buf_add(b, "&#%u;", (unsigned)*p);
            else ls_buf_add(b, "%c", *p);
        }
    }
}
static int reply_head(LsConn *c, int status, const char *type, uint64_t length, const char *extra)
{
    LsBuf b = {0};
    const char *reason = status == 200 ? "OK" : status == 401 ? "Unauthorized" : "Error";
    ls_buf_add(&b, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %" PRIu64
               "\r\nConnection: close\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
               "X-Content-Type-Options: nosniff\r\n%s\r\n", status, reason, type, length, extra ? extra : "");
    if (getenv("FDTP_LOCALSEND_DEBUG")) fprintf(stderr, "debug\treply %d\n", status);
    int rc = b.failed ? -1 : ls_write(c, b.s, b.len);
    ls_buf_free(&b); return rc;
}
static void reply_buffer(LsConn *c, int status, const char *type, LsBuf *b)
{
    if (b->failed) { ls_http_reply(c, 500, NULL); return; }
    if (!reply_head(c, status, type, b->len, NULL)) (void)ls_write(c, b->s, b->len);
}
/* The page: FDTP's navy and cyan, readable on a phone, light and dark. No
 * JavaScript and nothing from elsewhere. */
static const char page_style[] =
    ":root{--bg:#eef1f7;--card:#fff;--ink:#16203a;--muted:#5b6478;--line:#c9d1e3;--accent:#1556d6;"
    "--err-bg:#fdecec;--err:#b42318}"
    "@media(prefers-color-scheme:dark){:root{--bg:#0b0f24;--card:#151b38;--ink:#e8ecf7;--muted:#a3abc2;"
    "--line:#2c3563;--accent:#4d8dff;--err-bg:#3a1a1a;--err:#ffb4aa}}"
    "body{margin:0;background:var(--bg);color:var(--ink);"
    "font:17px/1.6 -apple-system,system-ui,'Hiragino Sans','Noto Sans JP',sans-serif}"
    "header{background:#000055;color:#fff;padding:14px 20px;border-bottom:3px solid #00c8ff;"
    "display:flex;align-items:center;justify-content:space-between;gap:12px}"
    ".art{display:flex;align-items:flex-end;gap:6px;flex:none}.art svg{display:block}"
    "header h1{margin:0;font-size:22px;font-weight:600}"
    "header b{color:#00c8ff;font-family:ui-monospace,Menlo,monospace;letter-spacing:.06em;margin-right:.4em}"
    "header p{margin:2px 0 0;font-size:14px;opacity:.8}"
    "main{max-width:34rem;margin:0 auto;padding:20px 16px 32px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;"
    "box-shadow:0 1px 3px rgba(0,0,40,.08)}"
    "h2{font-size:18px;margin:0 0 12px;overflow-wrap:anywhere}"
    "label{display:block;font-weight:600;margin-bottom:10px}"
    "input{box-sizing:border-box;width:100%;font:600 32px/1.2 ui-monospace,Menlo,monospace;"
    "letter-spacing:.3em;text-align:center;padding:14px 8px;border:2px solid var(--accent);border-radius:10px;"
    "background:var(--bg);color:var(--ink)}"
    "input::placeholder{color:var(--line)}"
    "input:focus{outline:3px solid color-mix(in srgb,var(--accent) 35%,transparent);outline-offset:1px}"
    "button,.dl{display:block;box-sizing:border-box;width:100%;margin-top:14px;padding:14px;border:0;"
    "border-radius:10px;background:var(--accent);color:#fff;font-family:inherit;font-size:18px;font-weight:600;"
    "line-height:1.3;text-align:center;text-decoration:none}"
    ".err{background:var(--err-bg);color:var(--err);border-radius:10px;padding:10px 14px;margin:0 0 16px}"
    "ul{list-style:none;margin:0;padding:0}li{border-top:1px solid var(--line);padding:14px 0}"
    "li:first-child{border-top:0;padding-top:0}"
    ".name{font-weight:600;overflow-wrap:anywhere}.size{color:var(--muted);font-size:15px}"
    "li .dl{margin-top:8px}"
    ".note{color:var(--muted);font-size:14px;margin:16px 4px 0}";

static void size_text(char *out, size_t cap, uint64_t n, int ja)
{
    if (n < 1024) snprintf(out, cap, ja ? "%" PRIu64 " バイト" : "%" PRIu64 " bytes", n);
    else if (n < 1024 * 1024) snprintf(out, cap, "%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) snprintf(out, cap, "%.1f MB", n / 1048576.0);
    else snprintf(out, cap, "%.2f GB", n / 1073741824.0);
}
static void index_page(LsShare *s, LsConn *c, const LsHttp *h, int status, const char *sid)
{
    int ja = h->japanese;
    LsBuf b = {0};
    ls_buf_add(&b, "<!doctype html><html lang=\"%s\"><meta charset=\"UTF-8\">"
               "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
               "<meta name=\"color-scheme\" content=\"light dark\">"
               "<title>FDTP LocalSend</title><style>%s</style><body>"
               "<header><div><h1><b>FDTP</b>LocalSend</h1><p>%s</p></div>"
               "<div class=\"art\" aria-hidden=\"true\">%s%s</div></header><main><div class=\"card\">",
               ja ? "ja" : "en", page_style,
               ja ? "ポメラからファイルを受け取る" : "Download files from a Pomera", web_art_pomera, web_art_pomeranian);
    if (status == 200 && sid && *sid) {
        ls_buf_add(&b, "<h2>");
        html_escape(&b, s->identity->alias);
        ls_buf_add(&b, "%s</h2><ul>", ja ? " から受け取るファイル" : ": files to download");
        for (int i = 0; i < s->count; ++i) {
            LsSendFile *f = &s->files[i]; char size[64];
            size_text(size, sizeof(size), f->size, ja);
            ls_buf_add(&b, "<li><div class=\"name\">");
            html_escape(&b, f->name);
            ls_buf_add(&b, "</div><div class=\"size\">%s</div>"
                       "<a class=\"dl\" href=\"/api/localsend/v2/download?sessionId=%s&amp;fileId=%s\">%s</a></li>",
                       size, sid, f->id, ja ? "ダウンロード" : "Download");
        }
        ls_buf_add(&b, "</ul>");
    } else {
        if (status == 401) ls_buf_add(&b, "<p class=\"err\">%s</p>", ja ? "PIN が違います。ポメラの画面の PIN をもう一度入れてください。" : "Incorrect PIN. Enter the PIN on the Pomera's screen again.");
        /* Visible digits: the PIN is on the Pomera's screen anyway, and a
         * hidden field made people type it wrong. */
        ls_buf_add(&b, "<form method=\"get\" action=\"/\"><label for=\"pin\">%s</label>"
                   "<input id=\"pin\" name=\"pin\" type=\"text\" inputmode=\"numeric\" placeholder=\"000000\" "
                   "maxlength=\"12\" required autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
                   "<button type=\"submit\">%s</button></form>",
                   ja ? "ポメラの画面に出ている PIN（6 桁）" : "The PIN on the Pomera's screen (6 digits)", ja ? "開く" : "Open");
    }
    ls_buf_add(&b, "</div><p class=\"note\">%s</p></main></body></html>",
               ja ? "この通信は暗号化されません。自宅など信頼できる LAN だけで使ってください。"
                  : "This connection is not encrypted. Use it only on a LAN you trust, such as at home.");
    /* A PIN is accepted in the query by design, but is never included in
     * a response link or referer sent by the browser. */
    reply_buffer(c, status, "text/html; charset=utf-8", &b);
    ls_buf_free(&b);
}
static void file_list(LsShare *s, LsConn *c, const char *sid)
{
    LsBuf b = {0};
    ls_buf_add(&b, "{\"info\":"); ls_info(s->identity, &b, -1);
    ls_buf_add(&b, ",\"sessionId\":\"%s\",\"files\":{", sid);
    for (int i = 0; i < s->count; ++i) {
        LsSendFile *f = &s->files[i];
        ls_buf_add(&b, "%s\"%s\":{\"id\":\"%s\",\"fileName\":", i ? "," : "", f->id, f->id);
        ls_buf_quote(&b, f->name);
        ls_buf_add(&b, ",\"size\":%" PRIu64 ",\"fileType\":", f->size);
        ls_buf_quote(&b, ls_file_type(f->name));
        ls_buf_add(&b, ",\"sha256\":null,\"preview\":null}");
    }
    ls_buf_add(&b, "}}");
    reply_buffer(c, 200, "application/json", &b); ls_buf_free(&b);
}
static int unchanged(LsSendFile *f)
{
    struct stat st;
    return !fstat(f->fd, &st) && S_ISREG(st.st_mode) && st.st_size == f->snapshot.st_size &&
        st.st_mtime == f->snapshot.st_mtime && st.st_ctime == f->snapshot.st_ctime;
}
static void download(LsShare *s, LsConn *c, const LsHttp *h, const char *ip)
{
    char sid[128], fid[1024];   /* as upload's: a long unknown id is refused as unknown (403), not as malformed */
    if (ls_query(h->path, "sessionId", sid, sizeof(sid)) != 1) { ls_http_reply(c, 400, NULL); return; }
    int valid = 0;
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; s->failures < 5 && i < s->clients; ++i)
        if (!strcmp(s->sessions[i].id, sid) && !strcmp(s->sessions[i].ip, ip)) { valid = 1; break; }
    pthread_mutex_unlock(&s->lock);
    if (!valid) { ls_http_reply(c, 403, NULL); return; }
    if (ls_query(h->path, "fileId", fid, sizeof(fid)) != 1) { ls_http_reply(c, 400, NULL); return; }
    LsSendFile *f = NULL;
    for (int i = 0; i < s->count; ++i) if (!strcmp(s->files[i].id, fid)) { f = &s->files[i]; break; }
    if (!f) { ls_http_reply(c, 403, NULL); return; }
    if (!unchanged(f)) { ls_http_reply(c, 500, NULL); return; }
    char *encoded = ls_urlencode(f->name);
    if (!encoded) { ls_http_reply(c, 500, NULL); return; }
    LsBuf extra = {0};
    /* ASCII fallback does not interpret quotes, slashes, or control bytes. */
    ls_buf_add(&extra, "Content-Disposition: attachment; filename=\"download\"; filename*=UTF-8''%s\r\n", encoded ? encoded : "");
    free(encoded);
    if (extra.failed) { ls_buf_free(&extra); ls_http_reply(c, 500, NULL); return; }
    char field[4096]; ls_field(field, sizeof(field), f->name);
    printf("download\t%s\t%s\n", ip, field);
    int rc = reply_head(c, 200, "application/octet-stream", f->size, extra.s);
    ls_buf_free(&extra);
    unsigned char buf[65536]; uint64_t done = 0;
    while (!rc && done < f->size) {
        size_t want = f->size - done < sizeof(buf) ? (size_t)(f->size - done) : sizeof(buf);
        ssize_t got = pread(f->fd, buf, want, (off_t)done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0 || ls_write(c, buf, (size_t)got)) { rc = -1; break; }
        done += (uint64_t)got;
    }
    if (!rc && unchanged(f)) printf("downloaded\t%s\t%s\n", ip, field);
}
void ls_share_handle(LsShare *s, LsConn *c, const LsHttp *h, const char *ip)
{
    char route[4096]; snprintf(route, sizeof(route), "%s", h->path);
    char *q = strchr(route, '?'); if (q) *q = 0;
    if ((!strcmp(route, "/api/localsend/v2/info") && !strcmp(h->method, "GET")) ||
        (!strcmp(route, "/api/localsend/v2/register") && !strcmp(h->method, "POST"))) {
        /* Registration never controls the set of exported files. */
        if (!strcmp(h->method, "POST")) {
            char *body = ls_http_body(c, h, 1048576);
            LsJson *j = body ? ls_json_parse(body) : NULL; free(body);
            int ok = j && j->kind == 'o' && ls_json_str(j, "alias");
            ls_json_free(j);
            if (!ok) { ls_http_reply(c, 400, NULL); return; }
        }
        LsBuf b = {0}; ls_info(s->identity, &b, -1);
        reply_buffer(c, 200, "application/json", &b); ls_buf_free(&b); return;
    }
    if (!strcmp(route, "/") && !strcmp(h->method, "GET")) {
        char sid[65] = "", value[128];
        int has_query = ls_query(h->path, "pin", value, sizeof(value)) || ls_query(h->path, "sessionId", value, sizeof(value));
        int status = has_query ? authorize(s, h->path, ip, sid) : 200;
        if (status != 200 && status != 401) { ls_http_reply(c, status, NULL); return; }
        index_page(s, c, h, status, sid); return;
    }
    if (!strcmp(route, "/api/localsend/v2/prepare-download") && !strcmp(h->method, "POST")) {
        char sid[65] = "";
        int status = authorize(s, h->path, ip, sid);
        if (status == 200) file_list(s, c, sid);
        else ls_http_reply(c, status, NULL);
        return;
    }
    if (!strcmp(route, "/api/localsend/v2/download") && !strcmp(h->method, "GET")) { download(s, c, h, ip); return; }
    ls_http_reply(c, 404, NULL);
}
