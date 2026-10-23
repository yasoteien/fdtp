// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_I18N_H
#define FDTP_I18N_H

enum { I18N_JA, I18N_EN };

/* Mark a literal in a static table; translate it with T() when displayed. */
#define N_(s) (s)

#if defined(__GNUC__) || defined(__clang__)
#define I18N_FORMAT_ARG __attribute__((format_arg(1)))
#else
#define I18N_FORMAT_ARG
#endif
/* format_arg: printf checks still see T("...%d") as the literal. */
const char *T(const char *ja) I18N_FORMAT_ARG;
void i18n_set(int lang);
int i18n_lang(void);

#endif
