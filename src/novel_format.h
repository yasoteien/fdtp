// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_NOVEL_FORMAT_H
#define FDTP_NOVEL_FORMAT_H
/* Publisher format and the Web/manuscript forms of the novel editor
 * (docs/FDTP_REVISION1.md 4.1, 4.2).
 *
 * The rules, one switch each (~/.fdtprc `set NOVEL_FORMAT_<NAME>=0/1`, the
 * customizer). This table is the only list of them: config.c builds its
 * variables from it. X(NAME, default, help).
 *   1-5   the user's scripts (han2zen.py, syosetu_format.pl)
 *   6-12  added on 2026-10-22 (the user, then the main session)
 * Order of application to one line (no rule undoes another):
 *   ZENKAKU, then a scene-break line keeps everything else as written;
 *   HALFSPACE; then outside the text in 《》 of a ruby notation (and of an
 *   emphasis 《《》》): LEADER (・・・, as …… with LEADER2), EXCLAIM and its
 *   exceptions EXCLAIM_KEEP and SPACES in syosetu_format.pl's own order;
 *   LEADER_EVEN; DASH; PERIOD; and last INDENT.
 * The Kakuyomu export header (【タイトル】 to 【本文（N行）】) gets the
 * original scripts' rules 1-5 only. */
#include <stddef.h>
#include "i18n.h"
#include "novel_doc.h"

/* help: one short line (the customizer shows 「NOVEL_FORMAT_NAME: 1 = help」
 * in 76 columns). */
#define NOVEL_FORMAT_RULES(X) \
    X(ZENKAKU,      1, N_("半角の英数字・記号を全角に")) \
    X(LEADER,       1, N_("・・・を三点リーダに")) \
    X(EXCLAIM,      1, N_("！？の後に全角の空白")) \
    X(EXCLAIM_KEEP, 1, N_("行末・！！ ！？・」』）の前は空白なし")) \
    X(SPACES,       1, N_("全角の空白の 2 つ続きを 1 つに")) \
    X(LEADER2,      1, N_("・・・を……に（0 = … 1 つ）")) \
    X(HALFSPACE,    1, N_("半角の空白を抜く")) \
    X(INDENT,       1, N_("段落の頭を字下げ（「『（ の行は除く）")) \
    X(PERIOD,       1, N_("。」 。』 の 。 を取る")) \
    X(DASH,         1, N_("ダッシュを――に（かなの後のーは長音）")) \
    X(LEADER_EVEN,  1, N_("三点リーダを偶数個に")) \
    X(SCENE,        1, N_("記号だけの行は区切り（前後に空行）")) \
    X(BLANK,        1, N_("Web 形式の空行を除く（原稿形式に）"))

enum {
#define NF_ENUM(name, on, help) NF_##name,
    NOVEL_FORMAT_RULES(NF_ENUM)
#undef NF_ENUM
    NOVEL_FORMAT_COUNT
};
/* Names without the NOVEL_FORMAT_ prefix, defaults and help, by NF_*. */
extern const char *const novel_format_names[NOVEL_FORMAT_COUNT];
extern const int novel_format_defaults[NOVEL_FORMAT_COUNT];

/* What was changed, one entry per message of syosetu_format.pl's
 * history.log: text follows 「N行目」 (for example "の！を！　に修正"). */
typedef struct { size_t line; char *text; } NFMessage;
typedef struct { NFMessage *items; size_t count, cap; int failed; } NFLog;
void nf_log_free(NFLog *log);

/* A scene break: after removing spaces, 1 to 10 characters, none of them
 * kana, kanji, letters or digits, brackets, 。、，．！？ or leaders. */
int novel_scene_line(const char *s, size_t n);
/* A line of spaces only (half, full width, tabs) or empty. */
int novel_blank_line(const char *s, size_t n);
/* The Kakuyomu export header: the index of the first body line (the one
 * after 【本文（N行）】, N in ASCII or full-width digits), 0 when the text
 * does not begin with 【タイトル】. *title (may be NULL) gets the line of the
 * title (1). */
size_t novel_header(const NovelDoc *d, size_t *title);

/* One line (UTF-8, no line end) with rules[NOVEL_FORMAT_COUNT] (0/1);
 * header: a line of the Kakuyomu header. Messages go to log with lineno.
 * Returns a malloc'd line (*outlen bytes), NULL when out of memory. */
char *novel_format_line(const char *s, size_t n, const int *rules, int header, size_t lineno,
                        NFLog *log, size_t *outlen);
/* The three commands on a document, each as one undo group (C-z restores
 * it all). They return the number of lines changed (0: nothing to do), or
 * -1 when out of memory (the document is then as before). *cursor_line (may
 * be NULL) is moved to where that line went. The line end of the file is
 * kept (the document holds lines; novel_doc_save writes its line end). */
long novel_format_doc(NovelDoc *d, const int *rules, NFLog *log, size_t *cursor_line);
/* Manuscript: a single blank line goes, two or more become one; with
 * scene, a scene-break line keeps (or gets) one blank line on each side. */
long novel_form_manuscript(NovelDoc *d, int scene, NFLog *log, size_t *cursor_line);
/* Web: a blank line after each non-blank line followed by a non-blank one. */
long novel_form_web(NovelDoc *d, NFLog *log, size_t *cursor_line);

/* Appends to the log file in history.log's form:
 *   =====...  HOST : h  command  (blank)  Executed time is MM/DD/YY HH:MM:SS
 *   [OSTYPE : ...] USER : u  PWD : dir  Change TXT : name  messages  =====...
 * file is the path of the text (PWD is its directory). 0, or -1. */
int novel_format_log_append(const char *log_path, const char *command, const char *file, const NFLog *log);
#endif
