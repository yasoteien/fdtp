// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_LHA_LH5_H
#define FDTP_LHA_LH5_H
/* The LHa -lh5- compressor (revision 1 §13): LZSS over an 8 KB sliding
 * window (matches of 3 to 256 bytes) followed by static Huffman coding in
 * blocks, as the LHa archive format defines it. Written from the format, not
 * from another program's source. Data is pushed in pieces of any size; the
 * encoder keeps only its window (about 40 KB) and one block of symbols. */
#include <stddef.h>
#include <stdint.h>

/* Receives compressed bytes. Returns 0 to continue, -1 to stop. */
typedef int (*Lh5Output)(void *ctx, const unsigned char *data, size_t size);

typedef struct Lh5Encoder Lh5Encoder;

/* level 1 (fast) to 9 (thorough): how long the match search runs; the
 * format is the same at every level. NULL when memory runs out. */
Lh5Encoder *lh5_create(int level, Lh5Output output, void *ctx);
/* Compress the next size bytes. 0, or -1 when the output failed. */
int lh5_write(Lh5Encoder *e, const void *data, size_t size);
/* Compress what is left and flush the last byte. 0 or -1. */
int lh5_finish(Lh5Encoder *e);
/* Start a new, independent stream with the same level and output (the
 * next file of an archive). The hash tables are kept, not cleared: earlier
 * positions are simply never matched, so this costs nothing per file. */
void lh5_reset(Lh5Encoder *e);
/* Bytes handed to output so far (all of them after lh5_finish). */
uint64_t lh5_output_size(const Lh5Encoder *e);
void lh5_free(Lh5Encoder *e);
#endif
