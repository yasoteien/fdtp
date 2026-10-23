// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _XOPEN_SOURCE 700
#include "dirlist.h"
#include "font24.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Re-encode decoded characters so malformed filename bytes become '?'. */
static size_t encode(uint32_t code, char *out)
{
    if (code < 0x80) { out[0] = (char)code; return 1; }
    if (code < 0x800) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (code >> 18));
    out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
    out[3] = (char)(0x80 | (code & 0x3f));
    return 4;
}

static int clip(const char *start, const char *end, int limit, char **out)
{
    int cells = 0;
    while (start < end && *start) {
        const char *next = start;
        uint32_t code = font24_decode(&next);
        int width = font24_width(code);
        if (next > end || cells + width > limit) break;
        *out += encode(code, *out);
        cells += width;
        start = next;
    }
    return cells;
}

void dirlist_display_name(const char *name, char out[64])
{
    const char *end = name + strlen(name);
    const char *dot = strrchr(name, '.');
    char *dest = out;
    if (dot && dot != name && dot[1] && strcmp(name, "..")) {
        int cells = clip(name, dot, 8, &dest);
        while (cells++ < 8) *dest++ = ' ';
        *dest++ = '.';
        /* Up to 4 cells of extension (docx, pptx, txt~); a longer one shows
         * its first 3 and '~'. */
        char *ext = dest;
        if (clip(dot + 1, end, 5, &dest) > 4) { dest = ext; clip(dot + 1, end, 3, &dest); *dest++ = '~'; }
    } else {
        clip(name, end, 13, &dest);
    }
    *dest = '\0';
}

void dirlist_free(DirList *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) { free(list->entries[i].name); free(list->entries[i].link_target); }
    free(list->entries);
    free(list->path);
    memset(list, 0, sizeof(*list));
}

static int compare_entries(const void *left, const void *right)
{
    const DirEntry *a = left, *b = right;
    int a_parent = strcmp(a->name, "..") == 0;
    int b_parent = strcmp(b->name, "..") == 0;
    if (a_parent != b_parent) return b_parent - a_parent;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return strcmp(a->name, b->name);
}

static uint64_t product(uint64_t a, uint64_t b)
{
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

int dirlist_read(DirList *list, const char *path)
{
    DirList next = {0};
    DIR *dir = NULL;
    size_t capacity = 0;
    struct statvfs fs;
    if (!list || !path) { errno = EINVAL; return -1; }
    next.path = realpath(path, NULL);
    if (!next.path) goto fail;
    dir = opendir(next.path);
    if (!dir) goto fail;
    if (statvfs(next.path, &fs) < 0) goto fail;
    uint64_t block_size = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    next.total = product(fs.f_blocks, block_size);
    next.used = product(fs.f_blocks >= fs.f_bfree ? fs.f_blocks - fs.f_bfree : 0,
                        block_size);
    next.free_bytes = product(fs.f_bavail, block_size);
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno) goto fail;
            break;
        }
        int parent = strcmp(de->d_name, "..") == 0;
        if (!strcmp(de->d_name, ".") || (parent && !strcmp(next.path, "/")))
            continue;
        size_t path_len = strlen(next.path), name_len = strlen(de->d_name);
        if (path_len > SIZE_MAX - name_len - 2) { errno = ENOMEM; goto fail; }
        char *full = malloc(path_len + name_len + 2);
        if (!full) goto fail;
        memcpy(full, next.path, path_len);
        full[path_len] = '/';
        memcpy(full + path_len + 1, de->d_name, name_len + 1);
        struct stat st;
        int broken = 0;
        if (stat(full, &st) < 0) {
            int stat_error = errno;
            if (lstat(full, &st) < 0 || !S_ISLNK(st.st_mode)) {
                free(full);
                errno = stat_error;
                goto fail;
            }
            broken = 1;
        }
        struct stat lst;
        int is_link=lstat(full,&lst)==0 && S_ISLNK(lst.st_mode);
        char target[4096];
        ssize_t target_len=is_link ? readlink(full,target,sizeof(target)-1) : -1;
        if(target_len>=0) target[target_len]=0;
        free(full);
        if (next.count == capacity) {
            size_t grown = capacity ? capacity * 2 : 32;
            if (grown < capacity || grown > SIZE_MAX / sizeof(DirEntry)) {
                errno = ENOMEM;
                goto fail;
            }
            DirEntry *entries = realloc(next.entries, grown * sizeof(*entries));
            if (!entries) goto fail;
            next.entries = entries;
            capacity = grown;
        }
        DirEntry *entry = &next.entries[next.count];
        memset(entry, 0, sizeof(*entry));
        entry->name = strdup(de->d_name);
        if (!entry->name) goto fail;
        entry->ordinal=next.count;
        ++next.count;
        entry->mode=(unsigned long)st.st_mode;
        entry->is_link=is_link;
        if(target_len>=0 && !(entry->link_target=strdup(target))) goto fail;
        entry->size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
        entry->mtime = st.st_mtime;
        entry->is_dir = S_ISDIR(st.st_mode);
        entry->hidden = de->d_name[0] == '.' && strcmp(de->d_name,"..") != 0;
        entry->readonly = !(st.st_mode & S_IWUSR);
        entry->special = broken || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode));
        entry->archive = !broken && S_ISREG(st.st_mode);
        dirlist_display_name(entry->name, entry->display);
        if (!parent && !entry->is_dir) {
            ++next.file_count;
            next.bytes = UINT64_MAX - next.bytes < entry->size
                ? UINT64_MAX : next.bytes + entry->size;
        }
    }
    if (closedir(dir) < 0) { dir = NULL; goto fail; }
    dir = NULL;
    if (next.count > 1)
        qsort(next.entries, next.count, sizeof(*next.entries), compare_entries);
    dirlist_free(list);
    *list = next;
    return 0;

fail: {
        int error = errno;
        if (dir) closedir(dir);
        dirlist_free(&next);
        errno = error;
        return -1;
    }
}
