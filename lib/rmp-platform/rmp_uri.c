// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* text/uri-list and file paths. See rmp_uri.h. */
#include "rmp_uri.h"

#include <stdlib.h>
#include <string.h>

static int unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

char *rmp_uri_list_from_paths(const char *const *paths, size_t count, size_t *len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t need = 1;
    for (size_t i = 0; i < count; i++) {
        if (!paths[i] || paths[i][0] != '/') return NULL;
        need += 7 + 2;   /* file:// and CR LF */
        for (const unsigned char *p = (const unsigned char *)paths[i]; *p; p++) need += unreserved(*p) ? 1 : 3;
    }
    char *out = malloc(need), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < count; i++) {
        memcpy(o, "file://", 7);
        o += 7;
        for (const unsigned char *p = (const unsigned char *)paths[i]; *p; p++) {
            if (unreserved(*p)) *o++ = (char)*p;
            else {
                *o++ = '%';
                *o++ = hex[*p >> 4];
                *o++ = hex[*p & 15];
            }
        }
        *o++ = '\r';
        *o++ = '\n';
    }
    *o = 0;
    if (len) *len = (size_t)(o - out);
    return out;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One URI (no line end) to a malloc'd path, or NULL when it is not local. */
static char *path_of(const char *uri, size_t n, const char *hostname)
{
    if (n < 5 || strncmp(uri, "file:", 5)) return NULL;
    const char *p = uri + 5, *end = uri + n;
    if (end - p >= 2 && p[0] == '/' && p[1] == '/') {   /* an authority: the host */
        p += 2;
        const char *slash = memchr(p, '/', (size_t)(end - p));
        if (!slash) return NULL;
        size_t host = (size_t)(slash - p);
        if (host && !(host == 9 && !strncmp(p, "localhost", 9)) &&
            !(hostname && strlen(hostname) == host && !strncmp(p, hostname, host)))
            return NULL;
        p = slash;
    }
    if (p == end || *p != '/') return NULL;
    char *out = malloc((size_t)(end - p) + 1), *o = out;
    if (!out) return NULL;
    while (p < end) {
        if (*p == '%') {
            int hi = end - p >= 3 ? hex_value(p[1]) : -1, lo = hi >= 0 ? hex_value(p[2]) : -1;
            if (lo < 0 || (hi == 0 && lo == 0)) {
                free(out);
                return NULL;
            }
            *o++ = (char)(hi * 16 + lo);
            p += 3;
        } else if (*p == '?' || *p == '#') {   /* a query or fragment is no part of a file */
            break;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    return out;
}

size_t rmp_uri_list_to_paths(const char *data, size_t len, const char *hostname, char ***out)
{
    *out = NULL;
    size_t count = 0, cap = 0;
    char **paths = NULL;
    const char *p = data, *end = data ? data + len : data;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end, *next = nl ? nl + 1 : end;
        while (line_end > p && (line_end[-1] == '\r' || line_end[-1] == 0)) line_end--;
        /* Leading and trailing blanks are not part of a URI. */
        while (p < line_end && (*p == ' ' || *p == '\t')) p++;
        while (line_end > p && (line_end[-1] == ' ' || line_end[-1] == '\t')) line_end--;
        if (line_end > p && *p != '#') {
            char *path = path_of(p, (size_t)(line_end - p), hostname);
            if (path) {
                if (count == cap) {
                    size_t ncap = cap ? cap * 2 : 8;
                    char **grown = realloc(paths, ncap * sizeof *grown);
                    if (!grown) {
                        free(path);
                        rmp_uri_free_paths(paths, count);
                        return 0;
                    }
                    paths = grown;
                    cap = ncap;
                }
                paths[count++] = path;
            }
        }
        p = next;
    }
    *out = paths;
    return count;
}

void rmp_uri_free_paths(char **paths, size_t count)
{
    if (!paths) return;
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}
