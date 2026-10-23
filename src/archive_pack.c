// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "i18n.h"
/* Part of archive.o (included from archive.c): p, 「書庫の作成」 (revision 1
 * §13, after 卓駆★'s window). A box over the filer with the archive name
 * (with the name history), the format (LHA ZIP TAR TAR.GZ TAR.BZ2 TAR.XZ),
 * the compression level (1-9), the names' code for LHa and what happens to
 * a file of the same name when adding to an existing archive.
 *
 * LHa archives are made here (lha_write.c), in steps from op ticks so that
 * the box shows the progress (files, name, bytes) and Esc can stop it;
 * zip and tar run their commands (zip, tar piped into gzip/bzip2/xz). */

typedef enum { PK_LHA, PK_ZIP, PK_TAR, PK_TGZ, PK_TBZ, PK_TXZ, PK_COUNT } PackKind;
static const struct { const char *label, *ext; } pack_kinds[PK_COUNT] = {
    {"LHA", ".lzh"}, {"ZIP", ".zip"}, {"TAR", ".tar"}, {"TAR.GZ", ".tar.gz"}, {"TAR.BZ2", ".tar.bz2"}, {"TAR.XZ", ".tar.xz"},
};
enum { PF_NAME, PF_FORMAT, PF_LEVEL, PF_CODE, PF_SAME, PF_COUNT };
enum { PB_EDIT, PB_CONFIRM, PB_RUN };
/* Where the box is: columns 4-75, rows 4-20 (shadow to row 21) at 80 x 25; in
 * the middle of a larger screen (WIDE_NOTES.md). */
enum { PB_W = 72, PB_H = 17, PB_IN_W = PB_W - 4 };
#define PB_COL (4 + FD_XCOLS / 2)
#define PB_ROW (4 + FD_XROWS / 2)
#define PB_IN (PB_COL + 2)
#define PB_RIGHT (PB_COL + 18)
#define PB_LIST_ROWS 8
/* Each tick of a LHa job works this long (and at least 64 KB), unless
 * $FDTP_PACK_SLICE gives a fixed number of bytes per tick (tests). */
#define PB_TICK_SECONDS 0.15

/* The choices are kept for the next p of this run. */
static struct { int ready, format, level, utf8, newer_only; } pack_last;

typedef struct {
    Screen screen, *below;
    char *directory, **names; size_t count;
    Widgets name;                 /* the name field: editing and its history */
    int field, format, level, utf8, newer_only;
    int phase, add;
    char *target;                 /* absolute */
    LhaJob *job;
    size_t slice;                 /* $FDTP_PACK_SLICE, 0: by time */
    int external_pending;
} PackBox;

/* ---- names and extensions ------------------------------------------------- */

static int pack_kind_of(const char *name, size_t *suffix)
{
    static const struct { const char *ext; int kind; } known[] = {
        {".tar.gz", PK_TGZ}, {".tgz", PK_TGZ}, {".tar.bz2", PK_TBZ}, {".tbz2", PK_TBZ}, {".tbz", PK_TBZ},
        {".tar.xz", PK_TXZ}, {".txz", PK_TXZ}, {".tar", PK_TAR}, {".zip", PK_ZIP}, {".lzh", PK_LHA}, {".lha", PK_LHA},
    };
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(known) / sizeof(*known); ++i) {
        size_t e = strlen(known[i].ext);
        if (n > e && !strcasecmp(name + n - e, known[i].ext)) { if (suffix) *suffix = e; return known[i].kind; }
    }
    if (suffix) *suffix = 0;
    return -1;
}
static void pack_set_text(PackBox *b, const char *text)
{
    snprintf(b->name.text, sizeof(b->name.text), "%s", text);
    b->name.cursor = strlen(b->name.text); b->name.scroll = 0;
}
/* Choosing a format puts its extension on the name (unless it has one of
 * that format already, like .lha or .tgz). */
static void pack_choose(PackBox *b, int format)
{
    if (format < 0 || format >= PK_COUNT) return;
    b->format = format;
    size_t suffix;
    int kind = pack_kind_of(b->name.text, &suffix);
    if (kind == format) return;
    char text[WIDGET_CAP];
    size_t keep = strlen(b->name.text) - suffix;
    if (!keep) return; /* only an extension: leave it to the user */
    snprintf(text, sizeof(text), "%.*s%s", (int)keep, b->name.text, pack_kinds[format].ext);
    pack_set_text(b, text);
}
static int pack_field_enabled(const PackBox *b, int field)
{
    if (field == PF_LEVEL) return b->format != PK_TAR;
    if (field == PF_CODE) return b->format == PK_LHA;
    return 1;
}
static void pack_next_field(PackBox *b, int step)
{
    for (int i = 0; i < PF_COUNT; ++i) {
        b->field = (b->field + step + PF_COUNT) % PF_COUNT;
        if (pack_field_enabled(b, b->field)) return;
    }
}

