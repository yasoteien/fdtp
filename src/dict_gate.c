// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* The stock dictionary gate. Paths can be replaced only at compile time
 * for synthetic tests; a released binary has no runtime path setting. */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "dict_gate.h"
#if defined(__linux__) || defined(DICT_GATE_TEST_ROOT)
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef DICT_GATE_TEST_ROOT
#define DICT_GATE_TEST_ROOT ""
#endif
#define ROOT DICT_GATE_TEST_ROOT

static int read_value(const char *path, const char *want)
{
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    if (ok) {
        buf[strcspn(buf, "\n")] = 0;
        ok = strcmp(buf, want) == 0 && fgetc(f) == EOF && !ferror(f);
    }
    fclose(f);
    return ok;
}

static int option(const char *list, const char *want)
{
    size_t n = strlen(want);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && memcmp(p, want, n) == 0) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static int mount_ok(void)
{
    FILE *f = fopen(ROOT "/proc/self/mountinfo", "r");
    char *line = NULL;
    size_t cap = 0;
    int matches = 0, ok = 0;
    if (!f) return 0;
    while (getline(&line, &cap, f) >= 0) {
        char *save, *fields[6], *s = line;
        int i;
        for (i = 0; i < 6; ++i) {
            fields[i] = strtok_r(s, " \n", &save); s = NULL;
            if (!fields[i]) break;
        }
        if (i != 6 || strcmp(fields[4], "/mnt/pomera-dict") != 0) continue;
        ++matches;
        ok = 0;
        char *t;
        while ((t = strtok_r(NULL, " \n", &save)) && strcmp(t, "-") != 0) {}
        char *fs = strtok_r(NULL, " \n", &save);
        char *source = strtok_r(NULL, " \n", &save);
        char *opts = strtok_r(NULL, " \n", &save);
        if (!t || !fs || !source || !opts || strcmp(fs, "ext4") ||
            strcmp(fields[3], "/") || !option(fields[5], "ro") ||
            option(fields[5], "rw") || !option(opts, "ro") || option(opts, "rw") ||
            !(option(opts, "noload") || option(opts, "norecovery")) ||
            strncmp(source, "/dev/loop", 9) != 0 || !source[9]) continue;
        const char *p = source + 9;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p || strlen(source) > 40) continue;
        char path[256];
#define SYS_CHECK(file, value) \
        (snprintf(path, sizeof(path), ROOT "/sys/block/%s/" file, source + 5) > 0 && read_value(path, value))
        ok = SYS_CHECK("dev", fields[2]) && SYS_CHECK("ro", "1") &&
             SYS_CHECK("loop/backing_file", "/dev/mmcblk0") &&
             SYS_CHECK("loop/offset", "639631360") &&
             SYS_CHECK("loop/sizelimit", "268435456");
#undef SYS_CHECK
    }
    if (ferror(f)) ok = 0;
    free(line);
    fclose(f);
    return matches == 1 && ok;
}

static const char *const book_names[] = {"kr.jds", "mk.jds", "ej.jds", "je.jds"};

/* The JDS directory of the checked mount, or -1. */
static int open_dir(void)
{
    if (!mount_ok()) return -1;
    int base = open(ROOT "/mnt/pomera-dict", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (base < 0) return -1;
    int dir = openat(base, "JDS", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(base);
    return dir;
}

/* Book i opened and checked (a regular file, head SDSJ, its size as the
 * header says), or -1. */
static int open_book(int dir, unsigned i)
{
    /* Nonblocking also makes an unexpected FIFO fail without waiting. */
    int fd = openat(dir, book_names[i], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char h[8];
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 8 &&
        (uint64_t)st.st_size <= UINT32_MAX && pread(fd, h, sizeof(h), 0) == (ssize_t)sizeof(h) &&
        memcmp(h, "SDSJ", 4) == 0) {
        uint32_t size = (uint32_t)h[4] | (uint32_t)h[5] << 8 |
                        (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
        if ((uint64_t)st.st_size == size) return fd;
    }
    close(fd);
    return -1;
}

unsigned dict_gate_ok(void)
{
    unsigned books = 0;
    int dir = open_dir();
    if (dir < 0) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        int fd = open_book(dir, i);
        if (fd >= 0) { books |= 1u << i; close(fd); }
    }
    close(dir);
    return books;
}

int dict_gate_open(unsigned book)
{
    unsigned i = book == DICT_GATE_KR ? 0 : book == DICT_GATE_MK ? 1 :
                 book == DICT_GATE_EJ ? 2 : book == DICT_GATE_JE ? 3 : 4;
    if (i > 3) return -1;
    int dir = open_dir();
    if (dir < 0) return -1;
    int fd = open_book(dir, i);
    close(dir);
    return fd;
}
int dict_gate_open_atok(unsigned which)
{
    static const char *const names[DICT_GATE_ATOK_COUNT] = {
        "ATEX_SPORTS.DIC", "ATEX_ANIME.DIC", "ATEX_ENTERTAINMENT.DIC", "ATOK28NM.DIC"};
    static const char *const path[] = {"ATOK", "share", "atok28"};
    if (which >= DICT_GATE_ATOK_COUNT || !mount_ok()) return -1;
    int dir = open(ROOT "/mnt/pomera-dict", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    for (unsigned i = 0; dir >= 0 && i < sizeof(path) / sizeof(path[0]); ++i) {
        int next = openat(dir, path[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        dir = next;
    }
    if (dir < 0) return -1;
    int fd = openat(dir, names[which], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(dir);
    if (fd < 0) return -1;
    unsigned char h[12];
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && pread(fd, h, sizeof(h), 0) == (ssize_t)sizeof(h) &&
        !memcmp(h, "DIC\0", 4) && !memcmp(h + 8, "ATOK", 4))
        return fd;
    close(fd);
    return -1;
}
#else
int dict_gate_open_atok(unsigned which) { (void)which; return -1; }
unsigned dict_gate_ok(void) { return 0; }
int dict_gate_open(unsigned book) { (void)book; return -1; }
#endif
