// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 1c: /etc/fdtprc and ~/.fdtprc (docs/FDTP_STAGE1_OPS.md 1c).
 * The files are kept as layers of records so that the customizer can change
 * the user file and write back what FDTP understood, the rest as comments. */
#include "config_state.h"
#include "app.h"
#include "launcher.h"
#include "cells.h"
#include "canvas.h"
#include "i18n.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { R_TEXT, R_SKIP, R_SET, R_BIND, R_LAUNCH } Kind;
typedef struct {
    Kind kind;
    char *text;   /* R_TEXT/R_SKIP: the physical lines, joined with \n */
    char *trail;  /* "# ..." after an understood line, or NULL */
    char name[32]; char *value;                               /* R_SET */
    Key key; const char *id, *dir_id; char *shell, *comment;  /* R_BIND */
    char *ext, *target; int internal;                         /* R_LAUNCH */
} Record;
typedef struct { Record *items; size_t count, capacity; char *path; } Layer;
#define LAYERS 8
#define LAUNCHES 256
#define FILE_LIMIT (1024 * 1024)
typedef struct ConfigState ConfigState;
struct ConfigState {
    Layer layers[LAYERS]; int layer_count, user;
    char label[512];
    int dirty;
    int first_run, language_choice;
    int shell_saved, shell_exported; char *original_shell;
};

const char *const config_variable_names[CONFIG_VARIABLES] = {
    "EDITOR", "PAGER", "SHELL", "SORTTYPE", "DISPLAYMODE", "DOTFILE", "SYMLINK", "FILETYPE",
    "NOVEL_WORD_RUBY",
#define NOVEL_FORMAT_VARIABLE(name, on, help) "NOVEL_FORMAT_" #name,
    NOVEL_FORMAT_RULES(NOVEL_FORMAT_VARIABLE)
#undef NOVEL_FORMAT_VARIABLE
    "LANGUAGE",
    "SCREENSIZE",
    "AWAKE_MINUTES",
    "SPLIT_SIZE",
    "UNDO_LEVELS",
};
_Static_assert(NOVEL_FORMAT_COUNT <= (int)sizeof(((AppSettings *)0)->novel_format), "AppSettings.novel_format");
static const char *const internal_viewers[] = {
    "textview", "imageview", "pdfview", "sheetview", "docview", "slideview", "noveledit", "archive"
};

/* ---- small helpers ---- */
typedef struct { char *p; size_t n, cap; int fail; } Buf;
static void put(Buf *b, const char *s, size_t n)
{
    if (b->fail) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->fail = 1; return; }
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }
static void putc_(Buf *b, char c) { put(b, &c, 1); }
static size_t utf8_length(const char *s)
{
    unsigned char c = (unsigned char)*s;
    size_t n = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    for (size_t i = 1; i < n; ++i) if (!s[i]) return i;
    return n;
}
/* Copy at most size-1 bytes, not cutting a UTF-8 character. */
static void copy_text(char *out, size_t size, const char *s)
{
    size_t n = 0;
    if (!size) return;
    if (s) while (s[n]) { size_t k = utf8_length(s + n); if (n + k >= size) break; n += k; }
    if (n) memcpy(out, s, n);
    out[n] = 0;
}

static void record_free(Record *r)
{
    free(r->text); free(r->trail); free(r->value); free(r->shell); free(r->comment);
    free(r->ext); free(r->target); memset(r, 0, sizeof(*r));
}
static void layer_free(Layer *l)
{
    for (size_t i = 0; i < l->count; ++i) record_free(&l->items[i]);
    free(l->items); free(l->path); memset(l, 0, sizeof(*l));
}
static Record *layer_append(Layer *l)
{
    if (l->count == l->capacity) {
        size_t cap = l->capacity ? l->capacity * 2 : 32;
        Record *p = realloc(l->items, cap * sizeof(*p));
        if (!p) return NULL;
        l->items = p; l->capacity = cap;
    }
    Record *r = &l->items[l->count++]; memset(r, 0, sizeof(*r)); return r;
}
static void layer_remove(Layer *l, size_t i)
{
    record_free(&l->items[i]);
    memmove(l->items + i, l->items + i + 1, (l->count - i - 1) * sizeof(*l->items));
    --l->count;
}

/* ---- words: a small part of sh's quoting ---- */
#define WORDS 32
typedef struct { char *text; int quoted, dollar, command; } Word;
typedef struct { Word w[WORDS]; int count, shellish, error; size_t comment; char *buffer; } Split;
static int separator(char c) { return c == ';' || c == '&' || c == '|'; }
/* Splits line into words (buffer owned by sp; split_free). `#` at a word's
 * start begins a comment. A single character after `bind` is taken as it is,
 * so that `bind ( FNAME_RIGHT` and the like read as keys. */
static void split(const char *line, Split *sp)
{
    memset(sp, 0, sizeof(*sp)); sp->comment = SIZE_MAX;
    sp->buffer = malloc(strlen(line) + 1);
    if (!sp->buffer) { sp->error = 1; return; }
    const char *p = line; char *o = sp->buffer;
    int at_command = 1; /* the next word is in a command position */
    for (;;) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (sp->count == 1 && !sp->w[0].quoted && !strcmp(sp->w[0].text, "bind") && *p != '"' && *p != '\'') {
            size_t n = utf8_length(p);
            if (!p[n] || p[n] == ' ' || p[n] == '\t') {
                Word *w = &sp->w[sp->count++]; w->text = o;
                memcpy(o, p, n); o += n; *o++ = 0; p += n; continue;
            }
        }
        if (*p == '#') { sp->comment = (size_t)(p - line); break; }
        if (separator(*p)) { sp->shellish = 1; at_command = 1; ++p; continue; }
        if (sp->count == WORDS) { sp->error = 1; return; }
        Word *w = &sp->w[sp->count++]; w->text = o; w->quoted = *p == '"' || *p == '\'';
        w->command = at_command;
        while (*p && *p != ' ' && *p != '\t') {
            if (separator(*p)) { sp->shellish = 1; break; }
            if (*p == '\'') {
                ++p; while (*p && *p != '\'') *o++ = *p++;
                if (!*p) { sp->error = 1; return; }
                ++p; continue;
            }
            if (*p == '"') {
                ++p;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) { ++p; *o++ = *p++; continue; }
                    if (*p == '$' || *p == '`') w->dollar = 1;
                    *o++ = *p++;
                }
                if (!*p) { sp->error = 1; return; }
                ++p; continue;
            }
            if (*p == '\\') { ++p; if (!*p) break; *o++ = *p++; continue; }
            if (*p == '<' || *p == '>' || *p == '(' || *p == ')' || *p == '`') sp->shellish = 1;
            if (*p == '$' || *p == '`') w->dollar = 1;
            *o++ = *p++;
        }
        *o++ = 0;
        size_t n = strlen(w->text);
        static const char *const lead[] = {"then", "do", "else", "elif", "{", "!", "function"};
        at_command = 0;
        if (!w->quoted) {
            for (size_t i = 0; i < sizeof(lead) / sizeof(*lead); ++i) if (!strcmp(w->text, lead[i])) at_command = 1;
            if (n > 2 && !strcmp(w->text + n - 2, "()")) at_command = 1;
        }
        if (separator(*p)) { sp->shellish = 1; at_command = 1; ++p; }
    }
}
static void split_free(Split *sp) { free(sp->buffer); sp->buffer = NULL; }
static int is_word(const Word *w, const char *s) { return !w->quoted && !strcmp(w->text, s); }

