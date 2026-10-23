// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DIRLIST_H
#define FDTP_DIRLIST_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
typedef struct {
    char *name;
    char display[64]; /* UTF-8, at most 13 cells (8.4) */
    uint64_t size;
    time_t mtime;
    int is_dir, hidden, readonly, special, archive;
    unsigned long mode;
    size_t ordinal;
    int is_link, marked;
    char *link_target;
} DirEntry;
typedef struct {
    char *path; /* absolute canonical directory */
    DirEntry *entries;
    size_t count, file_count;
    uint64_t bytes, total, used, free_bytes;
} DirList;
/* On failure leaves the previous list intact; errors reported through errno. */
int dirlist_read(DirList *list, const char *path);
void dirlist_free(DirList *list);
void dirlist_display_name(const char *name, char out[64]);
#endif
