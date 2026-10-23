// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "widgets.h"
#include "i18n.h"
#include "font24.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src ? src : "");
    if (n >= cap) {
        n = cap - 1;
        while (n && (((unsigned char)src[n] & 0xc0) == 0x80)) --n;
    }
    memmove(dst, src ? src : "", n);
    dst[n] = 0;
}
static size_t previous(const char *text, size_t pos)
{
    if (!pos) return 0;
    --pos;
    while (pos && (((unsigned char)text[pos] & 0xc0) == 0x80)) --pos;
    return pos;
}
static size_t following(const char *text, size_t pos)
{
    if (!text[pos]) return pos;
    ++pos;
    while (((unsigned char)text[pos] & 0xc0) == 0x80) ++pos;
    return pos;
}
static int plain_char(const Key *key, uint32_t ch)
{
    return key->key == KCHAR && key->ch == ch && !(key->mods & (MOD_CTRL | MOD_ALT));
}
static int control(const Key *key, uint32_t ch)
{
    return key->key == KCHAR && key->ch == ch && (key->mods & (MOD_CTRL | MOD_ALT)) == MOD_CTRL;
}
void widgets_init(Widgets *w)
{
    memset(w, 0, sizeof(*w));
    w->history_index = -1;
}
/* ---- the remembered lists (revision 1 §12) ---------------------------- */
static const char *const kind_names[WIDGET_HISTORY_KINDS] = {"", "destination", "path", "command", "search", "name"};
static struct {
    int ready, home_denied;
    char path[WIDGET_CAP];
    int count[WIDGET_HISTORY_KINDS];
    char *items[WIDGET_HISTORY_KINDS][WIDGET_HISTORY_KEEP];
} store;
void widgets_history_close(void)
{
    for (int k = 0; k < WIDGET_HISTORY_KINDS; ++k) {
        for (int i = 0; i < store.count[k]; ++i) free(store.items[k][i]);
        store.count[k] = 0;
    }
    store.ready = 0;
    store.path[0] = 0;
}
void widgets_history_home(int allowed) { store.home_denied = !allowed; }
static void default_path(char *out, size_t cap)
{
    const char *env = getenv("FDTP_HISTORY"), *home = getenv("HOME");
    out[0] = 0;
    if (env) copy_text(out, cap, env);
    else if (!store.home_denied && home && *home) {
        int n = snprintf(out, cap, "%s/.fdtp/history", home);
        if (n < 0 || (size_t)n >= cap) out[0] = 0;
    }
}
/* One line: kind TAB text, where text escapes \\ \t \n \r. */
static int unescape(char *s)
{
    char *out = s;
    for (char *p = s; *p; ++p) {
        if (*p != '\\') { *out++ = *p; continue; }
        ++p;
        if (*p == '\\') *out++ = '\\';
        else if (*p == 't') *out++ = '\t';
        else if (*p == 'n') *out++ = '\n';
        else if (*p == 'r') *out++ = '\r';
        else return -1;
    }
    *out = 0;
    return 0;
}
static void store_insert(int kind, const char *text, int at_head)
{
    for (int i = 0; i < store.count[kind]; ++i)
        if (!strcmp(store.items[kind][i], text)) {
            if (!at_head) return;
            free(store.items[kind][i]);
            memmove(store.items[kind] + i, store.items[kind] + i + 1, sizeof(char *) * (size_t)(store.count[kind] - i - 1));
            --store.count[kind];
            break;
        }
    if (!at_head && store.count[kind] == WIDGET_HISTORY_KEEP) return;
    char *copy = strdup(text);
    if (!copy) return;
    if (!at_head) { store.items[kind][store.count[kind]++] = copy; return; }
    if (store.count[kind] == WIDGET_HISTORY_KEEP) free(store.items[kind][--store.count[kind]]);
    memmove(store.items[kind] + 1, store.items[kind], sizeof(char *) * (size_t)store.count[kind]);
    store.items[kind][0] = copy;
    ++store.count[kind];
}
int widgets_history_open(const char *path)
{
    widgets_history_close();
    store.ready = 1;
    if (path) copy_text(store.path, sizeof(store.path), path);
    else default_path(store.path, sizeof(store.path));
    if (!store.path[0]) return 0;
    FILE *f = fopen(store.path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || (size_t)n >= WIDGET_CAP) continue;
        *tab = 0;
        char *text = tab + 1;
        if (!*text || unescape(text)) continue;
        for (int k = 1; k < WIDGET_HISTORY_KINDS; ++k)
            if (!strcmp(line, kind_names[k])) { store_insert(k, text, 0); break; }
    }
    int failed = ferror(f);
    free(line);
    fclose(f);
    return failed ? -1 : 0;
}
static void ensure_store(void) { if (!store.ready) widgets_history_open(NULL); }
int widgets_history_count(WidgetHistoryKind kind)
{
    ensure_store();
    return kind > 0 && kind < WIDGET_HISTORY_KINDS ? store.count[kind] : 0;
}
const char *widgets_history_entry(WidgetHistoryKind kind, int index)
{
    ensure_store();
    if (kind <= 0 || kind >= WIDGET_HISTORY_KINDS || index < 0 || index >= store.count[kind]) return NULL;
    return store.items[kind][index];
}
static void write_escaped(FILE *f, const char *s)
{
    for (; *s; ++s) {
        if (*s == '\\') fputs("\\\\", f);
        else if (*s == '\t') fputs("\\t", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\r') fputs("\\r", f);
        else fputc(*s, f);
    }
}
/* Written whole to a temporary name and renamed over the old file; a missing
 * parent directory (~/.fdtp) is made. Failure leaves the lists in memory. */
static void store_save(void)
{
    if (!store.path[0]) return;
    char temp[WIDGET_CAP + 8], parent[WIDGET_CAP];
    copy_text(parent, sizeof(parent), store.path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) { *slash = 0; mkdir(parent, 0700); }
    int n = snprintf(temp, sizeof(temp), "%s.tmp", store.path);
    if (n < 0 || (size_t)n >= sizeof(temp)) return;
    FILE *f = fopen(temp, "w");
    if (!f) return;
    fputs("# FDTP history: kind<TAB>text, newest first\n", f);
    for (int k = 1; k < WIDGET_HISTORY_KINDS; ++k)
        for (int i = 0; i < store.count[k]; ++i) {
            fprintf(f, "%s\t", kind_names[k]);
            write_escaped(f, store.items[k][i]);
            fputc('\n', f);
        }
    int failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed || rename(temp, store.path)) unlink(temp);
}
static void remember(WidgetHistoryKind kind, const char *text)
{
    if (kind <= 0 || kind >= WIDGET_HISTORY_KINDS || !*text) return;
    ensure_store();
    store_insert(kind, text, 1);
    store_save();
}
/* The entries shown in the list: those beginning with the typed text. */
static int list_items(const Widgets *w, const char *out[WIDGET_HISTORY_KEEP])
{
    int kind = w->history_kind, n = 0;
    if (kind <= 0 || kind >= WIDGET_HISTORY_KINDS) return 0;
    ensure_store();
    size_t length = w->edited ? strlen(w->text) : 0;
    for (int i = 0; i < store.count[kind]; ++i)
        if (!strncmp(store.items[kind][i], w->text, length)) out[n++] = store.items[kind][i];
    return n;
}
/* ---- opening the widgets ---------------------------------------------- */
static int contains(const char *s, const char *part) { return strstr(s, part) != NULL; }
/* Revision 1 §7: errors get a box, other notices stay on row 23. The modules
 * all call app_notice, so the message decides. */