/* ---- keys ---- */
static const struct { const char *name; int key; } key_names[] = {
    {"Up",KUP},{"Down",KDOWN},{"Left",KLEFT},{"Right",KRIGHT},{"PgUp",KPGUP},{"PageUp",KPGUP},
    {"PgDn",KPGDN},{"PageDown",KPGDN},{"Home",KHOME},{"End",KEND},{"Ins",KINS},{"Del",KDEL},
    {"Bs",KBS},{"BackSpace",KBS},{"Delete",KDEL},{"Insert",KINS},{"Esc",KESC},{"Escape",KESC},
    {"Ret",KRET},{"Return",KRET},{"Enter",KRET},{"Tab",KTAB}
};
int config_key_parse(const char *text, Key *out)
{
    if (!text || !*text || !out) return 0;
    size_t n = utf8_length(text);
    if (!text[n]) return keys_parse(text, out);
    if ((text[0] == '^' || text[0] == '@') && text[1]) {
        const char *rest = text + 1; int mod = text[0] == '^' ? MOD_CTRL : MOD_ALT;
        if (!strcasecmp(rest, "Space") || !strcasecmp(rest, "Spc")) { *out = (Key){KCHAR, ' ', mod}; return 1; }
        if (rest[1]) return 0;
        unsigned char c = (unsigned char)rest[0];
        if (c < 0x21 || c > 0x7e) return 0;
        if (mod == MOD_CTRL) {
            switch (toupper(c)) {
            case '[': *out = (Key){KESC, 0, 0}; return 1;
            case 'I': *out = (Key){KTAB, 0, 0}; return 1;
            case 'M': *out = (Key){KRET, 0, 0}; return 1;
            case 'H': *out = (Key){KBS, 0, 0}; return 1;
            default: break;
            }
        }
        *out = (Key){KCHAR, isalpha(c) ? (uint32_t)tolower(c) : c, mod};
        return 1;
    }
    if (!strcasecmp(text, "Space") || !strcasecmp(text, "Spc")) { *out = (Key){KCHAR, ' ', 0}; return 1; }
    for (size_t i = 0; i < sizeof(key_names) / sizeof(*key_names); ++i)
        if (!strcasecmp(text, key_names[i].name)) { *out = (Key){key_names[i].key, 0, 0}; return 1; }
    if ((text[0] == 'F' || text[0] == 'f') && isdigit((unsigned char)text[1])) {
        char upper[8]; snprintf(upper, sizeof(upper), "F%s", text + 1);
        if (keys_parse(upper, out)) return 1;
    }
    return keys_parse(text, out);
}
static int encode(uint32_t ch, char *out)
{
    if (ch < 0x80) { out[0] = (char)ch; return 1; }
    if (ch < 0x800) { out[0] = (char)(0xc0 | ch >> 6); out[1] = (char)(0x80 | (ch & 63)); return 2; }
    if (ch < 0x10000) { out[0] = (char)(0xe0 | ch >> 12); out[1] = (char)(0x80 | (ch >> 6 & 63)); out[2] = (char)(0x80 | (ch & 63)); return 3; }
    out[0] = (char)(0xf0 | ch >> 18); out[1] = (char)(0x80 | (ch >> 12 & 63)); out[2] = (char)(0x80 | (ch >> 6 & 63)); out[3] = (char)(0x80 | (ch & 63)); return 4;
}
int config_key_name(const Key *k, char *out, size_t size)
{
    static const char *const special[] = {
        [KUP]="Up",[KDOWN]="Down",[KLEFT]="Left",[KRIGHT]="Right",[KPGUP]="PgUp",[KPGDN]="PgDn",
        [KHOME]="Home",[KEND]="End",[KINS]="Ins",[KDEL]="Del",[KBS]="Bs",[KRET]="Ret",[KTAB]="Tab",[KESC]="Esc"
    };
    static const char *const script[] = {
        [KUP]="UP",[KDOWN]="DOWN",[KLEFT]="LEFT",[KRIGHT]="RIGHT",[KPGUP]="PGUP",[KPGDN]="PGDN",
        [KHOME]="HOME",[KEND]="END",[KINS]="INS",[KDEL]="DEL",[KBS]="BS",[KRET]="RET",[KTAB]="TAB",[KESC]="ESC"
    };
    char buf[32], ch[5] = {0};
    if (!k || !out || !size) return -1;
    int mods = k->mods & (MOD_CTRL | MOD_ALT | (k->key == KCHAR ? 0 : MOD_SHIFT));
    char prefix[8] = "";
    if (mods & MOD_CTRL) strcat(prefix, "C-");
    if (mods & MOD_ALT) strcat(prefix, "M-");
    if (mods & MOD_SHIFT) strcat(prefix, "S-");
    if (k->key == KCHAR) {
        if (!k->ch || k->ch > 0x10ffff) return -1;
        ch[encode(k->ch, ch)] = 0;
        uint32_t c = k->ch;
        if (!mods) {
            if (c == ' ') snprintf(buf, sizeof(buf), "Space");
            else if (c < 0x80 && strchr("#\"'\\;&|<>()`", (int)c))
                snprintf(buf, sizeof(buf), c == '"' || c == '\\' ? "\"\\%s\"" : "\"%s\"", ch);
            else snprintf(buf, sizeof(buf), "%s", ch);
        } else if (mods == MOD_CTRL && c == ' ') snprintf(buf, sizeof(buf), "^Space");
        else if (mods == MOD_CTRL && c >= 'a' && c <= 'z' && c != 'i' && c != 'm' && c != 'h')
            snprintf(buf, sizeof(buf), "^%c", (char)toupper((int)c));
        else if (mods == MOD_ALT && c >= 'a' && c <= 'z') snprintf(buf, sizeof(buf), "@%c", (char)toupper((int)c));
        else if (mods == MOD_ALT && c == ' ') snprintf(buf, sizeof(buf), "@Space");
        else if (c == ' ') snprintf(buf, sizeof(buf), "%sSPC", prefix);
        else if (c > 0x20 && c < 0x7f && strchr("#\"'\\;&|<>()`", (int)c)) return -1;
        else if (mods == MOD_CTRL && c > 0x20 && c < 0x7f && !isalpha((int)c) && c != '[') snprintf(buf, sizeof(buf), "^%c", (char)c);
        else if (mods == MOD_ALT && c > 0x20 && c < 0x7f && !isalpha((int)c)) snprintf(buf, sizeof(buf), "@%c", (char)c);
        else snprintf(buf, sizeof(buf), "%s%s", prefix, ch);
    } else if (k->key >= KF1 && k->key <= KF12) snprintf(buf, sizeof(buf), "%sF%d", prefix, k->key - KF1 + 1);
    else if (k->key >= 0 && (size_t)k->key < sizeof(special) / sizeof(*special) && special[k->key])
        snprintf(buf, sizeof(buf), "%s%s", prefix, mods ? script[k->key] : special[k->key]);
    else return -1;
    if (strlen(buf) >= size) return -1;
    strcpy(out, buf); return 0;
}

