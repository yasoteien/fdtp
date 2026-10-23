// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_LHA_WRITE_H
#define FDTP_LHA_WRITE_H
/* Creating LHa archives (revision 1 §13): level 2 headers, members
 * compressed with -lh5- (lha_lh5.h), or stored (-lh0-) when compression
 * would make them larger. Directories are stored with their contents
 * (a -lhd- header each), symbolic links as LHa for UNIX does
 * (-lhd-, "name|target", mode S_IFLNK), modification times, UNIX modes and
 * CRC16 kept. Names are Shift_JIS (the default, readable on a PC-98) or
 * UTF-8, '/' between the directories in the 0x02 header.
 *
 * The work is a job done in steps, so that a screen can show progress:
 * lha_job_create, lha_job_add for each name, lha_job_start, then
 * lha_job_step until it returns 0. Files are read in 64 KB pieces. The new
 * archive is written to a temporary file beside dest and renamed over it at
 * the end; on failure or lha_job_free before the end nothing is left.
 * Error messages are Japanese, for row 23. */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int level;       /* 1..9: search effort of the compressor */
    int utf8_names;  /* 0: Shift_JIS (CP932), 1: UTF-8 as on disk */
    int newer_only;  /* adding: replace a member only by a newer file */
} LhaOptions;

typedef struct LhaJob LhaJob;

/* dest: the archive to write. With add, dest is an existing archive whose
 * members are kept (copied byte for byte) unless a file of the same name
 * replaces them: always, or with newer_only only when the file's time is
 * later than the member's. NULL on failure (err). */
LhaJob *lha_job_create(const char *dest, int add, const LhaOptions *options, char *err, size_t errlen);
/* Add dir/name (a file, a directory with everything under it, or a link);
 * name is the stored path ('/'-separated, relative). The archive itself is
 * never added. Other kinds of files (devices, sockets, fifos) are skipped
 * and counted. -1 when a name cannot be stored (not Shift_JIS, too long),
 * a file is 4 GB or larger, or a directory cannot be read. */
int lha_job_add(LhaJob *job, const char *dir, const char *name, char *err, size_t errlen);
/* Reads the existing archive (add), decides what is replaced and opens the
 * temporary file. 0 or -1. */
int lha_job_start(LhaJob *job, char *err, size_t errlen);
/* Writes about budget bytes (input read plus bytes copied). 1: more to do,
 * 0: finished (dest is in place), -1: failed (nothing written to dest). */
int lha_job_step(LhaJob *job, size_t budget, char *err, size_t errlen);
void lha_job_free(LhaJob *job);

/* Progress. Members are counted when written (copied ones included). */
size_t lha_job_done(const LhaJob *job);
size_t lha_job_total(const LhaJob *job);
uint64_t lha_job_bytes_done(const LhaJob *job);
uint64_t lha_job_bytes_total(const LhaJob *job);
/* The stored path (UTF-8 as on disk) being written, "" between members. */
const char *lha_job_current(const LhaJob *job);
/* After the end: members added or replaced, kept from the old archive,
 * files skipped (special files), files not replaced (newer_only),
 * and how many went in stored (-lh0-). */
typedef struct { size_t added, kept, skipped, older, stored; uint64_t in_bytes, out_bytes; } LhaStats;
void lha_job_stats(const LhaJob *job, LhaStats *stats);

/* CRC-16 as LHa uses it (polynomial 0xA001, reflected, initial 0). */
uint16_t lha_crc16(uint16_t crc, const void *data, size_t size);
#endif