static int reads_as_error(const char *m)
{
    if (i18n_lang() == I18N_EN) {
        static const char *const errors[] = {
            "Cannot ", "Failed", "failed", "Invalid", "invalid", "Wrong ",
            "Too many", "Too long", "Too large", "too long", "too large",
            "Out of memory", "out of range", "Out of range", "Empty ", "empty",
            "Not available"
        };
        for (size_t i = 0; i < sizeof(errors) / sizeof(*errors); ++i)
            if (contains(m, errors[i])) return 1;
    }
    static const char *const marks[] = {"失敗", "違います", "多すぎ", "長すぎ", "大きすぎ", "範囲外", "が空です", "正しくありません"}; /* i18n:keep */
    for (size_t i = 0; i < sizeof(marks) / sizeof(*marks); ++i) if (contains(m, marks[i])) return 1;
    if (!contains(m, "ません")) return 0; /* i18n:keep */
    if (contains(m, "見つかりません") || contains(m, "もうありません")) return 0; /* i18n:keep */
    /* 〜はありません (nothing to undo, no change) is not an error; 〜ではありません is. */
    for (const char *p = strstr(m, "はありません"); p; p = strstr(p + 1, "はありません")) /* i18n:keep */
        if (!(p - m >= 3 && !strncmp(p - 3, "で", 3))) return 0; /* i18n:keep */
    return 1;
}
void widgets_notice(Widgets *w, const char *message)
{
    w->kind = reads_as_error(message ? message : "") ? WIDGET_ERROR : WIDGET_NOTICE;
    copy_text(w->label, sizeof(w->label), message);
}
void widgets_error(Widgets *w, const char *message)
{
    w->kind = WIDGET_ERROR;
    copy_text(w->label, sizeof(w->label), message);
}
void widgets_confirm(Widgets *w, const char *message)
{
    w->kind = WIDGET_CONFIRM;
    copy_text(w->label, sizeof(w->label), message);
}
void widgets_input_history(Widgets *w, const char *label, const char *initial, const char *directory,
                           WidgetHistoryKind kind)
{
    w->kind = WIDGET_INPUT;
    copy_text(w->label, sizeof(w->label), label);
    copy_text(w->text, sizeof(w->text), initial);
    copy_text(w->directory, sizeof(w->directory), directory && *directory ? directory : ".");
    w->cursor = strlen(w->text);
    w->scroll = 0;
    w->history_index = -1;
    copy_text(w->draft, sizeof(w->draft), w->text);
    w->history_kind = kind > 0 && kind < WIDGET_HISTORY_KINDS ? kind : WIDGET_HISTORY_NONE;
    w->list_open = w->list_selected = w->list_top = w->edited = 0;
}
void widgets_input(Widgets *w, const char *label, const char *initial, const char *directory)
{
    widgets_input_history(w, label, initial, directory, WIDGET_HISTORY_NONE);
}
void widgets_info(Widgets *w, const char *title, const char *const *names, const char *const *values, int count)
{
    w->kind = WIDGET_INFO;
    copy_text(w->label, sizeof(w->label), title);
    w->info_count = count < 0 ? 0 : count > WIDGET_INFO_LINES ? WIDGET_INFO_LINES : count;
    w->info_top = 0;
    for (int i = 0; i < w->info_count; ++i) {
        copy_text(w->info_name[i], sizeof(w->info_name[i]), names ? names[i] : "");
        copy_text(w->info_value[i], sizeof(w->info_value[i]), values ? values[i] : "");
    }
}
void widgets_help(Widgets *w, const char *title, const WidgetHelpItem *items, int count)
{
    w->kind = WIDGET_HELP;
    copy_text(w->label, sizeof(w->label), title);
    w->help = items;
    w->help_count = items && count > 0 ? count : 0;
    w->help_top = 0;
}
void widgets_tree_hook(Widgets *w, void (*hook)(void *context), void *context)
{
    w->tree_hook = hook;
    w->tree_context = context;
}
static int tree_input(const Widgets *w)
{
    return w->kind == WIDGET_INPUT && (w->history_kind == WIDGET_HISTORY_DESTINATION || w->history_kind == WIDGET_HISTORY_PATH);
}
/* The letter of "(T)" in an option, lower case, or 0. */
static int mnemonic(const char *option)
{
    for (const char *p = strchr(option, '('); p; p = strchr(p + 1, '('))
        if (isalnum((unsigned char)p[1]) && p[2] == ')') return tolower((unsigned char)p[1]);
    return 0;
}
void widgets_choice(Widgets *w, const char *label, const char *const *options, int count)
{
    w->kind = count > 0 ? WIDGET_CHOICE : WIDGET_NONE;
    copy_text(w->label, sizeof(w->label), label);
    w->option_count = count < 0 ? 0 : count > WIDGET_OPTIONS ? WIDGET_OPTIONS : count;
    for (int i = 0; i < w->option_count; ++i) w->options[i] = options[i] ? options[i] : "";
    w->selected = 0;
}
static void add_history(Widgets *w)
{
    if (!w->text[0]) return;
    if (w->history_count && !strcmp(w->history[w->history_count - 1], w->text)) return;
    if (w->history_count == WIDGET_HISTORY) {
        memmove(w->history, w->history + 1, sizeof(w->history[0]) * (WIDGET_HISTORY - 1));
        --w->history_count;
    }
    copy_text(w->history[w->history_count++], WIDGET_CAP, w->text);
}
static void browse_history(Widgets *w, int direction)
{
    if (!w->history_count) return;
    if (direction < 0) {
        if (w->history_index < 0) {
            copy_text(w->draft, sizeof(w->draft), w->text);
            w->history_index = w->history_count - 1;
        } else if (w->history_index > 0) --w->history_index;
        else return;
    } else {
        if (w->history_index < 0) return;
        if (++w->history_index == w->history_count) w->history_index = -1;
    }
    copy_text(w->text, sizeof(w->text), w->history_index < 0 ? w->draft : w->history[w->history_index]);
    w->cursor = strlen(w->text);
    w->scroll = 0;
}
/* Complete the path before the cursor, relative to the filer's directory.
 * Multiple matches extend only their common prefix; one directory gains '/'. */
