// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Revision 1 item 39: the PDF menu (Alt+p; Alt+n and Alt+h on PDFs, item
 * 37), docs/FDTP_03 §8. A floating menu, then the questions in the usual
 * boxes (a password in a box of its own: '*' and no history), then the
 * work in pdftools_job.c a little at every tick under a box of progress,
 * and a box of what was written. Only on the Pomera (the bundled MuPDF);
 * without FDTP_WITH_MUPDF (the Mac) a notice says so. */
#include "i18n.h"
#include "pdftools.h"
#include <string.h>
#include <strings.h>
int pdftools_is_pdf(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 4 && !strcasecmp(name + n - 4, ".pdf");
}
#ifndef FDTP_WITH_MUPDF
int pdftools_open(App *a, const FileSelection *s, PdfToolsEntry first, int page)
{
    (void)s; (void)first; (void)page;
    app_notice(a, T("この環境では PDF の操作はできません")); return -1;
}
#else
#include "pdftools_job.h"
#include "order_box.h"
#include "raster.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef enum { OP_MERGE, OP_SPLIT, OP_EXTRACT, OP_DELETE, OP_REORDER, OP_ROTATE, OP_PROTECT, OP_UNLOCK,
               OP_COMPRESS, OP_JPEG, OP_IMAGES, OP_REPAIR, OP_COUNT } Op;
/* What an entry works on: one PDF, two or more PDFs, JPEG/PNG images. */
typedef enum { ONE_PDF, PDFS, IMAGES } Need;
/* The menu, in the order of item 39 (第 1 段, a line, 第 2 段). suffix: the
 * default new name (元_suffix.pdf). */
static const struct { char key; const char *name, *about, *done, *suffix; Need need; JobKind job; } ops[OP_COUNT] = {
    { 'm', N_("結合"), N_("複数の PDF を 1 つにつなぐ"), N_("結合しました"), "_merged", PDFS, JOB_MERGE },
    { 's', N_("分割"), N_("ページの範囲ごと・N ページごとに分ける"), N_("分割しました"), "", ONE_PDF, JOB_SPLIT },
    { 'e', N_("ページの抽出"), N_("選んだページだけを書き出す"), N_("ページを抽出しました"), "_extract", ONE_PDF, JOB_PAGES },
    { 'd', N_("ページの削除"), N_("選んだページを除いて書き出す"), N_("ページを削除しました"), "_deleted", ONE_PDF, JOB_PAGES },
    { 'o', N_("並べ替え"), N_("ページの順番を変える（例 3,1,2,4-）"), N_("並べ替えました"), "_reordered", ONE_PDF, JOB_PAGES },
    { 'r', N_("回転"), N_("選んだページを 90 度ずつ回す"), N_("回転しました"), "_rotated", ONE_PDF, JOB_ROTATE },
    { 'p', N_("保護"), N_("パスワードを付ける（AES-256）"), N_("パスワードを付けました"), "_protected", ONE_PDF, JOB_PROTECT },
    { 'u', N_("ロック解除"), N_("パスワードを外す"), N_("パスワードを外しました"), "_unlocked", ONE_PDF, JOB_UNLOCK },
    { 'c', N_("圧縮"), N_("中の画像の解像度を落として小さくする"), N_("圧縮しました"), "_compressed", ONE_PDF, JOB_COMPRESS },
    { 'j', "PDF→JPEG", N_("ページごとの JPEG にする"), N_("JPEG にしました"), "", ONE_PDF, JOB_JPEG },
    { 'i', N_("画像→PDF"), N_("JPEG・PNG を 1 枚 1 ページの PDF にする"), N_("PDF にしました"), "", IMAGES, JOB_IMAGES },
    { 'f', N_("修復"), N_("開いて保存し直す"), N_("修復しました"), "_repaired", ONE_PDF, JOB_REPAIR },
};
#define MENU_LINE_AFTER OP_UNLOCK   /* the line between the two groups */
#define MENU_NAME_W 14
/* A list of pages longer than this is refused (a typing mistake). */
#define PAGES_MAX 100000
#define PASSWORD_CAP 128
#define WORK_SLICE 0.15             /* seconds of work per tick */

typedef enum { PH_MENU, PH_ASK, PH_PASSWORD, PH_ORDER, PH_RUN, PH_CLOSE } Phase;
typedef enum { Q_NONE, Q_SPLIT_MODE, Q_RANGES, Q_EVERY, Q_ANGLE, Q_LEVEL, Q_DPI, Q_WHERE, Q_NAME, Q_OVERWRITE } Question;
typedef enum { PW_SOURCE, PW_NEW, PW_AGAIN } Secret;
typedef struct {
    Screen screen, *below;
    Op op;
    Phase phase;
    Question asked;
    int reask;              /* ask again at the next tick, after an error box */
    char *directory;
    size_t count;           /* the targets: names (in the directory), full paths, ... */
    char **names, **paths, **passwords;
    int *pages, *encrypted, *probed;
    int n_pdf, n_image, viewer_page;
    int menu, menu_top;
    /* The answers. */
    int mode;               /* split: 1 ranges, 2 every N pages */
    char ranges[WIDGET_CAP], every[32], dpi_text[16], name[WIDGET_CAP];
    int have_ranges, every_n, angle, level, dpi, where, have_name, overwrite_ok, order_ok, have_password;
    int *list; size_t list_count;
    size_t *groups; size_t group_count;
    char password[PASSWORD_CAP];
    /* The password box. */
    Secret secret_kind;
    size_t secret_source;
    char secret[PASSWORD_CAP], secret_label[300];
    size_t secret_len;
    char **outputs; size_t output_count;
    PdfJob *job;
} Tools;