/* ---- variables ---- */
static int number(const char *s, long *out)
{
    if (!s || !*s || strlen(s) > 6) return 0;
    for (const char *p = s; *p; ++p) if (!isdigit((unsigned char)*p)) return 0;
    *out = strtol(s, NULL, 10); return 1;
}
static int variable_index(const char *name)
{
    for (int i = 0; i < CONFIG_VARIABLES; ++i) if (!strcmp(name, config_variable_names[i])) return i;
    return -1;
}
/* SPLIT_SIZE: a number of KB, or with K or M after it (item 37). */
static int split_parse(const char *value, long *kb)
{
    char *end; long v = strtol(value, &end, 10);
    if (end == value || v < 1) return -1;
    if (*end == 'M' || *end == 'm') { v *= 1024; ++end; }
    else if (*end == 'K' || *end == 'k') ++end;
    if (*end == 'B' || *end == 'b') ++end;
    if (*end || v > APP_SPLIT_MAX_KB) return -1;
    *kb = v; return 0;
}
static int variable_valid(const char *name, const char *value)
{
    long v; int i = variable_index(name);
    if (i < 0 || !value) return 0;
    if (i <= 2) return strlen(value) < 256;
    if (i == CONFIG_LANGUAGE) return !strcmp(value, "auto") || !strcmp(value, "ja") || !strcmp(value, "en");
    if (i == CONFIG_SCREENSIZE) { int c, r; return screen_parse(value, &c, &r) == 0; }
    if (i == CONFIG_SPLIT) return split_parse(value, &v) == 0;
    if (!number(value, &v)) return 0;
    if (i == CONFIG_UNDO) return v >= 1 && v <= CONFIG_UNDO_MAX;
    if (i == CONFIG_AWAKE) return v <= CONFIG_AWAKE_MAX;
    if (i == 3) return v % 100 <= 5 && v < 200;
    if (i == 4) return v == 1 || v == 2 || v == 3 || v == 5;
    return v == 0 || v == 1;
}
static void variable_apply(AppSettings *s, const char *name, const char *value)
{
    long v = 0; int i = variable_index(name);
    if (i == CONFIG_LANGUAGE) { copy_text(s->language, sizeof(s->language), value); return; }
    if (i == CONFIG_SCREENSIZE) {
        if (screen_parse(value, &s->screen_cols, &s->screen_rows)) s->screen_cols = s->screen_rows = 0;
        return;
    }
    if (i == CONFIG_AWAKE) { if (number(value, &v)) s->awake_minutes = (int)v; return; }
    if (i == CONFIG_SPLIT) { if (!split_parse(value, &v)) s->split_kb = (int)v; return; }
    if (i == CONFIG_UNDO) { if (number(value, &v) && v >= 1) s->undo_levels = (int)v; return; }
    if (i > 2) number(value, &v);
    switch (i) {
    case 0: copy_text(s->editor, sizeof(s->editor), value); break;
    case 1: copy_text(s->pager, sizeof(s->pager), value); break;
    case 2: copy_text(s->shell, sizeof(s->shell), value); break;
    case 3: s->sorttype = (int)v; break;
    case 4: s->displaymode = (int)v; break;
    case 5: s->dotfile = (int)v; break;
    case 6: s->symlink = (int)v; break;
    case 7: s->filetype = (int)v; break;
    case 8: s->novel_word_ruby = (int)v; break;
    default: if (i > 8) s->novel_format[i - CONFIG_BASE_VARIABLES] = (signed char)v; break;
    }
}
static void settings_default(AppSettings *s)
{
    memset(s, 0, sizeof(*s)); strcpy(s->language, "auto"); s->sorttype = 1; s->displaymode = 2; s->awake_minutes = APP_AWAKE_DEFAULT;
    s->split_kb = APP_SPLIT_DEFAULT_KB; s->undo_levels = APP_UNDO_DEFAULT;
    /* From the table itself: config.o is linked without novel_format.o too. */
#define NOVEL_FORMAT_DEFAULT(name, on, help) s->novel_format[NF_##name] = on;
    NOVEL_FORMAT_RULES(NOVEL_FORMAT_DEFAULT)
#undef NOVEL_FORMAT_DEFAULT
}
const char *config_variable_help(const char *name)
{
    static const char *const help[CONFIG_VARIABLES] = {
        N_("C-e・Alt+e で使うエディタ（空なら $VISUAL・$EDITOR・vi）"),
        N_("使いません（FDclone との互換のため読むだけ）"),
        N_("Shift+F6 と x で使うシェル（空なら $SHELL）"),
        N_("0〜5 = 並べない・名前・拡張子・サイズ・日付・長さ、+100 で降順"),
        N_("列数（1 2 3 5）"),
        N_("1 = ドットファイルを隠す、0 = 見せる"),
        N_("1 = シンボリックリンクの指し先を表示"),
        N_("1 = 名前の後に種類の記号（/ * @ = |）"),
        N_("1 = 小説エディタの Word 書き出しでルビを Word のルビに"),
#define NOVEL_FORMAT_HELP(name, on, help) help,
        NOVEL_FORMAT_RULES(NOVEL_FORMAT_HELP)
#undef NOVEL_FORMAT_HELP
        N_("表示の言語: auto（環境）/ ja（日本語）/ en（英語）。RET で切替"),
        N_("画面の桁と行: auto（画面に合わせる。1024x600 では 85x25）か 桁x行（80x25〜320x90）"),
        N_("最後のキーからこの分（0〜1440）は画面を休ませず速く応える。0 = 休ませる（既定 15）"),
        N_("Alt+n の分割の大きさの既定: 数字に K か M（1K〜1023M。既定 10M）"),
        N_("docx・xlsx の Ctrl+z・Ctrl+y で戻せる段数（1〜1000。既定 100）"),
    };
    int i = variable_index(name);
    if (i < 0) return NULL;
    if (i >= CONFIG_BASE_VARIABLES && i < CONFIG_LANGUAGE) {
        static char rule_help[256];
        snprintf(rule_help, sizeof(rule_help), "%s: 1 = %s", name, T(help[i]));
        return rule_help;
    }
    return T(help[i]);
}
void config_variable_text(App *a, const char *name, char *out, size_t size)
{
    const AppSettings *s = &a->settings;
    switch (variable_index(name)) {
    case 0: copy_text(out, size, s->editor); break;
    case 1: copy_text(out, size, s->pager); break;
    case 2: copy_text(out, size, s->shell); break;
    case 3: snprintf(out, size, "%d", s->sorttype); break;
    case 4: snprintf(out, size, "%d", s->displaymode); break;
    case 5: snprintf(out, size, "%d", s->dotfile); break;
    case 6: snprintf(out, size, "%d", s->symlink); break;
    case 7: snprintf(out, size, "%d", s->filetype); break;
    case 8: snprintf(out, size, "%d", s->novel_word_ruby); break;
    case CONFIG_SCREENSIZE:
        if (s->screen_cols > 0) snprintf(out, size, "%dx%d", s->screen_cols, s->screen_rows);
        else copy_text(out, size, "auto");
        break;
    case CONFIG_AWAKE: snprintf(out, size, "%d", s->awake_minutes); break;
    case CONFIG_SPLIT:
        if (s->split_kb % 1024 == 0) snprintf(out, size, "%dM", s->split_kb / 1024);
        else snprintf(out, size, "%dK", s->split_kb);
        break;
    case CONFIG_UNDO: snprintf(out, size, "%d", s->undo_levels); break;
    case CONFIG_LANGUAGE: snprintf(out, size, "%s", s->language); break;
    default: {
        int i = variable_index(name);
        if (i > 8) snprintf(out, size, "%d", s->novel_format[i - CONFIG_BASE_VARIABLES]);
        else if (size) *out = 0;
        break;
    }
    }
}