static void complete_path(Widgets *w)
{
    char prefix[WIDGET_CAP], folder[WIDGET_CAP], common[WIDGET_CAP];
    if (w->cursor >= sizeof(prefix)) return;
    memcpy(prefix, w->text, w->cursor);
    prefix[w->cursor] = 0;
    char *slash = strrchr(prefix, '/');
    const char *base = slash ? slash + 1 : prefix;
    size_t head = slash ? (size_t)(slash - prefix + 1) : 0;
    int written;
    if (prefix[0] == '/') {
        if (head >= sizeof(folder)) return;
        memcpy(folder, prefix, head);
        folder[head] = 0;
    } else {
        written = snprintf(folder, sizeof(folder), "%s/%.*s", w->directory, (int)head, prefix);
        if (written < 0 || (size_t)written >= sizeof(folder)) return;
    }
    DIR *dir = opendir(folder);
    if (!dir) return;
    size_t base_len = strlen(base), common_len = 0;
    int matches = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (name[0] == '.' && base[0] != '.') continue;
        if (strncmp(name, base, base_len)) continue;
        if (!matches) {
            copy_text(common, sizeof(common), name);
            common_len = strlen(common);
        } else {
            size_t i = 0;
            while (i < common_len && name[i] && name[i] == common[i]) ++i;
            common_len = i;
            common[i] = 0;
        }
        ++matches;
    }
    closedir(dir);
    if (!matches) return;
    /* A byte prefix may end in the middle of a multibyte filename. */
    if (common_len) {
        size_t start = previous(common, common_len);
        unsigned char lead = (unsigned char)common[start];
        size_t needed = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (common_len - start < needed) common_len = start;
    }
    common[common_len] = 0;
    if (matches == 1 && common_len + 1 < sizeof(common)) {
        char path[WIDGET_CAP];
        written = snprintf(path, sizeof(path), "%s%s", folder, common);
        struct stat st;
        if (written >= 0 && (size_t)written < sizeof(path) && !stat(path, &st) && S_ISDIR(st.st_mode)) {
            common[common_len++] = '/';
            common[common_len] = 0;
        }
    }
    if (common_len < base_len) return;
    size_t suffix = strlen(w->text + w->cursor);
    if (head + common_len + suffix >= sizeof(w->text)) return;
    memmove(w->text + head + common_len, w->text + w->cursor, suffix + 1);
    memcpy(w->text + head, common, common_len);
    w->cursor = head + common_len;
}
/* ---- keys -------------------------------------------------------------- */
#define LIST_ROWS 8
static int list_key(Widgets *w, const Key *k)
{
    const char *items[WIDGET_HISTORY_KEEP];
    int n = list_items(w, items);
    if (k->key == KESC || (k->key == KDOWN && (k->mods & MOD_ALT))) { w->list_open = 0; return 1; }
    if (k->key == KRET) {
        if (n && w->list_selected < n) {
            copy_text(w->text, sizeof(w->text), items[w->list_selected]);
            w->cursor = strlen(w->text);
            w->scroll = 0;
        }
        w->list_open = 0;
        return 1;
    }
    int step = k->key == KUP ? -1 : k->key == KDOWN ? 1 : k->key == KPGUP ? -LIST_ROWS : k->key == KPGDN ? LIST_ROWS : 0;
    if (!step) return 0;
    if (n) {
        w->list_selected += step;
        if (w->list_selected < 0) w->list_selected = 0;
        if (w->list_selected >= n) w->list_selected = n - 1;
    }
    return 1;
}
/* ---- scrolling boxes (revision 1 item 25) ------------------------------ */
/* ↑↓ one row, PgUp PgDn Space a page, Home End the ends; 1 when k is one
 * of them (also at an end, so the key does not close the box). */
