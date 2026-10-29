// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "localsend_ui.h"
#include "i18n.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define EVENT_CAP 16384
#define SAVED_KEEP 64

typedef enum { LS_MENU, LS_SCAN, LS_SEND, LS_RECEIVE, LS_BROWSER } Mode;
typedef enum { ASK_NONE, ASK_MENU, ASK_PIN, ASK_RECEIVE, ASK_OFFICIAL } Question;
typedef struct { char fp[128], alias[256], ip[128], model[256]; } Peer;
typedef struct {
    Screen screen, *below;
    App *app;
    Mode mode;
    Question question;
    char *directory, **paths, *helper;
    size_t count, peer_count, selected, top;
    int ip_top, ip_lines, ip_room;
    Peer *peers;
    char alias[256], pin[7], peer_pin[65], target[128], ips[4096];   /* pin: ours (receive); peer_pin: the receiver's */
    pid_t child;
    int input, output, ended, failed, pin_again, dropping;
    char event[EVENT_CAP], status[1024], what[1024], reply[8];
    size_t used, reply_at;
    char *saved[SAVED_KEEP];
    size_t saved_count, batch_saved;   /* batch_saved: of the sender being received */
    char **urls, **downloads;
    size_t url_count, download_count, browser_top, browser_lines, browser_room;
    uint64_t total_bytes, finished_bytes, file_bytes, done_bytes;
    size_t finished_files;
    double official_checked;   /* when the official app was last looked for */
    int official_seen, official_kept;   /* it ran; FDTM was chosen over it */
} LocalSend;

static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, name);
    return p;
}
/* The official LocalSend app on the Mac (docs/FDTP_MACOS.md): the window
 * adapter's question, or for the tests a file that exists while it "runs". */
static int official_running(App *a)
{
    const char *fake = getenv("FDTP_LOCALSEND_OFFICIAL_FILE");
    if (fake && *fake) return access(fake, F_OK) == 0;
    return a->localsend_official && a->localsend_official();
}
static void status(LocalSend *l, const char *s)
{ snprintf(l->status, sizeof(l->status), "%s", s); l->app->changed = 1; }
static void close_fd(int *fd) { if (*fd >= 0) close(*fd); *fd = -1; }
static void stop_child(LocalSend *l)
{
    close_fd(&l->input); close_fd(&l->output);
    if (l->child > 0) {
        kill(l->child, SIGTERM);
        int result = 0;
        for (int i = 0; i < 30; ++i) {
            pid_t p = waitpid(l->child, &result, WNOHANG);
            if (p == l->child || (p < 0 && errno == ECHILD)) { l->child = 0; break; }
            struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
        }
        if (l->child > 0) {
            kill(l->child, SIGKILL);
            while (waitpid(l->child, &result, 0) < 0 && errno == EINTR) {}
            l->child = 0;
        }
    }
    l->used = 0; l->reply[0] = 0; l->reply_at = 0; l->dropping = 0;
}
static int pipe_flags(int p[2])
{
    if (pipe(p)) return -1;
    for (int i = 0; i < 2; ++i) if (fcntl(p[i], F_SETFD, FD_CLOEXEC) < 0) {
        close(p[0]); close(p[1]); return -1;
    }
    return 0;
}
static char *helper_path(void)
{
    const char *override = getenv("FDTP_LOCALSEND_HELPER");
    if (override) return strdup(override);
    char exe[4096]; int found = 0;
#ifdef __APPLE__
    uint32_t size = sizeof(exe);
    found = _NSGetExecutablePath(exe, &size) == 0;
    if (found) {
        char *resolved = realpath(exe, NULL);
        if (resolved && strlen(resolved) < sizeof(exe)) snprintf(exe, sizeof(exe), "%s", resolved);
        free(resolved);
    }
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) { exe[n] = 0; found = 1; }
#endif
    if (found) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            char *path = join(exe, "fdtp-localsend");
            struct stat st;
            if (path && !stat(path, &st) && S_ISREG(st.st_mode) && !access(path, X_OK)) return path;
            free(path);
        }
    }
    return strdup("fdtp-localsend");
}
static int start_child(LocalSend *l, int with_pin)
{
    stop_child(l);
    l->ended = l->failed = 0; l->pin_again = 0;
    l->total_bytes = l->finished_bytes = l->file_bytes = l->done_bytes = 0; l->finished_files = 0;
    char **argv = calloc(l->count + 16, sizeof(*argv));
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    if (!argv || pipe_flags(in) || pipe_flags(out)) goto fail;
    size_t n = 0;
    argv[n++] = l->helper;
    argv[n++] = l->mode == LS_SCAN ? "scan" : l->mode == LS_SEND ? "send" : l->mode == LS_BROWSER ? "serve" : "receive";
    argv[n++] = "--alias"; argv[n++] = l->alias;
    if (l->mode == LS_SCAN) { argv[n++] = "--seconds"; argv[n++] = "0"; }
    else if (l->mode == LS_SEND) {
        argv[n++] = "--to"; argv[n++] = l->target;
        if (with_pin) { argv[n++] = "--pin"; argv[n++] = l->peer_pin; }
        for (size_t i = 0; i < l->count; ++i) argv[n++] = l->paths[i];
    } else if (l->mode == LS_BROWSER) {
        argv[n++] = "--pin"; argv[n++] = l->pin;
        argv[n++] = "--";
        for (size_t i = 0; i < l->count; ++i) argv[n++] = l->paths[i];
    } else {
        argv[n++] = "--ask"; argv[n++] = "--pin"; argv[n++] = l->pin;
        argv[n++] = "--dir"; argv[n++] = l->directory;
    }
    l->child = fork();
    if (l->child < 0) { l->child = 0; goto fail; }
    if (!l->child) {
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0) _exit(126);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, STDERR_FILENO); close(null); }
        execvp(argv[0], argv);
        const char *message = errno == ENOENT ? "error\thelper_missing\n" : "error\thelper_start\n";
        ssize_t ignored = write(STDOUT_FILENO, message, strlen(message)); (void)ignored;
        _exit(127);
    }
    close_fd(&in[0]); close_fd(&out[1]);
    l->input = in[1]; l->output = out[0];
    if (fcntl(l->input, F_SETFL, O_NONBLOCK) < 0 || fcntl(l->output, F_SETFL, O_NONBLOCK) < 0) {
        stop_child(l); free(argv); status(l, T("補助プログラムを起動できません")); return -1;
    }
    free(argv); return 0;
