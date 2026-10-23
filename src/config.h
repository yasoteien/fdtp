// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_CONFIG_H
#define FDTP_CONFIG_H
#include <stddef.h>
#include "screen.h"
#include "keys.h"
#include "filer.h"
/* commands.h owns this mutable table; config does not duplicate its layout. */
typedef struct CommandTable CommandTable;
/* Stage 1c: ~/.fdtprc (FDclone's .fd2rc form) and the customizer (z / E).
 *
 * Understood lines: `#` comments, `bind`, `set NAME=value` / `set NAME value`
 * / `NAME=value`, `launch`. Everything else (alias, keymap, arch, export,
 * if/for/while/case blocks and their bodies, functions, lines with ; | & < >
 * ( ) outside quotes, `$` in a value, unknown variables, invalid binds) is
 * skipped and counted; the customizer keeps those lines as comments.
 * Later lines override earlier ones; the user file overrides the system file.
 * All strings are borrowed during calls. */

/* Called by app_init(): reads $FDTP_RC_SYSTEM (default /etc/fdtprc), then
 * $FDTP_RC_USER (default $HOME/.fdtprc; the file the customizer writes).
 * Missing files are not errors. Posts "N 行を読み飛ばしました" when lines were
 * skipped. Returns the number of skipped lines, -1 when out of memory. */
int config_startup(App *app);
/* Interactive first-run language picker. Headless calls are suppressed except
 * with FDTP_FIRST_RUN_TEST=1; FDTP_TEST_KEYS / FDTP_LANGUAGE always suppress it. */
void config_first_run(App *app, int headless);
/* Frees what config_startup/config_load keep. Called by app_destroy(). */
void config_free(App *app);
/* Reads one more file as a read-only layer above those already read (it is
 * kept, re-applied after customizer changes, never written). Posts the
 * skipped-lines notice. Returns 0, or -1 if the file cannot be read.
 * table must be &app->commands for the layer to survive re-application. */
int config_load(App *app, CommandTable *table, const char *path);
/* One `bind` without recording it: key in the notation below, command is the
 * rest of a bind line (`ID [ID2] [:comment]` or `"shell command" [:comment]`).
 * Lost when the customizer re-applies the files. Returns 0 or -1. */
int config_bind(App *app, CommandTable *table, const char *key, const char *command);
/* Key-to-key translation for the API (commands_keymap). The rc `keymap` line
 * (terminal escape sequences) is meaningless with evdev and is skipped. */
int config_keymap(App *app, CommandTable *table, const char *from, const char *to);
/* Pushes the customizer screen. Returns 0, -1 on failure (notice posted). */
int config_edit(App *app, CommandTable *table);

/* Keys: one character (`c` `C` `¥`), `^C` (Ctrl, also `^Space` `^[` = Esc,
 * `^I` = Tab, `^M` = Return, `^H` = Bs), `@C` (Alt+c; the letter's case is
 * ignored), names (case-insensitive) Up Down Left Right PgUp/PageUp
 * PgDn/PageDown Home End Ins Del Bs Esc Ret/Enter Tab Space/Spc F1..F12, and
 * the script notation of keys.h (`C-s` `M-e` `S-UP`). Returns 1 or 0. */
int config_key_parse(const char *text, Key *out);
/* The key in the notation above (the form written to ~/.fdtprc); quoted
 * when it would otherwise read as a comment or a quote. Returns 0 or -1. */
int config_key_name(const Key *key, char *out, size_t size);

/* Macros of bind/launch commands: %C the cursor's name, %P the current
 * directory, %M the marked names separated by spaces (the cursor's name when
 * nothing is marked: FileSelection.names), %X the cursor's name without its
 * extension, %% a percent sign. Each name is single-quoted for /bin/sh when it
 * holds characters other than [A-Za-z0-9_./+,:=@^-]. Other % sequences stay
 * as they are. Returns a malloc'd string (caller frees), NULL without memory.
 * selection may be NULL (all names empty). */
char *config_expand_macros(const char *command, const FileSelection *selection);
/* Expands command against the filer's selection and requests
 * /bin/sh -c COMMAND in the current directory, with a terminal and a key
 * wait afterwards. Returns 0 if requested, -1 after a notice. */
int config_run_shell(App *app, Screen *filer, const char *command);
#endif