static int scroll_key(const Key *k, int *top, int total, int visible)
{
    int last = total > visible ? total - visible : 0, at = *top;
    if (k->mods & (MOD_CTRL | MOD_ALT)) return 0;
    if (k->key == KUP) --at;
    else if (k->key == KDOWN) ++at;
    else if (k->key == KPGUP) at -= visible;
    else if (k->key == KPGDN || plain_char(k, ' ')) at += visible;
    else if (k->key == KHOME) at = 0;
    else if (k->key == KEND) at = last;
    else return 0;
    *top = at < 0 ? 0 : at > last ? last : at;
    return 1;
}
static void info_rows(const Widgets *w, int *total, int *visible);
/* The help box's layout: the rows of each column (a blank before a heading
 * that is not at a column's top), the widths and the box's size. */
#define HELP_COLUMNS 6
#define HELP_ROWS 640
/* HELP_CONTINUED: a section's heading again at the top of the next column,
 * 「見出し（続き）」. */
enum { HELP_BLANK, HELP_HEADING, HELP_ITEM, HELP_CONTINUED };
typedef struct { short kind, item; } HelpRow;
typedef struct {
    int columns, key_w, col_w, width, h, rows, visible;
    int first[HELP_COLUMNS], count[HELP_COLUMNS];
    HelpRow row[HELP_ROWS];
} HelpLayout;
static void help_layout(const Widgets *w, HelpLayout *L);
WidgetResult widgets_key(Widgets *w, const Key *k)
{
    if (w->kind == WIDGET_NONE) return WIDGET_PASS;
    if (w->kind == WIDGET_NOTICE) { w->kind = WIDGET_NONE; return WIDGET_PASS; }
    if (w->kind == WIDGET_INFO) {
        int total, visible;
        info_rows(w, &total, &visible);
        if (total > visible && scroll_key(k, &w->info_top, total, visible)) return WIDGET_HANDLED;
        w->kind = WIDGET_NONE;
        return WIDGET_HANDLED;
    }
    if (w->kind == WIDGET_HELP) {
        HelpLayout L;
        help_layout(w, &L);
        if (scroll_key(k, &w->help_top, L.rows, L.visible)) return WIDGET_HANDLED;
        if (k->key == KESC || k->key == KF1 || k->key == KRET || plain_char(k, 'q') || plain_char(k, 'h') || plain_char(k, '?'))
            w->kind = WIDGET_NONE;
        return WIDGET_HANDLED;
    }
    if (w->kind == WIDGET_ERROR) { w->kind = WIDGET_NONE; return WIDGET_HANDLED; }
    if (w->kind == WIDGET_INPUT && w->list_open && list_key(w, k)) return WIDGET_HANDLED;
    if (tree_input(w) && k->key == KF8 && !(k->mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT))) {
        w->list_open = 0;
        if (w->tree_hook) w->tree_hook(w->tree_context);
        return WIDGET_HANDLED;
    }
    if (k->key == KESC) { w->kind = WIDGET_NONE; return WIDGET_CANCEL; }
    if (w->kind == WIDGET_CONFIRM) {
        if (plain_char(k, 'y') || plain_char(k, 'Y')) { w->kind = WIDGET_NONE; return WIDGET_ACCEPT; }
        if (plain_char(k, 'n') || plain_char(k, 'N')) { w->kind = WIDGET_NONE; return WIDGET_CANCEL; }
        return WIDGET_HANDLED;
    }
    if (k->key == KRET) {
        if (w->kind == WIDGET_INPUT) { add_history(w); remember(w->history_kind, w->text); }
        w->kind = WIDGET_NONE;
        return WIDGET_ACCEPT;
    }
    if (w->kind == WIDGET_CHOICE) {
        if (k->key == KLEFT || k->key == KUP) w->selected = (w->selected + w->option_count - 1) % w->option_count;
        else if (k->key == KRIGHT || k->key == KDOWN) w->selected = (w->selected + 1) % w->option_count;
        else if (k->key == KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT))) {
            int typed = k->ch < 128 ? tolower((int)k->ch) : 0;
            for (int i = 0; typed && i < w->option_count; ++i)
                if (mnemonic(w->options[i]) == typed) { w->selected = i; w->kind = WIDGET_NONE; return WIDGET_ACCEPT; }
            for (int i = 1; i <= w->option_count; ++i) {
                int at = (w->selected + i) % w->option_count;
                const char *p = w->options[at];
                uint32_t initial = font24_decode(&p), typed = k->ch;
                if (initial < 128 && typed < 128) { initial = (uint32_t)tolower((int)initial); typed = (uint32_t)tolower((int)typed); }
                if (initial == typed) { w->selected = at; break; }
            }
        }
        return WIDGET_HANDLED;
    }
    if (w->history_kind && !w->list_open && (k->key == KDOWN || k->key == KUP)) {
        w->list_open = 1;
        w->list_selected = w->list_top = 0;
        return WIDGET_HANDLED;
    }
    size_t length = strlen(w->text);
    char before[WIDGET_CAP];
    memcpy(before, w->text, length + 1);
    if (k->key == KLEFT) w->cursor = previous(w->text, w->cursor);
    else if (k->key == KRIGHT) w->cursor = following(w->text, w->cursor);
    else if (k->key == KHOME || control(k, 'a')) w->cursor = 0;
    else if (k->key == KEND || control(k, 'e')) w->cursor = length;
    else if (k->key == KBS && w->cursor) {
        size_t at = previous(w->text, w->cursor);
        memmove(w->text + at, w->text + w->cursor, length - w->cursor + 1);
        w->cursor = at;
    } else if (k->key == KDEL && w->cursor < length) {
        size_t next = following(w->text, w->cursor);
        memmove(w->text + w->cursor, w->text + next, length - next + 1);
    } else if (control(k, 'k')) w->text[w->cursor] = 0;
    else if (control(k, 'u')) { w->text[0] = 0; w->cursor = 0; w->scroll = 0; }
    else if (k->key == KUP) browse_history(w, -1);
    else if (k->key == KDOWN) browse_history(w, 1);
    else if (k->key == KTAB) complete_path(w);
    else if (k->key == KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) &&
             ((k->ch >= 32 && k->ch <= 126) || k->ch == 0x00a5)) {
        size_t bytes=k->ch == 0x00a5 ? 2 : 1;
        if(length + bytes < sizeof(w->text)) {
            memmove(w->text + w->cursor + bytes, w->text + w->cursor, length - w->cursor + 1);
            if(bytes==2) { w->text[w->cursor++]=(char)0xc2; w->text[w->cursor++]=(char)0xa5; }
            else w->text[w->cursor++] = (char)k->ch;
        }
    }
    if (strcmp(before, w->text)) { w->edited = 1; w->list_selected = w->list_top = 0; }
    return WIDGET_HANDLED;
}
/* ---- drawing ------------------------------------------------------------ */
/* Boxes: at most 70 columns wide, text inset two columns from the frame. */
#define BOX_MAX 70
#define BOX_TEXT (BOX_MAX - 4)
#define BOX_KIND (CELL_BOX_FRAME | CELL_BOX_SHADOW)
static int range_width(const char *s, size_t start, size_t end)
{
    int width = 0;
    const char *p = s + start;
    while ((size_t)(p - s) < end && *p) width += font24_width(font24_decode(&p));
    return width;
}
/* Split s into rows of at most width columns (no wide character is cut).
 * starts[i]..starts[i+1] is row i; returns the number of rows (≤ max). */
