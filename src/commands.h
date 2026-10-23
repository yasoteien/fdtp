// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_COMMANDS_H
#define FDTP_COMMANDS_H
#include "keys.h"
#include "widgets.h"
#define COMMAND_BINDINGS 192
#define COMMAND_KEYMAPS 64
#define COMMAND_SHELLS 64
#define COMMAND_TEXT 512
#define COMMAND_COMMENT 64
/* id/dir_id are static identifiers. dir_id (FDclone's second command) runs
 * instead of id when the cursor is on a directory; NULL when absent.
 * shell is 1 + an index into shells[] for `bind KEY "command"` (id is then
 * COMMAND_SHELL_ID), 0 for internal commands. comment is the `:comment`. */
typedef struct { Key key; const char *id; const char *dir_id; int shell; char comment[COMMAND_COMMENT]; } CommandBinding;
typedef struct { Key from, to; } CommandKeymap;
typedef struct { char text[COMMAND_TEXT]; int used; } CommandShell;
typedef struct CommandTable {
    CommandBinding bindings[COMMAND_BINDINGS]; size_t count;
    CommandKeymap keymaps[COMMAND_KEYMAPS]; size_t map_count;
    CommandShell shells[COMMAND_SHELLS];
} CommandTable;
/* Pseudo identifier returned by commands_lookup() for shell bindings; it is
 * not a known command, so filer_command() ignores it. */
extern const char COMMAND_SHELL_ID[];
void commands_init(CommandTable *table);
/* command identifiers have static lifetime; unknown ids rejected.
 * Rebinding a key replaces its whole entry (second command, shell, comment). */
int commands_bind(CommandTable *table, const Key *key, const char *id);
/* dir_id and comment may be NULL. Returns 0, or -1 (unknown id/full table). */
int commands_bind_full(CommandTable *table, const Key *key, const char *id, const char *dir_id, const char *comment);
/* command is copied (at most COMMAND_TEXT-1 bytes, else -1). */
int commands_bind_shell(CommandTable *table, const Key *key, const char *command, const char *comment);
/* Removes the key's entry. Returns 0, or -1 when the key was not bound. */
int commands_unbind(CommandTable *table, const Key *key);
int commands_keymap(CommandTable *table, const Key *from, const Key *to);
/* Keymap applied first; NULL when unbound. */
const CommandBinding *commands_find(const CommandTable *table, const Key *key);
/* The shell command of a binding, or NULL for an internal command. */
const char *commands_shell(const CommandTable *table, const CommandBinding *binding);
const char *commands_lookup(const CommandTable *table, const Key *key);
int commands_known(const char *id);
/* The static identifier equal to name, or NULL. */
const char *commands_id(const char *name);
/* index-th identifier of the table of definitions (each once), NULL past it. */
const char *commands_name(size_t index);
/* The command's name in Japanese for the customizer, NULL if unknown. */
const char *commands_label(const char *id);
/* The table as commands_init makes it (the built-in keys; item 27). */
const CommandTable *commands_defaults(void);
/* The help box's rows (widgets_help; revision 1 item 25). */
const WidgetHelpItem *commands_help(int *count);
#endif