/* ---- drawing -------------------------------------------------------------- */

static int pack_text(Cells *c, int col, int row, const char *s, int fg, int bg, int limit)
{ return cells_text(c, col, row, s, fg, bg, limit); }
/* An option: one blank each side; chosen ones look like a button (white)
 * in the focused field, blue elsewhere. */
static int pack_option(Cells *c, int col, int row, const char *s, int chosen, int focused, int enabled, int width)
{
    int fg = enabled ? WHITE : BLUE, bg = BG;
    if (chosen) { bg = !enabled ? BLUE : focused ? WHITE : BLUE; fg = !enabled ? BG : focused ? BG : WHITE; }
    int w = cells_width(s) + 2;
    if (width < w) width = w;
    for (int x = 0; x < width; ++x) cells_put(c, col + x, row, ' ', fg, bg);
    pack_text(c, col + 1, row, s, fg, bg, width - 1);
    return width;
}
static void pack_label(Cells *c, int col, int row, const char *s, int focused, int enabled)
{
    pack_text(c, col, row, s, !enabled ? BLUE : focused ? BG : CYAN, focused && enabled ? CYAN : BG, PB_IN_W);
}
static int pack_history_items(const PackBox *b, const char *out[WIDGET_HISTORY_KEEP])
{
    int n = 0, count = widgets_history_count(WIDGET_HISTORY_NAME);
    size_t length = b->name.edited ? strlen(b->name.text) : 0;
    for (int i = 0; i < count && n < WIDGET_HISTORY_KEEP; ++i) {
        const char *e = widgets_history_entry(WIDGET_HISTORY_NAME, i);
        if (e && !strncmp(e, b->name.text, length)) out[n++] = e;
    }
    return n;
}
static void pack_draw_field(PackBox *b, Cells *c, int row)
{
    Widgets *w = &b->name;
    int focused = b->field == PF_NAME && b->phase != PB_RUN, start = PB_IN + 1, available = PB_IN_W - 2;
    for (int x = PB_IN; x < PB_IN + PB_IN_W; ++x) cells_put(c, x, row, ' ', BG, WHITE);
    const char *p = w->text + w->cursor;
    uint32_t ch = *p ? font24_decode(&p) : ' ';
    int caret = font24_width(ch);
    if (w->scroll > w->cursor) w->scroll = w->cursor;
    /* Keep the caret in view (the same rule as the row-23 field). */
    for (;;) {
        const char *s = w->text + w->scroll; int width = 0;
        while ((size_t)(s - w->text) < w->cursor) width += font24_width(font24_decode(&s));
        if (w->scroll >= w->cursor || width + caret <= available) break;
        const char *next = w->text + w->scroll; font24_decode(&next); w->scroll = (size_t)(next - w->text);
    }
    cells_text(c, start, row, w->text + w->scroll, BG, WHITE, available);
    if (!focused) return;
    char before[WIDGET_CAP];
    size_t n = w->cursor - w->scroll;
    memcpy(before, w->text + w->scroll, n); before[n] = 0;
    int x = start + cells_width(before);
    if (x + font24_width(ch) > start + available) ch = ' ';
    cells_put(c, x, row, ch, WHITE, BG);
}
static void pack_draw_list(PackBox *b, Cells *c)
{
    Widgets *w = &b->name;
    const char *items[WIDGET_HISTORY_KEEP];
    int n = pack_history_items(b, items), rows = n < PB_LIST_ROWS ? (n ? n : 1) : PB_LIST_ROWS;
    if (w->list_selected >= n) w->list_selected = n ? n - 1 : 0;
    if (w->list_top > w->list_selected) w->list_top = w->list_selected;
    if (w->list_selected >= w->list_top + PB_LIST_ROWS) w->list_top = w->list_selected - PB_LIST_ROWS + 1;
    if (w->list_top > n - rows) w->list_top = n - rows < 0 ? 0 : n - rows;
    int col = PB_IN, row = PB_ROW + 4, width = PB_IN_W;
    if (cells_box(c, col, row, width, rows + 2, CELL_BOX_FRAME | CELL_BOX_SHADOW) < 0) return;
    if (!n) { pack_text(c, col + 2, row + 1, w->edited ? T("（一致する履歴はありません）") : T("（履歴はありません）"), CYAN, BG, width - 4); return; }
    for (int i = 0; i < rows; ++i) {
        int at = w->list_top + i, chosen = at == w->list_selected, y = row + 1 + i;
        if (chosen) for (int x = col + 1; x < col + width - 1; ++x) cells_put(c, x, y, ' ', BG, CYAN);
        pack_text(c, col + 2, y, items[at], chosen ? BG : WHITE, chosen ? CYAN : BG, width - 6);
    }
    if (w->list_top > 0) pack_text(c, col + width - 3, row + 1, "↑", CYAN, BG, 2);
    if (w->list_top + rows < n) pack_text(c, col + width - 3, row + rows, "↓", CYAN, BG, 2);
}
static void pack_draw_progress(PackBox *b, Cells *c)
{
    int row = PB_ROW + 14;
    char line[512];
    const char *base = strrchr(b->target, '/');
    if (!b->job) {
        snprintf(line, sizeof(line), T("圧縮中 %s"), base ? base + 1 : b->target);
        pack_text(c, PB_IN, row, line, YELLOW, BG, PB_IN_W);
        return;
    }
    size_t done = lha_job_done(b->job), total = lha_job_total(b->job);
    snprintf(line, sizeof(line), T("圧縮中 %zu / %zu 個  "), done < total ? done + 1 : total, total);
    int x = PB_IN + pack_text(c, PB_IN, row, line, YELLOW, BG, PB_IN_W);
    pack_text(c, x, row, lha_job_current(b->job), WHITE, BG, PB_IN + PB_IN_W - x);
    uint64_t all = lha_job_bytes_total(b->job), now = lha_job_bytes_done(b->job);
    int percent = all ? (int)(now * 100 / all) : 100, filled = percent * 25 / 100;
    for (int i = 0; i < 25; ++i) cells_put(c, PB_IN + 2 * i, row + 1, i < filled ? 0x25a0 : 0x25a1, i < filled ? YELLOW : WHITE, BG);
    snprintf(line, sizeof(line), T("%3d%%  Esc で中止"), percent);
    pack_text(c, PB_IN + 52, row + 1, line, CYAN, BG, PB_IN_W - 52);
}
static void pack_draw(Screen *s, App *a, Canvas *canvas, Cells *c)
{
    PackBox *b = s->self;
    if (b->below && b->below->draw) b->below->draw(b->below, a, canvas, c);
    int box = cells_box(c, PB_COL, PB_ROW, PB_W, PB_H, CELL_BOX_FRAME | CELL_BOX_SHADOW);
    if (box < 0) return;
    c->boxes[box].rule = PB_ROW + 4;
    int edit = b->phase != PB_RUN, f = edit ? b->field : -1;
    pack_text(c, PB_IN, PB_ROW + 1, T("書庫の作成"), YELLOW, BG, 20);
    if (f == PF_NAME) { const char *hint = T("↓ 履歴"); int w = cells_width(hint); pack_text(c, PB_IN + PB_IN_W - w, PB_ROW + 1, hint, CYAN, BG, w); }
    pack_label(c, PB_IN, PB_ROW + 2, T("書庫名"), f == PF_NAME, 1);
    pack_draw_field(b, c, PB_ROW + 3);
    pack_label(c, PB_IN, PB_ROW + 5, T("形式"), f == PF_FORMAT, 1);
    for (int i = 0; i < PK_COUNT; ++i) pack_option(c, PB_IN, PB_ROW + 6 + i, pack_kinds[i].label, i == b->format, f == PF_FORMAT, 1, 11);
    int level_on = pack_field_enabled(b, PF_LEVEL);
    pack_label(c, PB_RIGHT, PB_ROW + 5, T("圧縮率"), f == PF_LEVEL, level_on);
    if (level_on) {
        int x = PB_RIGHT, fg = f == PF_LEVEL ? YELLOW : WHITE;
        x += pack_text(c, x, PB_ROW + 6, T("低 "), CYAN, BG, 3);
        for (int i = 1; i <= 9; ++i) { cells_put(c, x, PB_ROW + 6, i <= b->level ? 0x25a0 : 0x25a1, i <= b->level ? fg : WHITE, BG); x += 2; }
        char n[16]; snprintf(n, sizeof(n), T(" 高  %d"), b->level);
        pack_text(c, x, PB_ROW + 6, n, CYAN, BG, 8);
    } else pack_text(c, PB_RIGHT, PB_ROW + 6, T("（TAR は圧縮しません）"), BLUE, BG, 30);
    int code_on = pack_field_enabled(b, PF_CODE);
    pack_label(c, PB_RIGHT, PB_ROW + 8, T("名前"), f == PF_CODE, code_on);
    int x = PB_RIGHT + 6;
    x += pack_option(c, x, PB_ROW + 8, "SJIS", !b->utf8, f == PF_CODE, code_on, 0) + 1;
    pack_option(c, x, PB_ROW + 8, "UTF-8", b->utf8, f == PF_CODE, code_on, 0);
    if (!code_on) pack_text(c, x + 9, PB_ROW + 8, T("（LHA の時）"), BLUE, BG, 14);
    pack_label(c, PB_RIGHT, PB_ROW + 10, T("同名のファイル（書庫に追加する時）"), f == PF_SAME, 1);
    pack_option(c, PB_RIGHT + 2, PB_ROW + 11, T("無条件で上書き"), !b->newer_only, f == PF_SAME, 1, 20);
    pack_option(c, PB_RIGHT + 2, PB_ROW + 12, T("日付が新しい時だけ"), b->newer_only, f == PF_SAME, 1, 20);
    if (edit) pack_text(c, PB_IN, PB_ROW + 14, T("TAB 欄を移る  ↑↓←→ 選ぶ  RET 作成  Esc 取り消し"), CYAN, BG, PB_IN_W);
    else pack_draw_progress(b, c);
    if (edit && b->name.list_open) pack_draw_list(b, c);
}

