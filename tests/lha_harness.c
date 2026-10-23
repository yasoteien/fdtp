// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* tests/check_lha.py: drives src/lha_write.c as the p box does.
 * lha-harness [-l LEVEL] [-u] [-a] [-n] [-s BYTES] DEST DIR NAME...
 *   -u UTF-8 names (default Shift_JIS), -a add to DEST, -n newer only,
 *   -s bytes per step (default 65536).
 * Prints one JSON line: result, steps, progress samples, statistics. */
#include "lha_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    LhaOptions o = {6, 0, 0};
    int add = 0, c;
    size_t slice = 65536;
    while ((c = getopt(argc, argv, "l:uans:")) != -1) {
        if (c == 'l') o.level = atoi(optarg);
        else if (c == 'u') o.utf8_names = 1;
        else if (c == 'a') add = 1;
        else if (c == 'n') o.newer_only = 1;
        else if (c == 's') slice = (size_t)strtoull(optarg, NULL, 10);
        else return 2;
    }
    if (argc - optind < 3) { fprintf(stderr, "usage: lha-harness [-l N] [-u] [-a] [-n] [-s BYTES] DEST DIR NAME...\n"); return 2; }
    char err[256] = "";
    const char *dest = argv[optind], *dir = argv[optind + 1];
    LhaJob *j = lha_job_create(dest, add, &o, err, sizeof(err));
    int r = j ? 0 : -1;
    for (int i = optind + 2; i < argc && !r; ++i) r = lha_job_add(j, dir, argv[i], err, sizeof(err));
    if (!r) r = lha_job_start(j, err, sizeof(err));
    long steps = 0;
    uint64_t last_done = 0;
    int monotonic = 1;
    char first_name[512] = "";
    size_t first_done = 0, first_total = 0;
    while (!r) {
        int s = lha_job_step(j, slice, err, sizeof(err));
        ++steps;
        if (lha_job_bytes_done(j) < last_done || lha_job_bytes_done(j) > lha_job_bytes_total(j)) monotonic = 0;
        last_done = lha_job_bytes_done(j);
        if (steps == 1) {
            snprintf(first_name, sizeof(first_name), "%s", lha_job_current(j));
            first_done = lha_job_done(j); first_total = lha_job_total(j);
        }
        if (s < 0) r = -1;
        if (s == 0) break;
    }
    LhaStats st = {0};
    if (j) lha_job_stats(j, &st);
    printf("{\"result\":%d,\"error\":\"", r);
    for (const char *p = err; *p; ++p) { if (*p == '"' || *p == '\\') putchar('\\'); putchar(*p); }
    printf("\",\"steps\":%ld,\"monotonic\":%s,\"first\":{\"name\":\"", steps, monotonic ? "true" : "false");
    for (const char *p = first_name; *p; ++p) { if (*p == '"' || *p == '\\') putchar('\\'); putchar(*p); }
    printf("\",\"done\":%zu,\"total\":%zu},\"added\":%zu,\"kept\":%zu,\"skipped\":%zu,\"older\":%zu,\"stored\":%zu,"
           "\"in\":%llu,\"out\":%llu}\n", first_done, first_total, st.added, st.kept, st.skipped, st.older, st.stored,
           (unsigned long long)st.in_bytes, (unsigned long long)st.out_bytes);
    lha_job_free(j);
    return r ? 1 : 0;
}