static int wrap(const char *s, int width, const char **starts, int max, int *widest)
{
    int rows = 0, col = 0;
    *widest = 0;
    if (max < 1) return 0;
    starts[rows++] = s;
    const char *p = s;
    while (*p) {
        const char *at = p;
        uint32_t ch = font24_decode(&p);
        if (ch == '\n') {   /* a forced break; the newline itself is not drawn */
            if (rows == max) { p = at; break; }
            starts[rows++] = p;
            col = 0;
            continue;
        }
        int cw = font24_width(ch);
        if (col + cw > width) {
            if (rows == max) { p = at; break; }
            starts[rows++] = at;
            col = 0;
        }
        col += cw;
        if (col > *widest) *widest = col;
    }
    starts[rows] = p;
    return rows;
}
static void row_text(Cells *c, int col, int row, const char *from, const char *to, int fg, int bg, int limit)
{
    char buffer[WIDGET_CAP];
    size_t n = (size_t)(to - from);
    if (n >= sizeof(buffer)) n = sizeof(buffer) - 1;
    if (n && from[n - 1] == '\n') --n;
    memcpy(buffer, from, n);
    buffer[n] = 0;
    cells_text(c, col, row, buffer, fg, bg, limit);
}
static int centre_col(int w) { return (FD_COLS - w) / 2; }
/* Boxes stay between row 1 and row 22 (FD_LINE-1; their shadow may reach row 23). */
static int centre_row(int h)
{
    int row = FD_ROWS / 2 - h / 2;
    if (row + h > FD_LINE) row = FD_LINE - h;
    return row < 1 ? 1 : row;
}
static int clamp_width(int inner, int least)
{
    int w = inner + 4;
    if (w < least) w = least;
    return w > BOX_MAX ? BOX_MAX : w;
}
static void draw_message(Cells *c, const char *message, int fg, const char *hint)
{
    const char *starts[9];
    int widest, hint_width = hint ? cells_width(hint) : 0;
    int rows = wrap(message, BOX_TEXT, starts, 8, &widest);
    if (hint_width > widest) widest = hint_width;
    int w = clamp_width(widest, 24), h = rows + 2 + (hint ? 1 : 0);
    int col = centre_col(w), row = centre_row(h);
    if (cells_box(c, col, row, w, h, BOX_KIND) < 0) return;
    for (int i = 0; i < rows; ++i) row_text(c, col + 2, row + 1 + i, starts[i], starts[i + 1], fg, BG, w - 4);
    if (hint) cells_text(c, col + 2, row + 1 + rows, hint, CYAN, BG, w - 4);
}
/* Input box: rows 9-12, columns 5-74 at 80 x 25; the field is white with navy text. */
/* On a larger screen (WIDE_NOTES.md) the box stays this size, in the middle. */
enum { INPUT_W = BOX_MAX, FIELD_W = INPUT_W - 4 };
#define INPUT_COL ((FD_COLS - INPUT_W) / 2)
#define INPUT_ROW (9 + FD_XROWS / 2)
#define FIELD_COL (INPUT_COL + 2)
/* The pull-down list: rows under the field inside the same box, below a
 * rule (卓駆★'s 「コピー先は？」 window). first_row is the first list row. */