static void wipe(char *s, size_t n) { volatile char *p = s; while (n--) *p++ = 0; }
static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2; char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, name);
    return p;
}
static const char *base_of(const char *path) { const char *b = strrchr(path, '/'); return b ? b + 1 : path; }
static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}
static int is_image(const char *name) { return has_ext(name, ".jpg") || has_ext(name, ".jpeg") || has_ext(name, ".png"); }
/* name without its extension (".pdf", or the image's), into out. */
static void stem(const char *name, char *out, size_t size)
{
    const char *dot = strrchr(name, '.');
    size_t n = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    if (n >= size) n = size - 1;
    while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
    memcpy(out, name, n); out[n] = 0;
}
static void size_text(long long bytes, char *out, size_t size)
{
    if (bytes < 1024) snprintf(out, size, T("%lld バイト"), bytes);
    else if (bytes < 1024 * 1024) snprintf(out, size, "%.1f KB", bytes / 1024.0);
    else snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
}

static void free_outputs(Tools *t)
{
    for (size_t i = 0; i < t->output_count; ++i) free(t->outputs[i]);
    free(t->outputs); t->outputs = NULL; t->output_count = 0;
}
static void tools_destroy(Screen *s)
{
    Tools *t = s->self;
    job_free(t->job);
    for (size_t i = 0; i < t->count; ++i) {
        free(t->names[i]); free(t->paths[i]);
        if (t->passwords[i]) { wipe(t->passwords[i], strlen(t->passwords[i])); free(t->passwords[i]); }
    }
    free(t->names); free(t->paths); free(t->passwords); free(t->pages); free(t->encrypted); free(t->probed);
    free(t->list); free(t->groups); free_outputs(t);
    wipe(t->password, sizeof(t->password)); wipe(t->secret, sizeof(t->secret));
    free(t->directory); free(t);
}
/* Closes the tools now when they are on top (else at the next tick: the
 * order box may still be over them). t may be gone afterwards. */
static void close_tools(App *a, Tools *t)
{
    if (app_top(a) == &t->screen) app_pop(a); else t->phase = PH_CLOSE;
}
static void reload_filers(App *a) { for (int i = 0; i < a->depth; ++i) filer_reload(a->screens[i], a); }
static void fail(App *a, Tools *t, const char *message)
{
    char copy[300]; snprintf(copy, sizeof(copy), "%s", message);
    close_tools(a, t); app_error(a, copy);
}

/* ---- page ranges ----------------------------------------------------------- */

static void skip_blanks(const char **p) { while (**p == ' ') ++*p; }
static int number(const char **p, long *value)
{
    if (**p < '0' || **p > '9') return 0;
    long v = 0;
    while (**p >= '0' && **p <= '9') { if (v < 10000000) v = v * 10 + (**p - '0'); ++*p; }
    *value = v; return 1;
}
/* "1-3,5,8-" (and "-3", "5-1" backwards) of a count-page PDF into 0-based
 * pages, one group per item. NULL when it reads, else the message. */
static const char *parse_ranges(const char *text, int count, char *message, size_t size,
                                int **list, size_t *n, size_t **groups, size_t *group_count)
{
    const char *p = text; size_t cap = 64, gcap = 8;
    *n = *group_count = 0; *list = malloc(cap * sizeof(int)); *groups = malloc(gcap * sizeof(size_t));
    if (!*list || !*groups) return T("メモリが足りません");
    skip_blanks(&p);
    if (!*p) return T("ページの範囲が空です");
    for (;;) {
        long from, to;
        skip_blanks(&p);
        if (*p == '-') from = 1; else if (!number(&p, &from)) break;
        skip_blanks(&p);
        if (*p == '-') { ++p; skip_blanks(&p); if (!number(&p, &to)) to = count; } else to = from;
        if (from < 1 || to < 1 || from > count || to > count) {
            snprintf(message, size, T("ページの範囲が違います（1〜%d）"), count); return message;
        }
        if (*group_count + 2 > gcap) { size_t *g = realloc(*groups, (gcap *= 2) * sizeof(size_t)); if (!g) return T("メモリが足りません"); *groups = g; }
        (*groups)[(*group_count)++] = *n;
        for (long k = from;; k += from <= to ? 1 : -1) {
            if (*n >= PAGES_MAX) return T("ページの指定が多すぎます");
            if (*n == cap) { int *l = realloc(*list, (cap *= 2) * sizeof(int)); if (!l) return T("メモリが足りません"); *list = l; }
            (*list)[(*n)++] = (int)k - 1;
            if (k == to) break;
        }
        skip_blanks(&p);
        if (*p == ',') { ++p; continue; }
        if (!*p) { (*groups)[*group_count] = *n; return NULL; }
        break;
    }
    return T("ページの範囲が正しくありません（例 1-3,5,8-）");
}
/* Drops pages listed again (keeps the first). */
static void unique(int *list, size_t *n, int count)
{
    char *seen = calloc((size_t)count, 1); size_t m = 0;
    if (!seen) return;
    for (size_t i = 0; i < *n; ++i) if (!seen[list[i]]) { seen[list[i]] = 1; list[m++] = list[i]; }
    *n = m; free(seen);
}
static int repeated(const int *list, size_t n, int count)
{
    char *seen = calloc((size_t)count, 1); int again = 0;
    if (!seen) return 0;
    for (size_t i = 0; i < n && !again; ++i) { again = seen[list[i]]; seen[list[i]] = 1; }
    free(seen); return again;
}

/* ---- the questions --------------------------------------------------------- */

