// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* The stock dictionaries in the reader box (revision 1 §16): the choice of
 * book, the keys and the box; fdtp-dict reads the books (dict_ui.h). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "dict_ui.h"
#include "i18n.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* The books in the order of the stock 「辞書選択」; the names are titles. */
static const struct { const char *arg, *tab; } books[4] = {
    {"kr", "角川類語新辞典"}, /* i18n:keep */
    {"mk", "明鏡国語辞典"},   /* i18n:keep */
    {"ej", "ジーニアス英和"}, /* i18n:keep */
    {"je", "ジーニアス和英"}, /* i18n:keep */
};
enum { KR, MK, EJ, JE };
static unsigned avail;
static int cur = MK, last_japanese = MK, shown, count, at;
static char word[256];
static char out[WIDGET_READER_CAP + 1024];

/* fdtp-dict: $FDTP_DICT_HELPER, else beside this program; NULL when absent. */
static char *helper_path(void)
{
    const char *override = getenv("FDTP_DICT_HELPER");
    if (override) return *override ? strdup(override) : NULL;
    char exe[4096];
    int found = 0;
#ifdef __APPLE__
    uint32_t size = sizeof(exe);
    found = _NSGetExecutablePath(exe, &size) == 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) { exe[n] = 0; found = 1; }
#endif
    char *slash = found ? strrchr(exe, '/') : NULL;
    if (!slash) return NULL;
    *slash = 0;
    size_t len = strlen(exe) + sizeof("/fdtp-dict");
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/fdtp-dict", exe);
    struct stat st;
    if (!stat(path, &st) && S_ISREG(st.st_mode) && !access(path, X_OK)) return path;
    free(path);
    return NULL;
}

/* Runs fdtp-dict with argv[1..] and reads its output into out (at most
 * cap-1 bytes). Returns its exit status, or -1. */
static int run(char *const args[], char *dst, size_t cap)
{
    char *path = helper_path();
    if (!path) return -1;
    int p[2];
    if (pipe(p)) { free(path); return -1; }
    pid_t child = fork();
    if (child < 0) { close(p[0]); close(p[1]); free(path); return -1; }
    if (child == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 2); }
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *argv[8] = {path};
        for (int i = 0; args[i] && i < 6; ++i) argv[i + 1] = args[i];
        execv(path, argv);
        _exit(127);
    }
    free(path);
    close(p[1]);
    size_t n = 0;
    for (;;) {
        char sink[4096];
        char *to = n + 1 < cap ? dst + n : sink;
        size_t room = n + 1 < cap ? cap - 1 - n : sizeof(sink);
        ssize_t r = read(p[0], to, room);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (to != sink) n += (size_t)r;   /* the rest is read and dropped */
    }
    dst[n] = 0;
    close(p[0]);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int dict_ui_available(void)
{
    char buf[32];
    char *args[] = {"books", NULL};
    avail = run(args, buf, sizeof(buf)) == 0 ? (unsigned)strtoul(buf, NULL, 10) & 15u : 0;
    return avail != 0;
}
const char *dict_ui_word(void) { return word; }