fail:
    close_fd(&in[0]); close_fd(&in[1]); close_fd(&out[0]); close_fd(&out[1]); free(argv);
    status(l, T("補助プログラムを起動できません")); return -1;
}
static int make_pin(char pin[7])
{
    static unsigned previous = 1000000;
    uint32_t value; int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (;;) {
        size_t used = 0;
        while (used < sizeof(value)) {
            ssize_t n = read(fd, (char *)&value + used, sizeof(value) - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return -1; }
            used += (size_t)n;
        }
        if (value >= UINT32_MAX - UINT32_MAX % 1000000) continue;
        value %= 1000000;
        if (value != previous) break;
    }
    close(fd); previous = value; snprintf(pin, 7, "%06u", value); return 0;
}
/* The addresses to tell the other side: IPv4 only, without the loopback and
 * 169.254 (link-local). IPv6 only when there is no IPv4, and then neither
 * ::1 nor fe80 (the photo of the receive screen showed all of them, a global
 * IPv6 one among them, which is better not shown or photographed). */
static void addresses(LocalSend *l)
{
    /* FDTP_LOCALSEND_IPS=wlan0=192.0.2.36,...: the manual's pictures show
     * example addresses ("wlan0: 192.0.2.36  ..."). */
    const char *fixed = getenv("FDTP_LOCALSEND_IPS");
    if (fixed && *fixed) {
        size_t n = 0;
        for (const char *p = fixed; *p && n + 3 < sizeof(l->ips); ++p) {
            if (*p == '=') { l->ips[n++] = ':'; l->ips[n++] = ' '; }
            else if (*p == ',') { l->ips[n++] = ' '; l->ips[n++] = ' '; }
            else l->ips[n++] = *p;
        }
        l->ips[n] = 0;
        return;
    }
    struct ifaddrs *all;
    if (getifaddrs(&all)) { snprintf(l->ips, sizeof(l->ips), "%s", T("IP を取得できません")); return; }
    for (int pass = 0; pass < 2 && !l->ips[0]; ++pass) {
        for (struct ifaddrs *a = all; a; a = a->ifa_next) {
            if (!a->ifa_addr) continue;
            int family = a->ifa_addr->sa_family;
            const void *addr;
            if (pass == 0 && family == AF_INET) {
                const struct in_addr *in = &((struct sockaddr_in *)a->ifa_addr)->sin_addr;
                uint32_t v = ntohl(in->s_addr);
                if ((v >> 24) == 127 || (v >> 16) == 0xa9fe) continue;
                addr = in;
            } else if (pass == 1 && family == AF_INET6) {
                const struct in6_addr *in6 = &((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr;
                if (IN6_IS_ADDR_LOOPBACK(in6) || IN6_IS_ADDR_LINKLOCAL(in6)) continue;
                addr = in6;
            } else continue;
            char ip[INET6_ADDRSTRLEN], item[256];
            if (!inet_ntop(family, addr, ip, sizeof(ip))) continue;
            snprintf(item, sizeof(item), "%s%s: %s", l->ips[0] ? "  " : "", a->ifa_name, ip);
            size_t have = strlen(l->ips), add = strlen(item);
            if (have + add < sizeof(l->ips)) memcpy(l->ips + have, item, add + 1);
        }
    }
    if (!l->ips[0]) snprintf(l->ips, sizeof(l->ips), "%s", T("ネットワークにつながっていません"));
    freeifaddrs(all);
}
static const char *error_text(const char *code)
{
    if (!strcmp(code, "port_busy")) return T("他の LocalSend（公式アプリなど）が 53317 番を使っています。受け取れません。送るのはできます");
    if (!strcmp(code, "refused")) return T("相手が断りました");
    if (!strcmp(code, "pin")) return T("PIN が違います");
    if (!strcmp(code, "network")) return T("相手につながりません");
    if (!strcmp(code, "io")) return T("ファイルを読み書きできません");
    if (!strcmp(code, "helper_missing")) return T("fdtp-localsend がありません（OpenSSL 無しで組み立てた時）");
    if (!strcmp(code, "helper_start")) return T("補助プログラムを起動できません");
    return T("補助プログラムの応答が正しくありません");
}
static int split_fields(char *line, char **fields, int max)
{
    int n = 0;
    while (n < max) {
        fields[n++] = line;
        char *tab = strchr(line, '\t');
        if (!tab) break;
        *tab = 0; line = tab + 1;
    }
    return n;
}
static void peer_event(LocalSend *l, char **f)
{
    size_t i;
    for (i = 0; i < l->peer_count; ++i) if (!strcmp(l->peers[i].fp, f[1])) break;
    if (i == l->peer_count) {
        Peer *peers = realloc(l->peers, (i + 1) * sizeof(*peers));
        if (!peers) { status(l, T("メモリが足りません")); return; }
        l->peers = peers; ++l->peer_count;
    }
    Peer *p = &l->peers[i];
    snprintf(p->fp, sizeof(p->fp), "%s", f[1]); snprintf(p->alias, sizeof(p->alias), "%s", f[2]);
    snprintf(p->ip, sizeof(p->ip), "%s", f[3]); snprintf(p->model, sizeof(p->model), "%s", f[7][0] ? f[7] : f[6]);
}
static void browser_append(LocalSend *l, char ***items, size_t *count, const char *text)
{
    char *copy = strdup(text);
    if (!copy) { status(l, T("メモリが足りません")); return; }
    char **grown = realloc(*items, (*count + 1) * sizeof(**items));
    if (!grown) { free(copy); status(l, T("メモリが足りません")); return; }
    *items = grown; grown[(*count)++] = copy;
}
static void browser_url(LocalSend *l, const char *url)
{
    for (size_t i = 0; i < l->url_count; ++i) if (!strcmp(l->urls[i], url)) return;
    browser_append(l, &l->urls, &l->url_count, url);
}
static void event_line(LocalSend *l, char *line)
{
    char *f[9]; int n = split_fields(line, f, 9);
    if (!strcmp(f[0], "peer") && n >= 8 && l->mode == LS_SCAN) peer_event(l, f);
    else if (!strcmp(f[0], "ready") && l->mode == LS_RECEIVE) status(l, T("受け取りを待っています"));
    else if (!strcmp(f[0], "ready") && n >= 3 && l->mode == LS_BROWSER) {
        browser_url(l, f[2]); status(l, T("ブラウザからの受け取りを待っています"));
    } else if (!strcmp(f[0], "url") && n >= 2 && l->mode == LS_BROWSER) browser_url(l, f[1]);
    else if (!strcmp(f[0], "download") && n >= 3 && l->mode == LS_BROWSER) {
        snprintf(l->status, sizeof(l->status), T("%s に %s を渡しています"), f[1], f[2]);
    } else if (!strcmp(f[0], "downloaded") && n >= 3 && l->mode == LS_BROWSER) {
        const char *format = T("%s が %s を受け取りました");
        size_t size = strlen(format) + strlen(f[1]) + strlen(f[2]) + 1;
        char *record = malloc(size);
        if (record) {
            snprintf(record, size, format, f[1], f[2]);
            if (l->download_count == SAVED_KEEP) {
                free(l->downloads[0]);
                memmove(l->downloads, l->downloads + 1, (SAVED_KEEP - 1) * sizeof(*l->downloads));
                --l->download_count;
            }
            browser_append(l, &l->downloads, &l->download_count, record); free(record);
            status(l, T("ブラウザからの受け取りを待っています"));
        } else status(l, T("メモリが足りません"));
    } else if (!strcmp(f[0], "pin") && n >= 3 && !strcmp(f[1], "wrong") && l->mode == LS_BROWSER) {
        snprintf(l->status, sizeof(l->status), T("%s が PIN を間違えました"), f[2]);
    }
    else if (!strcmp(f[0], "pin") && n >= 2 && l->mode == LS_SEND) {
        l->pin_again = 1;
        status(l, !strcmp(f[1], "wrong") ? T("PIN が違います") : T("相手の PIN が必要です"));
    } else if (!strcmp(f[0], "request") && n >= 4 && l->mode == LS_RECEIVE && l->question == ASK_NONE) {
        char question[256];
        unsigned long long files = strtoull(f[2], NULL, 10), bytes = strtoull(f[3], NULL, 10);
        snprintf(question, sizeof(question), T("%.100s から %llu 個（%.1f MB）を受け取りますか"), f[1], files, bytes / 1048576.0);
        widgets_confirm(&l->app->widgets, question); l->question = ASK_RECEIVE; l->batch_saved = 0;
        l->total_bytes = bytes; l->finished_bytes = l->done_bytes = l->file_bytes = 0;
    } else if (!strcmp(f[0], "file") && n >= 3) {
        if (l->mode == LS_SEND && l->file_bytes == l->done_bytes && l->what[0]) ++l->finished_files;
        l->finished_bytes += l->done_bytes; l->done_bytes = 0; l->file_bytes = strtoull(f[2], NULL, 10);
        snprintf(l->what, sizeof(l->what), "%s", f[1]);
    } else if (!strcmp(f[0], "progress") && n >= 4) {
        snprintf(l->what, sizeof(l->what), "%s", f[1]);
        l->done_bytes = strtoull(f[2], NULL, 10); l->file_bytes = strtoull(f[3], NULL, 10);
    } else if (!strcmp(f[0], "saved") && n >= 2) {
        char *path = strdup(f[1]);
        if (path) {
            size_t i = l->saved_count++ % SAVED_KEEP; ++l->batch_saved;
            free(l->saved[i]); l->saved[i] = path;
        } else status(l, T("メモリが足りません"));
    } else if (!strcmp(f[0], "done")) {
        if (l->mode == LS_SEND) { snprintf(l->status, sizeof(l->status), T("送りました（%zu 個）"), l->count); l->ended = 1; }
        else if (l->mode == LS_RECEIVE) {
            l->what[0] = 0;
            snprintf(l->status, sizeof(l->status), T("%zu 個を受け取りました。続けて受け取れます（Esc で終わります）"), l->batch_saved);
        }
        else l->ended = 1;
    } else if (!strcmp(f[0], "cancelled") && l->mode == LS_RECEIVE) {
        /* The sender left (or sent nothing for a while): a question still
         * open about it is closed unanswered. */
        if (l->question == ASK_RECEIVE) { l->app->widgets.kind = WIDGET_NONE; l->question = ASK_NONE; }
        l->what[0] = 0;
        const char *why = n >= 2 ? f[1] : "";
        status(l, !strcmp(why, "timeout") ? T("相手から届かないので取りやめました。続けて受け取れます")
                : !strcmp(why, "failed") ? T("受け取りに失敗しました。続けて受け取れます")
                : T("相手が取りやめました。続けて受け取れます"));
    } else if (!strcmp(f[0], "error") && n >= 2) {
        if (!strcmp(f[1], "pin") && l->pin_again) return;
        l->failed = 1; status(l, error_text(f[1]));
    }
    l->app->changed = 1;
}
static void send_reply(LocalSend *l)
{
    if (!l->reply[0] || l->input < 0) return;
    struct sigaction ignore = {0}, old;
    ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old)) return;
    ssize_t n = write(l->input, l->reply + l->reply_at, strlen(l->reply) - l->reply_at);
    int error = errno;
    sigaction(SIGPIPE, &old, NULL);
    if (n > 0) { l->reply_at += (size_t)n; if (!l->reply[l->reply_at]) { l->reply[0] = 0; l->reply_at = 0; } }
    else if (n < 0 && error != EAGAIN && error != EINTR) { l->reply[0] = 0; status(l, T("補助プログラムとの接続が切れました")); }
}
static void official_wins(LocalSend *l);
static void tick(Screen *s, App *a, double now)
{
    LocalSend *l = s->self;
    send_reply(l);
    char buf[4096]; size_t budget = 65536;
    while (l->output >= 0 && budget) {
        ssize_t n = read(l->output, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { close_fd(&l->output); break; }
        budget -= (size_t)n;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                if (!l->dropping) { l->event[l->used] = 0; event_line(l, l->event); }
                l->used = 0; l->dropping = 0;
            } else if (l->used + 1 < sizeof(l->event) && !l->dropping) {
                if (buf[i] != '\r') l->event[l->used++] = buf[i];
            } else { l->dropping = 1; l->failed = 1; status(l, error_text("protocol")); }
        }
    }
    if (l->child > 0 && l->output < 0) {
        int result; pid_t p = waitpid(l->child, &result, WNOHANG);
        if (p == l->child || (p < 0 && errno == ECHILD)) {
            l->child = 0; close_fd(&l->input);
            if (!l->ended && !l->failed && !l->pin_again) { l->failed = 1; status(l, T("補助プログラムとの接続が切れました")); }
        }
    }
    if (l->pin_again && !l->child && a->widgets.kind == WIDGET_NONE) {
        l->pin_again = 0; l->question = ASK_PIN;
        widgets_input(&a->widgets, T("相手の画面の PIN"), "", NULL); a->changed = 1;
    }
    if (l->failed && l->question == ASK_RECEIVE) { a->widgets.kind = WIDGET_NONE; l->question = ASK_NONE; }
    /* The official app started while FDTM accepts (receive, browser): ask
     * which to use, or follow LOCALSEND_OFFICIAL. Once a second is enough. */
    if ((l->mode == LS_RECEIVE || l->mode == LS_BROWSER) && l->child > 0 && !l->official_kept &&
        (now - l->official_checked >= 1 || now < l->official_checked)) {
        l->official_checked = now;
        int running = official_running(a);
        if (running && !l->official_seen) {
            l->official_seen = 1;
            if (a->settings.localsend_official == 1) l->official_kept = 1;
            else if (a->settings.localsend_official == 2) official_wins(l);
            else if (l->question == ASK_NONE && a->widgets.kind == WIDGET_NONE) {
                const char *options[] = {T("(F)FDTM で続ける"), T("(L)公式アプリを使う")};
                widgets_choice(&a->widgets, T("公式の LocalSend が起動しました。どちらで受け取りますか"), options, 2);
                l->question = ASK_OFFICIAL; a->changed = 1;
            } else l->official_seen = 0;   /* ask when the box in the way has closed */
        } else if (!running) l->official_seen = 0;
    }
}