static void advance(App *a, Tools *t);
static void ask_input(App *a, Tools *t, Question q, const char *label, const char *initial, WidgetHistoryKind kind)
{
    t->phase = PH_ASK; t->asked = q;
    widgets_input_history(&a->widgets, label, initial, t->directory, kind);
}
static void ask_choice(App *a, Tools *t, Question q, const char *label, const char *const *options, int n, int selected)
{
    t->phase = PH_ASK; t->asked = q;
    const char *translated[3];
    for (int i = 0; i < n; ++i) translated[i] = T(options[i]);
    widgets_choice(&a->widgets, label, translated, n);
    a->widgets.selected = selected;
}
static void ask_password(App *a, Tools *t, Secret kind, size_t source)
{
    t->phase = PH_PASSWORD; t->secret_kind = kind; t->secret_source = source;
    wipe(t->secret, sizeof(t->secret)); t->secret_len = 0;
    if (kind == PW_SOURCE) snprintf(t->secret_label, sizeof(t->secret_label), T("%.200s のパスワード"), t->names[source]);
    else snprintf(t->secret_label, sizeof(t->secret_label), "%s", kind == PW_NEW ? T("付けるパスワード") : T("確かめのため、もう一度"));
    a->changed = 1;
}
static void order_done(App *a, void *context, const char *const *names, size_t count, int accepted);
static void ask_order(App *a, Tools *t)
{
    t->phase = PH_ORDER;
    if (order_box_open(a, t->op == OP_MERGE ? T("結合する PDF と順番") : T("PDF にする画像と順番"),
                       (const char *const *)t->names, t->count, order_done, t) < 0) close_tools(a, t);
}
/* The order box's answer: the targets in the chosen order. */
static void order_done(App *a, void *context, const char *const *names, size_t count, int accepted)
{
    Tools *t = context;
    if (!accepted || count != t->count) { close_tools(a, t); return; }
    for (size_t i = 0; i < count; ++i)
        for (size_t k = i; k < count; ++k)
            if (!strcmp(t->names[k], names[i])) {
                char *n = t->names[k], *p = t->paths[k];
                memmove(t->names + i + 1, t->names + i, (k - i) * sizeof(char *));
                memmove(t->paths + i + 1, t->paths + i, (k - i) * sizeof(char *));
                t->names[i] = n; t->paths[i] = p; break;
            }
    t->order_ok = 1; advance(a, t);
}

static const char *const split_modes[] = { N_("(R) ページの範囲で"), N_("(N) N ページごと") };
static const char *const angles[] = { N_("(R) 右に 90 度"), N_("(U) 180 度"), N_("(L) 左に 90 度") };
static const char *const levels[] = { N_("(S) 強く（72 dpi）"), N_("(M) ふつう（150 dpi）"), N_("(W) 弱く（220 dpi）") };
static const int level_dpi[] = { 72, 150, 220 }, level_quality[] = { 50, 70, 85 };
static const char *const places[] = { N_("(N) 新しい名前で書き出す"), N_("(R) 元のファイルを置き換える") };

static int single_output(Op op) { return op != OP_MERGE && op != OP_SPLIT && op != OP_JPEG && op != OP_IMAGES; }
static void default_ranges(Tools *t, char *out, size_t size)
{
    int n = t->pages[0], p = t->viewer_page;
    if (p >= n) p = n - 1;
    if (t->op == OP_SPLIT) {
        if (p >= 0 && p + 1 < n) snprintf(out, size, "1-%d,%d-", p + 1, p + 2); else out[0] = 0;
    } else if (t->op == OP_REORDER) snprintf(out, size, "1-%d", n);
    else if (p >= 0) snprintf(out, size, "%d", p + 1);
    else if (t->op == OP_ROTATE || t->op == OP_JPEG) snprintf(out, size, "1-%d", n);
    else out[0] = 0;
}
static void ask_ranges(App *a, Tools *t)
{
    char label[160], initial[64];
    const char *what = t->op == OP_SPLIT ? T("分けるページの範囲") : t->op == OP_EXTRACT ? T("抽出するページ") :
                       t->op == OP_DELETE ? T("削除するページ") : t->op == OP_REORDER ? T("新しいページの順番") :
                       t->op == OP_ROTATE ? T("回転するページ") : T("JPEG にするページ");
    snprintf(label, sizeof(label), T("%s（全 %d ページ。例 %s）"), what, t->pages[0], t->op == OP_REORDER ? "3,1,2,4-" : "1-3,5,8-");
    if (!t->ranges[0]) default_ranges(t, initial, sizeof(initial));
    ask_input(a, t, Q_RANGES, label, t->ranges[0] ? t->ranges : initial, WIDGET_HISTORY_NONE);
}
/* Reads the ranges for the entry; 0, or -1 after the error box. */
static int take_ranges(App *a, Tools *t)
{
    char message[96]; int n = t->pages[0];
    free(t->list); free(t->groups); t->list = NULL; t->groups = NULL;
    const char *error = parse_ranges(t->ranges, n, message, sizeof(message), &t->list, &t->list_count, &t->groups, &t->group_count);
    if (!error && (t->op == OP_EXTRACT || t->op == OP_REORDER) && repeated(t->list, t->list_count, n))
        error = T("同じページが 2 回以上あります");
    if (!error && t->op == OP_DELETE) {
        char *gone = calloc((size_t)n, 1); size_t m = 0;
        if (!gone) error = T("メモリが足りません");
        else {
            for (size_t i = 0; i < t->list_count; ++i) gone[t->list[i]] = 1;
            int *kept = realloc(t->list, (size_t)n * sizeof(int));
            if (!kept) error = T("メモリが足りません");
            else { t->list = kept; for (int i = 0; i < n; ++i) if (!gone[i]) t->list[m++] = i; t->list_count = m; }
            free(gone);
            if (!error && !m) error = T("全部のページは削除できません");
        }
    }
    if (!error && t->op == OP_REORDER) {
        /* Pages not written keep their order after the ones written. */
        char *seen = calloc((size_t)n, 1); int *all = realloc(t->list, (size_t)n * sizeof(int));
        if (!seen || !all) { free(seen); if (all) t->list = all; error = T("メモリが足りません"); }
        else {
            t->list = all;
            for (size_t i = 0; i < t->list_count; ++i) seen[t->list[i]] = 1;
            for (int i = 0; i < n; ++i) if (!seen[i]) t->list[t->list_count++] = i;
            int same = 1; for (int i = 0; i < n; ++i) if (t->list[i] != i) same = 0;
            free(seen);
            if (same) error = T("ページの順番が変わりません");
        }
    }
    if (!error && (t->op == OP_ROTATE || t->op == OP_JPEG)) unique(t->list, &t->list_count, n);
    if (error) { app_error(a, error); t->reask = 1; return -1; }
    t->have_ranges = 1; return 0;
}
static int take_every(App *a, Tools *t)
{
    const char *p = t->every; long v = 0; int n = t->pages[0];
    skip_blanks(&p);
    int ok = number(&p, &v); skip_blanks(&p);
    if (!ok || *p || v < 1 || v >= n) {
        char message[96]; snprintf(message, sizeof(message), T("ページの数が違います（1〜%d）"), n - 1);
        app_error(a, message); t->reask = 1; return -1;
    }
    free(t->list); free(t->groups);
    t->group_count = (size_t)((n + v - 1) / v);
    t->list = malloc((size_t)n * sizeof(int)); t->groups = malloc((t->group_count + 1) * sizeof(size_t));
    if (!t->list || !t->groups) { fail(a, t, T("メモリが足りません")); return -1; }
    for (int i = 0; i < n; ++i) t->list[i] = i;
    for (size_t g = 0; g <= t->group_count; ++g) t->groups[g] = g * (size_t)v < (size_t)n ? g * (size_t)v : (size_t)n;
    t->list_count = (size_t)n; t->every_n = (int)v; return 0;
}
static int take_name(App *a, Tools *t)
{
    const char *error = NULL;
    if (!t->name[0]) error = T("名前が空です");
    else if (strchr(t->name, '/')) error = T("名前に / は使えません（元と同じディレクトリに書き出します）");
    else if (!strcmp(t->name, ".") || !strcmp(t->name, "..")) error = T("名前が正しくありません");
    else if (!pdftools_is_pdf(t->name)) {
        if (strlen(t->name) + 5 > sizeof(t->name)) error = T("名前が長すぎます");
        else strcat(t->name, ".pdf");
    }
    if (error) { app_error(a, error); t->reask = 1; return -1; }
    t->have_name = 1; return 0;
}