/* ---- running -------------------------------------------------------------- */

static void pack_remember(const PackBox *b)
{
    pack_last.ready = 1; pack_last.format = b->format; pack_last.level = b->level;
    pack_last.utf8 = b->utf8; pack_last.newer_only = b->newer_only;
}
static double pack_clock(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static void pack_finish(App *a, int ok, const char *message)
{
    char msg[320];
    if (ok) snprintf(msg, sizeof(msg), "%s", message);
    else snprintf(msg, sizeof(msg), "%s", *message ? message : T("圧縮できません"));
    reload_filers(a);
    app_pop(a);
    if (ok) app_notice(a, msg); else app_error(a, msg);
}
static const char *pack_base(const PackBox *b) { const char *s = strrchr(b->target, '/'); return s ? s + 1 : b->target; }

static struct timespec pack_mtime(const struct stat *s)
{
#ifdef __APPLE__
    return s->st_mtimespec;
#else
    return s->st_mtim;
#endif
}
/* remove_tree for any path (nftw on the Mac will not start at a file). */
static void pack_remove(const char *path)
{
    struct stat st;
    if (!lstat(path, &st) && S_ISDIR(st.st_mode)) remove_tree(path);
    else unlink(path);
}
/* Copy src into dst for a tar rebuild (the old members are already in the
 * staging directory). newer_only: an existing file is replaced only by a
 * newer one. older counts the files not replaced. */
static int pack_copy_into(const char *src, const char *dst, int newer_only, size_t *older, char *msg, size_t msgn)
{
    struct stat s, d;
    if (lstat(src, &s) < 0) { snprintf(msg, msgn, T("ありません: %.100s"), src); return -1; }
    int exists = !lstat(dst, &d);
    if (S_ISDIR(s.st_mode)) {
        if (exists && !S_ISDIR(d.st_mode)) {
            if (newer_only && !(s.st_mtime > d.st_mtime)) { ++*older; return 0; }
            pack_remove(dst); exists = 0;
        }
        if (!exists && mkdir(dst, (s.st_mode & 07777) | 0700) < 0) { snprintf(msg, msgn, "%s", T("作業用のディレクトリを作れません")); return -1; }
        DIR *dir = opendir(src);
        if (!dir) { snprintf(msg, msgn, T("ディレクトリを読めません: %.100s"), src); return -1; }
        int r = 0;
        for (struct dirent *e; !r && (e = readdir(dir));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *a = join2(src, e->d_name), *b = join2(dst, e->d_name);
            r = a && b ? pack_copy_into(a, b, newer_only, older, msg, msgn) : (snprintf(msg, msgn, "%s", T("メモリが足りません")), -1);
            free(a); free(b);
        }
        closedir(dir);
        struct timespec times[2] = {{0, UTIME_OMIT}, pack_mtime(&s)};
        if (!r) utimensat(AT_FDCWD, dst, times, 0);
        return r;
    }
    if (!S_ISREG(s.st_mode) && !S_ISLNK(s.st_mode)) return 0;
    if (exists) {
        if (newer_only && !(s.st_mtime > d.st_mtime)) { ++*older; return 0; }
        pack_remove(dst);
    }
    if (S_ISLNK(s.st_mode)) {
        char target[4096]; ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0) { snprintf(msg, msgn, T("リンクを読めません: %.100s"), src); return -1; }
        target[n] = 0;
        if (symlink(target, dst) < 0) { snprintf(msg, msgn, "%s", T("作業用のファイルを作れません")); return -1; }
        return 0;
    }
    int in = open(src, O_RDONLY), out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL, s.st_mode & 07777);
    int r = in < 0 || out < 0 ? -1 : 0, saved = errno;
    char buf[65536];
    while (!r) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { r = n < 0 ? -1 : 0; break; }
        for (ssize_t done = 0; done < n && !r;) { ssize_t w = write(out, buf + done, (size_t)(n - done)); if (w <= 0) r = -1; else done += w; }
        if (r) saved = errno;
    }
    if (!r) { struct timespec times[2] = {{0, UTIME_OMIT}, pack_mtime(&s)}; futimens(out, times); }
    if (in >= 0) close(in);
    if (out >= 0 && close(out) < 0) r = -1;
    if (r) snprintf(msg, msgn, T("コピーできません: %.100s（%s）"), src, strerror(saved));
    return r;
}