/* Wrap without altering the notice's wording or splitting UTF-8. */
static size_t line_bytes(const char *text, int width)
{
    size_t n = 0; int used = 0;
    while (text[n]) {
        size_t bytes = 1;
        while ((text[n + bytes] & 0xc0) == 0x80) ++bytes;
        char ch[8] = "";
        if (bytes >= sizeof(ch)) break;
        memcpy(ch, text + n, bytes);
        int w = cells_width(ch);
        if (used + w > width) break;
        n += bytes; used += w;
    }
    /* Keep ASCII words such as LAN together. */
    if (n && text[n] && text[n] > ' ' && text[n] < 127 && text[n - 1] > ' ' && text[n - 1] < 127) {
        size_t cut = n;
        while (cut && text[cut - 1] > ' ' && text[cut - 1] < 127) --cut;
        if (cut) n = cut;
    }
    /* Kinsoku: no line begins with 、。 and the like (the photo of the receive
     * screen had a line starting with 、); the character before goes down too. */
    static const char *const no_start[] = {"、", "。", "，", "．", "）", "」", "』", "】", "！", "？", "：", "；", "ー", "・"};
    for (size_t i = 0; n && text[n] && i < sizeof(no_start) / sizeof(no_start[0]); ++i) {
        if (strncmp(text + n, no_start[i], strlen(no_start[i]))) continue;
        size_t back = n - 1;
        while (back && (text[back] & 0xc0) == 0x80) --back;
        if (back) n = back;
        break;
    }
    return n;
}
static int wrap(Cells *c, int row, const char *text, int color)
{
    int width = FD_COLS - 4;
    while (*text && row < FRAME_BOTTOM) {
        size_t n = line_bytes(text, width); char line[4096];
        if (!n || n >= sizeof(line)) break;
        memcpy(line, text, n); line[n] = 0;
        cells_text(c, 2, row++, line, color, BG, width); text += n;
    }
    return row;
}
static int draw_ips(LocalSend *l, Cells *c, int row)
{
    int lines = 0, width = FD_COLS - 4;
    for (const char *p = l->ips; *p; ++lines) {
        size_t n = line_bytes(p, width); if (!n) break; p += n;
    }
    l->ip_lines = lines; l->ip_room = FD_ROWS - 23;
    if (l->ip_room < 2) l->ip_room = 2;
    if (l->ip_room > lines) l->ip_room = lines;
    if (l->ip_top + l->ip_room > lines) l->ip_top = lines - l->ip_room;
    cells_text(c, 2, row++, lines > l->ip_room ? "IP (PgUp/PgDn)" : "IP", CYAN, BG, width);
    const char *p = l->ips;
    for (int i = 0; *p; ++i) {
        size_t n = line_bytes(p, width); char line[4096];
        if (!n || n >= sizeof(line)) break;
        if (i >= l->ip_top && i < l->ip_top + l->ip_room) {
            memcpy(line, p, n); line[n] = 0; cells_text(c, 2, row++, line, CYAN, BG, width);
        }
        p += n;
    }
    return row;
}
static void big_pin(Cells *c, int row, const char *pin)
{
    static const char digits[10][16] = {
        "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
        "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"
    };
    int x = (FD_COLS - 29) / 2;
    for (int i = 0; i < 6; ++i) for (int y = 0; y < 5; ++y) for (int col = 0; col < 3; ++col)
        cells_put(c, x + i * 5 + col, row + y, ' ', YELLOW, digits[pin[i] - '0'][y * 3 + col] == '1' ? YELLOW : BG);
}
/* Browser URLs, selections and records share one scrollable list. Keep the
 * warning and PIN in place while every wrapped line remains reachable. */
