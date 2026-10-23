// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_PDFTOOLS_JOB_H
#define FDTP_PDFTOOLS_JOB_H
#include <stddef.h>
/* The writing behind the PDF menu (revision 1 item 39), with MuPDF (Linux
 * only; pdftools.c asks the questions). Every output is first written as a
 * hidden file beside it (".NAME.fdtp-XXXXXX", never in /tmp), opened again
 * to check its pages, and renamed only when all of them are done; a failure
 * or job_free before the end removes them, so nothing half written stays. */
typedef enum { JOB_MERGE, JOB_SPLIT, JOB_PAGES, JOB_ROTATE, JOB_PROTECT, JOB_UNLOCK,
               JOB_COMPRESS, JOB_JPEG, JOB_IMAGES, JOB_REPAIR } JobKind;
typedef struct {
    JobKind kind;
    /* Full paths, and their passwords (NULL: none). MERGE and IMAGES: all
     * the sources in order; the others: one PDF. */
    const char *const *sources;
    const char *const *passwords;
    size_t source_count;
    /* 0-based pages. PAGES: the new document's pages in order; ROTATE and
     * JPEG: the pages to turn or draw; SPLIT: output g has pages
     * [groups[g], groups[g + 1]) of them (groups has group_count + 1). */
    const int *pages;
    size_t page_count;
    const size_t *groups;
    size_t group_count;
    int angle;              /* ROTATE: 90, 180 or 270, added to the page's own */
    int dpi;                /* JPEG; COMPRESS: images above 1.5 times this get it */
    int quality;            /* COMPRESS: the JPEG quality of recompressed photos */
    const char *password;   /* PROTECT: the new password (AES-256) */
    /* Full paths of the outputs: SPLIT one per group, JPEG one per page,
     * the others one (it may be the source itself). */
    const char *const *outputs;
    size_t output_count;
} JobSpec;
typedef struct PdfJob PdfJob;
/* Copies spec and opens the source(s). NULL with error filled on failure. */
PdfJob *job_start(const JobSpec *spec, char *error, size_t size);
/* Works for about seconds (at least one step): 0 more to do, 1 finished
 * (the outputs are in place), -1 failed (job_error; the temporaries are
 * removed). */
int job_step(PdfJob *job, double seconds);
const char *job_error(const PdfJob *job);
/* Units done of total, and a line saying what is being done. */
void job_progress(const PdfJob *job, int *done, int *total, const char **what);
/* After 1: output i's pages, bytes, and (JPEG) width x height in pixels. */
void job_result(const PdfJob *job, size_t i, int *pages, long long *bytes, int *width, int *height);
/* The size of the (first) source in bytes. */
long long job_source_bytes(const PdfJob *job);
/* Ends the job; temporaries not renamed yet are removed. NULL is fine. */
void job_free(PdfJob *job);
/* Opens path: 0 when it opens (with password, if it needs one), 1 when it
 * needs a password (none given, or a wrong one), -1 when it cannot be read
 * as a PDF. pages (the count) and encrypted may be NULL. */
int job_probe(const char *path, const char *password, int *pages, int *encrypted);
#endif