/* tar (piped into the compressor) of names in cwd into a temporary beside
 * the target, then renamed over it. */
static int pack_tar(PackBox *b, const char *cwd, char *const *names, size_t count, char *msg, size_t msgn)
{
    static const char *const tools[PK_COUNT] = {NULL, NULL, NULL, "gzip", "bzip2", "xz"};
    const char *tool = tools[b->format];
    if (tool) { char *p = find_command(tool); if (!p) { snprintf(msg, msgn, T("%s がありません"), tool); return -1; } free(p); }
    size_t tn = strlen(b->target) + 16;
    char *temp = malloc(tn);
    const char **argv = calloc(count + 8, sizeof(*argv));
    char **owned = calloc(count + 1, sizeof(*owned));
    int result = -1, fd = -1;
    if (!temp || !argv || !owned) { snprintf(msg, msgn, "%s", T("メモリが足りません")); goto out; }
    snprintf(temp, tn, "%s.tmpXXXXXX", b->target);
    if ((fd = mkstemp(temp)) < 0) { snprintf(msg, msgn, T("書庫を作れません: %.80s"), strerror(errno)); free(temp); temp = NULL; goto out; }
    mode_t mask = umask(0); umask(mask); fchmod(fd, 0666 & ~mask);
    size_t c = 0;
    argv[c++] = "tar"; argv[c++] = "-cf"; argv[c++] = tool ? "-" : temp;
    for (size_t i = 0; i < count; ++i) {
        /* A name starting with '-' would be read as an option. */
        if (names[i][0] == '-') { if (!(owned[i] = join2(".", names[i]))) { snprintf(msg, msgn, "%s", T("メモリが足りません")); goto out; } argv[c++] = owned[i]; }
        else argv[c++] = names[i];
    }
    RunResult r;
    int started;
    if (tool) {
        char level[8]; snprintf(level, sizeof(level), "-%d", b->format == PK_TXZ && b->level > 6 ? 6 : b->level);
        const char *comp[] = {tool, "-c", level, NULL};
        started = run_pipeline(argv, comp, cwd, fd, &r);
    } else started = run_command(argv, cwd, &r);
    if (started < 0) { snprintf(msg, msgn, errno == ENOENT ? T("%s がありません") : T("%s を起動できません"), "tar"); goto out; }
    if (r.status) { char line[160]; first_line(&r, line, sizeof(line)); snprintf(msg, msgn, T("圧縮できません: %.120s"), line); run_free(&r); goto out; }
    run_free(&r);
    if (fsync(fd) < 0 || close(fd) < 0) { fd = -1; snprintf(msg, msgn, "%s", T("書き込めません")); goto out; }
    fd = -1;
    if (rename(temp, b->target) < 0) { snprintf(msg, msgn, T("書庫を置けません: %.80s"), strerror(errno)); goto out; }
    free(temp); temp = NULL;
    result = 0;
out:
    if (fd >= 0) close(fd);
    if (temp) { unlink(temp); free(temp); }
    for (size_t i = 0; i < count; ++i) free(owned ? owned[i] : NULL);
    free(owned); free(argv);
    return result;
}
/* Adding to a tar: the old members and the new files are put together in a
 * staging directory under the cache, and the archive is made again. */
