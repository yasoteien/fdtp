// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Stage 1c, remade for revision 1 item 27: the customizer (z). Three
 * headings, variables / keys / launchers, in FD's frame (the CYAN two-dot
 * line with round corners) over the whole screen but the message line and
 * the band, with lines between the headings, the column titles, the list
 * and the explanation, and a line between the names and the values.
 *
 *   row 0            frame, 「FDTP の設定」 (and 「変更あり」) in its top line
 *   row 1            the headings (←→), 「書き込み先: …」 at the right
 *   row 2            line
 *   row 3            column titles
 *   row 4            line
 *   rows 5..R-6      the list (15 rows at 25 rows), ↑↓ PgUp PgDn Home End
 *   row R-5          line
 *   row R-4          what the chosen item is, how ~/.fdtprc writes it
 *   row R-3          frame, 「↑↓ 1-15/171」 in its bottom line
 *   row R-2, R-1     message line, the band (keys of the customizer)
 *
 * The keys heading lists every key from the start (not only the bound
 * ones): a-z, Alt+a-z, Ctrl+a-z, F1-F12, Shift+F1-F12, the digits and
 * symbols, the other keys, then any other key bound in a file, then
 * 「（追加）」. Each shows its built-in command as 「内蔵エディタ（Reserved）」,
 * the bind of a file, or 「（空き）」. RET changes it; a built-in one asks
 * first. An empty answer removes the user file's line (back to the built-in
 * command or to free). */
#include "config_state.h"
#include "app.h"
#include "widgets.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAB_VARIABLES, TAB_KEYS, TAB_LAUNCH, TABS };
enum { ROW_TABS = 1, ROW_TITLES = 3, LIST_TOP = 5 };
#define LIST_ROWS (FD_ROWS - 10)        /* 15 at 25 rows (WIDE_NOTES.md) */
#define ROW_RULE (FD_ROWS - 5)
#define ROW_HELP (FD_ROWS - 4)
typedef enum { E_NONE, E_EDIT, E_ADD, E_QUIT, E_OVERRIDE } Pending;
typedef enum { K_HEADING, K_KEY, K_ADD } KeyKind;
typedef struct { KeyKind kind; Key key; const char *heading; } KeyItem;
#define KEY_ITEMS (COMMAND_BINDINGS + 256)
typedef struct {
    int tab, cursor[TABS], top[TABS];
    Pending pending;
    Key key; char name[48], label[48];
    KeyItem keys[KEY_ITEMS]; int key_count;
} Editor;

