// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* text/uri-list (RFC 2483) to and from local file paths, for drag and drop
 * (rmp_xdnd.c). Pure C, no X headers: tested on its own (tests/test_uri.c). */
#ifndef RMP_URI_H
#define RMP_URI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "file:///a%20b\r\n" for each absolute path (bytes outside A-Z a-z 0-9
 * - . _ ~ / as %XX, UTF-8 bytes included). malloc'd, NULL on failure or when
 * a path is not absolute. *len (may be NULL) gets the length. */
char *rmp_uri_list_from_paths(const char *const *paths, size_t count, size_t *len);

/* The local paths in a text/uri-list: lines split at CR LF (or LF), lines
 * beginning with # skipped, only file: URIs whose host is empty, localhost
 * or hostname (may be NULL) taken, %XX decoded. A URI with a bad %, %00, a
 * relative path or another scheme is left out. *out gets a malloc'd array
 * of malloc'd strings (rmp_uri_free_paths); returns how many, 0 with *out
 * NULL when none (or out of memory). */
size_t rmp_uri_list_to_paths(const char *data, size_t len, const char *hostname, char ***out);
void rmp_uri_free_paths(char **paths, size_t count);

#ifdef __cplusplus
}
#endif
#endif
