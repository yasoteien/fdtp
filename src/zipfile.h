// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_ZIPFILE_H
#define FDTP_ZIPFILE_H
/* A small zip reader/writer on zlib, shared by the Office viewers and the
 * archive browser (docs/FDTP_DECISIONS.md: no libzip). Reads store and
 * deflate, including ZIP64 archives; writes plain zip only (no ZIP64).
 * All functions report a Japanese message in err (may be NULL) on failure. */
#include <stddef.h>
#include <stdint.h>

enum { ZIP_STORE = 0, ZIP_DEFLATE = 8 };

typedef struct {
    char *name;                 /* NUL-terminated, as stored (no conversion) */
    uint16_t version_made, version_needed, flags, method, mod_time, mod_date;
    uint16_t internal_attr;
    uint32_t external_attr, crc32;
    uint64_t compressed_size, uncompressed_size, local_offset;
    unsigned char *extra;       /* central directory extra field, raw */
    unsigned char *comment;
    uint16_t extra_len, comment_len;
    int zip64;                  /* the entry used ZIP64 fields */
} ZipEntry;

typedef struct ZipArchive ZipArchive;

/* Opens and reads the central directory. 0 on success, -1 on failure. */
int zip_open(const char *path, ZipArchive **out, char *err, size_t errlen);
void zip_close(ZipArchive *z);
size_t zip_count(const ZipArchive *z);
const ZipEntry *zip_entry(const ZipArchive *z, size_t index);
/* Exact (byte) name match; -1 when absent. */
long zip_find(const ZipArchive *z, const char *name);
/* 1 when the archive or one of its entries uses ZIP64 records. */
int zip_is_zip64(const ZipArchive *z);
const char *zip_path(const ZipArchive *z);

/* The sink returns 0 to continue, nonzero to abort (zip_read_stream then
 * returns -1 without touching err when it is the sink's own failure). */
typedef int (*ZipSink)(void *ctx, const unsigned char *data, size_t size);
/* Streams one entry's uncompressed bytes; checks the size and CRC32 at the
 * end (a mismatch is reported after the data has been passed on). */
int zip_read_stream(ZipArchive *z, size_t index, ZipSink sink, void *ctx, char *err, size_t errlen);
/* Reads one entry into a malloc'd buffer with an extra NUL byte (not counted
 * in *size). Entries larger than limit fail. */
int zip_read(ZipArchive *z, size_t index, size_t limit, unsigned char **data, size_t *size,
             char *err, size_t errlen);

typedef struct ZipWriter ZipWriter;
/* Producers call this to append uncompressed bytes. 0 ok, -1 failure. */
int zip_writer_write(ZipWriter *w, const void *data, size_t size);
typedef int (*ZipProducer)(void *ctx, ZipWriter *w);

typedef struct {
    const char *name;
    int remove;                 /* 1: drop the entry, the rest is ignored */
    int method;                 /* ZIP_STORE or ZIP_DEFLATE */
    const void *data;           /* used when produce is NULL */
    size_t size;
    ZipProducer produce;        /* writes the content through the writer */
    void *ctx;
} ZipChange;

/* Writes a new archive to a temporary file beside dest, then rename()s it
 * over dest (src may be dest itself; src may be NULL for a new archive).
 * Entries of src are copied byte for byte (local header, data, descriptor,
 * central record) in their order; an entry named by a change is replaced
 * in place or removed, and changed names not in src are appended in the
 * order given. A replaced entry keeps its attributes and comment. dest's
 * file mode is kept when it exists. ZIP64 sources are refused. On failure
 * dest is untouched and the temporary file removed. Producers may read
 * from src (reads use pread). */
int zip_rewrite(ZipArchive *src, const char *dest, const ZipChange *changes, size_t count,
                char *err, size_t errlen);
#endif