/* The outputs' full paths; -1 after an error box (the question asked again). */
static int make_outputs(App *a, Tools *t)
{
    char base[WIDGET_CAP], name[WIDGET_CAP + 64];
    stem(t->names[0], base, sizeof(base) - 64);
    free_outputs(t);
    size_t n = t->op == OP_SPLIT ? t->group_count : t->op == OP_JPEG ? t->list_count : 1;
    t->outputs = calloc(n, sizeof(char *));
    if (!t->outputs) { fail(a, t, T("メモリが足りません")); return -1; }
    int digits = 1; for (int c = t->pages[0]; c >= 10; c /= 10) ++digits;
    for (size_t i = 0; i < n; ++i) {
        if (t->op == OP_SPLIT) {
            int first = t->list[t->groups[i]] + 1, last = t->list[t->groups[i + 1] - 1] + 1;
            if (first == last) snprintf(name, sizeof(name), "%s_%d.pdf", base, first);
            else snprintf(name, sizeof(name), "%s_%d-%d.pdf", base, first, last);
        } else if (t->op == OP_JPEG) {
            /* The page number with leading zeros to the width of the last one. */
            char number[24], padded[40]; int len = snprintf(number, sizeof(number), "%d", t->list[i] + 1), k = 0;
            while (len + k < digits && k < 10) padded[k++] = '0';
            memcpy(padded + k, number, (size_t)len + 1);
            snprintf(name, sizeof(name), "%s_%s.jpg", base, padded);
        }
        else if (t->where == 2) snprintf(name, sizeof(name), "%s", t->names[0]);
        else snprintf(name, sizeof(name), "%s", t->name);
        if (!(t->outputs[i] = join(t->directory, name))) { t->output_count = i; fail(a, t, T("メモリが足りません")); return -1; }
        t->output_count = i + 1;
        for (size_t k = 0; k < i; ++k)
            if (!strcmp(t->outputs[k], t->outputs[i])) {
                char message[300]; snprintf(message, sizeof(message), T("同じ名前のファイルが 2 つできます: %.200s"), name);
                app_error(a, message); t->have_ranges = 0; t->reask = 1; free_outputs(t); return -1;
            }
    }
    return 0;
}

