// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_CONFIG_STATE_H
#define FDTP_CONFIG_STATE_H
/* Shared between config.c (files) and config_edit.c (the customizer). */
#include "config.h"
#include "commands.h"
#include "novel_format.h"
/* EDITOR ... FILETYPE, NOVEL_WORD_RUBY, then NOVEL_FORMAT_<NAME> (novel_format.h),
 * then LANGUAGE, SCREENSIZE (WIDE_NOTES.md), AWAKE_MINUTES (revision 1 item 28),
 * SPLIT_SIZE (item 37) and UNDO_LEVELS (item 38). LANGUAGE precedes the
 * screen/awake/split/undo group to preserve its END-relative navigation. */
#define CONFIG_BASE_VARIABLES 9
#define CONFIG_LANGUAGE (CONFIG_BASE_VARIABLES + NOVEL_FORMAT_COUNT)
#define CONFIG_SCREENSIZE (CONFIG_LANGUAGE + 1)
#define CONFIG_AWAKE (CONFIG_SCREENSIZE + 1)
#define CONFIG_SPLIT (CONFIG_AWAKE + 1)
#define CONFIG_UNDO (CONFIG_SPLIT + 1)
#define CONFIG_VARIABLES (CONFIG_UNDO + 1)
/* AWAKE_MINUTES: 0 .. this. */
#define CONFIG_AWAKE_MAX 1440
/* UNDO_LEVELS: 1 .. this. */
#define CONFIG_UNDO_MAX 1000
extern const char *const config_variable_names[CONFIG_VARIABLES];
/* One-line explanation of a variable for the customizer, NULL if none. */
const char *config_variable_help(const char *name);
/* Current value as text ("" when not set). */
void config_variable_text(App *app, const char *name, char *out, size_t size);
/* Changes the user file's setting. value NULL or "" removes the user lines
 * (back to the system file / default). Re-applies everything.
 * Returns 0, or -1 for an unknown name or an invalid value. */
int config_set_variable(App *app, const char *name, const char *value);
/* rhs is the rest of a bind line; NULL or "" removes the user lines for key. */
int config_set_binding(App *app, const Key *key, const char *rhs);
/* line is "KEY rhs". */
int config_add_binding(App *app, const char *line);
/* The rest of a bind line for the table's entry (ID [ID2] [:comment] or
 * "command" [:comment]). */
void config_binding_text(const CommandTable *table, const CommandBinding *binding, char *out, size_t size);
/* Launchers from all the files, later ones overriding the same extension. */
size_t config_launch_count(App *app);
/* ext without its dot; target written as in a launch line. 0, or -1. */
int config_launch_get(App *app, size_t index, const char **ext, char *target, size_t size);
/* target as written in a launch line; NULL or "" removes the user lines. */
int config_set_launch(App *app, const char *ext, const char *target);
/* line is "EXT target". */
int config_add_launch(App *app, const char *line);
/* The user file for messages: $HOME shortened to ~. "" if there is none. */
const char *config_user_label(App *app);
/* Writes the user file (old one kept as FILE.bak). 0, or -1 after a notice. */
int config_write(App *app);
/* Nonzero when the customizer changed something since the last write. */
int config_dirty(App *app);
#endif
