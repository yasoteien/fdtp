// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Test entrance for stage 4: opens a table directly (the launcher wiring is
 * the main session's), runs a key script and writes SNAP cells as JSON.
 * Also exposes zipfile.c to tests/check_sheet.py.
 *   sheet_harness open FILE SCRIPT PREFIX
 *   sheet_harness zip-list ZIP
 *   sheet_harness zip-cat ZIP NAME OUT
 *   sheet_harness zip-rewrite SRC DEST [put NAME FILE store|deflate | del NAME]...
 *   sheet_harness shift FORMULA DROW DCOL
 *   sheet_harness check FORMULA */
#include "sheetview.h"
#include "sheet_model.h"
#include "zipfile.h"
#include "app.h"
#include "raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* app.c's links to the filer and the settings, which this entrance leaves
 * out (update when app.c gains more). */
void filer_reload(Screen *s, App *a) { (void)s; (void)a; }
void filer_apply_settings(Screen *s, App *a) { (void)s; (void)a; }
/* UNDO_LEVELS (revision 1 item 38) from $FDTP_UNDO_LEVELS, else the default. */
int config_startup(App *a)
{
    const char *e = getenv("FDTP_UNDO_LEVELS");
    a->settings.undo_levels = e && atoi(e) > 0 ? atoi(e) : APP_UNDO_DEFAULT;
    return 0;
}
void config_free(App *a) { (void)a; }

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 32) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
static int snapshot(App *a, Canvas *canvas, Cells *cells, const char *prefix, unsigned serial)
{
    app_draw(a, canvas, cells);
    /* As headless.c: a pixel screen (the table, since its grid) drew the canvas itself. */
    Screen *top = app_top(a);
    if (!top || !top->pixel) { FDRect rects[FD_ROWS_MAX]; raster_update(canvas, cells, rects); }
    char path[4096];
    snprintf(path, sizeof(path), "%s-%03u.ppm", prefix, serial);
    if (canvas_write_ppm(canvas, path)) return -1;
    snprintf(path, sizeof(path), "%s-%03u.cells", prefix, serial);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\"rows\":[\n");
    for (int y = 0; y < FD_ROWS; ++y) {
        fputc('[', f);
        for (int x = 0; x < FD_COLS; ++x) {
            Cell v = cells->data[y][x];
            fprintf(f, "%s[%u,%u,%u,%u]", x ? "," : "", v.code, v.fg, v.bg, v.part);
        }
        fprintf(f, "]%s\n", y + 1 < FD_ROWS ? "," : "");
    }
    fprintf(f, "],\"depth\":%d,\"frame\":%d,\"widget\":%d,\"notice\":", a->depth, cells->frame, (int)a->widgets.kind);
    json_string(f, widgets_message(&a->widgets) ? a->widgets.label : "");
    fprintf(f, "}\n");
    return fclose(f) ? -1 : 0;
}
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *p = malloc(cap);
    while (p) {
        size_t k = fread(p + len, 1, cap - len - 1, f);
        len += k;
        if (!k) break;
        if (len + 1 == cap) { char *q = realloc(p, cap * 2); if (!q) { free(p); p = NULL; break; } p = q; cap *= 2; }
    }
    fclose(f);
    if (p) { p[len] = 0; if (n) *n = len; }
    return p;
}
static int open_table(const char *file, const char *script_path, const char *prefix)
{
    char *script = slurp(script_path, NULL);
    if (!script) { perror(script_path); return 2; }
    App a;
    app_init(&a);
    if (getenv("FDTP_SHEET_NOBACKUP")) sheetview_set_backup(0);
    Screen *s = sheetview_create(&a, file);
    if (!s) { fprintf(stderr, "%s\n", a.widgets.label); printf("NOTICE %s\n", a.widgets.label); free(script); app_destroy(&a); return 3; }
    if (app_push(&a, s)) { s->destroy(s); free(script); app_destroy(&a); return 2; }
    Canvas *canvas = malloc(sizeof(*canvas));
    Cells cells;
    cells_init(&cells);
    if (!canvas) { free(script); app_destroy(&a); return 2; }
    canvas_init(canvas);
    KeyScript k;
    keys_script_init(&k, script);
    unsigned serial = 0;
    int rc = 0;
    for (;;) {
        ScriptEvent e = keys_script_next(&k);
        if (e.kind == SCRIPT_END) break;
        if (e.kind == SCRIPT_ERROR) { fprintf(stderr, "script: %s\n", k.error); rc = 2; break; }
        if (e.kind == SCRIPT_WAIT) { app_tick(&a, a.now + e.seconds); continue; }
        if (e.kind == SCRIPT_KEY) {
            if (!app_top(&a)) { fprintf(stderr, "script: key after the screen closed\n"); rc = 4; break; }
            app_key(&a, &e.key);
            continue;
        }
        if (snapshot(&a, canvas, &cells, prefix, ++serial)) { perror(prefix); rc = 2; break; }
        printf("SNAP %u %d %s\n", serial, a.depth, widgets_message(&a.widgets) ? a.widgets.label : "");
    }
    printf("END %d\n", a.depth);
    app_destroy(&a);
    free(canvas);
    free(script);
    return rc;
}
int main(int argc, char **argv)
{
    char err[256] = "";
    if (argc == 5 && !strcmp(argv[1], "open")) return open_table(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "zip-list")) {
        ZipArchive *z;
        if (zip_open(argv[2], &z, err, sizeof(err))) { printf("ERROR %s\n", err); return 3; }
        printf("ZIP64 %d\n", zip_is_zip64(z));
        for (size_t i = 0; i < zip_count(z); ++i) {
            const ZipEntry *e = zip_entry(z, i);
            printf("%s\t%u\t%llu\t%llu\t%08x\t%u\n", e->name, e->method, (unsigned long long)e->compressed_size,
                   (unsigned long long)e->uncompressed_size, e->crc32, e->flags);
        }
        zip_close(z);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "zip-cat")) {
        ZipArchive *z;
        if (zip_open(argv[2], &z, err, sizeof(err))) { printf("ERROR %s\n", err); return 3; }
        long i = zip_find(z, argv[3]);
        unsigned char *data;
        size_t n;
        if (i < 0) { printf("ERROR missing\n"); zip_close(z); return 3; }
        if (zip_read(z, (size_t)i, 1u << 30, &data, &n, err, sizeof(err))) { printf("ERROR %s\n", err); zip_close(z); return 3; }
        FILE *f = fopen(argv[4], "wb");
        int ok = f && fwrite(data, 1, n, f) == n;
        if (f && fclose(f)) ok = 0;
        free(data); zip_close(z);
        return ok ? 0 : 2;
    }
    if (argc >= 4 && !strcmp(argv[1], "zip-rewrite")) {
        ZipArchive *z = NULL;
        if (strcmp(argv[2], "-") && zip_open(argv[2], &z, err, sizeof(err))) { printf("ERROR %s\n", err); return 3; }
        ZipChange changes[32];
        char *data[32];
        size_t n = 0;
        memset(data, 0, sizeof(data));
        for (int i = 4; i < argc && n < 32;) {
            if (!strcmp(argv[i], "put") && i + 3 < argc) {
                size_t len = 0;
                data[n] = slurp(argv[i + 2], &len);
                if (!data[n]) { perror(argv[i + 2]); return 2; }
                changes[n] = (ZipChange){argv[i + 1], 0, strcmp(argv[i + 3], "store") ? ZIP_DEFLATE : ZIP_STORE, data[n], len, NULL, NULL};
                ++n; i += 4;
            } else if (!strcmp(argv[i], "del") && i + 1 < argc) {
                changes[n++] = (ZipChange){argv[i + 1], 1, 0, NULL, 0, NULL, NULL};
                i += 2;
            } else return 2;
        }
        int rc = zip_rewrite(z, argv[3], changes, n, err, sizeof(err));
        for (size_t i = 0; i < n; ++i) free(data[i]);
        zip_close(z);
        if (rc) { printf("ERROR %s\n", err); return 3; }
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "shift")) {
        char *t = formula_shift(argv[2], atol(argv[3]), atol(argv[4]));
        printf("%s\n", t ? t : "(null)");
        free(t);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "check")) { printf("%d\n", (int)formula_check(argv[2])); return 0; }
    fprintf(stderr, "usage: see tests/sheet_harness.c\n");
    return 2;
}
