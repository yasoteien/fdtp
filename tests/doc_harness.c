// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Test entrance for stage 5: opens a docx directly (the launcher already
 * calls docview_create; this avoids the filer), runs a key script and
 * writes SNAP cells as JSON for tests/check_doc.py.
 *   doc_harness open FILE SCRIPT PREFIX */
#include "docview.h"
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
    FDRect rects[FD_ROWS_MAX];
    raster_update(canvas, cells, rects);
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
    char separators[64];
    cells_separator_text(cells, separators, sizeof(separators));
    /* The boxes (box 0: the screen's frame, revision 1 item 29) and the lines
     * in them: [at, from, to, vertical, box]. */
    fprintf(f, "],\"boxes\":[");
    for (int i = 0; i < cells->box_count; ++i) {
        const CellBox *b = &cells->boxes[i];
        fprintf(f, "%s[%d,%d,%d,%d,%d,%d]", i ? "," : "", b->col, b->row, b->w, b->h, b->rule, b->kind);
    }
    fprintf(f, "],\"lines\":[");
    for (int i = 0; i < cells->line_count; ++i) {
        const CellLine *l = &cells->lines[i];
        fprintf(f, "%s[%d,%d,%d,%d,%d]", i ? "," : "", l->at, l->from, l->to, l->vertical, l->box);
    }
    fprintf(f, "],\"depth\":%d,\"frame\":%d,\"separators\":%s,\"widget\":%d,\"notice\":", a->depth, cells->frame,
            separators, (int)a->widgets.kind);
    json_string(f, widgets_message(&a->widgets) ? a->widgets.label : "");
    fprintf(f, "}\n");
    return fclose(f) ? -1 : 0;
}
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *p = malloc(cap);
    while (p) {
        size_t k = fread(p + len, 1, cap - len - 1, f);
        len += k;
        if (!k) break;
        if (len + 1 == cap) {
            char *q = realloc(p, cap * 2);
            if (!q) { free(p); p = NULL; break; }
            p = q;
            cap *= 2;
        }
    }
    fclose(f);
    if (p) p[len] = 0;
    return p;
}
static int open_doc(const char *file, const char *script_path, const char *prefix)
{
    char *script = slurp(script_path);
    if (!script) { perror(script_path); return 2; }
    App a;
    app_init(&a);
    Screen *s = docview_create(&a, file);
    if (!s) { printf("NOTICE %s\n", a.widgets.label); free(script); app_destroy(&a); return 3; }
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
    if (argc == 5 && !strcmp(argv[1], "open")) return open_doc(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: doc_harness open FILE SCRIPT PREFIX\n");
    return 2;
}