static void draw_list(Widgets *w, Cells *c, int first_row, int n, const char **items)
{
    int visible = n < LIST_ROWS ? n : LIST_ROWS;
    if (!n) {
        cells_text(c, FIELD_COL, first_row, w->edited ? T("（一致する履歴はありません）") : T("（履歴はありません）"), CYAN, BG, FIELD_W);
        return;
    }
    for (int i = 0; i < visible; ++i) {
        int at = w->list_top + i, chosen = at == w->list_selected, y = first_row + i;
        if (chosen) for (int x = FIELD_COL; x < FIELD_COL + FIELD_W; ++x) cells_put(c, x, y, ' ', BG, CYAN);
        cells_text(c, FIELD_COL + 1, y, items[at], chosen ? BG : WHITE, chosen ? CYAN : BG, FIELD_W - 4);
    }
    if (w->list_top > 0) cells_text(c, FIELD_COL + FIELD_W - 2, first_row, "↑", CYAN, BG, 2);
    if (w->list_top + visible < n) cells_text(c, FIELD_COL + FIELD_W - 2, first_row + visible - 1, "↓", CYAN, BG, 2);
}
static void draw_input(Widgets *w, Cells *c)
{
    const char *items[WIDGET_HISTORY_KEEP];
    int n = 0, list_rows = 0;
    if (w->list_open) {
        n = list_items(w, items);
        if (w->list_selected >= n) w->list_selected = n ? n - 1 : 0;
        list_rows = n < LIST_ROWS ? (n ? n : 1) : LIST_ROWS;
        if (w->list_top > w->list_selected) w->list_top = w->list_selected;
        if (w->list_selected >= w->list_top + LIST_ROWS) w->list_top = w->list_selected - LIST_ROWS + 1;
        if (w->list_top > n - list_rows) w->list_top = n - list_rows < 0 ? 0 : n - list_rows;
    }
    int box = cells_box(c, INPUT_COL, INPUT_ROW, INPUT_W, list_rows ? 5 + list_rows : 4, BOX_KIND);
    if (box < 0) return;
    if (list_rows) c->boxes[box].rule = INPUT_ROW + 3;
    const char *hint = tree_input(w) && w->tree_hook ? T("↓ 履歴  F8 ツリー") : w->history_kind ? T("↓ 履歴") : "";
    int hint_width = cells_width(hint);
    cells_text(c, FIELD_COL, INPUT_ROW + 1, w->label, CYAN, BG, FIELD_W - hint_width - 1);
    if (hint_width) cells_text(c, FIELD_COL + FIELD_W - hint_width, INPUT_ROW + 1, hint, CYAN, BG, hint_width);
    /* White field with one blank column inside each end, so the navy caret
     * shows even at the start. */
    int row = INPUT_ROW + 2, start = FIELD_COL + 1, available = FIELD_W - 2;
    for (int x = FIELD_COL; x < FIELD_COL + FIELD_W; ++x) cells_put(c, x, row, ' ', BG, WHITE);
    const char *p = w->text + w->cursor;
    uint32_t ch = *p ? font24_decode(&p) : ' ';
    int cursor_width = font24_width(ch);
    if (cursor_width > available) cursor_width = 1;
    if (w->scroll > w->cursor) w->scroll = w->cursor;
    while (w->scroll < w->cursor && range_width(w->text, w->scroll, w->cursor) + cursor_width > available)
        w->scroll = following(w->text, w->scroll);
    cells_text(c, start, row, w->text + w->scroll, BG, WHITE, available);
    int cursor_col = start + range_width(w->text, w->scroll, w->cursor);
    if (cursor_col + font24_width(ch) > start + available) ch = ' ';
    cells_put(c, cursor_col, row, ch, WHITE, BG);
    if (list_rows) draw_list(w, c, INPUT_ROW + 4, n, items);
}
/* Options side by side when they fit (each with a blank on both sides, the
 * chosen one reversed like a button), otherwise one per row. */
static void draw_choice(Widgets *w, Cells *c)
{
    const char *lines[4];
    int label, across = 0, widest = 0;
    int label_rows = wrap(w->label, BOX_TEXT, lines, 3, &label);
    if (label_rows < 1) label_rows = 1;
    for (int i = 0; i < w->option_count; ++i) {
        int ow = cells_width(w->options[i]);
        across += ow + 2;
        if (ow + 2 > widest) widest = ow + 2;
    }
    int horizontal = across <= BOX_TEXT;
    int inner = horizontal ? across : widest;
    if (label > inner) inner = label;
    int extra = label_rows - 1;
    int width = clamp_width(inner, 24), h = (horizontal ? 4 : w->option_count + 3) + extra;
    int col = centre_col(width), row = centre_row(h), text = width - 4;
    if (cells_box(c, col, row, width, h, BOX_KIND) < 0) return;
    for (int i = 0; i < label_rows; ++i) row_text(c, col + 2, row + 1 + i, lines[i], lines[i + 1], CYAN, BG, text);
    int x = col + 2, end = col + 2 + text;
    for (int i = 0; i < w->option_count; ++i) {
        int chosen = i == w->selected, fg = chosen ? BG : WHITE, bg = chosen ? WHITE : BG;
        int y = (horizontal ? row + 2 : row + 2 + i) + extra;
        if (!horizontal) x = col + 2;
        if (x >= end) break;
        cells_put(c, x++, y, ' ', fg, bg);
        x += cells_text(c, x, y, w->options[i], fg, bg, end - x - 1);
        if (!horizontal) while (x < end - 1) cells_put(c, x++, y, ' ', fg, bg);
        if (x < end) cells_put(c, x++, y, ' ', fg, bg);
    }
}
/* An information box's rows: each value wrapped (at most 8 rows). */
typedef struct {
    int names, value_width, widest_value, total;
    int lines[WIDGET_INFO_LINES];
    const char *starts[WIDGET_INFO_LINES][9];
} InfoLayout;
static void info_layout(const Widgets *w, InfoLayout *L)
{
    L->names = 0;
    for (int i = 0; i < w->info_count; ++i) {
        int nw = cells_width(w->info_name[i]);
        if (nw > L->names) L->names = nw;
    }
    if (L->names > 24) L->names = 24;
    L->value_width = BOX_TEXT - L->names - 2;
    L->widest_value = L->total = 0;
    for (int i = 0; i < w->info_count; ++i) {
        int widest;
        L->lines[i] = wrap(w->info_value[i], L->value_width, L->starts[i], 8, &widest);
        L->total += L->lines[i];
        if (widest > L->widest_value) L->widest_value = widest;
    }
}
/* Rows between the title rule and the bottom frame: FD_ROWS - 7 at most
 * (rows 1 to FD_LINE - 1 with the title, the rule and the frame). */
static int info_room(void) { return FD_ROWS - 7; }
static void info_rows(const Widgets *w, int *total, int *visible)
{
    static InfoLayout L;   /* large; the UI runs on one thread */
    info_layout(w, &L);
    *total = L.total;
    *visible = L.total < info_room() ? L.total : info_room();
}
/* 「↑↓ 先頭-最後/全部」 in a gap of the bottom frame line, centred or at
 * the right (three columns in from the corner). */