/* ---- one logical line ---- */
static char *comment_text(Split *sp, int from)
{
    Buf b = {0};
    if (from >= sp->count) return NULL;
    puts_(&b, sp->w[from].text + 1);
    for (int i = from + 1; i < sp->count; ++i) { putc_(&b, ' '); puts_(&b, sp->w[i].text); }
    if (b.fail) { free(b.p); return NULL; }
    if (!b.p) return strdup("");
    char *s = malloc(COMMAND_COMMENT); if (s) copy_text(s, COMMAND_COMMENT, b.p);
    free(b.p); return s;
}
/* 1 and out filled when the line is a bind/set/assignment/launch FDTP
 * understands; 0 otherwise. */
static int parse_line(const char *line, Record *out)
{
    Split sp; split(line, &sp);
    int ok = 0;
    memset(out, 0, sizeof(*out));
    if (sp.error || sp.shellish || !sp.count) goto done;
    Word *w = sp.w;
    if (is_word(&w[0], "bind")) {
        if (sp.count < 3 || !config_key_parse(w[1].text, &out->key)) goto done;
        int at = 3;
        if (w[2].quoted) {
            if (!*w[2].text || strlen(w[2].text) >= COMMAND_TEXT) goto done;
            out->shell = strdup(w[2].text); out->id = COMMAND_SHELL_ID;
        } else {
            if (!(out->id = commands_id(w[2].text))) goto done;
            if (sp.count > 3 && w[3].text[0] != ':') { if (!(out->dir_id = commands_id(w[3].text))) goto done; at = 4; }
        }
        if (at < sp.count) {
            if (w[at].text[0] != ':' || !(out->comment = comment_text(&sp, at))) goto done;
        }
        if (out->id == COMMAND_SHELL_ID && !out->shell) goto done;
        out->kind = R_BIND; ok = 1;
    } else if (is_word(&w[0], "launch")) {
        if (sp.count != 3) goto done;
        const char *ext = w[1].text;
        if (*ext == '*') ++ext;
        if (*ext == '.') ++ext;
        if (!*ext || strchr(ext, '/') || strlen(ext) > 32 || !*w[2].text) goto done;
        out->internal = 0;
        if (!w[2].quoted)
            for (size_t i = 0; i < sizeof(internal_viewers) / sizeof(*internal_viewers); ++i)
                if (!strcmp(w[2].text, internal_viewers[i])) out->internal = 1;
        out->ext = strdup(ext); out->target = strdup(w[2].text);
        if (!out->ext || !out->target) goto done;
        out->kind = R_LAUNCH; ok = 1;
    } else {
        const char *name, *value; char nm[32];
        int set = is_word(&w[0], "set"), first = set ? 1 : 0;
        if (set && sp.count == 3) {
            if (w[1].quoted || w[2].dollar || strchr(w[1].text, '=')) goto done;
            name = w[1].text; value = w[2].text;
            if (strlen(name) >= sizeof(nm)) goto done;
            strcpy(nm, name);
        } else if (sp.count == first + 1 && !w[first].quoted && !w[first].dollar) {
            const char *eq = strchr(w[first].text, '=');
            if (!eq || (size_t)(eq - w[first].text) >= sizeof(nm) || eq == w[first].text) goto done;
            memcpy(nm, w[first].text, (size_t)(eq - w[first].text)); nm[eq - w[first].text] = 0;
            value = eq + 1;
        } else goto done;
        if (!variable_valid(nm, value)) goto done;
        strcpy(out->name, nm);
        if (!(out->value = strdup(value))) goto done;
        out->kind = R_SET; ok = 1;
    }
done:
    if (ok && sp.comment != SIZE_MAX) out->trail = strdup(line + sp.comment);
    if (!ok) record_free(out);
    split_free(&sp);
    return ok;
}
static int block_change(const char *line, int *opened, int *closed, int *shellish)
{
    Split sp; split(line, &sp);
    *opened = *closed = 0; *shellish = sp.shellish || sp.error;
    static const char *const open[] = {"if", "case", "for", "while", "until", "{"};
    static const char *const close[] = {"fi", "esac", "done", "}"};
    /* Keywords count only where a command starts; braces anywhere. */
    for (int i = 0; i < sp.count; ++i) {
        int brace = is_word(&sp.w[i], "{") || is_word(&sp.w[i], "}");
        if (!sp.w[i].command && !brace) continue;
        for (size_t j = 0; j < sizeof(open) / sizeof(*open); ++j) if (is_word(&sp.w[i], open[j])) ++*opened;
        for (size_t j = 0; j < sizeof(close) / sizeof(*close); ++j) if (is_word(&sp.w[i], close[j])) ++*closed;
    }
    int words = sp.count; split_free(&sp); return words;
}