static int pack_tar_add(PackBox *b, size_t *older, char *msg, size_t msgn)
{
    Archive old;
    if (archive_read(&old, b->target, msg, msgn) < 0) return -1;
    for (size_t i = 0; i < old.count; ++i) {
        const char *bad = member_danger(&old, &old.members[i]);
        if (bad) { snprintf(msg, msgn, T("危険な名前があるので追加しません: %.60s"), bad); archive_free(&old); return -1; }
    }
    char *root = cache_root(), *stage = root ? join2(root, "pack-XXXXXX") : NULL;
    free(root);
    if (!stage || !mkdtemp(stage)) { free(stage); archive_free(&old); snprintf(msg, msgn, "%s", T("作業用のディレクトリを作れません")); return -1; }
    int result = extract_members(&old, NULL, 0, stage, msg, msgn);
    archive_free(&old);
    for (size_t i = 0; !result && i < b->count; ++i) {
        char *src = resolve(b->directory, b->names[i]), *dst = join2(stage, b->names[i]);
        result = src && dst ? pack_copy_into(src, dst, b->newer_only, older, msg, msgn) : (snprintf(msg, msgn, "%s", T("メモリが足りません")), -1);
        free(src); free(dst);
    }
    char **tops = NULL; size_t n = 0, cap = 0;
    if (!result) {
        DIR *dir = opendir(stage);
        if (!dir) { result = -1; snprintf(msg, msgn, "%s", T("作業用のディレクトリを読めません")); }
        else {
            for (struct dirent *e; (e = readdir(dir));) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (n == cap) { cap = cap ? cap * 2 : 16; char **g = realloc(tops, cap * sizeof(*g)); if (!g) { result = -1; break; } tops = g; }
                if (!(tops[n] = strdup(e->d_name))) { result = -1; break; }
                ++n;
            }
            closedir(dir);
            if (result) snprintf(msg, msgn, "%s", T("メモリが足りません"));
        }
    }
    if (!result) result = pack_tar(b, stage, tops, n, msg, msgn);
    for (size_t i = 0; i < n; ++i) free(tops[i]);
    free(tops);
    remove_tree(stage); free(stage);
    return result;
}
static int pack_zip(PackBox *b, char *msg, size_t msgn)
{
    const char **argv = calloc(b->count + 10, sizeof(*argv));
    char **owned = calloc(b->count + 1, sizeof(*owned));
    int result = -1;
    if (!argv || !owned) { snprintf(msg, msgn, "%s", T("メモリが足りません")); goto out; }
    char level[8]; snprintf(level, sizeof(level), "-%d", b->level);
    size_t c = 0;
    argv[c++] = "zip"; argv[c++] = "-r"; argv[c++] = "-q"; argv[c++] = "-y"; argv[c++] = level;
    if (b->add && b->newer_only) argv[c++] = "-u";
    argv[c++] = b->target;
    for (size_t i = 0; i < b->count; ++i) {
        if (b->names[i][0] == '-') { if (!(owned[i] = join2(".", b->names[i]))) { snprintf(msg, msgn, "%s", T("メモリが足りません")); goto out; } argv[c++] = owned[i]; }
        else argv[c++] = b->names[i];
    }
    RunResult r;
    run_locale = "C.UTF-8";
    int started = run_command(argv, b->directory, &r);
    run_locale = "C";
    if (started < 0) { snprintf(msg, msgn, "%s", errno == ENOENT ? T("zip がありません") : T("zip を起動できません")); goto out; }
    /* 12: nothing to do (-u with nothing newer). */
    if (!r.status || (b->add && r.status == 12)) result = 0;
    else { char line[160]; first_line(&r, line, sizeof(line)); snprintf(msg, msgn, T("圧縮できません: %.120s"), line); if (!b->add) unlink(b->target); }
    run_free(&r);
out:
    for (size_t i = 0; i < b->count; ++i) free(owned ? owned[i] : NULL);
    free(owned); free(argv);
    return result;
}
static void pack_run_external(App *a, PackBox *b)
{
    char msg[256] = "";
    size_t older = 0;
    int ok;
    if (b->format == PK_ZIP) ok = !pack_zip(b, msg, sizeof(msg));
    else ok = !(b->add ? pack_tar_add(b, &older, msg, sizeof(msg)) : pack_tar(b, b->directory, b->names, b->count, msg, sizeof(msg)));
    if (ok) {
        if (!b->add) snprintf(msg, sizeof(msg), T("圧縮しました: %.100s"), pack_base(b));
        else if (older) snprintf(msg, sizeof(msg), T("書庫に追加しました: %.100s（古いので入れなかった物 %zu 個）"), pack_base(b), older);
        else snprintf(msg, sizeof(msg), T("書庫に追加しました: %.100s"), pack_base(b));
    }
    pack_finish(a, ok, msg);
}
static void pack_run_lha(App *a, PackBox *b)
{
    char err[256] = "";
    double until = pack_clock() + PB_TICK_SECONDS;
    size_t spent = 0;
    for (;;) {
        uint64_t before = lha_job_bytes_done(b->job);
        int r = lha_job_step(b->job, 65536, err, sizeof(err));
        spent += (size_t)(lha_job_bytes_done(b->job) - before) + 1;
        if (r < 0) { pack_finish(a, 0, err); return; }
        if (r == 0) break;
        if (b->slice ? spent >= b->slice : pack_clock() >= until) { a->changed = 1; return; }
    }
    LhaStats st; lha_job_stats(b->job, &st);
    char msg[320];
    int n = snprintf(msg, sizeof(msg), b->add ? T("書庫に追加しました: %.100s（入れた物 %zu 個") : T("圧縮しました: %.100s（%zu 個"), pack_base(b), st.added);
    if (b->add && st.older) n += snprintf(msg + n, sizeof(msg) - (size_t)n, T("、古いので入れなかった物 %zu 個"), st.older);
    if (st.skipped) n += snprintf(msg + n, sizeof(msg) - (size_t)n, T("、入れられない種類 %zu 個"), st.skipped);
    snprintf(msg + n, sizeof(msg) - (size_t)n, "%s", T("）"));
    pack_finish(a, 1, msg);
}