/* ---- the keys heading ---- */
static const char symbols[] = "1234567890-^@[;:],./\\!\"#$%&'()=~`{+*}<>?_|";
static const int specials[] = { KUP, KDOWN, KLEFT, KRIGHT, KPGUP, KPGDN, KHOME, KEND, KINS, KDEL, KBS, KRET, KTAB, KESC };
static void add(Editor *e, KeyKind kind, Key key, const char *heading)
{
    if (e->key_count < KEY_ITEMS) e->keys[e->key_count++] = (KeyItem){kind, key, heading};
}
static int listed(const Editor *e, const Key *k)
{
    for (int i = 0; i < e->key_count; ++i) if (e->keys[i].kind == K_KEY && keys_equal(&e->keys[i].key, k)) return 1;
    return 0;
}
static void build_keys(Editor *e, const CommandTable *t)
{
    e->key_count = 0;
    add(e, K_HEADING, (Key){0}, N_("英字"));
    for (uint32_t c = 'a'; c <= 'z'; ++c) add(e, K_KEY, (Key){KCHAR, c, 0}, NULL);
    add(e, K_HEADING, (Key){0}, N_("Alt+英字"));
    for (uint32_t c = 'a'; c <= 'z'; ++c) add(e, K_KEY, (Key){KCHAR, c, MOD_ALT}, NULL);
    add(e, K_HEADING, (Key){0}, N_("Ctrl+英字"));
    for (uint32_t c = 'a'; c <= 'z'; ++c) add(e, K_KEY, (Key){KCHAR, c, MOD_CTRL}, NULL);
    add(e, K_HEADING, (Key){0}, N_("ファンクションキー"));
    for (int f = KF1; f <= KF12; ++f) add(e, K_KEY, (Key){f, 0, 0}, NULL);
    add(e, K_HEADING, (Key){0}, N_("Shift+ファンクションキー"));
    for (int f = KF1; f <= KF12; ++f) add(e, K_KEY, (Key){f, 0, MOD_SHIFT}, NULL);
    add(e, K_HEADING, (Key){0}, N_("数字・記号"));
    for (const char *p = symbols; *p; ++p) add(e, K_KEY, (Key){KCHAR, (unsigned char)*p, 0}, NULL);
    add(e, K_KEY, (Key){KCHAR, 0x00a5, 0}, NULL);
    add(e, K_HEADING, (Key){0}, N_("そのほかのキー"));
    for (size_t i = 0; i < sizeof(specials) / sizeof(*specials); ++i) add(e, K_KEY, (Key){specials[i], 0, 0}, NULL);
    add(e, K_KEY, (Key){KCHAR, ' ', 0}, NULL);
    add(e, K_KEY, (Key){KCHAR, ' ', MOD_CTRL}, NULL);
    int others = 0;
    for (size_t i = 0; i < t->count; ++i) {
        if (listed(e, &t->bindings[i].key)) continue;
        if (!others++) add(e, K_HEADING, (Key){0}, N_("ほかの bind"));
        add(e, K_KEY, t->bindings[i].key, NULL);
    }
    add(e, K_ADD, (Key){0}, NULL);
}
/* A key as people read it: 「a」「Alt+a」「Ctrl+a」「Shift+F1」「↑」. */
static void key_label(const Key *k, char *out, size_t size)
{
    static const char *const names[] = {
        [KUP]="↑",[KDOWN]="↓",[KLEFT]="←",[KRIGHT]="→",[KPGUP]="PgUp",[KPGDN]="PgDn",[KHOME]="Home",[KEND]="End",
        [KINS]="Ins",[KDEL]="Del",[KBS]="BS",[KRET]="Enter",[KTAB]="Tab",[KESC]="Esc"
    };
    char base[16] = "";
    if (k->key == KCHAR) {
        if (k->ch == ' ') snprintf(base, sizeof(base), "Space");
        else if (k->ch == 0x00a5) snprintf(base, sizeof(base), "￥"); /* i18n:keep */
        else if (k->ch < 0x80) snprintf(base, sizeof(base), "%c", (char)k->ch);
        else if (config_key_name(&(Key){KCHAR, k->ch, 0}, base, sizeof(base)) < 0) snprintf(base, sizeof(base), "?");
    } else if (k->key >= KF1 && k->key <= KF12) snprintf(base, sizeof(base), "F%d", k->key - KF1 + 1);
    else if (k->key >= 0 && (size_t)k->key < sizeof(names) / sizeof(*names) && names[k->key]) snprintf(base, sizeof(base), "%s", names[k->key]);
    else snprintf(base, sizeof(base), "?");
    snprintf(out, size, "%s%s%s%s", k->mods & MOD_CTRL ? "Ctrl+" : "", k->mods & MOD_ALT ? "Alt+" : "",
             k->mods & MOD_SHIFT ? "Shift+" : "", base);
}
static const CommandBinding *bound(const CommandTable *t, const Key *k)
{
    for (size_t i = 0; i < t->count; ++i) if (keys_equal(&t->bindings[i].key, k)) return &t->bindings[i];
    return NULL;
}
/* Left Ctrl+C ends FDTP at once (the platform); it cannot be bound. */
static int fixed_key(const Key *k) { return k->key == KCHAR && k->ch == 'c' && k->mods == MOD_CTRL; }
/* The key's built-in binding when it is still the one in force. */
static const CommandBinding *reserved(const CommandTable *t, const Key *k)
{
    const CommandBinding *b = bound(t, k), *d = bound(commands_defaults(), k);
    if (!b || !d || b->shell || d->shell || b->id != d->id || b->dir_id != d->dir_id || strcmp(b->comment, d->comment)) return NULL;
    return d;
}
static void reserved_text(const CommandBinding *d, char *out, size_t size)
{
    const char *label = commands_label(d->id);
    snprintf(out, size, T("%s（Reserved）"), label ? label : d->id);
}

