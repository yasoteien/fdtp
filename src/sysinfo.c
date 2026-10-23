// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _DARWIN_C_SOURCE
/* Revision 1 item 31: the system information (Alt+i), as FD's: the least,
 * on one screen (80 x 25 too), in the frame of every full screen (cells.h
 * frame_open, docs/FDTP_03 section 7). It also shows the Linux settings FDTP
 * cannot change (the GPU's time before it sleeps). Everything is read once,
 * when it opens; any key goes back.
 *
 * /proc, /sys, /etc and /dev are read under FDTP_SYSINFO_ROOT when it is set
 * (the tests: tests/check_sysinfo.py makes a tree of them on the Mac). What
 * cannot be read says why: 「ありません」 (no such file), 「読めません（権限）」. */
#include "i18n.h"
#include "sysinfo.h"
#include "version.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

/* The Pomera's GPU (revision 1 item 28, FDTP_05 section 8): it should stay
 * awake as long as AWAKE_MINUTES' default, 15 minutes. */
#define GPU_DELAY "/sys/devices/platform/10090000.gpu/power/autosuspend_delay_ms"
#define GPU_ADVISED_MS 900000L
#define ROWS_MAX 24
#define LABEL_COL 2
#define LINE_COL 18          /* the line between the names and the values */
#define VALUE_COL 20

/* tail: a path after the value, in （）, cut on the left (…) to fit. */
typedef struct { char label[32], value[160], tail[512]; int color, rule; } Row;
typedef struct { Screen screen; Row rows[ROWS_MAX]; int count; } SysInfo;

static void path_of(char *out, size_t size, const char *path)
{
    const char *root = getenv("FDTP_SYSINFO_ROOT");
    snprintf(out, size, "%s%s", root && *root ? root : "", path);
}
/* Why a file cannot be read, for the value. */
static const char *why(int error)
{
    return error == ENOENT || error == ENOTDIR ? T("ありません") : error == EACCES || error == EPERM ? T("読めません（権限）") : T("読めません");
}
/* The start of the file (NUL-terminated, the last newline taken off); -1
 * with errno when it cannot be read. */
static int read_text(const char *path, char *out, size_t size)
{
    char full[1024];
    path_of(full, sizeof(full), path);
    FILE *f = fopen(full, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, size - 1, f);
    int failed = ferror(f);
    fclose(f);
    if (failed) { errno = EIO; return -1; }
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
    return 0;
}
static Row *add(SysInfo *s, const char *label, int color)
{
    if (s->count >= ROWS_MAX) return NULL;
    Row *r = &s->rows[s->count++];
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "");
    r->value[0] = r->tail[0] = 0; r->color = color; r->rule = 0;
    return r;
}
static void rule(SysInfo *s)
{
    Row *r = add(s, NULL, WHITE);
    if (r) r->rule = 1;
}
static void span(char *out, size_t size, long seconds)
{
    long d = seconds / 86400, h = seconds / 3600 % 24, m = seconds / 60 % 60;
    if (d) snprintf(out, size, T("%ld 日 %ld 時間 %ld 分"), d, h, m);
    else if (h) snprintf(out, size, T("%ld 時間 %ld 分"), h, m);
    else snprintf(out, size, T("%ld 分 %ld 秒"), m, seconds % 60);
}
static void bytes(char *out, size_t size, double n)
{
    if (n >= 1024.0 * 1024 * 1024) snprintf(out, size, "%.1f GB", n / (1024.0 * 1024 * 1024));
    else snprintf(out, size, "%.0f MB", n / (1024.0 * 1024));
}

/* ---- the rows ---- */
static void versions(SysInfo *s)
{
    Row *r = add(s, "FDTP", WHITE);
    if (r) snprintf(r->value, sizeof(r->value), "%s", FDTP_VERSION);
    struct utsname u;
    if ((r = add(s, T("カーネル"), WHITE))) {
        /* %.40s: the row is shorter than three utsname fields can be. */
        if (uname(&u) == 0) snprintf(r->value, sizeof(r->value), "%.40s %.60s %.20s", u.sysname, u.release, u.machine);
        else snprintf(r->value, sizeof(r->value), "%s", T("読めません"));
    }
    char text[128];
    if ((r = add(s, "Debian", WHITE))) {
        if (read_text("/etc/debian_version", text, sizeof(text)) == 0) snprintf(r->value, sizeof(r->value), "%s", text);
        else snprintf(r->value, sizeof(r->value), "%s", why(errno));
    }
    if ((r = add(s, T("起動してから"), WHITE))) {
        double up;
        if (read_text("/proc/uptime", text, sizeof(text)) < 0) snprintf(r->value, sizeof(r->value), "%s", why(errno));
        else if (sscanf(text, "%lf", &up) == 1 && up >= 0) span(r->value, sizeof(r->value), (long)up);
        else snprintf(r->value, sizeof(r->value), "%s", T("読めません"));
    }
}
/* /proc/bus/input/devices: a device whose name holds name, and whether its
 * event device can be read. 「有り（読める）」「有り（読めません）」「無し」. */