/* ---- loading ---- */
static int lines_in(const char *s) { int n = 1; for (; *s; ++s) n += *s == '\n'; return n; }
/* Reads path into layer. Missing file: 0 and an empty layer. -1 on error. */
static int load_layer(Layer *layer, const char *path, int *skipped)
{
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    Buf file = {0}; char chunk[4096]; size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        put(&file, chunk, n);
        if (file.n > FILE_LIMIT) { file.fail = 1; break; }
    }
    int failed = ferror(f) || file.fail; fclose(f);
    if (failed) { free(file.p); return -1; }
    int depth = 0, result = 0;
    const char *p = file.p ? file.p : "";
    while (*p) {
        Buf raw = {0}, joined = {0};
        for (;;) {
            const char *end = strchr(p, '\n'); size_t len = end ? (size_t)(end - p) : strlen(p);
            size_t body = len; if (body && p[body - 1] == '\r') --body;
            size_t slashes = 0; while (slashes < body && p[body - 1 - slashes] == '\\') ++slashes;
            if (raw.n) putc_(&raw, '\n');
            put(&raw, p, body);
            int more = (slashes & 1) && end && end[1];
            put(&joined, p, more ? body - 1 : body);
            p = end ? end + 1 : p + len;
            if (!more) break;
        }
        if (raw.fail || joined.fail) { free(raw.p); free(joined.p); result = -1; break; }
        const char *text = raw.p ? raw.p : "", *line = joined.p ? joined.p : "";
        int opened, closed, shellish, words = block_change(line, &opened, &closed, &shellish);
        Record r; int skip = 0;
        if (depth > 0 || opened > 0) { skip = 1; depth += opened - closed; if (depth < 0) depth = 0; }
        else if (closed > 0 || shellish) skip = 1;
        else if (!words) { memset(&r, 0, sizeof(r)); r.kind = R_TEXT; }
        else if (!parse_line(line, &r)) skip = 1;
        if (skip) { memset(&r, 0, sizeof(r)); r.kind = R_SKIP; *skipped += lines_in(text); }
        if (r.kind == R_TEXT || r.kind == R_SKIP) r.text = strdup(text);
        Record *slot = layer_append(layer);
        if (!slot || ((r.kind == R_TEXT || r.kind == R_SKIP) && !r.text)) { record_free(&r); free(raw.p); free(joined.p); result = -1; break; }
        *slot = r;
        free(raw.p); free(joined.p);
    }
    free(file.p);
    return result;
}
static void export_shell(App *a)
{
    ConfigState *st = a->config;
    if (!st->shell_saved) {
        const char *s = getenv("SHELL"); st->original_shell = s ? strdup(s) : NULL; st->shell_saved = 1;
    }
    if (a->settings.shell[0]) { setenv("SHELL", a->settings.shell, 1); st->shell_exported = 1; }
    else if (st->shell_exported) {
        if (st->original_shell) setenv("SHELL", st->original_shell, 1); else unsetenv("SHELL");
        st->shell_exported = 0;
    }
}
static void apply_language(const AppSettings *s)
{
    const char *lang = getenv("FDTP_LANGUAGE");
    if (!lang || (strcmp(lang, "ja") && strcmp(lang, "en"))) lang = s->language;
    if (!strcmp(lang, "auto")) {
        static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
        lang = "";
        for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            const char *value = getenv(names[i]);
            if (value && *value) { lang = value; break; }
        }
        i18n_set(!strncmp(lang, "ja", 2) ? I18N_JA : I18N_EN);
    } else i18n_set(!strcmp(lang, "ja") ? I18N_JA : I18N_EN);
}
/* Defaults, then every layer in order. Returns how many binds did not fit. */
static int apply_all(App *a)
{
    ConfigState *st = a->config; AppSettings old = a->settings; int failed = 0;
    commands_init(&a->commands); settings_default(&a->settings);
    for (int i = 0; i < st->layer_count; ++i) {
        Layer *l = &st->layers[i];
        for (size_t j = 0; j < l->count; ++j) {
            Record *r = &l->items[j];
            if (r->kind == R_SET) variable_apply(&a->settings, r->name, r->value);
            else if (r->kind == R_BIND) {
                int e = r->shell ? commands_bind_shell(&a->commands, &r->key, r->shell, r->comment)
                                 : commands_bind_full(&a->commands, &r->key, r->id, r->dir_id, r->comment);
                if (e < 0) ++failed;
            }
        }
    }
    apply_language(&a->settings);
    export_shell(a);
    if (memcmp(&old, &a->settings, sizeof(old)))
        for (int i = 0; i < a->depth; ++i) filer_apply_settings(a->screens[i], a);
    a->changed = 1;
    return failed;
}
static int register_layer(const Layer *l)
{
    int failed = 0;
    for (size_t j = 0; j < l->count; ++j)
        if (l->items[j].kind == R_LAUNCH && launcher_register(NULL, l->items[j].ext, l->items[j].target) < 0) ++failed;
    return failed;
}
/* The launcher's table from scratch: its built-ins, then every layer. */
static int register_all(App *a)
{
    int failed = 0;
    launcher_reset();
    for (int i = 0; i < a->config->layer_count; ++i) failed += register_layer(&a->config->layers[i]);
    return failed;
}
static void report(App *a, int skipped, int unfit, int launch_failed)
{
    char message[256] = "", part[96];
    if (skipped) { snprintf(part, sizeof(part), i18n_lang() == I18N_EN && skipped == 1 ? "Skipped %d line" : T("%d 行を読み飛ばしました"), skipped); strcat(message, part); }
    if (unfit) { snprintf(part, sizeof(part), i18n_lang() == I18N_EN && unfit == 1 ? "%sCannot add %d key binding" : T("%sキーの割り当て %d 件が入りません"), *message ? (i18n_lang() == I18N_EN ? ". " : T("。")) : "", unfit); strcat(message, part); }
    if (launch_failed) { snprintf(part, sizeof(part), i18n_lang() == I18N_EN && launch_failed == 1 ? "%sCannot register %d launcher" : T("%sランチャ %d 件を登録できません"), *message ? (i18n_lang() == I18N_EN ? ". " : T("。")) : "", launch_failed); strcat(message, part); }
    if (*message) app_notice(a, message);
}
static void make_label(ConfigState *st)
{
    const char *path = st->user >= 0 ? st->layers[st->user].path : NULL, *home = getenv("HOME");
    size_t h = home ? strlen(home) : 0;
    if (!path) st->label[0] = 0;
    else if (h && !strncmp(path, home, h) && path[h] == '/') snprintf(st->label, sizeof(st->label), "~%s", path + h);
    else snprintf(st->label, sizeof(st->label), "%s", path);
    /* Messages on line 23 must fit: keep the end of a long path. */
    const char *tail = st->label; int cut = 0;
    while (cells_width(tail) > 40) { ++tail; while ((*tail & 0xc0) == 0x80) ++tail; cut = 1; }
    /* The tail is at most 40 cells (under 200 bytes), so "…" + tail fits. */
    if (cut) { size_t n = strlen(tail); memmove(st->label + 3, tail, n + 1); memcpy(st->label, "…", 3); }
}
int config_startup(App *a)
{
    ConfigState *st = calloc(1, sizeof(*st));
    settings_default(&a->settings);
    apply_language(&a->settings);
    if (!st) return -1;
    a->config = st; st->user = -1;
    const char *system = getenv("FDTP_RC_SYSTEM"), *user = getenv("FDTP_RC_USER"), *home = getenv("HOME");
    if (!system) system = "/etc/fdtprc";
    char *user_path = NULL;
    if (user) user_path = *user ? strdup(user) : NULL;
    else if (home && *home) {
        size_t n = strlen(home) + 10; user_path = malloc(n);
        if (user_path) snprintf(user_path, n, "%s/.fdtprc", home);
    }
    int skipped = 0, launch_failed = 0;
    Layer *sys = &st->layers[st->layer_count++];
    if (*system) { sys->path = strdup(system); if (sys->path && load_layer(sys, system, &skipped) < 0) app_notice(a, T("設定ファイルを読めません")); }
    st->user = st->layer_count++;
    Layer *usr = &st->layers[st->user]; usr->path = user_path;
    if (user_path && load_layer(usr, user_path, &skipped) < 0) app_notice(a, T("~/.fdtprc を読めません"));
    make_label(st);
    int unfit = apply_all(a);
    launch_failed += register_all(a);
    report(a, skipped, unfit, launch_failed);
    return skipped;
}
void config_free(App *a)
{
    ConfigState *st = a ? a->config : NULL;
    if (!st) return;
    for (int i = 0; i < st->layer_count; ++i) layer_free(&st->layers[i]);
    free(st->original_shell); free(st); a->config = NULL;
}
int config_load(App *a, CommandTable *table, const char *path)
{
    (void)table;
    ConfigState *st = a->config;
    if (!st || !path || st->layer_count == LAYERS) { app_notice(a, T("設定ファイルを読めません")); return -1; }
    Layer *l = &st->layers[st->layer_count];
    int skipped = 0;
    if (!(l->path = strdup(path)) || load_layer(l, path, &skipped) < 0) { layer_free(l); app_notice(a, T("設定ファイルを読めません")); return -1; }
    ++st->layer_count;
    int unfit = apply_all(a);
    report(a, skipped, unfit, register_all(a));
    return 0;
}
static int parse_text(const char *head, const char *key, const char *rest, Record *r)
{
    Buf b = {0};
    puts_(&b, head); if (key) { putc_(&b, ' '); puts_(&b, key); }
    putc_(&b, ' '); puts_(&b, rest ? rest : "");
    int ok = !b.fail && parse_line(b.p, r);
    free(b.p); return ok;
}
int config_bind(App *a, CommandTable *table, const char *key, const char *command)
{
    Record r;
    if (!table || !key || !command || !parse_text("bind", key, command, &r) || r.kind != R_BIND) { app_notice(a, T("キーの割り当てが正しくありません")); return -1; }
    int e = r.shell ? commands_bind_shell(table, &r.key, r.shell, r.comment) : commands_bind_full(table, &r.key, r.id, r.dir_id, r.comment);
    record_free(&r);
    if (e < 0) app_notice(a, T("キーの割り当てが入りません"));
    return e;
}
int config_keymap(App *a, CommandTable *table, const char *from, const char *to)
{
    Key f, t;
    if (!table || !config_key_parse(from, &f) || !config_key_parse(to, &t) || commands_keymap(table, &f, &t) < 0) {
        app_notice(a, T("キーの対応が正しくありません")); return -1;
    }
    return 0;
}