/* ---- the list ---- */
static int item_count(Editor *e, App *a)
{
    if (e->tab == TAB_VARIABLES) return CONFIG_VARIABLES;
    if (e->tab == TAB_KEYS) { build_keys(e, &a->commands); return e->key_count; }
    return (int)config_launch_count(a) + 1;
}
static int heading_at(const Editor *e, int i) { return e->tab == TAB_KEYS && i >= 0 && i < e->key_count && e->keys[i].kind == K_HEADING; }
/* The cursor never rests on a heading: it goes on in direction (or back). */
static void skip_heading(Editor *e, int n, int direction)
{
    int *c = &e->cursor[e->tab];
    if (*c >= n) *c = n - 1;
    if (*c < 0) *c = 0;
    while (heading_at(e, *c) && *c + direction >= 0 && *c + direction < n) *c += direction;
    while (heading_at(e, *c) && *c - direction >= 0 && *c - direction < n) *c -= direction;
}
static void clamp(Editor *e, App *a)
{
    int n = item_count(e, a), *c = &e->cursor[e->tab], *top = &e->top[e->tab];
    skip_heading(e, n, 1);
    if (*c < *top) *top = *c;
    /* the heading over the first key of a section stays in view */
    if (*c == *top && heading_at(e, *c - 1)) *top = *c - 1;
    if (*c >= *top + LIST_ROWS) *top = *c - LIST_ROWS + 1;
    if (*top > n - LIST_ROWS) *top = n - LIST_ROWS;
    if (*top < 0) *top = 0;
}
/* Label and value of item i of the current heading; *dim when the value is
 * a placeholder (「（空き）」…). */