/* Begin the work: LHa builds the job here (names that cannot be stored are
 * refused before anything is written, and the box stays); the commands run
 * on the next tick. */
static int pack_start(App *a, PackBox *b)
{
    pack_remember(b);
    if (b->format == PK_LHA) {
        char err[256] = "";
        LhaOptions o = {b->level, b->utf8, b->newer_only};
        LhaJob *j = lha_job_create(b->target, b->add, &o, err, sizeof(err));
        int r = j ? 0 : -1;
        for (size_t i = 0; i < b->count && !r; ++i) r = lha_job_add(j, b->directory, b->names[i], err, sizeof(err));
        if (!r) r = lha_job_start(j, err, sizeof(err));
        if (r) { lha_job_free(j); b->phase = PB_EDIT; app_error(a, err); return 0; }
        b->job = j;
    } else b->external_pending = 1;
    b->phase = PB_RUN;
    a->changed = 1;
    return 0;
}
/* RET: check the name; an existing archive is added to after a question. */
static int pack_accept(App *a, PackBox *b)
{
    char text[WIDGET_CAP];
    snprintf(text, sizeof(text), "%s", b->name.text);
    if (!*text) { app_notice(a, T("名前が空です")); return 0; }
    if (ends_with(text, ".7z")) { app_notice(a, T("この環境では使えません")); return 0; }
    int kind = pack_kind_of(text, NULL);
    if (kind < 0) {
        /* No archive extension: the chosen format's is added. */
        size_t n = strlen(text);
        snprintf(text + n, sizeof(text) - n, "%s", pack_kinds[b->format].ext);
        pack_set_text(b, text);
    } else b->format = kind;
    if (text[strlen(text) - 1] == '/') { app_notice(a, T("名前が正しくありません")); return 0; }
    char *p = resolve(b->directory, text);
    if (!p) { app_notice(a, T("メモリが足りません")); return 0; }
    for (size_t i = 0; i < b->count; ++i) {
        char *q = resolve(b->directory, b->names[i]);
        int same = q && !strcmp(p, q); free(q);
        if (same) { free(p); app_notice(a, T("圧縮する物と同じ名前です")); return 0; }
    }
    char *slash = strrchr(p, '/'); struct stat st;
    if (slash) {
        *slash = 0;
        int dir = !stat(*p ? p : "/", &st) && S_ISDIR(st.st_mode);
        *slash = '/';
        if (!dir) { free(p); app_notice(a, T("行先のディレクトリがありません")); return 0; }
    }
    free(b->target); b->target = p;
    /* The answer goes to the name history (as the field accepts it). */
    Key ret = {KRET, 0, 0};
    b->name.list_open = 0;
    widgets_key(&b->name, &ret);
    widgets_input_history(&b->name, T("書庫名"), text, b->directory, WIDGET_HISTORY_NAME);
    if (!lstat(p, &st)) {
        if (S_ISDIR(st.st_mode)) { app_notice(a, T("同じ名前のディレクトリがあります")); return 0; }
        char ask[300];
        snprintf(ask, sizeof(ask), T("%.100s はあります。書庫に追加しますか？"), pack_base(b));
        b->phase = PB_CONFIRM;
        widgets_confirm(&a->widgets, ask);
        return 0;
    }
    b->add = 0;
    return pack_start(a, b);
}