static const char *device(const char *devices, const char *name)
{
    for (const char *p = devices; (p = strstr(p, "N: Name=\"")); ) {
        p += 9;
        const char *end = strchr(p, '"');
        if (!end) break;
        int match = 0;
        for (const char *q = p; q + strlen(name) <= end; ++q) if (!strncmp(q, name, strlen(name))) { match = 1; break; }
        if (!match) continue;
        const char *block = strstr(end, "\n\n"), *h = strstr(end, "H: Handlers=");
        if (!h || (block && h > block)) return T("有り");
        const char *ev = strstr(h, "event");
        if (!ev || (block && ev > block)) return T("有り");
        char node[64], full[1024];
        int n = 0;
        while (n < 40 && ev[n] && ev[n] != ' ' && ev[n] != '\n') ++n;
        snprintf(node, sizeof(node), "/dev/input/%.*s", n, ev);
        path_of(full, sizeof(full), node);
        return access(full, R_OK) == 0 ? T("有り（読める）") : T("有り（読めません）");
    }
    return T("無し");
}
static void screen_rows(SysInfo *s, App *a)
{
    Row *r = add(s, T("画面"), WHITE);
    if (r) snprintf(r->value, sizeof(r->value), T("%dx%d（絵 %dx%d）・%s"), FD_COLS, FD_ROWS, CANVAS_W, CANVAS_H,
                    a->output ? a->output : T("画面なし（ヘッドレス）"));
    if (!(r = add(s, T("キーの装置"), WHITE))) return;
    static char devices[32768];
    if (read_text("/proc/bus/input/devices", devices, sizeof(devices)) < 0) { snprintf(r->value, sizeof(r->value), "%s", why(errno)); return; }
    snprintf(r->value, sizeof(r->value), "keyd: %s  gpio-keys: %s", device(devices, "keyd virtual keyboard"), device(devices, "gpio-keys"));
}
static void power_rows(SysInfo *s, App *a)
{
    Row *r = add(s, "AWAKE_MINUTES", WHITE);
    int minutes = a->settings.awake_minutes;
    if (r) {
        if (minutes <= 0) snprintf(r->value, sizeof(r->value), "%s", T("0（使わない）"));
        else if (a->output && !strcmp(a->output, "X")) snprintf(r->value, sizeof(r->value), T("%d 分（X のウィンドウでは使わない）"), minutes);
        else if (a->last_key > 0) {
            long left = (long)(a->last_key + minutes * 60.0 - a->now);
            char t[64];
            span(t, sizeof(t), left > 0 ? left : 0);
            snprintf(r->value, sizeof(r->value), T("%d 分（残り %s）"), minutes, t);
        } else snprintf(r->value, sizeof(r->value), T("%d 分"), minutes);
    }
    char text[64];
    if (!(r = add(s, T("GPU の休むまで"), WHITE))) return;
    if (read_text(GPU_DELAY, text, sizeof(text)) < 0) { snprintf(r->value, sizeof(r->value), "%s", why(errno)); return; }
    char *end;
    long ms = strtol(text, &end, 10);
    if (end == text) { snprintf(r->value, sizeof(r->value), T("読めません（%s）"), text); return; }
    char t[64];
    if (ms < 0) snprintf(t, sizeof(t), "%s", T("休まない"));
    else if (ms >= 60000 && ms % 60000 == 0) snprintf(t, sizeof(t), T("%ld 分"), ms / 60000);
    else snprintf(t, sizeof(t), T("%.1f 秒"), ms / 1000.0);
    snprintf(r->value, sizeof(r->value), T("%ld ms（%s）"), ms, t);
    if (ms >= 0 && ms < GPU_ADVISED_MS) {
        /* Shorter than 15 minutes: yellow, and how to change it. (Negative:
         * the kernel's autosuspend is off, the GPU does not sleep.) */
        r->color = YELLOW;
        Row *h = add(s, "", YELLOW);
        if (h) snprintf(h->value, sizeof(h->value), "%s", T("15 分より短い。変え方: 運用マニュアル §8（sudo）"));
    }
}
static long meminfo(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}
/* The first supply with a capacity (a battery); nothing when none. */
static void battery(SysInfo *s)
{
    char dir[1024];
    path_of(dir, sizeof(dir), "/sys/class/power_supply");
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char capacity[32] = "", status[64] = "";
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        if (read_text(path, capacity, sizeof(capacity)) < 0) { capacity[0] = 0; continue; }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", e->d_name);
        if (read_text(path, status, sizeof(status)) < 0) status[0] = 0;
        break;
    }
    closedir(d);
    if (!capacity[0]) return;
    static const char *const names[][2] = {{"Charging", N_("充電中")}, {"Discharging", N_("放電中")}, {"Full", N_("満充電")},
                                            {"Not charging", N_("充電していない")}, {"Unknown", N_("不明")}};
    const char *state = status;
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) if (!strcmp(status, names[i][0])) state = T(names[i][1]);
    Row *r = add(s, T("電池"), WHITE);
    if (r) snprintf(r->value, sizeof(r->value), "%s%%%s%s", capacity, *state ? T("・") : "", state);
}
static void resource_rows(SysInfo *s, const char *directory)
{
    Row *r = add(s, T("メモリ"), WHITE);
    static char text[8192];
    if (r) {
        if (read_text("/proc/meminfo", text, sizeof(text)) < 0) snprintf(r->value, sizeof(r->value), "%s", why(errno));
        else {
            long total = meminfo(text, "MemTotal:"), free = meminfo(text, "MemAvailable:");
            char a[32], b[32];
            bytes(a, sizeof(a), total * 1024.0); bytes(b, sizeof(b), free * 1024.0);
            if (total < 0) snprintf(r->value, sizeof(r->value), "%s", T("読めません"));
            else if (free < 0) snprintf(r->value, sizeof(r->value), T("全体 %s"), a);
            else snprintf(r->value, sizeof(r->value), T("使える %s／全体 %s"), b, a);
        }
    }
    if ((r = add(s, T("ディスクの空き"), WHITE))) {
        struct statvfs v;
        if (!directory || statvfs(directory, &v) < 0) snprintf(r->value, sizeof(r->value), "%s", why(errno));
        else {
            char a[32], b[32];
            bytes(a, sizeof(a), (double)v.f_bavail * (double)v.f_frsize);
            bytes(b, sizeof(b), (double)v.f_blocks * (double)v.f_frsize);
            snprintf(r->value, sizeof(r->value), T("%s／全体 %s"), a, b);
            char home[sizeof(r->tail)];
            snprintf(r->tail, sizeof(r->tail), "%s", cells_home_path(directory, home, sizeof(home)));
        }
    }
    battery(s);
}

