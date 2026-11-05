// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* text/uri-list <-> paths (rmp_uri.c), the drag and drop's data: what other
 * programs send must come out as the same bytes of the path, and what is
 * not a local file must be left out. */
#include "../rmp_uri.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static size_t parse(const char *text, const char *host, char ***out)
{
    return rmp_uri_list_to_paths(text, strlen(text), host, out);
}

static void round_trip(const char *path)
{
    const char *one[1] = { path };
    size_t len = 0;
    char *list = rmp_uri_list_from_paths(one, 1, &len);
    CHECK(list && len == strlen(list));
    CHECK(len >= 2 && list[len - 2] == '\r' && list[len - 1] == '\n');
    CHECK(!strncmp(list, "file:///", 8) || !strcmp(path, "/"));
    for (size_t i = 7; i < len - 2; i++)   /* only unreserved, / and %XX */
        CHECK((list[i] >= 'A' && list[i] <= 'Z') || (list[i] >= 'a' && list[i] <= 'z') ||
              (list[i] >= '0' && list[i] <= '9') || strchr("-._~/%", list[i]));
    char **paths;
    CHECK(rmp_uri_list_to_paths(list, len, NULL, &paths) == 1);
    CHECK(!strcmp(paths[0], path));
    rmp_uri_free_paths(paths, 1);
    free(list);
}

int main(void)
{
    /* Encoding and back: blanks, %, #, ?, non-ASCII (UTF-8 and a lone
     * Shift_JIS byte), control characters, the root, a trailing slash. */
    round_trip("/data/u/a b.txt");
    round_trip("/data/u/100%.txt");
    round_trip("/tmp/#1?x=y&z;.mid");
    round_trip("/data/u/ダウンロード/画像.png");
    round_trip("/tmp/\x83\x7c.txt");
    round_trip("/tmp/tab\tand\nnewline");
    round_trip("/");
    round_trip("/tmp/dir/");
    round_trip("/tmp/a+b=c,d'e(f)~g");

    const char *two[2] = { "/a b", "/c" };
    size_t len;
    char *list = rmp_uri_list_from_paths(two, 2, &len);
    CHECK(list && !strcmp(list, "file:///a%20b\r\nfile:///c\r\n"));
    free(list);
    const char *relative[1] = { "a/b" };
    CHECK(!rmp_uri_list_from_paths(relative, 1, NULL));

    char **p;
    size_t n;
    /* What Thunar and xfdesktop send (CR LF, upper and lower case hex). */
    n = parse("file:///data/u/a%20b\r\nfile:///data/u/%e7%94%bb%E5%83%8F\r\n", NULL, &p);
    CHECK(n == 2 && !strcmp(p[0], "/data/u/a b") && !strcmp(p[1], "/data/u/画像"));
    rmp_uri_free_paths(p, n);
    /* LF only, no line end at the end, blank and comment lines, blanks
     * around a line, a NUL at the end (some programs count it). */
    n = parse("# comment\n\n  file:///x  \nfile:///y", NULL, &p);
    CHECK(n == 2 && !strcmp(p[0], "/x") && !strcmp(p[1], "/y"));
    rmp_uri_free_paths(p, n);
    {
        const char with_nul[] = "file:///z\r\n";
        n = rmp_uri_list_to_paths(with_nul, sizeof with_nul, NULL, &p);   /* the NUL included */
        CHECK(n == 1 && !strcmp(p[0], "/z"));
        rmp_uri_free_paths(p, n);
    }
    /* Hosts: empty, localhost, this host's name; another host is left out. */
    n = parse("file://localhost/a\r\nfile://iris/b\r\nfile://other/c\r\nfile:/d\r\n", "iris", &p);
    CHECK(n == 3 && !strcmp(p[0], "/a") && !strcmp(p[1], "/b") && !strcmp(p[2], "/d"));
    rmp_uri_free_paths(p, n);
    n = parse("file://iris/b\r\n", NULL, &p);
    CHECK(n == 0 && !p);
    /* Not local files, or broken: another scheme, %00, a bad or cut %, no
     * path, a relative path. The good one between them stays. */
    n = parse("http://example.com/a\r\nfile:///nul%00x\r\nfile:///bad%zz\r\nfile:///cut%4\r\n"
              "file:///ok%2Fslash\r\nfile://\r\nfile:rel\r\nFILE:///upper\r\n",
              NULL, &p);
    CHECK(n == 1 && !strcmp(p[0], "/ok/slash"));
    rmp_uri_free_paths(p, n);
    /* A query or fragment is not part of the file. */
    n = parse("file:///a%23b#frag\r\nfile:///c?q\r\n", NULL, &p);
    CHECK(n == 2 && !strcmp(p[0], "/a#b") && !strcmp(p[1], "/c"));
    rmp_uri_free_paths(p, n);
    /* Nothing at all. */
    CHECK(parse("", NULL, &p) == 0 && !p);
    CHECK(rmp_uri_list_to_paths(NULL, 0, NULL, &p) == 0 && !p);
    /* Many: the array grows. */
    {
        char big[64 * 16 + 1] = "";
        for (int i = 0; i < 64; i++) {
            char line[16];
            snprintf(line, sizeof line, "file:///f%02d\r\n", i);
            strcat(big, line);
        }
        n = parse(big, NULL, &p);
        CHECK(n == 64 && !strcmp(p[0], "/f00") && !strcmp(p[63], "/f63"));
        rmp_uri_free_paths(p, n);
    }
    puts("PASS test_uri");
    return 0;
}