static int pack_key(Screen *s, App *a, const Key *k)
{
    PackBox *b = s->self;
    if (b->phase == PB_RUN) {
        if (k->key == KESC) {
            /* Only the LHa job can stop; the commands run within one tick. */
            if (b->job) { lha_job_free(b->job); b->job = NULL; app_notice(a, T("圧縮を中止しました")); return 1; }
        }
        return 0;
    }
    if (b->phase == PB_CONFIRM) {
        WidgetResult r = widgets_key(&a->widgets, k);
        if (r == WIDGET_ACCEPT) { b->add = 1; return pack_start(a, b); }
        if (r == WIDGET_CANCEL) b->phase = PB_EDIT;
        return 0;
    }
    if (k->key == KTAB) { b->name.list_open = 0; pack_next_field(b, k->mods & MOD_SHIFT ? -1 : 1); return 0; }
    int list = b->field == PF_NAME && b->name.list_open;
    if (!list && k->key == KESC) { pack_remember(b); app_notice(a, T("操作を中止しました")); return 1; }
    if (!list && k->key == KRET) return pack_accept(a, b);
    int plain = !(k->mods & (MOD_CTRL | MOD_ALT));
    switch (b->field) {
    case PF_NAME: {
        char before[WIDGET_CAP];
        snprintf(before, sizeof(before), "%s", b->name.text);
        widgets_key(&b->name, k);
        if (strcmp(before, b->name.text)) { int kind = pack_kind_of(b->name.text, NULL); if (kind >= 0) b->format = kind; }
        if (!pack_field_enabled(b, b->field)) b->field = PF_NAME;
        return 0;
    }
    case PF_FORMAT:
        if (k->key == KUP && b->format > 0) pack_choose(b, b->format - 1);
        else if (k->key == KDOWN && b->format < PK_COUNT - 1) pack_choose(b, b->format + 1);
        else if (k->key == KHOME) pack_choose(b, 0);
        else if (k->key == KEND) pack_choose(b, PK_COUNT - 1);
        return 0;
    case PF_LEVEL:
        if ((k->key == KLEFT || k->key == KDOWN) && b->level > 1) --b->level;
        else if ((k->key == KRIGHT || k->key == KUP) && b->level < 9) ++b->level;
        else if (k->key == KCHAR && plain && k->ch >= '1' && k->ch <= '9') b->level = (int)(k->ch - '0');
        return 0;
    case PF_CODE:
        if (k->key == KLEFT || k->key == KRIGHT || k->key == KUP || k->key == KDOWN) b->utf8 = !b->utf8;
        return 0;
    case PF_SAME:
        if (k->key == KLEFT || k->key == KRIGHT || k->key == KUP || k->key == KDOWN) b->newer_only = !b->newer_only;
        return 0;
    }
    return 0;
}
static void pack_tick(Screen *s, App *a, double now)
{
    (void)now;
    PackBox *b = s->self;
    if (b->phase != PB_RUN) return;
    if (b->job) pack_run_lha(a, b);
    else if (b->external_pending) { b->external_pending = 0; pack_run_external(a, b); }
}
static void pack_destroy(Screen *s)
{
    PackBox *b = s->self;
    lha_job_free(b->job);
    for (size_t i = 0; i < b->count; ++i) free(b->names[i]);
    free(b->names); free(b->directory); free(b->target); free(b);
}