/* Answers kept in t; asks the next question, or starts the work. */
static void start(App *a, Tools *t);
static void advance(App *a, Tools *t)
{
    Op op = t->op;
    t->asked = Q_NONE;
    if ((op == OP_MERGE || op == OP_IMAGES) && t->count > 1 && !t->order_ok) { ask_order(a, t); return; }
    if (op != OP_IMAGES)
        for (size_t i = 0; i < t->count; ++i) {
            if (t->probed[i]) continue;
            int r = job_probe(t->paths[i], t->passwords[i], &t->pages[i], &t->encrypted[i]);
            if (r < 0) { char m[300]; snprintf(m, sizeof(m), T("PDF を開けません: %.200s"), t->names[i]); fail(a, t, m); return; }
            if (r > 0) {
                if (t->passwords[i]) {
                    wipe(t->passwords[i], strlen(t->passwords[i])); free(t->passwords[i]); t->passwords[i] = NULL;
                    app_error(a, T("パスワードが違います"));
                }
                ask_password(a, t, PW_SOURCE, i); return;
            }
            t->probed[i] = 1;
        }
    if (op == OP_UNLOCK && !t->encrypted[0]) { fail(a, t, T("この PDF にはパスワードが付いていません")); return; }
    switch (op) {
    case OP_SPLIT:
        if (t->pages[0] < 2) { fail(a, t, T("1 ページの PDF は分けられません")); return; }
        if (!t->mode) { ask_choice(a, t, Q_SPLIT_MODE, T("分割のしかた"), split_modes, 2, 0); return; }
        if (t->mode == 1 && !t->have_ranges) { ask_ranges(a, t); return; }
        if (t->mode == 2 && !t->every_n) {
            char label[96]; snprintf(label, sizeof(label), T("何ページごとに分けますか（全 %d ページ）"), t->pages[0]);
            ask_input(a, t, Q_EVERY, label, t->every[0] ? t->every : "1", WIDGET_HISTORY_NONE); return;
        }
        break;
    case OP_EXTRACT: case OP_DELETE: case OP_REORDER: case OP_ROTATE: case OP_JPEG:
        if (!t->have_ranges) { ask_ranges(a, t); return; }
        if (op == OP_ROTATE && !t->angle) { ask_choice(a, t, Q_ANGLE, T("回す向き"), angles, 3, 0); return; }
        if (op == OP_JPEG && !t->dpi) {
            ask_input(a, t, Q_DPI, T("解像度（dpi、72〜300）"), t->dpi_text[0] ? t->dpi_text : "150", WIDGET_HISTORY_NONE); return;
        }
        break;
    case OP_PROTECT:
        if (!t->have_password) { ask_password(a, t, PW_NEW, 0); return; }
        break;
    case OP_COMPRESS:
        if (!t->level) { ask_choice(a, t, Q_LEVEL, T("どのくらい縮めますか（画像の解像度）"), levels, 3, 1); return; }
        break;
    default: break;
    }
    if (single_output(op) && !t->where) {
        char label[300]; snprintf(label, sizeof(label), T("書き出し先（元: %.200s）"), t->names[0]);
        ask_choice(a, t, Q_WHERE, label, places, 2, 0); return;
    }
    if ((op == OP_MERGE || op == OP_IMAGES || (single_output(op) && t->where == 1)) && !t->have_name) {
        char initial[WIDGET_CAP];
        if (t->name[0]) snprintf(initial, sizeof(initial), "%s", t->name);
        else { char base[WIDGET_CAP - 32]; stem(t->names[0], base, sizeof(base)); snprintf(initial, sizeof(initial), "%s%s.pdf", base, ops[op].suffix); }
        ask_input(a, t, Q_NAME, T("書き出す名前"), initial, WIDGET_HISTORY_NAME); return;
    }
    if (!t->outputs && make_outputs(a, t) < 0) return;
    if (!t->overwrite_ok) {
        size_t there = 0; const char *first = NULL;
        for (size_t i = 0; i < t->output_count; ++i) {
            struct stat st;
            if (lstat(t->outputs[i], &st)) continue;
            if (S_ISDIR(st.st_mode)) {
                char m[300]; snprintf(m, sizeof(m), T("%.200s はディレクトリです"), base_of(t->outputs[i]));
                /* A name typed is asked again; made names (split, JPEG) cannot be. */
                if (op == OP_SPLIT || op == OP_JPEG || t->where == 2) { fail(a, t, m); return; }
                app_error(a, m); t->have_name = 0; t->reask = 1; free_outputs(t); return;
            }
            if (!there++) first = base_of(t->outputs[i]);
        }
        if (there) {
            char m[300];
            int self = t->output_count == 1 && !strcmp(t->outputs[0], t->paths[0]) && op != OP_IMAGES;
            if (self) snprintf(m, sizeof(m), T("元のファイル %.200s を置き換えますか？"), first);
            else if (there == 1) snprintf(m, sizeof(m), T("%.200s は既にあります。上書きしますか？"), first);
            else snprintf(m, sizeof(m), T("%zu 個のファイルが既にあります（%.120s など）。上書きしますか？"), there, first);
            t->phase = PH_ASK; t->asked = Q_OVERWRITE; widgets_confirm(&a->widgets, m); return;
        }
        t->overwrite_ok = 1;
    }
    start(a, t);
}

static void answer(App *a, Tools *t)
{
    Widgets *w = &a->widgets;
    switch (t->asked) {
    case Q_SPLIT_MODE: t->mode = w->selected + 1; break;
    case Q_RANGES: snprintf(t->ranges, sizeof(t->ranges), "%s", w->text); if (take_ranges(a, t) < 0) return; break;
    case Q_EVERY: snprintf(t->every, sizeof(t->every), "%.31s", w->text); if (take_every(a, t) < 0) return; break;
    case Q_ANGLE: t->angle = 90 * (w->selected + 1); break;
    case Q_LEVEL: t->level = w->selected + 1; break;
    case Q_DPI: {
        snprintf(t->dpi_text, sizeof(t->dpi_text), "%.15s", w->text);
        const char *p = t->dpi_text; long v = 0; skip_blanks(&p);
        int ok = number(&p, &v); skip_blanks(&p);
        if (!ok || *p || v < 72 || v > 300) { app_error(a, T("解像度が違います（72〜300）")); t->reask = 1; return; }
        t->dpi = (int)v; break;
    }
    case Q_WHERE: t->where = w->selected + 1; break;
    case Q_NAME: snprintf(t->name, sizeof(t->name), "%s", w->text); if (take_name(a, t) < 0) return; break;
    case Q_OVERWRITE: t->overwrite_ok = 1; break;
    default: return;
    }
    advance(a, t);
}

/* ---- the work -------------------------------------------------------------- */

static void start(App *a, Tools *t)
{
    JobSpec s;
    memset(&s, 0, sizeof(s));
    s.kind = ops[t->op].job;
    s.sources = (const char *const *)t->paths; s.passwords = (const char *const *)t->passwords; s.source_count = t->count;
    s.pages = t->list; s.page_count = t->list_count;
    s.groups = t->op == OP_SPLIT ? t->groups : NULL; s.group_count = t->op == OP_SPLIT ? t->group_count : 0;
    s.angle = t->angle;
    s.dpi = t->op == OP_COMPRESS ? level_dpi[t->level - 1] : t->dpi;
    s.quality = t->op == OP_COMPRESS ? level_quality[t->level - 1] : 0;
    s.password = t->op == OP_PROTECT ? t->password : NULL;
    s.outputs = (const char *const *)t->outputs; s.output_count = t->output_count;
    char error[300];
    t->job = job_start(&s, error, sizeof(error));
    wipe(t->password, sizeof(t->password));
    if (!t->job) { fail(a, t, error); return; }
    t->phase = PH_RUN; a->changed = 1;
}

