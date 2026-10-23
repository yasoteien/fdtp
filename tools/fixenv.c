// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* For tools/pixel_compare.py on the Mac (DYLD_INSERT_LIBRARIES): two runs of
 * the checks at different times show different disk usage and the times of
 * files the checks made. This library makes both the same: statvfs/statfs
 * report fixed numbers, and localtime (the list's dates; not localtime_r,
 * which the attribute editor's round trips need) shows any time after
 * mid-2025 as 2025-10-09 08:53 UTC.
 *   cc -dynamiclib -o build/fixenv.dylib tools/fixenv.c */
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <time.h>

#define AFTER 1750000000
#define SHOWN 1760000000

static void fix_vfs(struct statvfs *s)
{
    s->f_bsize = s->f_frsize = 4096;
    s->f_blocks = 1000000; s->f_bfree = 400000; s->f_bavail = 300000;
}
static int my_statvfs(const char *path, struct statvfs *s)
{
    int r = statvfs(path, s);
    if (!r) fix_vfs(s);
    return r;
}
static int my_fstatvfs(int fd, struct statvfs *s)
{
    int r = fstatvfs(fd, s);
    if (!r) fix_vfs(s);
    return r;
}
static int my_statfs(const char *path, struct statfs *s)
{
    int r = statfs(path, s);
    if (!r) { s->f_bsize = 4096; s->f_blocks = 1000000; s->f_bfree = 400000; s->f_bavail = 300000; }
    return r;
}
static struct tm *my_localtime(const time_t *t)
{
    time_t v = t && *t > AFTER ? SHOWN : t ? *t : 0;
    return localtime(&v);
}

#define INTERPOSE(mine, theirs) \
    __attribute__((used)) static struct { const void *a, *b; } interpose_##theirs \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)mine, (const void *)theirs }
INTERPOSE(my_statvfs, statvfs);
INTERPOSE(my_fstatvfs, fstatvfs);
INTERPOSE(my_statfs, statfs);
INTERPOSE(my_localtime, localtime);