static void scroll_mark(Cells *c, int box, const char *text, int right)
{
    CellBox *b = &c->boxes[box];
    int tw = cells_width(text);
    if (tw + 6 > b->w) return;
    int x = right ? b->col + b->w - 3 - tw : b->col + (b->w - tw) / 2, y = b->row + b->h - 1;
    b->bottom_from = (short)(x - 1);
    b->bottom_to = (short)(x + tw + 1);
    cells_text(c, x, y, text, CYAN, BG, tw);
}
static void draw_info(Widgets *w, Cells *c)
{
    static InfoLayout L;
    info_layout(w, &L);
    int visible = L.total < info_room() ? L.total : info_room(), last = L.total - visible;
    if (w->info_top > last) w->info_top = last;
    if (w->info_top < 0) w->info_top = 0;
    char mark[64] = "";
    if (L.total > visible)
        snprintf(mark, sizeof(mark), "↑↓ %d-%d/%d", w->info_top + 1, w->info_top + visible, L.total);
    int inner = L.names + 2 + L.widest_value, title = cells_width(w->label);
    if (title > inner) inner = title;
    if (cells_width(mark) + 2 > inner) inner = cells_width(mark) + 2;
    int width = clamp_width(inner, 30), h = visible + 4;
    int col = centre_col(width), row = centre_row(h);
    int box = cells_box(c, col, row, width, h, BOX_KIND);
    if (box < 0) return;
    c->boxes[box].rule = (short)(row + 2);
    cells_text(c, col + 2, row + 1, w->label, YELLOW, BG, width - 4);
    int r = 0, y = row + 3;
    for (int i = 0; i < w->info_count; ++i)
        for (int j = 0; j < L.lines[i]; ++j, ++r) {
            if (r < w->info_top || r >= w->info_top + visible) continue;
            if (j == 0 || r == w->info_top) {
                int x = col + 2 + cells_text(c, col + 2, y, j ? "" : w->info_name[i], CYAN, BG, L.names);
                while (x < col + 2 + L.names) cells_put(c, x++, y, ' ', WHITE, BG);
                if (!j) cells_text(c, x, y, ": ", CYAN, BG, 2);
            }
            row_text(c, col + 4 + L.names, y, L.starts[i][j], L.starts[i][j + 1], WHITE, BG, L.value_width);
            ++y;
        }
    if (mark[0]) scroll_mark(c, box, mark, 0);
}
/* ---- the help box (revision 1 item 25) ---------------------------------- */
/* Columns of the help box are this far apart (a vertical line in the
 * middle of the gap). */
#define HELP_GAP 3
static int help_section_rows(const Widgets *w, int at, int *next)
{
    int n = 0, i = at;
    if (i < w->help_count && !w->help[i].key) { ++n; ++i; }
    while (i < w->help_count && w->help[i].key) { ++n; ++i; }
    *next = i;
    return n;
}
/* Sections into n columns, filled in order, each at most height rows: a
 * blank at the top of every column (under the title) and before a heading
 * below another section. split 0 keeps each section in one column; split 1
 * may go on in the next column (never after a heading alone; the heading
 * again as 「（続き）」 at the top). Returns the number of columns used, n + 1
 * when it does not fit, filling L's rows (L not NULL). */
static int help_pack(const Widgets *w, int n, int height, int split, HelpLayout *L)
{
    int column = 0, used = 0, rows = 0;
    if (L) { L->first[0] = 0; L->count[0] = 0; }
#define HELP_PLACE(k, it) do { \
        if (L) { \
            if (rows >= HELP_ROWS) return n + 1; \
            L->row[rows++] = (HelpRow){(short)(k), (short)(it)}; \
            ++L->count[column]; \
        } \
        ++used; \
    } while (0)
#define HELP_NEXT_COLUMN() do { \
        if (++column >= n) return n + 1; \
        used = 0; \
        if (L) { L->first[column] = rows; L->count[column] = 0; } \
        HELP_PLACE(HELP_BLANK, -1); \
    } while (0)
    HELP_PLACE(HELP_BLANK, -1);
    for (int i = 0; i < w->help_count;) {
        int next, size = help_section_rows(w, i, &next), titled = !w->help[i].key;
        if (!split) {
            if (used > 1 && used + 1 + size > height) HELP_NEXT_COLUMN();
            if (used > 1) HELP_PLACE(HELP_BLANK, -1);
        } else if (used > 1) {
            if (used + 3 > height) HELP_NEXT_COLUMN();
            else HELP_PLACE(HELP_BLANK, -1);
        }
        for (int j = i; j < next; ++j) {
            int heading = !w->help[j].key;
            if (split && used > 1 && (used >= height || (heading && used + 2 > height))) {
                HELP_NEXT_COLUMN();
                if (!heading && titled) HELP_PLACE(HELP_CONTINUED, i);
            }
            HELP_PLACE(heading ? HELP_HEADING : HELP_ITEM, j);
        }
        i = next;
    }