/* The box of what was written (item 39: the pages and the size). */
static void finish(App *a, Tools *t)
{
    enum { ROWS = 64 };
    static char names[ROWS][64], values[ROWS][512];
    const char *np[ROWS], *vp[ROWS];
    char title[96], size[48];
    int n = 0, total_pages = 0; long long total_bytes = 0;
    snprintf(title, sizeof(title), "%s", T(ops[t->op].done));
    for (size_t i = 0; i < t->output_count; ++i) {
        int pages, w, h; long long bytes;
        job_result(t->job, i, &pages, &bytes, &w, &h);
        total_pages += pages; total_bytes += bytes > 0 ? bytes : 0;
        if (n >= ROWS - 2 && i + 1 < t->output_count) {
            snprintf(names[n], sizeof(names[n]), "%s", T("ほか"));
            snprintf(values[n], sizeof(values[n]), T("%zu 個"), t->output_count - i); ++n; break;
        }
        size_text(bytes, size, sizeof(size));
        if (t->output_count > 1) snprintf(names[n], sizeof(names[n]), "%zu", i + 1);
        else snprintf(names[n], sizeof(names[n]), "%s", T("ファイル"));
        if (t->op == OP_JPEG) snprintf(values[n], sizeof(values[n]), T("%.300s  %dx%d・%s"), base_of(t->outputs[i]), w, h, size);
        else snprintf(values[n], sizeof(values[n]), T("%.300s  %d ページ・%s"), base_of(t->outputs[i]), pages, size);
        ++n;
    }
    if (t->output_count > 1) {
        size_text(total_bytes, size, sizeof(size));
        snprintf(names[n], sizeof(names[n]), "%s", T("合計"));
        if (t->op == OP_JPEG) snprintf(values[n], sizeof(values[n]), T("%zu 枚・%s"), t->output_count, size);
        else snprintf(values[n], sizeof(values[n]), T("%zu 個・%d ページ・%s"), t->output_count, total_pages, size);
        ++n;
    }
    if (t->op == OP_COMPRESS) {
        long long before = job_source_bytes(t->job);
        int pages, w, h; long long after; job_result(t->job, 0, &pages, &after, &w, &h);
        char was[48]; size_text(before, was, sizeof(was));
        snprintf(names[n], sizeof(names[n]), "%s", T("元の大きさ"));
        snprintf(values[n], sizeof(values[n]), T("%s（%.0f%% になりました）"), was, before > 0 ? after * 100.0 / before : 100.0);
        ++n;
    }
    for (int i = 0; i < n; ++i) { np[i] = names[i]; vp[i] = values[i]; }
    reload_filers(a);
    app_pop(a);
    widgets_info(&a->widgets, title, np, vp, n);
}

/* ---- keys ------------------------------------------------------------------ */

/* 1 when the entry can work on the targets; else the reason. */
static const char *refusal(const Tools *t, Op op)
{
    switch (ops[op].need) {
    case PDFS:
        if (t->n_pdf < 2) return T("結合する PDF を 2 つ以上マークしてください");
        return t->n_pdf < (int)t->count ? T("PDF 以外のファイルが混ざっています") : NULL;
    case IMAGES:
        if (!t->n_image) return T("JPEG か PNG の画像を選んでください");
        return t->n_image < (int)t->count ? T("JPEG・PNG 以外のファイルが混ざっています") : NULL;
    default:
        if (t->count > 1) return T("PDF を 1 つだけ選んでください（マークを外すとカーソルの PDF）");
        return t->n_pdf ? NULL : T("PDF を選んでください");
    }
}
static void choose(App *a, Tools *t, Op op)
{
    const char *why = refusal(t, op);
    if (why) { app_error(a, why); return; }
    t->op = op; advance(a, t);
}
static void menu_key(App *a, Tools *t, const Key *k)
{
    if (k->key == KESC || (k->key == KCHAR && k->ch == 'q' && !(k->mods & (MOD_CTRL | MOD_ALT)))) { close_tools(a, t); return; }
    if (k->key == KUP) t->menu = (t->menu + OP_COUNT - 1) % OP_COUNT;
    else if (k->key == KDOWN) t->menu = (t->menu + 1) % OP_COUNT;
    else if (k->key == KHOME || k->key == KPGUP) t->menu = 0;
    else if (k->key == KEND || k->key == KPGDN) t->menu = OP_COUNT - 1;
    else if (k->key == KRET) choose(a, t, (Op)t->menu);
    else if (k->key == KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT)))
        for (int i = 0; i < OP_COUNT; ++i) if ((uint32_t)ops[i].key == k->ch) { t->menu = i; choose(a, t, (Op)i); break; }
}
static void password_key(App *a, Tools *t, const Key *k)
{
    int plain = !(k->mods & (MOD_CTRL | MOD_ALT));
    if (k->key == KESC) { close_tools(a, t); return; }
    if (k->key == KBS) { if (t->secret_len) t->secret[--t->secret_len] = 0; return; }
    if (k->key == KCHAR && k->ch == 'u' && (k->mods & MOD_CTRL)) { wipe(t->secret, sizeof(t->secret)); t->secret_len = 0; return; }
    if (k->key == KCHAR && plain && k->ch >= 32 && k->ch <= 126) {
        if (t->secret_len + 1 < sizeof(t->secret)) t->secret[t->secret_len++] = (char)k->ch;
        return;
    }
    if (k->key != KRET) return;
    if (t->secret_kind == PW_SOURCE) {
        char *copy = strdup(t->secret);
        wipe(t->secret, sizeof(t->secret)); t->secret_len = 0;
        if (!copy) { fail(a, t, T("メモリが足りません")); return; }
        t->passwords[t->secret_source] = copy; t->phase = PH_ASK;
        advance(a, t); return;
    }
    if (t->secret_kind == PW_NEW) {
        if (!t->secret_len) { app_error(a, T("パスワードが空です")); return; }
        memcpy(t->password, t->secret, sizeof(t->password));
        ask_password(a, t, PW_AGAIN, 0); return;
    }
    int same = !strcmp(t->password, t->secret);
    wipe(t->secret, sizeof(t->secret)); t->secret_len = 0;
    if (!same) { wipe(t->password, sizeof(t->password)); app_error(a, T("パスワードが一致しません。もう一度入れてください")); ask_password(a, t, PW_NEW, 0); return; }
    t->have_password = 1; t->phase = PH_ASK; advance(a, t);
}
static int tools_key(Screen *s, App *a, const Key *k)
{
    Tools *t = s->self;
    switch (t->phase) {
    case PH_MENU: menu_key(a, t, k); return 0;
    case PH_PASSWORD: password_key(a, t, k); return 0;
    case PH_RUN:
        if (k->key == KESC) { job_free(t->job); t->job = NULL; reload_filers(a); close_tools(a, t); app_notice(a, T("操作を中止しました")); }
        return 0;
    case PH_ASK: {
        if (t->reask) return 0;
        WidgetResult r = widgets_key(&a->widgets, k);
        if (r == WIDGET_CANCEL) close_tools(a, t);
        else if (r == WIDGET_ACCEPT) answer(a, t);
        return 0;
    }
    default: return 0;
    }
}
static void tools_tick(Screen *s, App *a, double now)
{
    (void)now; Tools *t = s->self;
    if (t->phase == PH_CLOSE) { app_pop(a); return; }
    if (t->reask && a->widgets.kind == WIDGET_NONE) { t->reask = 0; advance(a, t); return; }
    if (t->phase != PH_RUN || a->widgets.kind != WIDGET_NONE) return;
    int r = job_step(t->job, WORK_SLICE);
    a->changed = 1;
    if (r > 0) finish(a, t);
    else if (r < 0) {
        char message[300]; snprintf(message, sizeof(message), "%s", job_error(t->job));
        job_free(t->job); t->job = NULL; reload_filers(a); fail(a, t, message);
    }
}