/* ---- the screen ---- */
static void sysinfo_draw(Screen *screen, App *a, Canvas *canvas, Cells *c)
{
    (void)a; (void)canvas;
    SysInfo *s = screen->self;
    if (frame_open(c) < 0) return;
    frame_title(c, FRAME_TITLE_COL, T("システム情報"), LINE_COL - 1 - FRAME_TITLE_COL - 1, NULL, 0);
    int y = 1, from = 0;
    for (int i = 0; i < s->count && y < FRAME_BOTTOM; ++i, ++y) {
        const Row *r = &s->rows[i];
        if (r->rule) { frame_vline(c, LINE_COL, from, y); frame_hline(c, y); from = y; continue; }
        cells_text(c, LABEL_COL, y, r->label, CYAN, BG, LINE_COL - 1 - LABEL_COL);
        int room = FRAME_RIGHT - 1 - VALUE_COL, x = VALUE_COL + cells_text(c, VALUE_COL, y, r->value, r->color, BG, room);
        if (r->tail[0]) {
            /* （path）, its start left out while it does not fit. */
            const char *t = r->tail;
            int cut = 0, left = VALUE_COL + room - x - 4;
            while (*t && cells_width(t) + (cut ? 1 : 0) > left) { ++t; while ((*t & 0xc0) == 0x80) ++t; cut = 1; }
            x += cells_text(c, x, y, T("（"), r->color, BG, 2);
            if (cut) x += cells_text(c, x, y, "…", r->color, BG, 2);
            x += cells_text(c, x, y, t, r->color, BG, left);
            cells_text(c, x, y, T("）"), r->color, BG, 2);
        }
    }
    frame_vline(c, LINE_COL, from, FRAME_BOTTOM);
    const FrameKey keys[] = {{T("何かキー"), T("戻る")}};
    frame_keys(c, 2, keys, 1);
}
static int sysinfo_key(Screen *screen, App *a, const Key *k) { (void)screen; (void)a; (void)k; return 1; }
static void sysinfo_destroy(Screen *screen) { free(screen->self); }
Screen *sysinfo_create(App *a, const char *directory)
{
    SysInfo *s = calloc(1, sizeof(*s));
    if (!s) { app_notice(a, T("メモリが足りません")); return NULL; }
    versions(s); rule(s);
    screen_rows(s, a); rule(s);
    power_rows(s, a); rule(s);
    resource_rows(s, directory);
    s->screen = (Screen){s, sysinfo_key, sysinfo_draw, 0, NULL, sysinfo_destroy};
    return &s->screen;
}