static void browser_item(LocalSend *l, Cells *c, int row, size_t *line, const char *text, int color)
{
    while (*text) {
        size_t n = line_bytes(text, FD_COLS - 4);
        if (!n) break;
        if (c && *line >= l->browser_top && *line - l->browser_top < l->browser_room) {
            char part[EVENT_CAP];
            if (n >= sizeof(part)) break;
            memcpy(part, text, n); part[n] = 0;
            cells_text(c, 2, row + (int)(*line - l->browser_top), part, color, BG, FD_COLS - 4);
        }
        ++*line; text += n;
    }
}
static size_t browser_contents(LocalSend *l, Cells *c, int row)
{
    size_t line = 0;
    browser_item(l, c, row, &line, T("ブラウザで次の所を開いてください"), CYAN);
    for (size_t i = 0; i < l->url_count; ++i) browser_item(l, c, row, &line, l->urls[i], WHITE);
    browser_item(l, c, row, &line, T("渡す物:"), CYAN);
    for (size_t i = 0; i < l->count; ++i) {
        const char *name = strrchr(l->paths[i], '/');
        browser_item(l, c, row, &line, name ? name + 1 : l->paths[i], WHITE);
    }
    browser_item(l, c, row, &line, T("ダウンロードの記録:"), CYAN);
    for (size_t i = 0; i < l->download_count; ++i) browser_item(l, c, row, &line, l->downloads[i], WHITE);
    return line;
}
static void draw(Screen *s, App *a, Canvas *canvas, Cells *c)
{
    LocalSend *l = s->self;
    if (l->mode == LS_MENU) { if (l->below && l->below->draw) l->below->draw(l->below, a, canvas, c); return; }
    frame_open(c); frame_title(c, FRAME_TITLE_COL, l->mode == LS_RECEIVE ? T("LocalSend: 受け取る") : l->mode == LS_BROWSER ? T("LocalSend: ブラウザで渡す") : T("LocalSend: 送る"), FD_COLS - 8, NULL, 0);
    int row = wrap(c, 1, T("同じネットワークにいる人には、この機器の名前が見えます。自宅など信頼できる LAN か、スマートフォンのインターネット共有（テザリング）で使ってください"), YELLOW);
    frame_hline(c, row++);
    if (l->mode == LS_RECEIVE) {
        char label[512]; snprintf(label, sizeof(label), T("自分の名前: %s"), l->alias); row = wrap(c, row, label, WHITE);
        row = draw_ips(l, c, row);
        snprintf(label, sizeof(label), "PIN: %s", l->pin); row = wrap(c, row, label, YELLOW);
        big_pin(c, row, l->pin); row += 5;
        row = wrap(c, row, T("受け取り先:"), CYAN);
        cells_path(c, 2, row++, l->directory, WHITE, BG, FD_COLS - 4);
    }
    if (l->mode == LS_BROWSER) {
        row = wrap(c, row, T("暗号化されません。自宅など信頼できる LAN だけで使ってください"), YELLOW);
        char label[32]; snprintf(label, sizeof(label), "PIN: %s", l->pin); row = wrap(c, row, label, YELLOW);
        big_pin(c, row, l->pin); row += 5;
    }
    row = wrap(c, row, l->status, l->failed ? YELLOW : CYAN);
    if (l->mode == LS_BROWSER) {
        l->browser_room = row < FRAME_BOTTOM ? (size_t)(FRAME_BOTTOM - row) : 0;
        l->browser_lines = browser_contents(l, NULL, row);
        size_t last = l->browser_lines > l->browser_room ? l->browser_lines - l->browser_room : 0;
        if (l->browser_top > last) l->browser_top = last;
        browser_contents(l, c, row);
    } else if (l->mode == LS_SCAN) {
        size_t room = row < FRAME_BOTTOM ? (size_t)(FRAME_BOTTOM - row) : 0;
        if (l->selected < l->top) l->top = l->selected;
        if (room && l->selected >= l->top + room) l->top = l->selected - room + 1;
        for (size_t i = l->top; i < l->peer_count && row < FRAME_BOTTOM; ++i, ++row) {
            Peer *p = &l->peers[i]; char label[1024];
            snprintf(label, sizeof(label), "%s  %s  %s", p->alias, p->model, p->ip);
            int selected = i == l->selected;
            for (int col = 2; col < FD_COLS - 2; ++col) cells_put(c, col, row, ' ', selected ? BG : WHITE, selected ? CYAN : BG);
            cells_text(c, 2, row, label, selected ? BG : WHITE, selected ? CYAN : BG, FD_COLS - 4);
        }
    } else {
        if (l->what[0]) {
            row = wrap(c, row, l->what, WHITE);
            double fraction = l->file_bytes ? (double)l->done_bytes / l->file_bytes : 0;
            double overall = l->mode == LS_SEND ? (l->count ? (l->finished_files + fraction) / l->count : 0) :
                (l->total_bytes ? ((double)l->finished_bytes + l->done_bytes) / l->total_bytes : 0);
            if (overall > 1 || (l->mode == LS_SEND && l->ended)) overall = 1;
            char progress[128]; snprintf(progress, sizeof(progress), T("全体: %.0f%%"), overall * 100); row = wrap(c, row, progress, CYAN);
        }
        size_t room = row < FRAME_BOTTOM ? (size_t)(FRAME_BOTTOM - row) : 0;
        size_t from = l->saved_count > room ? l->saved_count - room : 0;
        if (l->saved_count - from > SAVED_KEEP) from = l->saved_count - SAVED_KEEP;
        for (size_t i = from; i < l->saved_count && row < FRAME_BOTTOM; ++i)
            cells_path(c, 2, row++, l->saved[i % SAVED_KEEP], WHITE, BG, FD_COLS - 4);
    }
    const FrameKey keys[] = {{"↑↓", T("送り先")}, {"Enter", T("送る")}, {"Esc", T("止めて戻る")}};
    if (l->mode == LS_SCAN) frame_keys(c, 2, keys, 3);
    else if (l->mode == LS_BROWSER) {
        const FrameKey browser[] = {{"↑↓ PgUp/PgDn", T("一覧を移動")}, {"Home/End", T("先頭/末尾")}, {"Esc", T("止めて戻る")}};
        frame_keys(c, 2, browser, 3);
    }
    else { const FrameKey esc[] = {{"", T("Esc で止めます")}}; frame_keys(c, 2, esc, 1); }
}
/* Stops accepting so that the official app can take port 53317. */
static void official_wins(LocalSend *l)
{
    stop_child(l); l->ended = 1; l->failed = 1;
    status(l, T("公式の LocalSend アプリに任せました。受け取りは公式アプリでしてください（Esc で戻ります）"));
}
static void leave(LocalSend *l, App *a)
{
    stop_child(l);
    for (int i = 0; i < a->depth; ++i) if (a->screens[i] != &l->screen) filer_reload(a->screens[i], a);
}
/* The receiver chooses its PIN: the official apps take any text (FxDR7m),
 * not only six digits. */