#undef HELP_NEXT_COLUMN
#undef HELP_PLACE
    return column + 1;
}
/* The least height with which the items go into n columns. */
static int help_height(const Widgets *w, int n, int split, int total)
{
    int lo = 1, hi = total;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (help_pack(w, n, mid, split, NULL) <= n) hi = mid; else lo = mid + 1;
    }
    return lo;
}
static void help_layout(const Widgets *w, HelpLayout *L)
{
    int key_w = 0, text_w = 0, heading_w = 0, total = 0;
    for (int i = 0; i < w->help_count;) {
        int next;
        total += help_section_rows(w, i, &next) + 1;   /* the blank above */
        i = next;
    }
    for (int i = 0; i < w->help_count; ++i) {
        const WidgetHelpItem *it = &w->help[i];
        int tw = cells_width(it->text ? T(it->text) : "");
        if (!it->key) { if (tw > heading_w) heading_w = tw; continue; }
        int kw = cells_width(it->key);
        if (kw > key_w) key_w = kw;
        if (tw > text_w) text_w = tw;
    }
    int col_w = key_w + 2 + text_w;
    int continued_w = cells_width(T("（続き）"));
    if (heading_w + 4 + continued_w > col_w) col_w = heading_w + 4 + continued_w;   /* + 「（続き）」 */
    int room = FD_COLS - 1 - 4;   /* the frame, a blank each side and the shadow */
    if (col_w > room) col_w = room;
    int most = 1;
    while (most < HELP_COLUMNS && (most + 1) * col_w + most * HELP_GAP <= room) ++most;
    int height = FD_ROWS - 3 - 2;  /* rows 1 to FD_LINE - 1, less the frame */
    /* The fewest columns that show everything with whole sections, else with
     * sections going on in the next column, else as many columns as fit
     * (whole sections) and the box scrolls. */
    int columns = most, split = 0, tallest = help_height(w, most, 0, total), found = 0;
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (int n = 1; n <= most && !found; ++n) {
            int need = help_height(w, n, pass, total);
            if (need <= height) { columns = n; split = pass; tallest = need; found = 1; }
        }
    L->columns = help_pack(w, columns, tallest, split, L);
    if (L->columns > columns) L->columns = columns;
    L->rows = 0;
    for (int i = 0; i < L->columns; ++i) if (L->count[i] > L->rows) L->rows = L->count[i];
    L->key_w = key_w;
    L->col_w = col_w;
    L->visible = L->rows < height ? L->rows : height;
    if (L->visible < 1) L->visible = 1;
    L->width = L->columns * col_w + (L->columns - 1) * HELP_GAP + 4;
    L->h = L->visible + 2;
}
static void draw_help(Widgets *w, Cells *c)
{
    static HelpLayout L;
    help_layout(w, &L);
    int last = L.rows - L.visible;
    if (w->help_top > last) w->help_top = last;
    if (w->help_top < 0) w->help_top = 0;
    int col = centre_col(L.width), row = centre_row(L.h);
    int box = cells_box(c, col, row, L.width, L.h, BOX_KIND);
    if (box < 0) return;
    int right = col + L.width - 1, bottom = row + L.h - 1;
    /* The title in a gap of the top line, at the left as the headings (clear
     * of the lines between columns); the hint at the right of the bottom. */
    int tw = cells_width(w->label);
    if (tw > L.width - 6) tw = L.width - 6;
    int tx = col + 3;
    c->boxes[box].top_from = (short)(tx - 1);
    c->boxes[box].top_to = (short)(tx + tw + 1);
    cells_text(c, tx, row, w->label, YELLOW, BG, tw);
    for (int k = 0; k < L.columns; ++k) {
        int x = col + 2 + k * (L.col_w + HELP_GAP);
        int left_edge = k ? x - 2 : col, right_edge = k + 1 < L.columns ? x + L.col_w + 1 : right;
        if (k) cells_box_line(c, 1, x - 2, row, bottom);
        for (int i = 0; i < L.visible; ++i) {
            int r = w->help_top + i, y = row + 1 + i;
            if (r >= L.count[k]) break;
            HelpRow hr = L.row[L.first[k] + r];
            if (hr.kind == HELP_BLANK) continue;
            const WidgetHelpItem *it = &w->help[hr.item];
            if (hr.kind == HELP_HEADING || hr.kind == HELP_CONTINUED) {
                int n = cells_text(c, x + 1, y, T(it->text), YELLOW, BG, L.col_w - 2);
                if (hr.kind == HELP_CONTINUED) n += cells_text(c, x + 1 + n, y, T("（続き）"), YELLOW, BG, L.col_w - 2 - n);
                cells_box_line(c, 0, y, left_edge, x);
                cells_box_line(c, 0, y, x + 1 + n, right_edge);
                continue;
            }
            cells_text(c, x, y, it->key, CYAN, BG, L.key_w);
            cells_text(c, x + L.key_w + 2, y, it->text ? T(it->text) : "", WHITE, BG, L.col_w - L.key_w - 2);
        }
    }
    char hint[96];
    if (L.rows > L.visible)
        snprintf(hint, sizeof(hint), T("↑↓ PgUp PgDn  %d-%d/%d  Esc: 戻る"), w->help_top + 1, w->help_top + L.visible, L.rows);
    else snprintf(hint, sizeof(hint), "%s", T("Esc: 戻る"));
    scroll_mark(c, box, hint, 1);
}
int widgets_message(const Widgets *w) { return w->kind == WIDGET_NOTICE || w->kind == WIDGET_ERROR; }
int widgets_row23(const Widgets *w) { return w->kind == WIDGET_NOTICE; }
void widgets_input_field(int *row, int *start, int *end)
{
    *row = INPUT_ROW + 2; *start = FIELD_COL + 1; *end = FIELD_COL + FIELD_W - 1;
}
void widgets_draw(Widgets *w, Cells *cells)
{
    switch (w->kind) {
    case WIDGET_NONE: return;
    case WIDGET_NOTICE:
        for (int x = 0; x < FD_COLS; ++x) cells_put(cells, x, FD_LINE, ' ', WHITE, BG);
        cells_text(cells, 2, FD_LINE, w->label, WHITE, BG, FD_COLS - 2);
        return;
    case WIDGET_ERROR: draw_message(cells, w->label, WHITE, T("何かキーを押してください")); return;
    case WIDGET_CONFIRM: {
        char message[sizeof(w->label) + 16];
        snprintf(message, sizeof(message), "%s (Y/N)", w->label);
        draw_message(cells, message, YELLOW, NULL);
        return;
    }
    case WIDGET_INPUT: draw_input(w, cells); return;
    case WIDGET_CHOICE: draw_choice(w, cells); return;
    case WIDGET_INFO: draw_info(w, cells); return;
    case WIDGET_HELP: draw_help(w, cells); return;
    }
}