static void item_text(Editor *e, App *a, int i, char *label, size_t lsize, char *value, size_t vsize, int *dim)
{
    *label = *value = 0; *dim = 0;
    if (e->tab == TAB_VARIABLES) {
        snprintf(label, lsize, "%s", config_variable_names[i]);
        config_variable_text(a, config_variable_names[i], value, vsize);
        if (!*value) { snprintf(value, vsize, "%s", T("（未設定）")); *dim = 1; }
    } else if (e->tab == TAB_KEYS) {
        KeyItem *k = &e->keys[i];
        if (k->kind == K_HEADING) { snprintf(label, lsize, "%s", T(k->heading)); return; }
        if (k->kind == K_ADD) { snprintf(label, lsize, "%s", T("（追加）")); snprintf(value, vsize, "%s", T("一覧に無いキーの割り当てを足します")); *dim = 1; return; }
        key_label(&k->key, label, lsize);
        const CommandBinding *b = bound(&a->commands, &k->key), *d = reserved(&a->commands, &k->key);
        if (fixed_key(&k->key) && !b) snprintf(value, vsize, "%s", T("FDTP の即時終了（Reserved）"));
        else if (d) reserved_text(d, value, vsize);
        else if (b) {
            config_binding_text(&a->commands, b, value, vsize);
            const char *name = b->shell || b->comment[0] ? NULL : commands_label(b->id);
            if (name && strlen(value) + strlen(name) + 8 < vsize) { strcat(value, T("（")); strcat(value, name); strcat(value, T("）")); }
        } else { snprintf(value, vsize, "%s", T("（空き）")); *dim = 1; }
    } else {
        const char *ext;
        if (config_launch_get(a, (size_t)i, &ext, value, vsize) < 0) {
            snprintf(label, lsize, "%s", T("（追加）")); snprintf(value, vsize, "%s", T("拡張子と起動するものを足します")); *dim = 1;
        } else snprintf(label, lsize, ".%s", ext);
    }
}
/* The line under the list: what the chosen item is. */
static void item_help(Editor *e, App *a, char *out, size_t size)
{
    int i = e->cursor[e->tab];
    *out = 0;
    if (e->tab == TAB_VARIABLES) {
        const char *help = config_variable_help(config_variable_names[i]);
        snprintf(out, size, "%s", help ? help : "");
    } else if (e->tab == TAB_KEYS) {
        KeyItem *k = &e->keys[i];
        char name[40];
        if (k->kind == K_ADD) { snprintf(out, size, T("「キー 割り当て」の形で入れます（例: @B \"ls -l %s\" :一覧）"), "%P"); return; }
        if (k->kind != K_KEY || config_key_name(&k->key, name, sizeof(name)) < 0) return;
        const CommandBinding *b = bound(&a->commands, &k->key), *d = reserved(&a->commands, &k->key);
        if (fixed_key(&k->key) && !b) snprintf(out, size, "%s", T("左の Ctrl+c は FDTP の即時終了で、変えられません"));
        else if (d) snprintf(out, size, T("組み込み: bind %s %s（RET で上書き）"), name, d->id);
        else if (b) snprintf(out, size, T("~/.fdtprc: bind %s …（RET で直す。空で消すと組み込みか空きに戻る）"), name);
        else snprintf(out, size, T("空き。~/.fdtprc では bind %s 命令 か bind %s \"コマンド\""), name, name);
    } else snprintf(out, size, T("内部の表示名（textview pdfview …）か \"コマンド %s\""), "%C");
}
static int name_width(Editor *e, App *a)
{
    int w = e->tab == TAB_KEYS ? 12 : 17;
    if (e->tab == TAB_VARIABLES)
        for (int i = 0; i < CONFIG_VARIABLES; ++i) {
            int width = cells_width(config_variable_names[i]);
            if (width > w) w = width;
        }
    (void)a;
    return w;
}
static void editor_draw(Screen *s, App *a, Canvas *c, Cells *cells)
{
    (void)c; Editor *e = s->self;
    static const char *const tabs[TABS] = {N_("変数"), N_("キー"), N_("ランチャ")};
    static const char *const heads[TABS][2] = {
        {N_("変数"), N_("値")},
        {N_("キー"), N_("割り当て（組み込みは（Reserved）、~/.fdtprc の bind、（空き））")},
        {N_("拡張子"), N_("起動するもの（内部の表示名か \"コマンド\"）")}
    };
    cells_clear(cells);
    clamp(e, a);
    int right = FRAME_RIGHT, inner = FD_COLS - 4;
    /* The screen's frame (cells.h), the title (and 「変更あり」) in its top line. */
    if (frame_open(cells) < 0) return;
    frame_title(cells, FRAME_TITLE_COL, T("FDTP の設定"), 20, config_dirty(a) ? T("変更あり") : NULL, MAGENTA);
    /* Row 1: the headings, then where it is written. */
    int x = 2;
    for (int t = 0; t < TABS; ++t) {
        int on = t == e->tab;
        cells_put(cells, x++, ROW_TABS, ' ', WHITE, on ? CYAN : BG);
        x += cells_text(cells, x, ROW_TABS, T(tabs[t]), on ? BG : WHITE, on ? CYAN : BG, 20);
        cells_put(cells, x++, ROW_TABS, ' ', WHITE, on ? CYAN : BG);
        x += 2;
    }
    char line[600];
    const char *label = config_user_label(a);
    int room = right - 1 - x - 1, cut = 0;
    for (;;) {
        snprintf(line, sizeof(line), T("書き込み先: %s%s"), cut ? "…" : "", *label || cut ? label : T("（なし）"));
        if (!*label || cells_width(line) <= room) break;
        ++label; while ((*label & 0xc0) == 0x80) ++label; cut = 1;
    }
    int lw = cells_width(line);
    if (lw > room) lw = room;
    cells_text(cells, right - 1 - lw, ROW_TABS, line, CYAN, BG, lw);
    /* The columns, the lines between the parts. */
    int nw = name_width(e, a), divider = 2 + nw + 1, value_x = divider + 2, value_width = right - 1 - value_x;
    frame_hline(cells, ROW_TABS + 1);
    frame_hline(cells, ROW_TITLES + 1);
    frame_hline(cells, ROW_RULE);
    cells_text(cells, 2, ROW_TITLES, T(heads[e->tab][0]), CYAN, BG, nw);
    cells_text(cells, value_x, ROW_TITLES, T(heads[e->tab][1]), CYAN, BG, value_width);
    int n = item_count(e, a), top = e->top[e->tab], from = ROW_TABS + 1;
    for (int row = 0; row < LIST_ROWS; ++row) {
        int i = top + row, y = LIST_TOP + row;
        if (i >= n) break;
        char name[64], value[COMMAND_TEXT + 80]; int dim;
        item_text(e, a, i, name, sizeof(name), value, sizeof(value), &dim);
        if (heading_at(e, i)) {
            /* A heading: yellow, in a line across the box. The line between
             * the columns joins it (┼), or stops at a long heading's text. */
            int w = cells_width(name), clear = 3 + w + 1 <= divider || divider < 2;
            if (y - 1 >= from) cells_box_line(cells, 1, divider, from, clear ? y : y - 1);
            from = clear ? y : y + 1;
            w = cells_text(cells, 3, y, name, YELLOW, BG, inner);
            cells_box_line(cells, 0, y, 0, 2);
            cells_box_line(cells, 0, y, 3 + w, right);
            continue;
        }
        int selected = i == e->cursor[e->tab];
        int fg = selected ? BG : WHITE, bg = selected ? WHITE : BG;
        if (selected) for (int cx = 1; cx < right; ++cx) cells_put(cells, cx, y, ' ', fg, bg);
        cells_text(cells, 2, y, name, selected ? fg : YELLOW, bg, nw);
        cells_text(cells, value_x, y, value, selected ? fg : dim ? CYAN : WHITE, bg, value_width);
    }
    if (ROW_RULE >= from) cells_box_line(cells, 1, divider, from, ROW_RULE);
    char help[600];
    item_help(e, a, help, sizeof(help));
    cells_text(cells, 2, ROW_HELP, help, CYAN, BG, inner);
    /* Where the list is, in a gap of the bottom line (the frame's status). */
    if (n > LIST_ROWS) {
        char mark[64]; int last = top + LIST_ROWS < n ? top + LIST_ROWS : n;
        snprintf(mark, sizeof(mark), "↑↓ %d-%d/%d", top + 1, last, n);
        frame_status(cells, FRAME_STATUS_END, mark, inner);
    }
    static const FrameKey keys[] = {{"←→", N_("見出し")}, {"↑↓ PgUp PgDn", N_("項目")}, {"RET", N_("直す（空で取り消し）")}, {"Esc", N_("終わる")}};
    FrameKey translated[4];
    for (int k = 0; k < 4; ++k) { translated[k] = keys[k]; translated[k].name = T(keys[k].name); }
    frame_keys(cells, 2, translated, 4);
}
static void open_input(Editor *e, App *a, const char *label, const char *value)
{
    (void)e;
    widgets_input(&a->widgets, label, value, NULL);
}
static void edit_key(Editor *e, App *a)
{
    char label[160], value[COMMAND_TEXT + 80] = "";
    const CommandBinding *b = bound(&a->commands, &e->key);
    if (b) config_binding_text(&a->commands, b, value, sizeof(value));
    snprintf(label, sizeof(label), T("%s の割り当て（bind %s の右側）"), e->label, e->name);
    e->pending = E_EDIT;
    open_input(e, a, label, value);
}
static void begin_edit(Editor *e, App *a)
{
    char label[96], value[COMMAND_TEXT + 80]; int i = e->cursor[e->tab];
    e->pending = E_EDIT;
    if (e->tab == TAB_VARIABLES) {
        if (i == CONFIG_OFFICIAL) {
            int v = a->settings.localsend_official;
            config_set_variable(a, "LOCALSEND_OFFICIAL", v == 0 ? "fdtm" : v == 1 ? "official" : "ask");
            e->pending = E_NONE;
            return;
        }
        if (i == CONFIG_LANGUAGE) {
            const char *v = a->settings.language;
            config_set_variable(a, "LANGUAGE", !strcmp(v, "auto") ? "ja" : !strcmp(v, "ja") ? "en" : "auto");
            e->pending = E_NONE;
            return;
        }
        snprintf(e->name, sizeof(e->name), "%s", config_variable_names[i]);
        config_variable_text(a, e->name, value, sizeof(value));
        open_input(e, a, e->name, value);
    } else if (e->tab == TAB_KEYS) {
        build_keys(e, &a->commands);
        KeyItem *k = &e->keys[i];
        if (k->kind == K_HEADING) { e->pending = E_NONE; return; }
        if (k->kind == K_ADD) { e->pending = E_ADD; open_input(e, a, T("追加（キー 割り当て）"), ""); return; }
        e->key = k->key;
        key_label(&k->key, e->label, sizeof(e->label));
        if (config_key_name(&k->key, e->name, sizeof(e->name)) < 0) snprintf(e->name, sizeof(e->name), "?");
        if (fixed_key(&k->key) && !bound(&a->commands, &k->key)) {
            e->pending = E_NONE; app_notice(a, T("Ctrl+c は FDTP の即時終了です（変えられません）")); return;
        }
        const CommandBinding *d = reserved(&a->commands, &k->key);
        if (d) {
            char text[64], message[256];
            reserved_text(d, text, sizeof(text));
            snprintf(message, sizeof(message), T("%s は %sです。割り当てを変えますか？"), e->label, text);
            widgets_confirm(&a->widgets, message); e->pending = E_OVERRIDE; return;
        }
        edit_key(e, a);
    } else {
        const char *ext;
        if (config_launch_get(a, (size_t)i, &ext, value, sizeof(value)) < 0) {
            e->pending = E_ADD; open_input(e, a, T("追加（拡張子 起動するもの）"), ""); return;
        }
        snprintf(e->name, sizeof(e->name), "%s", ext);
        snprintf(label, sizeof(label), T(".%s の起動"), ext);
        open_input(e, a, label, value);
    }
}
static void commit(Editor *e, App *a, const char *text)
{
    int r = 0;
    if (e->pending == E_ADD && !*text) return;
    if (e->tab == TAB_VARIABLES) r = config_set_variable(a, e->name, text);
    else if (e->tab == TAB_KEYS) r = e->pending == E_ADD ? config_add_binding(a, text) : config_set_binding(a, &e->key, text);
    else r = e->pending == E_ADD ? config_add_launch(a, text) : config_set_launch(a, e->name, text);
    if (r < 0) app_notice(a, e->tab == TAB_VARIABLES ? T("値が正しくありません") : T("指定が正しくありません"));
}
static int editor_key(Screen *s, App *a, const Key *k)
{
    Editor *e = s->self; Widgets *ui = &a->widgets;
    if ((e->pending == E_QUIT || e->pending == E_OVERRIDE) && ui->kind == WIDGET_CONFIRM) {
        Pending was = e->pending;
        if (k->key == KESC) { ui->kind = WIDGET_NONE; e->pending = E_NONE; return 0; }
        WidgetResult r = widgets_key(ui, k);
        if (r == WIDGET_ACCEPT) {
            e->pending = E_NONE;
            if (was == E_OVERRIDE) { edit_key(e, a); return 0; }
            return config_write(a) == 0;
        }
        if (r == WIDGET_CANCEL) { e->pending = E_NONE; return was == E_QUIT; }
        return 0;
    }
    if ((e->pending == E_EDIT || e->pending == E_ADD) && ui->kind == WIDGET_INPUT) {
        WidgetResult r = widgets_key(ui, k);
        if (r == WIDGET_ACCEPT) { char text[WIDGET_CAP]; snprintf(text, sizeof(text), "%s", ui->text); commit(e, a, text); }
        if (r == WIDGET_ACCEPT || r == WIDGET_CANCEL) e->pending = E_NONE;
        return 0;
    }
    e->pending = E_NONE;
    int n = item_count(e, a), *c = &e->cursor[e->tab], direction = 1;
    switch (k->key) {
    case KLEFT: e->tab = (e->tab + TABS - 1) % TABS; break;
    case KRIGHT: e->tab = (e->tab + 1) % TABS; break;
    case KUP: if (*c) --*c; direction = -1; break;
    case KDOWN: if (*c + 1 < n) ++*c; break;
    case KPGUP: *c = *c > LIST_ROWS ? *c - LIST_ROWS : 0; direction = -1; break;
    case KPGDN: *c = *c + LIST_ROWS < n ? *c + LIST_ROWS : n - 1; break;
    case KHOME: *c = 0; break;
    case KEND: *c = n - 1; direction = -1; break;
    case KRET: begin_edit(e, a); break;
    case KESC: {
        char message[600]; const char *label = config_user_label(a);
        snprintf(message, sizeof(message), T("%s に書き込みますか？"), *label ? label : "~/.fdtprc");
        widgets_confirm(ui, message); e->pending = E_QUIT; break;
    }
    default: break;
    }
    if (k->key != KLEFT && k->key != KRIGHT) skip_heading(e, item_count(e, a), direction);
    clamp(e, a);
    return 0;
}
static void editor_destroy(Screen *s) { free(s->self); free(s); }
int config_edit(App *a, CommandTable *table)
{
    (void)table;
    if (!a->config) { app_notice(a, T("メモリが足りません")); return -1; }
    Screen *s = calloc(1, sizeof(*s)); Editor *e = calloc(1, sizeof(*e));
    if (!s || !e) { free(s); free(e); app_notice(a, T("メモリが足りません")); return -1; }
    s->self = e; s->key = editor_key; s->draw = editor_draw; s->destroy = editor_destroy;
    if (app_push(a, s) < 0) { editor_destroy(s); app_notice(a, T("画面を開けません")); return -1; }
    return 0;
}