static int valid_pin(const char *pin)
{
    size_t n = strlen(pin);
    if (!n || n >= 65) return 0;
    for (const unsigned char *p = (const unsigned char *)pin; *p; ++p) if (*p < 32 || *p == 127) return 0;
    return 1;
}
/* docs/FDTP_MACOS.md: while the official app runs, FDTM only sends
 * (unless LOCALSEND_OFFICIAL=fdtm). */
static int official_blocks(LocalSend *l, App *a)
{
    if (a->settings.localsend_official == 1 || !official_running(a)) return 0;
    (void)l; app_error(a, T("公式の LocalSend アプリが動いています。受け取りは公式アプリでしてください。FDTM からは送れます"));
    return 1;
}
static int key(Screen *s, App *a, const Key *k)
{
    LocalSend *l = s->self;
    if (l->question != ASK_NONE) {
        Question question = l->question;
        Key answer = *k;
        if (question == ASK_RECEIVE && k->key == KRET) answer = (Key){KCHAR, 'n', 0};
        WidgetResult r = widgets_key(&a->widgets, &answer);
        if (r != WIDGET_ACCEPT && r != WIDGET_CANCEL) return 0;
        l->question = ASK_NONE;
        if (question == ASK_MENU) {
            if (r == WIDGET_CANCEL) return 1;
            if (a->widgets.selected == 0 || a->widgets.selected == 2) {
                if (!l->count) { app_error(a, T("送る物を選んでください（.. は送れません）")); return 1; }
                if (a->widgets.selected == 2) {
                    if (official_blocks(l, a)) return 1;
                    l->mode = LS_BROWSER;
                    if (make_pin(l->pin)) { app_error(a, T("PIN を作れません")); return 1; }
                    status(l, T("ブラウザで渡す準備をしています")); start_child(l, 1);
                } else { l->mode = LS_SCAN; status(l, T("送り先を探しています")); start_child(l, 0); }
            } else {
                if (official_blocks(l, a)) return 1;
                l->mode = LS_RECEIVE;
                if (make_pin(l->pin)) { app_error(a, T("PIN を作れません")); return 1; }
                addresses(l); status(l, T("受け取りを準備しています")); start_child(l, 1);
            }
        } else if (question == ASK_OFFICIAL) {
            if (r == WIDGET_ACCEPT && a->widgets.selected == 1) official_wins(l);
            else { l->official_kept = 1; status(l, T("FDTM で受け付けを続けます。公式アプリは 53317 番を使えません")); }
        } else if (question == ASK_PIN) {
            if (r == WIDGET_CANCEL) { leave(l, a); return 1; }
            if (!valid_pin(a->widgets.text)) {
                l->question = ASK_PIN; widgets_input(&a->widgets, T("相手の画面の PIN"), "", NULL); return 0;
            }
            snprintf(l->peer_pin, sizeof(l->peer_pin), "%s", a->widgets.text); l->what[0] = 0;
            status(l, T("送っています")); start_child(l, 1);
        } else if (question == ASK_RECEIVE) {
            snprintf(l->reply, sizeof(l->reply), "%s\n", r == WIDGET_ACCEPT ? "accept" : "reject"); l->reply_at = 0; send_reply(l);
            status(l, r == WIDGET_ACCEPT ? T("受け取っています") : T("受け取りを待っています"));
        }
        return 0;
    }
    if (k->key == KESC) { leave(l, a); return 1; }
    if (l->mode == LS_BROWSER) {
        size_t last = l->browser_lines > l->browser_room ? l->browser_lines - l->browser_room : 0;
        if (k->key == KUP && l->browser_top) --l->browser_top;
        if (k->key == KDOWN && l->browser_top < last) ++l->browser_top;
        if (k->key == KPGUP) l->browser_top = l->browser_top > l->browser_room ? l->browser_top - l->browser_room : 0;
        if (k->key == KPGDN) l->browser_top = last - l->browser_top > l->browser_room ? l->browser_top + l->browser_room : last;
        if (k->key == KHOME) l->browser_top = 0;
        if (k->key == KEND) l->browser_top = last;
    }
    if (l->mode == LS_RECEIVE) {
        if (k->key == KPGUP) { l->ip_top -= l->ip_room; if (l->ip_top < 0) l->ip_top = 0; }
        if (k->key == KPGDN && l->ip_top + l->ip_room < l->ip_lines) l->ip_top += l->ip_room;
    }
    if (l->mode == LS_SCAN) {
        if (k->key == KUP && l->selected) --l->selected;
        if (k->key == KDOWN && l->selected + 1 < l->peer_count) ++l->selected;
        if (k->key == KRET && l->peer_count) {
            snprintf(l->target, sizeof(l->target), "%s", l->peers[l->selected].fp);
            l->mode = LS_SEND; status(l, T("送っています")); start_child(l, 0);
        }
    }
    return 0;
}
static void destroy(Screen *s)
{
    LocalSend *l = s->self; stop_child(l);
    for (size_t i = 0; i < l->count; ++i) free(l->paths[i]);
    for (size_t i = 0; i < SAVED_KEEP; ++i) free(l->saved[i]);
    for (size_t i = 0; i < l->url_count; ++i) free(l->urls[i]);
    for (size_t i = 0; i < l->download_count; ++i) free(l->downloads[i]);
    free(l->urls); free(l->downloads);
    free(l->paths); free(l->directory); free(l->helper); free(l->peers); memset(l->pin, 0, sizeof(l->pin)); memset(l->peer_pin, 0, sizeof(l->peer_pin)); free(l);
}
int localsend_ui_open(App *a, const FileSelection *selection)
{
    LocalSend *l = calloc(1, sizeof(*l));
    if (!l) goto fail;
    l->input = l->output = -1; l->app = a; l->below = app_top(a);
    l->directory = realpath(selection->directory, NULL); l->helper = helper_path();
    l->paths = calloc(selection->count ? selection->count : 1, sizeof(*l->paths));
    if (!l->directory || !l->helper || !l->paths) goto cleanup;
    for (size_t i = 0; i < selection->count; ++i) {
        if (!strcmp(selection->names[i], "..")) continue;
        char *path = join(l->directory, selection->names[i]);
        if (!path) goto cleanup;
        l->paths[l->count++] = path;
    }
    if (a->settings.localsend_alias[0]) snprintf(l->alias, sizeof(l->alias), "%s", a->settings.localsend_alias);
    else if (gethostname(l->alias, sizeof(l->alias) - 1)) snprintf(l->alias, sizeof(l->alias), "FDTP");
    l->screen = (Screen){l, key, draw, 0, tick, destroy};
    if (app_push(a, &l->screen)) goto cleanup;
    const char *options[] = {T("(S)送る"), T("(R)受け取る"), T("(B)ブラウザで渡す")};
    widgets_choice(&a->widgets, "LocalSend", options, 3); l->question = ASK_MENU;
    return 0;
cleanup: destroy(&(Screen){.self = l});
fail: app_error(a, T("メモリが足りません")); return -1;
}