/* Looks up text in book cur, entry index: fills word and count, the box. */
static void show(Widgets *w, const char *text, int how, int index)
{
    static const char *const hows[] = {"exact", "from", "upto"};
    char number[16];
    snprintf(number, sizeof(number), "%d", index);
    char *args[] = {"look", (char *)books[cur].arg, (char *)hows[how], number, (char *)text, NULL};
    char heading[512] = "";
    const char *body = "";
    int rc = run(args, out, sizeof(out));
    count = 0;
    if (rc == 0) {
        /* word\t…\ncount\t…\nheading\t…\n\n then the text */
        char *line = out, *next;
        for (int i = 0; i < 3 && line; ++i, line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab++ = 0;
            if (!strcmp(line, "word")) snprintf(word, sizeof(word), "%s", tab);
            else if (!strcmp(line, "count")) count = atoi(tab);
            else if (!strcmp(line, "heading")) snprintf(heading, sizeof(heading), "%s", tab);
        }
        if (line && *line == '\n') ++line;
        body = line ? line : "";
    } else {
        snprintf(word, sizeof(word), "%s", text);
    }
    if (count < 0) count = 0;
    at = index < count ? index : 0;
    char title[600];
    if (rc != 0) snprintf(title, sizeof(title), "%s", T("辞書を開けません"));
    else if (!count) { snprintf(title, sizeof(title), T("「%s」は見つかりません"), word); body = T("RET で別の語を引けます。←→ で辞書を替えます。"); }
    else if (count > 1) snprintf(title, sizeof(title), "%s  (%d/%d)", heading, at + 1, count);
    else snprintf(title, sizeof(title), "%s", heading);
    const char *tabs[4];
    int ntabs = 0, tab = 0;
    for (int i = 0; i < 4; ++i)
        if (avail & 1u << i) {
            if (i == cur) tab = ntabs;
            tabs[ntabs++] = books[i].tab;
        }
    widgets_reader(w, T("電子辞書"), tabs, ntabs, tab, title, body,
                   count > 1 ? T("←→:辞書 S←→:項目 RET:別の語 Esc:閉じる") : T("←→:辞書 RET:別の語 Esc:閉じる"));
    shown = 1;
}

static int ascii(const char *s)
{
    for (; *s; ++s) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

void dict_ui_lookup(Widgets *w, const char *text, int how)
{
    if (!dict_ui_available()) { shown = 0; widgets_error(w, T("純正の辞書が見つかりません")); return; }
    while (*text == ' ' || *text == '\t') ++text;
    char t[1024];
    snprintf(t, sizeof(t), "%s", text);
    for (size_t n = strlen(t); n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n'); ) t[--n] = 0;
    for (char *p = t; *p; ++p) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    cur = ascii(t) && (avail & 1u << EJ) ? EJ : last_japanese;
    if (!(avail & 1u << cur))
        for (int i = 0; i < 4; ++i) if (avail & 1u << i) { cur = i; break; }
    show(w, t, how, 0);
}

int dict_ui_reopen(Widgets *w)
{
    if (!shown || !avail) return 0;
    char again[256];
    snprintf(again, sizeof(again), "%s", word);
    show(w, again, DICT_UI_EXACT, at);
    return 1;
}

int dict_ui_key(Widgets *w, const Key *k)
{
    if (k->key == KESC) { w->kind = WIDGET_NONE; return DICT_UI_CLOSED; }
    if (k->key == KRET && !(k->mods & (MOD_CTRL | MOD_ALT))) return DICT_UI_ASK;
    char again[256];
    snprintf(again, sizeof(again), "%s", word);
    /* Tab → or Shift+Tab ←: the next or previous book (the same word) */
    int arrow = (k->key == KLEFT || k->key == KRIGHT) && !(k->mods & (MOD_CTRL | MOD_ALT));
    if ((k->key == KTAB && !(k->mods & (MOD_CTRL | MOD_ALT))) || (arrow && !(k->mods & MOD_SHIFT))) {
        int step = (k->key == KTAB ? k->mods & MOD_SHIFT : k->key == KLEFT) ? 3 : 1;
        for (int i = 1; i <= 4; ++i) {
            int next = (cur + step * i) % 4;
            if (avail & 1u << next) { cur = next; break; }
        }
        if (cur != EJ) last_japanese = cur;
        show(w, again, DICT_UI_EXACT, 0);
        return DICT_UI_OPEN;
    }
    if (arrow && (k->mods & MOD_SHIFT) && count > 1) {   /* Shift+← →: the entries of the word */
        show(w, again, DICT_UI_EXACT, (at + (k->key == KRIGHT ? 1 : count - 1)) % count);
        return DICT_UI_OPEN;
    }
    widgets_key(w, k);
    return w->kind == WIDGET_READER ? DICT_UI_OPEN : DICT_UI_CLOSED;
}