/* ---- drawing --------------------------------------------------------------- */

#define BOX_KIND (CELL_BOX_FRAME | CELL_BOX_SHADOW)
static int centre_row(int h)
{
    int row = FD_ROWS / 2 - h / 2;
    if (row + h > FD_LINE) row = FD_LINE - h;
    return row < 1 ? 1 : row;
}
/* text in a gap of the box's top (bottom 0) or bottom line, from x. */
static void gap_text(Cells *c, int box, int x, int bottom, const char *text, int fg)
{
    CellBox *b = &c->boxes[box];
    int y = bottom ? b->row + b->h - 1 : b->row, w = cells_text(c, x, y, text, fg, BG, b->col + b->w - 2 - x);
    if (bottom) { b->bottom_from = (short)(x - 1); b->bottom_to = (short)(x + w + 1); }
    else { b->top_from = (short)(x - 1); b->top_to = (short)(x + w + 1); }
}
static void draw_menu(Tools *t, Cells *c)
{
    int about = 0;
    for (int i = 0; i < OP_COUNT; ++i) { int w = cells_width(T(ops[i].about)); if (w > about) about = w; }
    char target[300];
    if (t->count == 1) snprintf(target, sizeof(target), T("対象 %.200s"), t->names[0]);
    else if (t->n_image == (int)t->count) snprintf(target, sizeof(target), T("対象 画像 %zu 枚"), t->count);
    else if (t->n_pdf == (int)t->count) snprintf(target, sizeof(target), T("対象 PDF %zu 個"), t->count);
    else snprintf(target, sizeof(target), T("対象 %zu 個（PDF %d・画像 %d）"), t->count, t->n_pdf, t->n_image);
    if (t->viewer_page >= 0 && t->count == 1) {
        size_t n = strlen(target); snprintf(target + n, sizeof(target) - n, T("  今のページ %d"), t->viewer_page + 1);
    }
    const char *hint = T("↑↓ 選ぶ  RET 決める  Esc 戻る");
    int inner = 4 + MENU_NAME_W + about, tw = cells_width(target);
    if (tw > inner) inner = tw;
    int w = inner + 4; if (w > FD_COLS - 2) w = FD_COLS - 2;
    /* The rows: the items and the line between the groups; scrolled when
     * they do not fit (they always do at 25 rows: 13 of FD_ROWS - 8). */
    int rows = OP_COUNT + 1, room = FD_ROWS - 8, visible = rows < room ? rows : room;
    int at = t->menu + (t->menu > MENU_LINE_AFTER);
    if (at < t->menu_top) t->menu_top = at;
    if (at >= t->menu_top + visible) t->menu_top = at - visible + 1;
    int h = visible + 4, col = (FD_COLS - w) / 2, row = centre_row(h);
    int box = cells_box(c, col, row, w, h, BOX_KIND);
    if (box < 0) return;
    c->boxes[box].rule = (short)(row + 2);
    gap_text(c, box, col + 2, 0, T("PDF のメニュー"), YELLOW);
    cells_text(c, col + 2, row + 1, target, WHITE, BG, w - 4);
    for (int r = 0; r < visible; ++r) {
        int line = t->menu_top + r, y = row + 3 + r;
        if (line == MENU_LINE_AFTER + 1) { cells_box_line(c, 0, y, col, col + w - 1); continue; }
        int i = line - (line > MENU_LINE_AFTER), chosen = i == t->menu, ok = !refusal(t, (Op)i);
        int bg = chosen ? CYAN : BG, x = col + 2;
        if (chosen) for (int k = col + 1; k < col + w - 1; ++k) cells_put(c, k, y, ' ', BG, CYAN);
        cells_put(c, x, y, (uint32_t)ops[i].key, chosen ? BG : YELLOW, bg);
        cells_text(c, x + 2, y, T(ops[i].name), chosen ? BG : ok ? WHITE : BLUE, bg, MENU_NAME_W);
        cells_text(c, x + 2 + MENU_NAME_W, y, T(ops[i].about), chosen ? BG : ok ? CYAN : BLUE, bg, w - 6 - MENU_NAME_W);
    }
    char mark[96];
    if (visible < rows) snprintf(mark, sizeof(mark), "%s  %d-%d/%d", hint, t->menu_top + 1, t->menu_top + visible, rows);
    else snprintf(mark, sizeof(mark), "%s", hint);
    int mw = cells_width(mark);
    if (mw + 6 <= w) gap_text(c, box, col + w - 3 - mw, 1, mark, CYAN);
}
/* As the input box (widgets_input_field), the field showing '*'. */
static void draw_password(Tools *t, Cells *c)
{
    int frow, fstart, fend;
    widgets_input_field(&frow, &fstart, &fend);
    int col = fstart - 3, row = frow - 2, w = fend - fstart + 6, field = fstart - 1, fw = w - 4;
    if (cells_box(c, col, row, w, 4, BOX_KIND) < 0) return;
    const char *hint = T("* で隠しています");
    int hw = cells_width(hint);
    cells_text(c, field, row + 1, t->secret_label, CYAN, BG, fw - hw - 1);
    cells_text(c, field + fw - hw, row + 1, hint, CYAN, BG, hw);
    for (int x = field; x < field + fw; ++x) cells_put(c, x, frow, ' ', BG, WHITE);
    int room = fend - fstart - 1, shown = (int)t->secret_len < room ? (int)t->secret_len : room;
    for (int i = 0; i < shown; ++i) cells_put(c, fstart + i, frow, '*', BG, WHITE);
    cells_put(c, fstart + shown, frow, ' ', WHITE, BG);
}
static void draw_progress(Tools *t, Cells *c)
{
    int done = 0, total = 0; const char *what = "";
    job_progress(t->job, &done, &total, &what);
    int w = 64 < FD_COLS - 4 ? 64 : FD_COLS - 4, h = 5, col = (FD_COLS - w) / 2, row = centre_row(h);
    int box = cells_box(c, col, row, w, h, BOX_KIND);
    if (box < 0) return;
    char title[64]; snprintf(title, sizeof(title), T("%s中"), T(ops[t->op].name));
    gap_text(c, box, col + 2, 0, title, YELLOW);
    cells_text(c, col + 2, row + 1, what, WHITE, BG, w - 4);
    int percent = total > 0 ? done * 100 / total : 0, filled = percent * 25 / 100;
    for (int i = 0; i < 25; ++i) cells_put(c, col + 2 + 2 * i, row + 2, i < filled ? 0x25a0 : 0x25a1, i < filled ? YELLOW : WHITE, BG);
    char line[32]; snprintf(line, sizeof(line), "%3d%%", percent);
    cells_text(c, col + 53, row + 2, line, CYAN, BG, w - 55);
    cells_text(c, col + 2, row + 3, T("Esc で中止"), CYAN, BG, w - 4);
}
static void tools_draw(Screen *s, App *a, Canvas *canvas, Cells *cells)
{
    Tools *t = s->self;
    if (t->below && t->below->draw) t->below->draw(t->below, a, canvas, cells);
    if (a->widgets.kind != WIDGET_NONE && a->widgets.kind != WIDGET_NOTICE) return;
    int boxes = cells->box_count;
    if (t->phase == PH_MENU) draw_menu(t, cells);
    else if (t->phase == PH_PASSWORD) draw_password(t, cells);
    else if (t->phase == PH_RUN) draw_progress(t, cells);
    /* Over a picture (the PDF viewer, Alt+p there) the boxes go on the canvas
     * here; a cell screen's are drawn with the rest of the cells. */
    if (s->pixel && cells->box_count > boxes) { raster_boxes(canvas, cells); canvas->full_dirty = 1; }
}