/* ---- canonical text (what the customizer writes) ---- */
static int plain(const char *s)
{
    if (!*s) return 0;
    for (; *s; ++s) if (!isalnum((unsigned char)*s) && !strchr("_./+,:=@^%~-", *s) && (unsigned char)*s < 0x80) return 0;
    return 1;
}
static void put_quoted(Buf *b, const char *s, int dollar)
{
    putc_(b, '"');
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\' || (dollar && (*s == '$' || *s == '`'))) putc_(b, '\\');
        putc_(b, *s);
    }
    putc_(b, '"');
}
static void put_rhs(Buf *b, const char *id, const char *dir_id, const char *shell, const char *comment)
{
    if (shell) put_quoted(b, shell, 0);
    else { puts_(b, id); if (dir_id) { putc_(b, ' '); puts_(b, dir_id); } }
    if (comment && *comment) { puts_(b, " :"); puts_(b, comment); }
}
static void put_record(Buf *b, const Record *r)
{
    char key[40];
    switch (r->kind) {
    case R_TEXT: puts_(b, r->text); return;
    case R_SKIP:
        for (const char *p = r->text;;) {
            const char *end = strchr(p, '\n');
            puts_(b, "# 読み飛ばし: "); put(b, p, end ? (size_t)(end - p) : strlen(p)); /* i18n:keep */
            if (!end) break;
            putc_(b, '\n'); p = end + 1;
        }
        return;
    case R_SET:
        puts_(b, r->name); putc_(b, '=');
        if (plain(r->value)) puts_(b, r->value); else put_quoted(b, r->value, 1);
        break;
    case R_BIND:
        if (config_key_name(&r->key, key, sizeof(key)) < 0) snprintf(key, sizeof(key), "?");
        puts_(b, "bind "); puts_(b, key); putc_(b, ' ');
        put_rhs(b, r->id, r->dir_id, r->shell, r->comment);
        break;
    case R_LAUNCH:
        puts_(b, "launch ."); puts_(b, r->ext); putc_(b, ' ');
        if (r->internal) puts_(b, r->target); else put_quoted(b, r->target, 0);
        break;
    }
    if (r->trail) { putc_(b, ' '); puts_(b, r->trail); }
}
void config_binding_text(const CommandTable *t, const CommandBinding *bd, char *out, size_t size)
{
    Buf b = {0};
    put_rhs(&b, bd->id, bd->dir_id, commands_shell(t, bd), bd->comment);
    copy_text(out, size, b.fail || !b.p ? "" : b.p); free(b.p);
}

/* ---- customizer changes (user layer) ---- */
static Layer *user_layer(App *a)
{
    ConfigState *st = a->config;
    return st && st->user >= 0 ? &st->layers[st->user] : NULL;
}
typedef int (*Match)(const Record *, const Record *);
static int same_set(const Record *a, const Record *b) { return a->kind == R_SET && !strcmp(a->name, b->name); }
static int same_bind(const Record *a, const Record *b) { return a->kind == R_BIND && keys_equal(&a->key, &b->key); }
static int same_launch(const Record *a, const Record *b) { return a->kind == R_LAUNCH && !strcmp(a->ext, b->ext); }
/* replace the last matching record (keeping its trailing comment), or append;
 * with remove, drop every match. Takes ownership of r. */
static int upsert(App *a, Record *r, Match match, int remove)
{
    Layer *l = user_layer(a);
    if (!l) { record_free(r); return -1; }
    if (remove) {
        for (size_t i = l->count; i-- > 0;) if (match(&l->items[i], r)) layer_remove(l, i);
        record_free(r);
    } else {
        size_t i = l->count;
        while (i-- > 0) if (match(&l->items[i], r)) break;
        if (i != SIZE_MAX) { r->trail = l->items[i].trail; l->items[i].trail = NULL; record_free(&l->items[i]); l->items[i] = *r; }
        else {
            Record *slot = layer_append(l);
            if (!slot) { record_free(r); return -1; }
            *slot = *r;
        }
    }
    a->config->dirty = 1;
    int unfit = apply_all(a);
    if (unfit) report(a, 0, unfit, 0);
    return 0;
}
int config_set_variable(App *a, const char *name, const char *value)
{
    Record r; memset(&r, 0, sizeof(r)); r.kind = R_SET;
    if (variable_index(name) < 0) return -1;
    copy_text(r.name, sizeof(r.name), name);
    if (!value || !*value) return upsert(a, &r, same_set, 1);
    if (!variable_valid(name, value) || !(r.value = strdup(value))) return -1;
    return upsert(a, &r, same_set, 0);
}
int config_set_binding(App *a, const Key *key, const char *rhs)
{
    Record r; char name[40];
    if (!rhs || !*rhs) { memset(&r, 0, sizeof(r)); r.kind = R_BIND; r.key = *key; return upsert(a, &r, same_bind, 1); }
    if (config_key_name(key, name, sizeof(name)) < 0 || !parse_text("bind", name, rhs, &r)) return -1;
    if (r.trail) { record_free(&r); return -1; }
    return upsert(a, &r, same_bind, 0);
}
int config_add_binding(App *a, const char *line)
{
    Record r;
    if (!line || !parse_text("bind", NULL, line, &r)) return -1;
    if (r.trail) { record_free(&r); return -1; }
    return upsert(a, &r, same_bind, 0);
}
static const Record *find_launch(App *a, const char *ext, int skip_user)
{
    ConfigState *st = a->config; const Record *found = NULL;
    for (int i = 0; i < st->layer_count; ++i) {
        if (skip_user && i == st->user) continue;
        for (size_t j = 0; j < st->layers[i].count; ++j) {
            const Record *r = &st->layers[i].items[j];
            if (r->kind == R_LAUNCH && !strcmp(r->ext, ext)) found = r;
        }
    }
    return found;
}
static int launch_record(App *a, Record *r)
{
    if (upsert(a, r, same_launch, 0) < 0) return -1;
    if (register_all(a)) app_notice(a, T("ランチャを登録できません"));
    return 0;
}
int config_set_launch(App *a, const char *ext, const char *target)
{
    Record r; char head[48];
    if (!ext || !*ext) return -1;
    if (!target || !*target) {
        memset(&r, 0, sizeof(r)); r.kind = R_LAUNCH; r.ext = strdup(ext);
        if (!r.ext || upsert(a, &r, same_launch, 1) < 0) return -1;
        if (register_all(a)) app_notice(a, T("ランチャを登録できません"));
        return 0;
    }
    snprintf(head, sizeof(head), "launch .%s", ext);
    if (!parse_text(head, NULL, target, &r)) return -1;
    if (r.trail) { record_free(&r); return -1; }
    return launch_record(a, &r);
}
int config_add_launch(App *a, const char *line)
{
    Record r;
    if (!line || !parse_text("launch", NULL, line, &r)) return -1;
    if (r.trail) { record_free(&r); return -1; }
    return launch_record(a, &r);
}
size_t config_launch_count(App *a)
{
    ConfigState *st = a->config; size_t n = 0;
    if (!st) return 0;
    for (int i = 0; i < st->layer_count; ++i)
        for (size_t j = 0; j < st->layers[i].count; ++j) {
            const Record *r = &st->layers[i].items[j];
            if (r->kind == R_LAUNCH && find_launch(a, r->ext, 0) == r) ++n;
        }
    return n;
}
int config_launch_get(App *a, size_t index, const char **ext, char *target, size_t size)
{
    ConfigState *st = a->config;
    if (!st) return -1;
    /* Order: where each extension first appears; value: its last line. */
    for (int i = 0; i < st->layer_count; ++i)
        for (size_t j = 0; j < st->layers[i].count; ++j) {
            const Record *r = &st->layers[i].items[j];
            if (r->kind != R_LAUNCH || find_launch(a, r->ext, 0) != r) continue;
            if (index--) continue;
            Buf b = {0};
            if (r->internal) puts_(&b, r->target); else put_quoted(&b, r->target, 0);
            *ext = r->ext; copy_text(target, size, b.fail || !b.p ? "" : b.p); free(b.p);
            return 0;
        }
    return -1;
}
const char *config_user_label(App *a) { return a->config ? a->config->label : ""; }
int config_dirty(App *a) { return a->config && a->config->dirty; }
int config_write(App *a)
{
    Layer *l = user_layer(a); char message[600];
    if (!l || !l->path) { app_notice(a, T("書き込み先がありません")); return -1; }
    Buf b = {0};
    for (size_t i = 0; i < l->count; ++i) { put_record(&b, &l->items[i]); putc_(&b, '\n'); }
    if (b.fail) { free(b.p); app_notice(a, T("メモリが足りません")); return -1; }
    size_t n = strlen(l->path) + 8;
    char *bak = malloc(n), *tmp = malloc(n);
    if (!bak || !tmp) { free(b.p); free(bak); free(tmp); app_notice(a, T("メモリが足りません")); return -1; }
    snprintf(bak, n, "%s.bak", l->path); snprintf(tmp, n, "%s.tmp", l->path);
    int result = -1;
    FILE *in = fopen(l->path, "rb");
    if (in) {
        FILE *out = fopen(bak, "wb"); char chunk[4096]; size_t k; int bad = !out;
        while (!bad && (k = fread(chunk, 1, sizeof(chunk), in)) > 0) if (fwrite(chunk, 1, k, out) != k) bad = 1;
        if (ferror(in)) bad = 1;
        fclose(in);
        if (out && fclose(out)) bad = 1;
        if (bad) { snprintf(message, sizeof(message), T("%s.bak を作れません"), config_user_label(a)); app_notice(a, message); goto out; }
    } else if (errno != ENOENT) { app_notice(a, T("~/.fdtprc を読めません")); goto out; }
    FILE *f = fopen(tmp, "wb");
    int bad = !f || (b.n && fwrite(b.p, 1, b.n, f) != b.n);
    if (f && fclose(f)) bad = 1;
    if (bad || rename(tmp, l->path) < 0) {
        remove(tmp);
        snprintf(message, sizeof(message), T("%s に書き込めません"), config_user_label(a)); app_notice(a, message); goto out;
    }
    /* What was written as comments is now the file's comment text. */
    for (size_t i = 0; i < l->count; ++i) if (l->items[i].kind == R_SKIP) {
        Buf c = {0}; Record *r = &l->items[i]; put_record(&c, r);
        if (!c.fail && c.p) { free(r->text); r->text = c.p; r->kind = R_TEXT; } else free(c.p);
    }
    a->config->dirty = 0; result = 0;
    snprintf(message, sizeof(message), T("%s に書き込みました"), config_user_label(a)); app_notice(a, message);
out:
    free(b.p); free(bak); free(tmp);
    return result;
}

/* ---- macros ---- */
static void put_name(Buf *b, const char *s)
{
    const char *p = s;
    int safe = *s != 0;
    for (; *p; ++p) if (!isalnum((unsigned char)*p) && !strchr("_./+,:=@^-", *p)) { safe = 0; break; }
    if (safe) { puts_(b, s); return; }
    putc_(b, '\'');
    for (p = s; *p; ++p) { if (*p == '\'') puts_(b, "'\\''"); else putc_(b, *p); }
    putc_(b, '\'');
}
char *config_expand_macros(const char *command, const FileSelection *sel)
{
    Buf b = {0};
    const char *current = sel && sel->current ? sel->current : "", *directory = sel && sel->directory ? sel->directory : "";
    if (!command) return NULL;
    for (const char *p = command; *p; ++p) {
        if (*p != '%' || !p[1]) { putc_(&b, *p); continue; }
        switch (p[1]) {
        case 'C': put_name(&b, current); break;
        case 'P': put_name(&b, directory); break;
        case 'M':
            for (size_t i = 0; sel && i < sel->count; ++i) { if (i) putc_(&b, ' '); put_name(&b, sel->names[i]); }
            break;
        case 'X': {
            const char *dot = strrchr(current, '.');
            int special = !strcmp(current, ".") || !strcmp(current, "..");
            size_t n = dot && dot != current && !special ? (size_t)(dot - current) : strlen(current);
            char *stem = strndup(current, n);
            if (!stem) { b.fail = 1; break; }
            put_name(&b, stem); free(stem); break;
        }
        case '%': putc_(&b, '%'); break;
        default: putc_(&b, '%'); putc_(&b, p[1]); break;
        }
        ++p;
    }
    if (b.fail) { free(b.p); return NULL; }
    return b.p ? b.p : strdup("");
}
int config_run_shell(App *a, Screen *filer, const char *command)
{
    FileSelection sel;
    if (filer_selection(filer, &sel) < 0) { app_notice(a, T("メモリが足りません")); return -1; }
    char *line = config_expand_macros(command, &sel);
    if (!line) { app_notice(a, T("メモリが足りません")); return -1; }
    const char *argv[] = {"/bin/sh", "-c", line, NULL};
    int e = app_request_external_at(a, argv, APP_EXTERNAL_TERMINAL | APP_EXTERNAL_PAUSE, sel.directory);
    free(line);
    if (e < 0) app_notice(a, T("外部コマンドを準備できません"));
    return e;
}

/* The picker shares the floating confirmation boxes' frame and shadow. */
static int first_run_key(App *a, const Key *key);
static void first_run_draw(App *a, Cells *c);
static void first_run_close(App *a)
{
    a->config->first_run = 0;
    a->first_run_key = NULL; a->first_run_draw = NULL;
}
void config_first_run(App *a, int headless)
{
    ConfigState *st = a->config;
    const char *test = getenv("FDTP_FIRST_RUN_TEST");
    if (!st || getenv("FDTP_TEST_KEYS") || getenv("FDTP_LANGUAGE") ||
        (headless && (!test || strcmp(test, "1")))) return;
    const char *path = st->user >= 0 ? st->layers[st->user].path : NULL;
    struct stat sb;
    if (!path || lstat(path, &sb) == 0 || errno != ENOENT) return;
    /* make install writes LANGUAGE to /etc/fdtprc: that answers it already. */
    for (int i = 0; i < st->layer_count; ++i)
        for (size_t j = 0; j < st->layers[i].count; ++j)
            if (st->layers[i].items[j].kind == R_SET && !strcmp(st->layers[i].items[j].name, "LANGUAGE")) return;
    st->first_run = 1;
    a->first_run_key = first_run_key; a->first_run_draw = first_run_draw;
    st->language_choice = i18n_lang() == I18N_EN;
    a->changed = 1;
}
static int first_run_key(App *a, const Key *key)
{
    ConfigState *st = a->config;
    if (!st || !st->first_run) return 0;
    if (key->key == KESC) { first_run_close(a); return 1; }
    if (key->key == KUP) st->language_choice = 0;
    if (key->key == KDOWN) st->language_choice = 1;
    int accept = key->key == KRET;
    if (key->key == KCHAR && (key->ch == '1' || key->ch == '2')) {
        st->language_choice = key->ch == '2'; accept = 1;
    }
    if (accept) {
        const char *value = st->language_choice ? "en" : "ja";
        const char *path = st->layers[st->user].path;
        /* Exclusive creation: a file created since startup must survive. */
        FILE *f = fopen(path, "wx");
        int failed = !f;
        if (f) {
            failed = fprintf(f, "# FDTP language\nLANGUAGE=%s\n", value) < 0;
            if (fclose(f)) failed = 1;
            if (failed) unlink(path);
        }
        first_run_close(a);
        if (failed) app_error(a, T("~/.fdtprc を作れません"));
        else {
            config_set_variable(a, "LANGUAGE", value);
            st->dirty = 0;
        }
    }
    return 1;
}
static void first_run_draw(App *a, Cells *c)
{
    ConfigState *st = a->config;
    if (!st || !st->first_run) return;
    int w = 30, h = 7, x = (FD_COLS - w) / 2, y = (FD_ROWS - h) / 2;
    if (cells_box(c, x, y, w, h, CELL_BOX_FRAME | CELL_BOX_SHADOW) < 0) return;
    cells_text(c, x + 2, y + 1, "Language / 言語", CYAN, BG, w - 4); /* i18n:keep */
    const char *options[] = {"1  日本語", "2  English"}; /* i18n:keep */
    for (int i = 0; i < 2; ++i) {
        int fg = st->language_choice == i ? BG : WHITE;
        int bg = st->language_choice == i ? WHITE : BG;
        for (int col = x + 2; col < x + w - 2; ++col) cells_put(c, col, y + 3 + i, ' ', fg, bg);
        cells_text(c, x + 3, y + 3 + i, options[i], fg, bg, w - 6);
    }
}