/* The first name: the cursor's (a file without its extension), or with
 * marks the current directory's name; then the format's extension. */
static void pack_initial(PackBox *b, const FileSelection *sel, int marked, char *out, size_t n)
{
    char base[512] = "";
    if (marked) {
        const char *d = sel->directory, *end = d + strlen(d);
        while (end > d + 1 && end[-1] == '/') --end;
        const char *start = end;
        while (start > d && start[-1] != '/') --start;
        snprintf(base, sizeof(base), "%.*s", (int)(end - start), start);
        if (!*base || !strcmp(base, "/")) snprintf(base, sizeof(base), "root");
    } else if (sel->current) {
        snprintf(base, sizeof(base), "%.400s", sel->current);
        char *path = resolve(sel->directory, sel->current);
        struct stat st;
        int dir = path && !stat(path, &st) && S_ISDIR(st.st_mode);
        free(path);
        char *dot = strrchr(base, '.');
        if (!dir && dot && dot != base) *dot = 0;
    }
    snprintf(out, n, "%s%s", base, pack_kinds[b->format].ext);
}

static int pack_open(App *a, const FileSelection *sel)
{
    PackBox *b = calloc(1, sizeof(*b));
    if (!b) { app_notice(a, T("メモリが足りません")); return -1; }
    b->screen = (Screen){.self = b, .key = pack_key, .draw = pack_draw, .tick = pack_tick, .destroy = pack_destroy};
    b->below = app_top(a);
    int marked = filer_marked(b->below);
    b->directory = strdup(sel->directory);
    b->names = calloc(sel->count ? sel->count : 1, sizeof(*b->names));
    if (!b->directory || !b->names) goto fail;
    for (size_t i = 0; i < sel->count; ++i) { if (!(b->names[i] = strdup(sel->names[i]))) goto fail; b->count = i + 1; }
    b->format = PK_TGZ; b->level = 6;
    if (pack_last.ready) { b->format = pack_last.format; b->level = pack_last.level; b->utf8 = pack_last.utf8; b->newer_only = pack_last.newer_only; }
    const char *slice = getenv("FDTP_PACK_SLICE");
    b->slice = slice && *slice ? (size_t)strtoull(slice, NULL, 10) : 0;
    char initial[600];
    pack_initial(b, sel, marked, initial, sizeof(initial));
    widgets_init(&b->name);
    widgets_input_history(&b->name, T("書庫名"), initial, b->directory, WIDGET_HISTORY_NAME);
    if (app_push(a, &b->screen) < 0) goto fail;
    return 0;
fail:
    pack_destroy(&b->screen);
    app_notice(a, T("メモリが足りません"));
    return -1;
}