/* ---- the entrance ------------------------------------------------------------ */

int pdftools_open(App *a, const FileSelection *s, PdfToolsEntry first, int page)
{
    const char *one[1];
    const char *const *names = s ? s->names : NULL; size_t count = s ? s->count : 0;
    if (s && first == PDFTOOLS_SPLIT && s->current) { one[0] = s->current; names = one; count = 1; }
    if (!s || !s->directory || !count) { app_notice(a, T("対象がありません")); return -1; }
    Tools *t = calloc(1, sizeof(*t));
    if (!t) { app_notice(a, T("メモリが足りません")); return -1; }
    t->screen = (Screen){ t, tools_key, tools_draw, 0, tools_tick, tools_destroy };
    t->below = app_top(a); t->screen.pixel = t->below ? t->below->pixel : 0;
    t->viewer_page = page; t->count = count; t->menu = 0;
    t->directory = strdup(s->directory);
    t->names = calloc(count, sizeof(char *)); t->paths = calloc(count, sizeof(char *)); t->passwords = calloc(count, sizeof(char *));
    t->pages = calloc(count, sizeof(int)); t->encrypted = calloc(count, sizeof(int)); t->probed = calloc(count, sizeof(int));
    int failed = !t->directory || !t->names || !t->paths || !t->passwords || !t->pages || !t->encrypted || !t->probed;
    for (size_t i = 0; !failed && i < count; ++i) {
        if (!(t->names[i] = strdup(names[i])) || !(t->paths[i] = join(t->directory, names[i]))) { failed = 1; break; }
        struct stat st; int file = !stat(t->paths[i], &st) && S_ISREG(st.st_mode);
        if (file && pdftools_is_pdf(names[i])) ++t->n_pdf;
        else if (file && is_image(names[i])) ++t->n_image;
    }
    if (failed) { tools_destroy(&t->screen); app_notice(a, T("メモリが足りません")); return -1; }
    const char *why = NULL; Op op = OP_MERGE;
    if (first == PDFTOOLS_MERGE) why = refusal(t, op = OP_MERGE);
    else if (first == PDFTOOLS_SPLIT) why = refusal(t, op = OP_SPLIT);
    else if (!t->n_pdf && !t->n_image) why = T("PDF を選んでください");
    if (why) { tools_destroy(&t->screen); app_error(a, why); return -1; }
    if (app_push(a, &t->screen) < 0) { tools_destroy(&t->screen); app_notice(a, T("メモリが足りません")); return -1; }
    Screen *me = &t->screen;
    if (first == PDFTOOLS_MENU) {
        t->phase = PH_MENU;
        for (int i = 0; i < OP_COUNT; ++i) if (!refusal(t, (Op)i)) { t->menu = i; break; }
        return 0;
    }
    t->op = op; t->phase = PH_ASK; advance(a, t);
    /* Still on the stack (perhaps under the order box): a box is up. */
    for (int i = 0; i < a->depth; ++i) if (a->screens[i] == me) return 0;
    return -1;
}
#endif
